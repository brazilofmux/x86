; c486test.asm — the 486's additions to protected mode and paging, as a
; transcript image (pgtest.asm's frame and layout; run by pgrun.py --m486
; against QEMU -cpu 486): CR0.ET, CR0.WP (and its effect on a TLB entry
; cached while it was clear), INVLPG, INVD/WBINVD/INVLPG privilege, the
; alignment check (CR0.AM, EFLAGS.AC, CPL 3), and XADD/CMPXCHG/BSWAP on
; memory and through the paging unit.
;
; Physical layout as pgtest: stage 2 at 7E00, IDT 6000, TSS 6800, ring-0
; stack 7000 (6E00 for the TSS's way in), ring-3 stack 5000; page
; directory 10000, page tables 11000 (0-4 MB, identity, user r/w) and
; 12000 (4-8 MB). Test pages: 30000 user r/w, 32000 user read-only;
; linear 400000 maps 34000, then 36000 after INVLPG.
        cpu 586                     ; (NASM files XADD and CMPXCHG under the 586; the 486 has them)
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
CR0_WP      equ 0x10000
CR0_AM      equ 0x40000
EFL_AC      equ 0x40000

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

        ; ---- CR0.ET
        mov byte [tno], 1
        mov eax, [cr0_boot]
        and eax, 0x10                       ; ET (the BIOS decides the rest)
        call ok_eax
        mov byte [tno], 2
        mov eax, cr0
        and eax, ~0x10
        mov cr0, eax
        mov eax, cr0
        and eax, 0x10                       ; still 1 on a 486
        call ok_eax

        ; ---- CR0.WP: supervisor writes to a read-only user page
        mov byte [tno], 3
        mov dword [resume], .t4
        mov dword [0x32008], 0x77           ; WP clear: lands (and the TLB has it, dirty)
        mov eax, [0x32008]
        call ok_eax
.t4:    mov byte [tno], 4
        mov dword [resume], .t5
        mov eax, cr0
        or eax, CR0_WP
        mov cr0, eax
        mov dword [0x3200C], 0x88           ; WP set: #PF, P|W
        call ok_eax
.t5:    mov byte [tno], 5
        mov dword [resume], .t6
        mov dword [0x30008], 0x99           ; a writable page is still writable
        mov eax, [0x30008]
        call ok_eax
.t6:    mov byte [tno], 6
        mov dword [resume], .t7
        mov eax, [0x32000]                  ; and a read-only one readable
        call ok_eax
.t7:    mov byte [tno], 7
        mov dword [resume], .t8
        mov eax, cr0
        and eax, ~CR0_WP
        mov cr0, eax
        mov dword [0x3200C], 0xAA           ; WP clear again: lands
        mov eax, [0x3200C]
        call ok_eax

        ; ---- INVLPG
.t8:    mov byte [tno], 8
        mov eax, [0x400010]                 ; 34010, now in the TLB
        call ok_eax
        mov byte [tno], 9
        mov dword [PT1 + 0], 0x36000 | 7
        invlpg [0x400000]
        mov eax, [0x400010]                 ; 36010
        call ok_eax
        mov byte [tno], 10
        mov dword [resume], .t11
        invd
        wbinvd
        mov eax, 1
        call ok_eax

        ; ---- the alignment check, ring 0 first: never
.t11:   mov byte [tno], 11
        mov dword [resume], .t20
        mov eax, cr0
        or eax, CR0_AM
        mov cr0, eax
        pushfd
        or dword [esp], EFL_AC
        popfd
        mov eax, [0x30001]
        pushfd
        and dword [esp], ~EFL_AC
        popfd
        call ok_eax
.t20:
        ; ---- ring 3
        mov byte [tno], 0x20
        mov ebx, r3_invd
        call ring3
        mov byte [tno], 0x21
        mov ebx, r3_wbinvd
        call ring3
        mov byte [tno], 0x22
        mov ebx, r3_invlpg
        call ring3
        mov byte [tno], 0x23
        mov ebx, r3_mis_read                ; AM set, AC clear: no check
        call ring3
        mov dword [r3flags], 0x202 | EFL_AC
        mov byte [tno], 0x24
        mov ebx, r3_mis_read                ; AM and AC: #AC(0)
        call ring3
        mov byte [tno], 0x25
        mov ebx, r3_ali_read                ; aligned: fine
        call ring3
        mov byte [tno], 0x26
        mov ebx, r3_mis_word                ; a word at an odd address
        call ring3
        mov byte [tno], 0x27
        mov ebx, r3_word2                   ; a word at 2 mod 4: fine
        call ring3
        mov byte [tno], 0x28
        mov ebx, r3_mis_push                ; the stack too
        call ring3
        mov byte [tno], 0x29
        mov ebx, r3_mis_byte                ; bytes are always aligned
        call ring3
        mov byte [tno], 0x2A
        mov ebx, r3_mis_write
        call ring3
        mov byte [tno], 0x2B
        mov ebx, r3_lds                     ; a 16:16 far pointer at 2 mod 4: fine
        call ring3
        mov byte [tno], 0x2C
        mov ebx, r3_popf_ac                 ; AC cleared by the program itself
        call ring3
        mov eax, cr0
        and eax, ~CR0_AM
        mov cr0, eax
        mov byte [tno], 0x2D
        mov ebx, r3_mis_read                ; AC set, AM clear: no check
        call ring3
        mov dword [r3flags], 0x202

        ; ---- the new instructions on memory, through paging
        mov byte [tno], 0x30
        mov ebx, r3_xadd
        call ring3
        mov byte [tno], 0x31
        mov eax, [0x30020]
        call ok_eax
        mov byte [tno], 0x32
        mov ebx, r3_cmpxchg_eq
        call ring3
        mov byte [tno], 0x33
        mov eax, [0x30020]
        call ok_eax
        mov byte [tno], 0x34
        mov ebx, r3_cmpxchg_ne_ro           ; unequal still writes: #PF on a read-only page
        call ring3
        mov byte [tno], 0x35
        mov ebx, r3_bswap
        call ring3

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

r3_invd:        invd
                int RET_VEC
r3_wbinvd:      wbinvd
                int RET_VEC
r3_invlpg:      invlpg [0x30000]
                int RET_VEC
r3_mis_read:    mov eax, [0x30001]
                int RET_VEC
r3_ali_read:    mov eax, [0x30004]
                int RET_VEC
r3_mis_word:    xor eax, eax
                mov ax, [0x30003]
                int RET_VEC
r3_word2:       xor eax, eax
                mov ax, [0x30002]
                int RET_VEC
r3_mis_push:    dec esp
                push eax
                int RET_VEC
r3_mis_byte:    xor eax, eax
                mov al, [0x30003]
                int RET_VEC
r3_mis_write:   mov dword [0x30011], 0x12345678
                mov eax, [0x30010]
                int RET_VEC
r3_lds:         mov word [0x30032], 0x1234  ; (word stores: a dword here would itself be #AC)
                mov word [0x30034], SEL_DATA3
                push ds
                lds ax, [0x30032]           ; offset 1234, selector 0023
                pop ds
                movzx eax, ax
                int RET_VEC
r3_popf_ac:     pushfd
                and dword [esp], ~EFL_AC
                popfd
                mov eax, [0x30001]
                int RET_VEC
r3_xadd:        mov eax, 7
                xadd [0x30020], eax         ; [30020] = 5 + 7, EAX = 5
                int RET_VEC
r3_cmpxchg_eq:  mov eax, 12
                mov ecx, 0x42
                cmpxchg [0x30020], ecx      ; equal: store 42
                int RET_VEC
r3_cmpxchg_ne_ro: mov eax, 1
                mov ecx, 2
                cmpxchg [0x32000], ecx      ; unequal, and the page is read-only
                int RET_VEC
r3_bswap:       mov eax, [0x30000]
                bswap eax
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

msg_start db "c486test", 10, 0
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
