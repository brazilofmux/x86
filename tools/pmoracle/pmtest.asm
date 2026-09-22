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
STUB_BASE   equ 0x6400
STUB_STRIDE equ 16
NVEC        equ 0x31           ; 0..30h; 30h is the deliberate return trap
RET_VEC     equ 0x30           ; not INT3: that is the Bochs debugger's own break
STACK_TOP   equ 0x7000
RING3_STACK equ 0x5000
SEL_CODE    equ 0x08
SEL_DATA    equ 0x10
SEL_CODE3   equ 0x70 | 3           ; DPL 3 code, RPL 3
SEL_DATA3   equ 0x18 | 3           ; DPL 3 data, RPL 3
SEL_TSS     equ 0x78
CASE_BYTES  equ 16                 ; 8 instruction bytes, AX, ring, spare

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
        ; One stub per vector: "mov ebp, <vector>; jmp fault". The vector is
        ; then in EBP in the first dump after any fault, which is what lets
        ; an emulator without an exception log still report which one it was.
        mov edi, STUB_BASE
        xor ecx, ecx
.stub:  mov byte [edi], 0xBD                   ; mov ebp, imm32
        mov [edi + 1], ecx
        mov byte [edi + 5], 0xE9               ; jmp rel32
        mov eax, fault
        sub eax, edi
        sub eax, 10
        mov [edi + 6], eax
        add edi, STUB_STRIDE
        inc ecx
        cmp ecx, NVEC
        jb  .stub

        mov di, IDT_BASE                       ; gate i -> stub i
        xor ecx, ecx
.idt:   mov eax, STUB_BASE
        push ecx
        imul ecx, ecx, STUB_STRIDE
        add eax, ecx
        pop ecx
        mov [di], ax
        mov word [di + 2], SEL_CODE
        mov word [di + 4], 0x8E00              ; present, DPL 0, 32-bit interrupt gate
        cmp ecx, RET_VEC                       ; the return trap has to be raisable
        jne .nogate3                           ; from ring 3, so its gate is DPL 3
        mov word [di + 4], 0xEE00              ; present, DPL 3
.nogate3:
        shr eax, 16
        mov [di + 6], ax
        add di, 8
        inc ecx
        cmp ecx, NVEC
        jb  .idt

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
        mov ax, SEL_TSS            ; a stack for transfers that come back inward
        ltr ax
        xor ebx, ebx               ; case index

next_case:
        ; A transfer out to CPL 3 nulls any data segment the new privilege
        ; cannot reach, and FS (DPL 0) is where the case table lives. It has
        ; to be put back before the table can be read again.
        mov ax, SEL_DATA
        mov fs, ax
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
        movzx edi, word [fs:esi + 10]          ; 0 = run at CPL 0, 3 = at CPL 3
        inc ebx
        mov ecx, 0xC0DEC0DE        ; a recognisable pattern in the registers the
        mov edx, 0xDA7ADA7A        ; instruction should not be touching
        test edi, edi
        jz  slot
        push dword SEL_DATA3                   ; drop to ring 3 the only way there is
        push dword RING3_STACK
        push dword 2                           ; EFLAGS, interrupts off
        push dword SEL_CODE3
        push dword slot
        iretd

        align 16
slot:   times 8 db 0x90            ; <<< patched per case; the harness watches here
slot_after:
        int RET_VEC                ; back to ring 0 whatever privilege we ran at

; A far transfer under test lands here. CS may be any code selector and CPL
; may still be 3 — conforming code does not change it — so the only way back
; that always works is the same trap every case uses.
far_target:
        int RET_VEC

; QEMU stops at the port write. Anything that ignores it sweeps again from
; the top, so a harness that sends more continues than there were cases
; cannot hang on a halted machine — it just sees the table twice.
done:   mov al, 0
        out 0xF4, al
        xor ebx, ebx
        jmp next_case

; A gate was taken. DS/SS may be unusable, so rebuild everything.
fault:
        mov ax, SEL_DATA           ; vector is implied by which stub we came through
        mov ds, ax
        mov es, ax
        mov fs, ax
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
        dw 0xFFFF, 0
        db 0, 0x9E, 0xCF, 0                             ; 48 code32 conforming, readable
        dw 0xFFFF, 0
        db 0, 0x1A, 0xCF, 0                             ; 50 code32 NOT PRESENT
gate58: dw far_target, SEL_CODE                         ; 58 call gate -> 08:far_target
        db 0, 0x8C
        dw 0
