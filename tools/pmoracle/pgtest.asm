; pgtest.asm — paging oracle: a boot image that runs scripted paging
; scenarios and writes one line per observation to port E9 (QEMU's
; -debugcon, dos-monster's debug console for booted images). Run it under
; both and diff the transcripts: pgrun.py.
;
; Physical layout: stage 2 at 7E00, IDT 6000, TSS 6800, ring-0 stack 7000
; (6E00 for the TSS's way in),
; ring-3 stack 5000; page directory 10000, page tables 11000 (0-4 MB,
; identity, user read/write) and 12000 (4-8 MB, supervisor PDE). Test
; pages: 30000 user r/w, 31000 supervisor only, 32000 user read-only,
; 33000 not present; linear 400000 maps physical 34000; nothing at 8 MB.
        cpu 386
        org 0x7C00

STAGE2_LMA  equ 0x7E00
STAGE2_SECS equ 8
IDT_BASE    equ 0x6000
TSS_BASE    equ 0x6800
STACK0      equ 0x7000
STACK3      equ 0x5000
STACK0T     equ 0x6E00             ; the TSS's ring-0 stack, apart from the driver's
PD          equ 0x10000
PT0         equ 0x11000
PT1         equ 0x12000
SEL_CODE    equ 0x08
SEL_DATA    equ 0x10
SEL_CODE3   equ 0x18 | 3
SEL_DATA3   equ 0x20 | 3
SEL_TSS     equ 0x28
RET_VEC     equ 0x30

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
        ; IDT: vectors 0..31 go to fault_N stubs, 30h is the ring-3 return trap
        mov edi, IDT_BASE
        xor ecx, ecx
.idt:   mov eax, fault_stubs
        mov edx, ecx
        shl edx, 3                  ; 8 bytes a stub
        add eax, edx
        mov [edi], ax
        mov word [edi+2], SEL_CODE
        mov word [edi+4], 0x8E00    ; interrupt gate, DPL 0
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
        mov word [edi+4], 0xEE00    ; interrupt gate, DPL 3
        shr eax, 16
        mov [edi+6], ax
        lidt [idtr]
        ; TSS: SS0:ESP0 for the way in from ring 3
        mov dword [TSS_BASE + 4], STACK0T
        mov dword [TSS_BASE + 8], SEL_DATA
        mov word [TSS_BASE + 0x66], 0x68
        mov ax, SEL_TSS
        ltr ax
        ; test data (physical, before paging)
        mov dword [0x30000], 0x11223344
        mov dword [0x31000], 0x55667788
        mov dword [0x32000], 0x99AABBCC
        mov dword [0x32FFC], 0x01020304
        mov dword [0x34010], 0xDEADBEEF
        ; page directory and tables
        mov edi, PD
        mov ecx, 1024
        xor eax, eax
        rep stosd
        mov dword [PD + 0], PT0 | 7         ; 0-4 MB: user, r/w, present
        mov dword [PD + 4], PT1 | 3         ; 4-8 MB: supervisor, r/w, present
        mov edi, PT0
        mov eax, 7                          ; identity, user r/w
        mov ecx, 1024
.pt0:   stosd
        add eax, 0x1000
        loop .pt0
        mov dword [PT0 + 0x31 * 4], 0x31000 | 3    ; supervisor only
        mov dword [PT0 + 0x32 * 4], 0x32000 | 5    ; user, read-only
        mov dword [PT0 + 0x33 * 4], 0x33000 | 6    ; not present
        mov edi, PT1
        mov ecx, 1024
        xor eax, eax
        rep stosd
        mov dword [PT1 + 0], 0x34000 | 7           ; linear 400000 -> 34000, user bits
        mov eax, PD
        mov cr3, eax
        mov eax, cr0
        or eax, 0x80000000
        mov cr0, eax
        jmp .paged                          ; (we are identity mapped)
.paged:
        mov esi, msg_start
        call puts

        ; ---- ring 0
        mov byte [tno], 1
        mov dword [resume], .t2
        mov eax, [0x30000]
        call ok_eax
.t2:    mov byte [tno], 2
        mov eax, [PT0 + 0x30 * 4]           ; PTE of 30000: accessed, not dirty
        and eax, 0xFFF
        call ok_eax
        mov byte [tno], 3
        mov dword [resume], .t4
        mov dword [0x30004], 0x55
        mov eax, [PT0 + 0x30 * 4]           ; now dirty too
        and eax, 0xFFF
        call ok_eax
.t4:    mov byte [tno], 4
        mov dword [resume], .t5
        mov eax, [0x33000]                  ; not present, read
        call ok_eax
.t5:    mov byte [tno], 5
        mov dword [resume], .t6
        mov dword [0x33004], eax            ; not present, write
        call ok_eax
.t6:    mov byte [tno], 6
        mov dword [resume], .t7
        mov dword [0x32008], 0x77           ; supervisor write to a read-only page: fine on a 386
        mov eax, [0x32008]
        call ok_eax
.t7:    mov byte [tno], 7
        mov dword [resume], .t8
        mov eax, [0x400010]                 ; remapped to 34010
        call ok_eax
.t8:    mov byte [tno], 8
        mov dword [resume], .t9
        mov eax, [0x800000]                 ; no page table there
        call ok_eax
.t9:    mov byte [tno], 9
        mov dword [resume], .t10
        mov eax, 0x33000                    ; fetch from a page that is not there
        jmp eax
.t10:   mov byte [tno], 10
        mov dword [resume], .t11
        mov eax, [0x32FFE]                  ; straddles into 33000
        call ok_eax
.t11:   mov byte [tno], 11
        mov eax, [PD]                       ; the PDE's accessed bit
        and eax, 0xFFF
        call ok_eax
        mov byte [tno], 12
        mov dword [resume], .t13
        mov dword [PT0 + 0x33 * 4], 0x33000 | 7    ; make 33000 present
        mov eax, cr3
        mov cr3, eax                        ; flush
        mov eax, [0x33000]
        call ok_eax
.t13:   mov dword [PT0 + 0x33 * 4], 0x33000 | 6    ; and gone again
        mov eax, cr3
        mov cr3, eax

        ; ---- ring 3: each case is a code fragment ending in INT 30h
        mov byte [tno], 20
        mov ebx, r3_read_user
        call ring3
        mov byte [tno], 21
        mov ebx, r3_read_super
        call ring3
        mov byte [tno], 22
        mov ebx, r3_write_ro
        call ring3
        mov byte [tno], 23
        mov ebx, r3_write_user
        call ring3
        mov byte [tno], 24
        mov ebx, r3_read_absent
        call ring3
        mov byte [tno], 25
        mov ebx, r3_read_superpde
        call ring3
        mov byte [tno], 26
        mov ebx, r3_read_ro
        call ring3
        mov byte [tno], 27
        mov eax, [PT0 + 0x30 * 4]
        and eax, 0xFFF
        call ok_eax

        mov esi, msg_done
        call puts
        mov al, 0
        out 0xF4, al                        ; QEMU isa-debug-exit / dos-monster: end
.h:     hlt
        jmp .h

; ring3: run the fragment at EBX in ring 3; it comes back through INT 30h
; (ret_trap) or a fault, either way to [resume] with the ring-0 stack.
ring3:
        mov [saved_esp], esp
        mov dword [resume], .back
        mov ax, SEL_DATA3                   ; ring-0 DS/ES would be nulled by the IRET
        mov ds, ax
        mov es, ax
        push dword SEL_DATA3
        push dword STACK3
        push dword 0x202
        push dword SEL_CODE3
        push ebx
        iretd
.back:  ret

r3_read_user:   mov eax, [0x30000]
                int RET_VEC
r3_read_super:  mov eax, [0x31000]
                int RET_VEC
r3_write_ro:    mov dword [0x32010], 1
                int RET_VEC
r3_write_user:  mov dword [0x30008], 2
                mov eax, [0x30008]
                int RET_VEC
r3_read_absent: mov eax, [0x33000]
                int RET_VEC
r3_read_superpde: mov eax, [0x400010]
                int RET_VEC
r3_read_ro:     mov eax, [0x32000]
                int RET_VEC

; the ring-3 return trap: report EAX, back to ring 0
ret_trap:
        mov bx, SEL_DATA
        mov ds, bx
        mov es, bx
        mov esp, [saved_esp]
        call ok_eax
        jmp [resume]

; fault stubs: push the vector, go to fault
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
        pop ecx                             ; vector
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

ok_eax:                                     ; "Tnn ok eax=XXXXXXXX"
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

msg_start db "pgtest", 10, 0
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
