; pmtest.asm — protected-mode oracle kernel.
;
; Stage 1 is a boot sector that loads stage 2; stage 2 enters 32-bit
; protected mode with a GDT holding one descriptor of each interesting
; shape, then runs a table of cases. Each case supplies the BYTES of the
; instruction under test, which are copied into a fixed slot and executed
; there — so the harness always finds the instruction at one address, and
; the case decides what it is.
;
; Nothing here asserts anything. It is a state generator: what the machine
; does is whatever the log says it did. expected.py holds what we believe
; the architecture requires, which is a separate question.
        cpu 386
        org 0x7C00

STAGE2_LMA  equ 0x7E00
STAGE2_SECS equ 4
IDT_BASE    equ 0x6000
STACK_TOP   equ 0x7000
SEL_CODE    equ 0x08
SEL_DATA    equ 0x10
CASE_BYTES  equ 16                 ; 8 instruction bytes, then AX, then spare

; ---------------------------------------------------------------- stage 1
        bits 16
stage1:
        cli
        cld
        xor ax, ax
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 0x7C00             ; DL still holds the boot drive
        mov ah, 0x02
        mov al, STAGE2_SECS
        mov ch, 0
        mov cl, 2
        mov dh, 0
        mov bx, STAGE2_LMA
        int 0x13
        jc  halt1
        jmp stage2
halt1:  hlt
        jmp halt1

        times 496 - ($ - $$) db 0
        dw 0x5350                  ; 'PS' magic, then the addresses the harness needs
        dw slot
        dw slot_after
        dw fault
        dw NCASES
        dw cases                   ; so the harness can read the cases itself
        dw CASE_BYTES
        times 510 - ($ - $$) db 0
        dw 0xAA55

; ---------------------------------------------------------------- stage 2
stage2:
        bits 16
        mov di, IDT_BASE           ; every vector 0..31 → fault, so a case that
        mov cx, 32                 ; faults simply resumes the sweep
        mov eax, fault
.idt:   mov [di], ax
        mov word [di + 2], SEL_CODE
        mov word [di + 4], 0x8E00  ; present, DPL 0, 32-bit interrupt gate
        push eax
        shr eax, 16
        mov [di + 6], ax
        pop eax
        add di, 8
        loop .idt

        lgdt [gdtr]
        lidt [idtr]
        mov eax, cr0
        or  al, 1
        mov cr0, eax
        jmp dword SEL_CODE:pm_entry

        bits 32
pm_entry:
        mov ax, SEL_DATA
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov fs, ax                 ; fs stays known-good: the tables live behind it
        mov gs, ax
        mov esp, STACK_TOP
        xor ebx, ebx               ; case index

next_case:
        cmp ebx, NCASES
        jae done
        ; copy this case's instruction bytes into the slot
        mov esi, cases
        imul edi, ebx, CASE_BYTES
        add esi, edi
        mov eax, [fs:esi]
        mov [slot], eax
        mov eax, [fs:esi + 4]
        mov [slot + 4], eax
        movzx eax, word [fs:esi + 8]
        inc ebx
        mov ecx, 0xC0DEC0DE        ; a recognisable pattern in the registers the
        mov edx, 0xDA7ADA7A        ; instruction should not be touching
        jmp slot

        align 16
slot:   times 8 db 0x90            ; <<< patched per case; the harness watches here
slot_after:
        mov ax, SEL_DATA           ; put the segment registers back before we
        mov ds, ax                 ; touch memory or take another fault
        mov es, ax
        mov ss, ax
        mov esp, STACK_TOP
        jmp next_case

done:   mov al, 0
        out 0xF4, al               ; isa-debug-exit, so QEMU stops
.spin:  hlt
        jmp .spin

; A gate was taken. DS/SS may be unusable, so rebuild everything.
fault:
        mov ax, SEL_DATA
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov esp, STACK_TOP
        jmp next_case

        align 8
