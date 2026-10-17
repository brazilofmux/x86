; qlock.asm — NT's queued spinlock, instruction for instruction, on two
; processors: a static i386 Linux binary with no libc (fork, a shared
; page, each half pinned to a CPU). Each half takes the lock N times with
; KeAcquireQueuedSpinLock's sequence (XCHG onto the tail, a WAIT bit set
; before the predecessor's Next is linked, a spin on the WAIT bit) and
; releases it with KeReleaseQueuedSpinLock's (XOR 3 into the successor's
; Lock word, else a LOCK CMPXCHG of the tail), and counts under it. The
; count must come out 2N; a half that waits more than 20 s for the other
; prints HANG and the lock state (what Windows 2000's dispatcher lock
; showed on dos-monster before the fix: both entries WAIT, the tail the
; second one).
;
; Build:  nasm -f bin -o qlock tools/qlock.asm
; Run:    mcopy -o -i IMG@@1048576 qlock ::/QLOCK  (into a copy of
;         disks/linux/tc.img), boot it with -smp 2, then in the guest:
;           sudo mount -t vfat /dev/sda1 /mnt/sda1; sudo cp /mnt/sda1/QLOCK /tmp/q; /tmp/q
BITS 32
org 0x08048000
N equ 1000000
ehdr:
    db 0x7F, "ELF", 1, 1, 1, 0
    times 8 db 0
    dw 2, 3
    dd 1, _start, phdr - $$, 0, 0
    dw ehdrsize, phdrsize, 1, 0, 0, 0
ehdrsize equ $ - ehdr
phdr:
    dd 1, 0, $$, $$, filesize, filesize, 7, 0x1000
phdrsize equ $ - phdr

; shared page layout (separate 64-byte lines)
LOCKW equ 0          ; the KSPIN_LOCK: tail pointer or 0
E0    equ 64         ; CPU 0's KSPIN_LOCK_QUEUE {Next, Lock}
E1    equ 128        ; CPU 1's
COUNT equ 192        ; incremented under the lock
DONE0 equ 256
DONE1 equ 320

_start:
    mov eax, 192                     ; mmap2(0, 4096, RW, SHARED|ANON, -1, 0)
    xor ebx, ebx
    mov ecx, 4096
    mov edx, 3
    mov esi, 0x21
    mov edi, -1
    xor ebp, ebp
    int 0x80
    mov [page], eax
    lea ecx, [eax + LOCKW]
    mov [eax + E0 + 4], ecx          ; entry->Lock = &lock, no flags
    mov [eax + E1 + 4], ecx
    mov eax, 2                       ; fork
    int 0x80
    test eax, eax
    jz child
    mov dword [mask], 1
    call pin
    mov esi, [page]
    lea edi, [esi + E0]
    mov ebp, DONE0
    jmp run

child:
    mov dword [mask], 2
    call pin
    mov esi, [page]
    lea edi, [esi + E1]
    mov ebp, DONE1

run:                                 ; esi page, edi my entry, ebp my DONE offset
    mov dword [iter], 0
.loop:
    mov eax, edi
    call acquire
    inc dword [esi + COUNT]          ; the critical section
    mov eax, edi
    call release
    ; a gap that varies with the iteration, so the lock is often free when
    ; the other half arrives (the release's CMPXCHG of the tail against the
    ; other's XCHG onto it), not only handed off under contention
    mov ecx, [iter]
    imul ecx, 7919
    and ecx, 0x3FF
    inc ecx
.gap:
    pause
    dec ecx
    jnz .gap
    inc dword [iter]
    cmp dword [iter], N
    jb .loop
    mov dword [esi + ebp], 1
    ; wait for the other half, 20 s at most
    mov eax, 13                      ; time(0)
    xor ebx, ebx
    int 0x80
    mov [t0], eax
.wait:
    cmp dword [esi + DONE0], 1
    jne .nf
    cmp dword [esi + DONE1], 1
    je .finished
.nf:
    mov eax, 13
    xor ebx, ebx
    int 0x80
    sub eax, [t0]
    cmp eax, 20
    jb .wait
    ; HANG: the lock word and both entries, low 12 bits of each in decimal
    mov eax, [esi + LOCKW]
    and eax, 0xFFF
    add eax, 1000000                 ; 1000000 + (tail & FFF): a tag
    call print
    mov eax, [esi + E0]
    and eax, 0xFFF
    add eax, 2000000
    call print
    mov eax, [esi + E0 + 4]
    and eax, 0xFFF
    add eax, 3000000
    call print
    mov eax, [esi + E1]
    and eax, 0xFFF
    add eax, 4000000
    call print
    mov eax, [esi + E1 + 4]
    and eax, 0xFFF
    add eax, 5000000
    call print
    jmp .exit
.finished:
    cmp ebp, DONE0
    jne .exit                        ; the parent prints the count
    mov eax, [esi + COUNT]
    call print
.exit:
    mov eax, 1
    xor ebx, ebx
    int 0x80

; KeAcquireQueuedSpinLock's core: eax = my entry
acquire:
    push ecx
    push edx
    mov ecx, [eax + 4]               ; &lock (flags clear between uses)
    mov edx, eax
    xchg edx, [ecx]                  ; onto the tail; edx = the old tail
    test edx, edx
    jnz .queued
    or ecx, 2                        ; LQ_OWN
    mov [eax + 4], ecx
    pop edx
    pop ecx
    ret
.queued:
    or ecx, 1                        ; LQ_WAIT, set before the link
    mov [eax + 4], ecx
    mov [edx], eax                   ; predecessor->Next = me
.spin:
    test dword [eax + 4], 1
    jz .got
    pause
    jmp .spin
.got:
    pop edx
    pop ecx
    ret

; KeReleaseQueuedSpinLock's core: eax = my entry
release:
    push ebx
    push ecx
    mov ebx, [eax]                   ; Next
    mov ecx, [eax + 4]
    test ebx, ebx
    btr ecx, 1                       ; LQ_OWN off
    mov [eax + 4], ecx
    jnz .handoff
    push eax
    lock cmpxchg [ecx], ebx          ; tail == me: tail = 0
    pop eax
    jnz .waitnext
    pop ecx
    pop ebx
    ret
.handoff:
    xor dword [ebx + 4], 3           ; successor: WAIT off, OWN on
    mov dword [eax], 0
    pop ecx
    pop ebx
    ret
.waitnext:
    mov ebx, [eax]
    test ebx, ebx
    jnz .handoff
    pause
    jmp .waitnext

pin:                                 ; sched_setaffinity(0, 4, &mask)
    mov eax, 241
    xor ebx, ebx
    mov ecx, 4
    mov edx, mask
    int 0x80
    ret

print:                               ; eax: unsigned, decimal + newline on stdout
    push ebx
    push ecx
    push edx
    push edi
    mov edi, buf + 11
    mov byte [edi], 10
    mov ebx, 10
.d: xor edx, edx
    div ebx
    add dl, '0'
    dec edi
    mov [edi], dl
    test eax, eax
    jnz .d
    mov ecx, edi
    mov edx, buf + 12
    sub edx, edi
    mov eax, 4
    mov ebx, 1
    int 0x80
    pop edi
    pop edx
    pop ecx
    pop ebx
    ret

page: dd 0
mask: dd 0
iter: dd 0
t0:   dd 0
buf:  times 12 db 0
filesize equ $ - $$
