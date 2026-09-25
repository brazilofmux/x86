; c586test.asm — the Pentium's additions to protected mode and paging, as a
; transcript image (c486test.asm's frame and layout; run by pgrun.py --m586
; against QEMU -cpu pentium, or Bochs's pentium): CPUID's feature bits and
; EFLAGS.ID; CR4; 4 MB pages (PSE) — a read through one, its accessed and
; dirty bits in the PDE, PS ignored with PSE clear, U/S from the PDE; RDTSC
; under CR4.TSD and RDMSR at ring 3; the TSC through WRMSR/RDMSR; an MSR
; that does not exist; CMPXCHG8B both ways, on a read-only page, across a
; page boundary into a read-only one (nothing may land), and in its
; register form (#UD).
;
; Physical layout as c486test, plus: 30FFC/31000 straddle the user r/w page
; 30000 and a read-only 31000; linear 800000 is a 4 MB user page and
; C00000 a supervisor one, both on physical 400000 (markers at 400010 and
; 5FF000; 400000 itself is 0, read as a page table when PSE is off).
        cpu 586
        org 0x7C00

STAGE2_LMA  equ 0x7E00
STAGE2_SECS equ 8
IDT_BASE    equ 0x6000
TSS_BASE    equ 0x6800
STACK0      equ 0x7000
STACK3      equ 0x5000
STACK0T     equ 0x6E00
PD          equ 0x10000
PT0         equ 0x11000
PT1         equ 0x12000
SEL_CODE    equ 0x08
SEL_DATA    equ 0x10
SEL_CODE3   equ 0x18 | 3
SEL_DATA3   equ 0x20 | 3
SEL_TSS     equ 0x28
RET_VEC     equ 0x30
CR4_TSD     equ 0x04
CR4_PSE     equ 0x10

; ---------------------------------------------------------------- stage 1
        bits 16
stage1:
        cli
        cld
        xor ax, ax
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 0x7C00
        mov ah, 0x02
        mov al, STAGE2_SECS
        mov ch, 0
        mov cl, 2
        mov dh, 0
        mov bx, STAGE2_LMA
        int 0x13
        jc  .h
        jmp stage2
.h:     hlt
        jmp .h
        times 510 - ($ - $$) db 0
        dw 0xAA55

; ---------------------------------------------------------------- stage 2
stage2:
        mov eax, cr0                ; ET as the machine came up
        mov [cr0_boot], eax
        mov al, 0xFF                ; every IRQ masked at the PIC: ring 3 runs with IF
        out 0x21, al                ; set, and a timer tick there reads as vector 8 (a
        out 0xA1, al                ; slow -V run on the host clock took one in T23)
        in al, 0x92                 ; A20 on
        or al, 2
        out 0x92, al
        lgdt [gdtr]
        mov eax, cr0
        or al, 1
        mov cr0, eax
        jmp SEL_CODE:pm32

        bits 32
pm32:
        mov ax, SEL_DATA
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax
        mov ss, ax
        mov esp, STACK0
        mov edi, IDT_BASE
        xor ecx, ecx
.idt:   mov eax, fault_stubs
        mov edx, ecx
        shl edx, 3
        add eax, edx
        mov [edi], ax
        mov word [edi+2], SEL_CODE
        mov word [edi+4], 0x8E00
        shr eax, 16
        mov [edi+6], ax
        add edi, 8
        inc ecx
        cmp ecx, 32
        jb .idt
        mov edi, IDT_BASE + RET_VEC * 8
        mov eax, ret_trap
        mov [edi], ax
        mov word [edi+2], SEL_CODE
        mov word [edi+4], 0xEE00
        shr eax, 16
        mov [edi+6], ax
        lidt [idtr]
        mov dword [TSS_BASE + 4], STACK0T
        mov dword [TSS_BASE + 8], SEL_DATA
        mov word [TSS_BASE + 0x66], 0x68
        mov ax, SEL_TSS
        ltr ax
        mov dword [0x30000], 0x11223344
        mov dword [0x30004], 0x55667788
        mov dword [0x30020], 5
        mov dword [0x32000], 0x99AABBCC
        mov dword [0x34010], 0xDEADBEEF
        mov dword [0x36010], 0xFEEDFACE
        mov dword [0x30FFC], 0xAAAAAAAA     ; a CMPXCHG8B across into a read-only page
        mov dword [0x31000], 0xBBBBBBBB
        mov dword [0x400000], 0             ; (as a page table, with PSE off: nothing present)
        mov dword [0x400010], 0xCAFEBABE    ; in the 4 MB page at 400000
        mov dword [0x5FF000], 0x13579BDF
        mov edi, PD
        mov ecx, 1024
        xor eax, eax
        rep stosd
        mov dword [PD + 0], PT0 | 7
        mov dword [PD + 4], PT1 | 7
        mov edi, PT0
        mov eax, 7
        mov ecx, 1024
.pt0:   stosd
        add eax, 0x1000
        loop .pt0
        mov dword [PT0 + 0x32 * 4], 0x32000 | 5    ; user, read-only
        mov dword [PT0 + 0x31 * 4], 0x31000 | 5    ; user, read-only
        mov dword [PD + 8], 0x400000 | 0x87        ; linear 800000: a 4 MB page, user r/w (PS)
        mov dword [PD + 12], 0x400000 | 0x83       ; linear C00000: the same, supervisor
        mov edi, PT1
        mov ecx, 1024
        xor eax, eax
        rep stosd
        mov dword [PT1 + 0], 0x34000 | 7           ; linear 400000 -> 34000
        mov eax, PD
        mov cr3, eax
        mov eax, cr0
        or eax, 0x80000000
        mov cr0, eax
        jmp .paged
.paged:
        mov esi, msg_start
        call puts


        ; ---- CPUID and EFLAGS.ID
        mov byte [tno], 1
        mov eax, 1
        cpuid
        mov eax, edx
        and eax, 0x138                      ; PSE TSC MSR CX8
        call ok_eax
        mov byte [tno], 2
        pushfd
        pop eax
        mov ecx, eax
        xor eax, 0x200000
        push eax
        popfd
        pushfd
        pop eax
        push ecx
        popfd
        xor eax, ecx
        and eax, 0x200000                   ; toggled
        call ok_eax

        ; ---- CR4 and 4 MB pages
        mov byte [tno], 3
        mov eax, cr4
        call ok_eax
        mov byte [tno], 4
        mov eax, CR4_PSE
        mov cr4, eax
        mov eax, cr4
        call ok_eax
        mov byte [tno], 5
        mov dword [resume], .t6
        mov eax, [0x800010]                 ; 400010 through the 4 MB page
        call ok_eax
.t6:    mov byte [tno], 6
        mov dword [resume], .t7
        mov eax, [0x9FF000]                 ; 5FF000, the same page
        call ok_eax
.t7:    mov byte [tno], 7
        mov eax, [PD + 8]                   ; the PDE: accessed
        call ok_eax
        mov byte [tno], 8
        mov dword [0x800020], 1
        mov eax, [PD + 8]                   ; and dirty
        call ok_eax
        mov byte [tno], 9
        mov dword [resume], .t0a
        xor eax, eax
        mov cr4, eax                        ; PSE off: PS ignored, 400000 is a page table
        mov eax, [0x800010]                 ; #PF, not present
        call ok_eax
.t0a:   mov eax, CR4_PSE
        mov cr4, eax
        mov byte [tno], 0x0A
        mov ebx, r3_big_super               ; U/S clear in the PDE: #PF at ring 3
        call ring3
        mov byte [tno], 0x0B
        mov ebx, r3_big_user
        call ring3

        ; ---- the TSC and the MSRs
        mov byte [tno], 0x10
        mov eax, CR4_PSE | CR4_TSD
        mov cr4, eax
        mov ebx, r3_rdtsc                   ; TSD: #GP(0) at ring 3
        call ring3
        mov eax, CR4_PSE
        mov cr4, eax
        mov byte [tno], 0x11
        mov ebx, r3_rdtsc
        call ring3
        mov byte [tno], 0x12
        mov ebx, r3_rdmsr                   ; #GP(0) at ring 3
        call ring3
        mov byte [tno], 0x13
        mov dword [resume], .t14
        mov ecx, 0x10
        mov edx, 1
        xor eax, eax
        wrmsr
        rdmsr
        mov eax, edx
        call ok_eax
.t14:   mov byte [tno], 0x14
        mov dword [resume], .t15
        rdtsc
        mov esi, eax
        mov edi, edx
        nop
        rdtsc
        sub eax, esi
        sbb edx, edi
        jc .t14z                            ; earlier
        or eax, edx
        jz .t14z                            ; the same
        mov eax, 1
        jmp .t14n
.t14z:  xor eax, eax
.t14n:  call ok_eax
.t15:   mov byte [tno], 0x15
        mov dword [resume], .t20
        mov ecx, 0x9999                     ; no such MSR
        rdmsr
        call ok_eax

        ; ---- CMPXCHG8B
.t20:   mov byte [tno], 0x20
        mov dword [resume], .t24
        mov dword [0x30040], 0x22222222
        mov dword [0x30044], 0x11111111
        mov bl, 0x7F
        add bl, 1                           ; OF SF AF set going in
        mov edx, 0x11111111
        mov eax, 0x22222222
        mov ecx, 0x33333333
        mov ebx, 0x44444444
        cmpxchg8b [0x30040]                 ; equal: ECX:EBX in, ZF
        pushfd
        pop dword [flg]
        call ok_eax
        mov byte [tno], 0x21
        mov eax, [0x30040]
        call ok_eax
        mov byte [tno], 0x22
        mov eax, [0x30044]
        call ok_eax
        mov byte [tno], 0x23
        mov eax, [flg]
        and eax, 0x8D5
        call ok_eax
.t24:   mov byte [tno], 0x24
        mov dword [resume], .t27
        mov dword [0x30040], 0x66666666
        mov dword [0x30044], 0x55555555
        mov bl, 0x7F
        add bl, 1
        mov edx, 0x11111111
        mov eax, 0x22222222
        mov ecx, 0x33333333
        mov ebx, 0x44444444
        cmpxchg8b [0x30040]                 ; unequal: the operand to EDX:EAX
        pushfd
        pop dword [flg]
        call ok_eax
        mov byte [tno], 0x25
        mov eax, edx
        call ok_eax
        mov byte [tno], 0x26
        mov eax, [flg]
        and eax, 0x8D5
        call ok_eax
.t27:   mov byte [tno], 0x27
        mov ebx, r3_cx8_ro                  ; unequal still writes: #PF on a read-only page
        call ring3
        mov byte [tno], 0x28
        mov eax, [0x32000]
        call ok_eax
        mov byte [tno], 0x29
        mov ebx, r3_cx8_cross               ; equal, the second half read-only: #PF
        call ring3
        mov byte [tno], 0x2A
        mov eax, [0x30FFC]                  ; and the first half did not land
        call ok_eax
        mov byte [tno], 0x2B
        mov eax, [0x31000]
        call ok_eax
        mov byte [tno], 0x2C
        mov dword [resume], .t50
        db 0x0F, 0xC7, 0xC8                 ; cmpxchg8b eax: #UD
        call ok_eax

.t50:   xor eax, eax
        mov cr4, eax
        mov esi, msg_done
        call puts
        mov al, 0
        out 0xF4, al
.h:     hlt
        jmp .h
ring3:
        mov [saved_esp], esp
        mov dword [resume], .back
        mov ax, SEL_DATA3
        mov ds, ax
        mov es, ax
        push dword SEL_DATA3
        push dword STACK3
        push dword [r3flags]
        push dword SEL_CODE3
        push ebx
        iretd
.back:  ret

r3_big_super:   mov eax, [0xC00010]
                int RET_VEC
r3_big_user:    mov eax, [0x800010]
                int RET_VEC
r3_rdtsc:       rdtsc
                mov eax, 1
                int RET_VEC
r3_rdmsr:       mov ecx, 0x10
                rdmsr
                int RET_VEC
r3_cx8_ro:      mov edx, 1
                mov eax, 2
                mov ecx, 3
                mov ebx, 4
                cmpxchg8b [0x32000]
                int RET_VEC
r3_cx8_cross:   mov edx, 0xBBBBBBBB
                mov eax, 0xAAAAAAAA
                mov ecx, 0x33333333
                mov ebx, 0x44444444
                cmpxchg8b [0x30FFC]
                int RET_VEC
ret_trap:
        mov bx, SEL_DATA
        mov ds, bx
        mov es, bx
        mov esp, [saved_esp]
        call ok_eax
        jmp [resume]

fault_stubs:
%assign v 0
%rep 32
        push dword v
        jmp near fault
        nop
%assign v v+1
%endrep

; fault: report vector, error code (if any) and CR2, resume the script
fault:
        mov bx, SEL_DATA
        mov ds, bx
        mov es, bx
        pop ecx
        mov esi, msg_t
        call puts
        movzx eax, byte [tno]
        call hex8
        mov esi, msg_v
        call puts
        mov eax, ecx
        call hex8
        cmp ecx, 8
        je .err
        cmp ecx, 17
        je .err
        cmp ecx, 10
        jb .noerr
        cmp ecx, 14
        ja .noerr
.err:   mov esi, msg_e
        call puts
        pop eax
        call hex16
.noerr: mov esi, msg_cr2
        call puts
        mov eax, cr2
        call hex32
        mov al, 10
        out 0xE9, al
        mov esp, [saved_esp]
        cmp dword [saved_esp], 0
        jne .go
        mov esp, STACK0
.go:    jmp [resume]

ok_eax:
        push eax
        mov esi, msg_t
        call puts
        movzx eax, byte [tno]
        call hex8
        mov esi, msg_ok
        call puts
        pop eax
        call hex32
        mov al, 10
        out 0xE9, al
        ret

puts:   lodsb
        test al, al
        jz .d
        out 0xE9, al
        jmp puts
.d:     ret
hex32:  push eax
        shr eax, 16
        call hex16
        pop eax
hex16:  push eax
        shr eax, 8
        call hex8
        pop eax
hex8:   push eax
        shr al, 4
        call .n
        pop eax
.n:     and al, 15
        add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     out 0xE9, al
        ret

msg_start db "c586test", 10, 0
msg_done  db "done", 10, 0
msg_t     db "T", 0
msg_ok    db " ok eax=", 0
msg_v     db " v=", 0
msg_e     db " e=", 0
msg_cr2   db " cr2=", 0
tno       db 0
          align 4
resume    dd 0
saved_esp dd 0
cr0_boot  dd 0
r3flags   dd 0x202
flg       dd 0

gdt:    dq 0
        dq 0x00CF9A000000FFFF       ; 08 code, ring 0
        dq 0x00CF92000000FFFF       ; 10 data, ring 0
        dq 0x00CFFA000000FFFF       ; 18 code, ring 3
        dq 0x00CFF2000000FFFF       ; 20 data, ring 3
        dw 0x67, TSS_BASE, 0x8900, 0 ; 28 386 TSS
gdt_end:
gdtr:   dw gdt_end - gdt - 1
        dd gdt
idtr:   dw 0x31 * 8 - 1
        dd IDT_BASE

        times (STAGE2_SECS + 1) * 512 - ($ - $$) db 0