gdt:
        dq 0                                            ; 00 null
        dw 0xFFFF, 0
        db 0, 0x9A, 0xCF, 0                             ; 08 code32 r/x DPL0
        dw 0xFFFF, 0
        db 0, 0x92, 0xCF, 0                             ; 10 data32 r/w DPL0
        dw 0xFFFF, 0
        db 0, 0xF2, 0xCF, 0                             ; 18 data32 r/w DPL3
        dw 0xFFFF, 0
        db 0, 0x12, 0xCF, 0                             ; 20 data32 not present
        dw 0xFFFF, 0
        db 0, 0x98, 0xCF, 0                             ; 28 code32 execute-only
        dw 0xFFFF, 0
        db 0, 0x96, 0xCF, 0                             ; 30 data32 expand-down
        dw 0x0FFF, 0
        db 0, 0x92, 0x40, 0                             ; 38 data32 limit 0FFFh, byte granular
        dw 0xFFFF, 0
        db 0, 0x90, 0xCF, 0                             ; 40 data32 READ-ONLY
gdt_end:

gdtr:   dw gdt_end - gdt - 1
        dd gdt
idtr:   dw 32 * 8 - 1
        dd IDT_BASE

; Each case: 8 bytes of instruction (NOP-padded), then the AX it runs with.
%macro CASE 2                      ; %1 = instruction bytes as a db list, %2 = ax
        %%s: db %1
        times 8 - ($ - %%s) db 0x90
        dw %2
        dw 0, 0, 0
%endmacro

%define MOV_DS 0x8E, 0xD8           ; mov ds, ax
%define MOV_SS 0x8E, 0xD0           ; mov ss, ax
%define MOV_ES 0x8E, 0xC0           ; mov es, ax

        align 16
cases:
        ; ---- DS: which descriptors are a legal data segment
        CASE {MOV_DS}, 0x0000      ; null: allowed, segment becomes unusable
        CASE {MOV_DS}, 0x0010      ; plain read/write data
        CASE {MOV_DS}, 0x0008      ; readable code is a legal DS
        CASE {MOV_DS}, 0x0018      ; data DPL3, RPL 0, CPL 0
        CASE {MOV_DS}, 0x001B      ; data DPL3, RPL 3
        CASE {MOV_DS}, 0x0020      ; not present
        CASE {MOV_DS}, 0x0028      ; execute-only code
        CASE {MOV_DS}, 0x0030      ; expand-down
        CASE {MOV_DS}, 0x0038      ; byte-granular limit 0FFFh
        CASE {MOV_DS}, 0x0003      ; null, RPL 3
        CASE {MOV_DS}, 0x0080      ; GDT index past the limit
        CASE {MOV_DS}, 0x0084      ; TI=1, LDTR never loaded
        CASE {MOV_DS}, 0x0088      ; GDT index past the limit
        CASE {MOV_DS}, 0x000C      ; TI=1, LDTR never loaded
        CASE {MOV_DS}, 0x0040      ; read-only data is a legal DS
        ; ---- SS: stricter. must be writable data, and DPL = RPL = CPL
        CASE {MOV_SS}, 0x0010      ; the one that should work
        CASE {MOV_SS}, 0x0000      ; null into SS is #GP(0), unlike DS
        CASE {MOV_SS}, 0x0020      ; not present -> #SS, not #GP
        CASE {MOV_SS}, 0x0028      ; execute-only code
        CASE {MOV_SS}, 0x0040      ; read-only data: legal DS, illegal SS
        CASE {MOV_SS}, 0x0018      ; DPL 3 with CPL 0: legal DS, illegal SS
        CASE {MOV_SS}, 0x001B      ; DPL 3, RPL 3, CPL 0
        ; ---- ES: same rules as DS, one spot check
        CASE {MOV_ES}, 0x0020      ; not present
        CASE {MOV_ES}, 0x0010      ; plain data
cases_end:
NCASES  equ (cases_end - cases) / CASE_BYTES

        times (512 * (1 + STAGE2_SECS)) - ($ - $$) db 0
