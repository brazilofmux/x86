; litmus.asm — message-passing litmus test for two processors, a static
; i386 Linux binary with no libc: fork, a shared page, each half pinned to
; a CPU. P0 stores x=1 then y=1 (plain stores); P1 loads y then x. On x86
; (TSO) y=1 with x=0 can never be seen; on a weaker machine it can.
; Prints the count of forbidden outcomes over N iterations.
;
; Build:  nasm -f bin -o litmus tools/litmus.asm
; Run:    mcopy -o -i IMG@@1048576 litmus ::/LITMUS  (into the Tiny Core
;         image's FAT partition, a copy of disks/linux/tc.img), boot it
;         with -smp 2, then in the guest:
;           sudo mount -t vfat /dev/sda1 /mnt/sda1; sudo cp /mnt/sda1/LITMUS /tmp/l; /tmp/l
; Measured 2026-10-16 (dos-monster -smp 2 on an M-series Mac): with plain
; host loads and stores 8, 4, 9 forbidden outcomes per run; with the
; translator's acquire loads and release stores (dbt_tso) 0, 0, 0.
BITS 32
org 0x08048000
N equ 2000000
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

; shared page layout: 0 x, 64 y, 128 flag0, 192 flag1, 256 count (separate lines)
X     equ 0
Y     equ 64
FLAG0 equ 128
FLAG1 equ 192
COUNT equ 256

_start:
    ; mmap2(0, 4096, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0)
    mov eax, 192
    xor ebx, ebx
    mov ecx, 4096
    mov edx, 3
    mov esi, 0x21
    mov edi, -1
    xor ebp, ebp
    int 0x80
    mov [page], eax
    ; fork
    mov eax, 2
    int 0x80
    test eax, eax
    jz child
    ; parent = P0 on CPU 0
    mov dword [mask], 1
    call pin
    mov esi, [page]
    mov ecx, 1
.p0loop:
    ; wait until P1 finished iteration ecx-1
    lea edx, [ecx - 1]
.w0: cmp [esi + FLAG1], edx
    jne .w0
    mov dword [esi + X], 0
    mov dword [esi + Y], 0
    mov eax, ecx
    xchg [esi + FLAG0], eax          ; locked: the resets are visible before the flag
    ; the body
    mov dword [esi + X], 1
    mov dword [esi + Y], 1
    inc ecx
    cmp ecx, N
    jbe .p0loop
    ; wait for the last result, then print count
.wl: cmp dword [esi + FLAG1], N
    jne .wl
    mov eax, [esi + COUNT]
    call print
    mov eax, 1
    xor ebx, ebx
    int 0x80

child:
    mov dword [mask], 2
    call pin
    mov esi, [page]
    mov ecx, 1
.p1loop:
.w1: cmp [esi + FLAG0], ecx
    jne .w1
    ; the body
    mov eax, [esi + Y]
    mov ebx, [esi + X]
    cmp eax, 1
    jne .ok
    test ebx, ebx
    jne .ok
    inc dword [esi + COUNT]          ; y seen, x not: forbidden on x86
.ok:
    mov [esi + FLAG1], ecx
    inc ecx
    cmp ecx, N
    jbe .p1loop
    mov eax, 1
    xor ebx, ebx
    int 0x80

pin:                                 ; sched_setaffinity(0, 4, &mask)
    mov eax, 241
    xor ebx, ebx
    mov ecx, 4
    mov edx, mask
    int 0x80
    ret

print:                               ; eax: unsigned, decimal + newline on stdout
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
    ret

page: dd 0
mask: dd 0
buf:  times 12 db 0
filesize equ $ - $$
