; cpu586.asm — the Pentium against the 486, as CPU-detection code and an
; operating system's startup see them.
;
; EFLAGS.ID (bit 21) toggles only where CPUID exists. Then, under #UD and
; #GP handlers of our own (real mode is CPL 0, so the privileged ones run):
; CPUID leaves 0 and 1 (GenuineIntel, a P54C: the features built — FPU, DE,
; PSE, TSC, MSR, MCE, CX8, not VME); RDTSC counting up; WRMSR/RDMSR of the
; TSC; RDMSR of an MSR the P5 lacks (#GP); CMPXCHG8B both ways, with the
; defined flags (only ZF changes; mask 8D5h as in cpu486); CR4 read, loaded
; with the bits we have, and loaded with VME (#GP); and DR4 under CR4.DE
; (#UD — on the 486, whose CR4 is #UD, DR4 is DR6 and reads fine). Expected
; outputs (cpu586-486.out, cpu586-586.out) are worked from the SDM.
;
;   nasm -f bin -o cpu586.com cpu586.asm
        cpu 586
        org 0x100

start:
        ; ---- EFLAGS.ID: CPUID
        mov edx, 0x200000
        call toggles
        mov dx, s_id0
        jz .id
        mov dx, s_id1
.id:    call puts

        ; ---- our #UD and #GP handlers
        xor ax, ax
        mov es, ax
        mov eax, [es:6*4]
        mov [old6], eax
        mov eax, [es:13*4]
        mov [old13], eax
        cli
        mov word [es:6*4], ud_handler
        mov [es:6*4+2], cs
        mov word [es:13*4], gp_handler
        mov [es:13*4+2], cs
        sti

        ; ---- CPUID 0: the highest leaf and the vendor
        mov dx, s_cpuid0
        call puts
        mov byte [skip], c0_e - c0_s
        call arm
        xor eax, eax
c0_s:   cpuid
c0_e:   call faulted
        jc .c1
        call hex32
        call space
        mov [vendor], ebx
        mov [vendor+4], edx
        mov [vendor+8], ecx
        mov dx, vendor
        call puts
        call crlf

.c1:    ; ---- CPUID 1: signature and features
        mov dx, s_cpuid1
        call puts
        mov byte [skip], c1_e - c1_s
        call arm
        mov eax, 1
c1_s:   cpuid
c1_e:   call faulted
        jc .tsc
        call hex32
        call space
        mov eax, edx
        call hex32
        call crlf

.tsc:   ; ---- RDTSC twice: the second is later
        mov dx, s_rdtsc
        call puts
        mov byte [skip], t1_e - t1_s
        call arm
t1_s:   rdtsc
t1_e:   call faulted
        jc .wr
        mov esi, eax
        mov edi, edx
        nop
        nop
        rdtsc
        sub eax, esi
        sbb edx, edi
        jc .tn                  ; earlier
        or eax, edx
        jz .tn                  ; the same
        mov dx, s_later
        jmp .tl
.tn:    mov dx, s_notlater
.tl:    call puts

.wr:    ; ---- WRMSR 10h = 1:00000000, then RDMSR 10h: EDX 1, EAX a few ticks
        mov dx, s_wrmsr
        call puts
        mov byte [skip], w_e - w_s
        call arm
        mov ecx, 0x10
        mov edx, 1
        xor eax, eax
w_s:    wrmsr
w_e:    call faulted
        jc .gp
        rdmsr
        push eax
        mov eax, edx
        call hex32
        call space
        pop eax
        mov dx, s_small
        cmp eax, 100
        jb .ws
        mov dx, s_big
.ws:    call puts

.gp:    ; ---- RDMSR 1Bh (the P6's APIC base): not a P5 MSR
        mov dx, s_rdmsr
        call puts
        mov byte [skip], g_e - g_s
        call arm
        mov ecx, 0x1B
g_s:    rdmsr
g_e:    call faulted
        jc .x1
        mov dx, s_ok
        call puts

.x1:    ; ---- CMPXCHG8B, equal: ECX:EBX stored, ZF set, EDX:EAX kept.
        ; The flags going in: 7Fh + 1 (OF SF AF set, ZF PF CF clear).
        mov dx, s_x1
        call puts
        mov dword [q64], 0x22222222
        mov dword [q64+4], 0x11111111
        mov edx, 0x11111111
        mov eax, 0x22222222
        mov ecx, 0x33333333
        mov ebx, 0x44444444
        mov byte [skip], x1_e - x1_s
        call arm
        push ax
        mov al, 0x7F
        add al, 1
        pop ax
x1_s:   cmpxchg8b [q64]
x1_e:   pushf
        call faulted
        jc .x1f
        call x8_line
        jmp .x2
.x1f:   popf

.x2:    ; ---- CMPXCHG8B, unequal: the operand into EDX:EAX, ZF clear
        mov dx, s_x2
        call puts
        mov dword [q64], 0x66666666
        mov dword [q64+4], 0x55555555
        mov edx, 0x11111111
        mov eax, 0x22222222
        mov ecx, 0x33333333
        mov ebx, 0x44444444
        mov byte [skip], x2_e - x2_s
        call arm
        push ax
        mov al, 0x7F
        add al, 1
        pop ax
x2_s:   cmpxchg8b [q64]
x2_e:   pushf
        call faulted
        jc .x2f
        call x8_line
        jmp .cr4
.x2f:   popf

.cr4:   ; ---- CR4: 0 at reset; TSD DE PSE MCE load and read back
        mov dx, s_cr4
        call puts
        mov byte [skip], r4_e - r4_s
        call arm
r4_s:   mov eax, cr4
r4_e:   call faulted
        jc .vme
        call hex32
        call space
        mov eax, 0x5C
        mov cr4, eax
        mov eax, cr4
        call hex32
        call crlf

.vme:   ; ---- CR4.VME: not built, so a reserved bit (#GP)
        mov dx, s_vme
        call puts
        mov byte [skip], v_e - v_s
        call arm
        mov eax, 0x5D
v_s:    mov cr4, eax
v_e:    call faulted
        jc .dr4
        mov dx, s_ok
        call puts

.dr4:   ; ---- DR4 with CR4.DE set (as it is now, on a Pentium)
        mov dx, s_dr4
        call puts
        mov byte [skip], d_e - d_s
        call arm
d_s:    mov eax, dr4
d_e:    call faulted
        jc .done
        mov dx, s_ok
        call puts

.done:  ; CR4 back to 0 (a #UD on the 486, skipped)
        mov byte [skip], z_e - z_s
        xor eax, eax
z_s:    mov cr4, eax
z_e:    xor ax, ax
        mov es, ax
        cli
        mov eax, [old6]
        mov [es:6*4], eax
        mov eax, [old13]
        mov [es:13*4], eax
        sti
        mov ax, 0x4C00
        int 0x21

; EDX EAX, the operand, and the flags left on the stack by the caller's PUSHF
x8_line:
        push eax
        mov eax, edx
        call hex32
        call space
        pop eax
        call hex32
        call space
        mov eax, [q64+4]
        call hex32
        call space
        mov eax, [q64]
        call hex32
        call space
        pop bx                  ; (our return address)
        pop ax                  ; the flags
        push bx
        call flags
        jmp crlf

; ZF clear if the EFLAGS bits in EDX can be flipped; EFLAGS as it was
toggles:
        pushfd
        pop eax
        mov ecx, eax
        xor eax, edx
        push eax
        popfd
        pushfd
        pop eax
        push ecx
        popfd
        xor eax, ecx
        test eax, edx
        ret

arm:    mov byte [fault], 0
        ret

; CF set (and the fault's line printed) if the instruction faulted.
; Flags otherwise untouched apart from CF.
faulted:
        cmp byte [fault], 0
        je .no
        push dx
        mov dx, s_ud
        cmp byte [fault], 6
        je .p
        mov dx, s_gpf
.p:     call puts
        pop dx
        stc
        ret
.no:    clc
        ret

; #UD and #GP: skip the instruction ([skip] bytes), note which
ud_handler:
        mov byte [cs:fault], 6
        jmp skipit
gp_handler:
        mov byte [cs:fault], 13
skipit: push bp
        mov bp, sp
        push ax
        xor ax, ax
        mov al, [cs:skip]
        add [bp+2], ax
        pop ax
        pop bp
        iret

; AX's low bits: the defined arithmetic flags
flags:
        and ax, 0x8D5
        push ax
        mov al, ah
        call hex4
        pop ax
        push ax
        shr al, 4
        call hex4
        pop ax
        jmp hex4

hex32:  push eax
        shr eax, 16
        call hex16
        pop eax
hex16:  push ax
        mov al, ah
        call hex8
        pop ax
hex8:   push ax
        shr al, 4
        call hex4
        pop ax
hex4:   push ax
        push dx
        and al, 15
        add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     mov dl, al
        mov ah, 2
        int 0x21
        pop dx
        pop ax
        ret

space:  push dx
        mov dx, s_sp
        call puts
        pop dx
        ret
crlf:   push dx
        mov dx, s_crlf
        call puts
        pop dx
        ret
puts:   push ax
        mov ah, 9
        int 0x21
        pop ax
        ret

s_id0     db "EFLAGS.ID: fixed (no CPUID)", 13, 10, "$"
s_id1     db "EFLAGS.ID: toggles (CPUID)", 13, 10, "$"
s_cpuid0  db "cpuid 0: $"
s_cpuid1  db "cpuid 1: $"
s_rdtsc   db "rdtsc: $"
s_later   db "counts up", 13, 10, "$"
s_notlater db "does not count", 13, 10, "$"
s_wrmsr   db "wrmsr tsc 1:00000000, rdmsr: $"
s_small   db "+ a few", 13, 10, "$"
s_big     db "+ too many", 13, 10, "$"
s_rdmsr   db "rdmsr 1Bh: $"
s_x1      db "cmpxchg8b equal -> edx eax m64 flags: $"
s_x2      db "cmpxchg8b unequal -> edx eax m64 flags: $"
s_cr4     db "cr4: $"
s_vme     db "cr4 <- VME: $"
s_dr4     db "dr4 under CR4.DE: $"
s_ok      db "ok", 13, 10, "$"
s_ud      db "#UD", 13, 10, "$"
s_gpf     db "#GP", 13, 10, "$"
s_sp      db " $"
s_crlf    db 13, 10, "$"
vendor    db "............$"
skip      db 0
fault     db 0
old6      dd 0
old13     dd 0
          align 8
q64       dq 0