gate60: dw far_target, SEL_CODE                         ; 60 call gate, NOT PRESENT
        db 0, 0x0C
        dw 0
gate68: dw far_target, SEL_DATA                         ; 68 call gate whose target is data
        db 0, 0x8C
        dw 0
        dw 0xFFFF, 0
        db 0, 0xFA, 0xCF, 0                             ; 70 code32 DPL 3, readable
tssdesc: dw 103, tss                                    ; 78 TSS, 32-bit, available
        db 0, 0x89, 0x00, 0
gate80: dw far_target, SEL_CODE                         ; 80 call gate DPL 3 -> 08:far_target
        db 0, 0xEC
        dw 0
gdt_end:

; Only ESP0/SS0 matter here: they are the stack an inward transfer lands on.
        align 4
tss:    dd 0                       ; back link
        dd STACK_TOP               ; ESP0
        dd SEL_DATA                ; SS0
        times 104 - ($ - tss) db 0

gdtr:   dw gdt_end - gdt - 1
        dd gdt
idtr:   dw NVEC * 8 - 1
        dd IDT_BASE

; Each case: 8 bytes of instruction (NOP-padded), then the AX it runs with.
%macro CASE 2-3 0                  ; %1 = instruction bytes, %2 = ax, %3 = CPL
        %%s: db %1
        times 8 - ($ - %%s) db 0x90
        dw %2
        dw %3
        dw 0, 0
%endmacro

%define MOV_DS 0x8E, 0xD8           ; mov ds, ax
%define MOV_SS 0x8E, 0xD0           ; mov ss, ax
%define MOV_ES 0x8E, 0xC0           ; mov es, ax

; A far JMP/CALL is opcode, offset32, selector16 — seven bytes, so the
; whole transfer target lives in the case rather than in a register.
%macro FARCASE 2-3 0               ; %1 = opcode (0xEA jmp / 0x9A call), %2 = selector, %3 = CPL
        %%s: db %1
        dd far_target
        dw %2
        times 8 - ($ - %%s) db 0x90
        dw 0
        dw %3
        dw 0, 0
%endmacro

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
        CASE {MOV_DS}, 0x00F0      ; GDT index past the limit
        CASE {MOV_DS}, 0x0084      ; TI=1, LDTR never loaded
        CASE {MOV_DS}, 0x00F8      ; GDT index past the limit
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
        ; ---- far JMP: which selectors are a legal transfer target at CPL 0
        FARCASE 0xEA, 0x0008       ; plain code, same privilege
        FARCASE 0xEA, 0x0048       ; conforming code
        FARCASE 0xEA, 0x0050       ; code, not present -> #NP
        FARCASE 0xEA, 0x0010       ; a data segment is not a transfer target
        FARCASE 0xEA, 0x0000       ; null selector
        FARCASE 0xEA, 0x00F0       ; index past the GDT limit
        FARCASE 0xEA, 0x0058       ; through a call gate (JMP may use one)
        FARCASE 0xEA, 0x0060       ; call gate, not present
        FARCASE 0xEA, 0x0068       ; call gate whose target selector is data
        ; ---- far CALL: same table, and it pushes a return address
        FARCASE 0x9A, 0x0008       ; plain code
        FARCASE 0x9A, 0x0058       ; through a call gate
        FARCASE 0x9A, 0x0050       ; not present
        FARCASE 0x9A, 0x0010       ; data segment
        FARCASE 0x9A, 0x0068       ; gate whose target is data
        ; ---- the same instructions again, but running at CPL 3
        CASE {MOV_DS}, 0x0010, 3   ; DPL 0 data is out of reach from ring 3
        CASE {MOV_DS}, 0x0018, 3   ; DPL 3 data, RPL 0
        CASE {MOV_DS}, 0x001B, 3   ; DPL 3 data, RPL 3
        CASE {MOV_DS}, 0x0008, 3   ; DPL 0 code
        FARCASE 0xEA, 0x0008, 3    ; non-conforming DPL 0 code from ring 3
        FARCASE 0xEA, 0x0048, 3    ; conforming DPL 0 code from ring 3
        FARCASE 0x9A, 0x0058, 3    ; call gate DPL 0, called from ring 3
        FARCASE 0x9A, 0x0080, 3    ; call gate DPL 3: the way in
        FARCASE 0xEA, 0x0070, 3    ; DPL 3 code, staying at ring 3
cases_end:
NCASES  equ (cases_end - cases) / CASE_BYTES

        times (512 * (1 + STAGE2_SECS)) - ($ - $$) db 0
