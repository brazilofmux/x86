; pmtest.asm — protected-mode oracle kernel.
;
; Boots, builds an IDT whose every gate abandons the frame and resumes,
; enters 32-bit protected mode with a GDT holding one descriptor of each
; interesting shape, then executes the instruction under test once per
; table entry at a FIXED address. The harness finds those executions in
; QEMU's -d cpu log by that address: the dump at it is the state going
; in, the next dump is the state coming out (or the fault handler).
;
; Nothing here asserts anything. It is a state generator; what the
; hardware does is whatever the log says it did.
        cpu 386
        bits 16
        org 0x7C00

IDT_BASE  equ 0x6000
STACK_TOP equ 0x7000
SEL_CODE  equ 0x08
SEL_DATA  equ 0x10

start:
        cli
        cld
        xor ax, ax
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 0x7C00

        ; every vector 0..31 → fault, so a faulting test resumes the sweep
        mov di, IDT_BASE
        mov cx, 32
        mov eax, fault
.idt:   mov [di], ax
        mov word [di + 2], SEL_CODE
        mov word [di + 4], 0x8E00          ; present, DPL 0, 32-bit interrupt gate
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
        mov fs, ax                         ; fs stays known-good: the table lives behind it
        mov gs, ax
        mov esp, STACK_TOP
        xor ebx, ebx                       ; case index

next_case:
        cmp ebx, (seltab_end - seltab) / 2
        jae done
        movzx eax, word [fs:seltab + ebx * 2]
        inc ebx
        ; a recognisable pattern in the other registers, so the log shows
        ; plainly what the instruction did and did not touch
        mov ecx, 0xC0DEC0DE
        mov edx, 0xDA7ADA7A

under_test:
        mov ds, ax                         ; <<< the instruction under test
after_test:
        mov ax, SEL_DATA                   ; put DS back before touching memory again
        mov ds, ax
        jmp next_case

done:   mov al, 0
        out 0xF4, al                       ; isa-debug-exit: stop the emulator
.spin:  hlt
        jmp .spin

; A gate got taken. DS/SS may be unusable, so reload everything from
; scratch, drop the frame, and carry on with the next case.
fault:
        mov ax, SEL_DATA
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov esp, STACK_TOP
        jmp next_case

        align 8
gdt:
        dq 0                                                  ; 00 null
        dw 0xFFFF, 0x0000
        db 0x00, 0x9A, 0xCF, 0x00         ; 08 code32 r/x  DPL0
        dw 0xFFFF, 0x0000
        db 0x00, 0x92, 0xCF, 0x00         ; 10 data32 r/w  DPL0
        dw 0xFFFF, 0x0000
        db 0x00, 0xF2, 0xCF, 0x00         ; 18 data32 r/w  DPL3
        dw 0xFFFF, 0x0000
        db 0x00, 0x12, 0xCF, 0x00         ; 20 data32, not present
        dw 0xFFFF, 0x0000
        db 0x00, 0x98, 0xCF, 0x00         ; 28 code32 execute-only
        dw 0xFFFF, 0x0000
        db 0x00, 0x96, 0xCF, 0x00         ; 30 data32 expand-down
        dw 0x0FFF, 0x0000
        db 0x00, 0x92, 0x40, 0x00         ; 38 data32 limit 0FFFh, byte granular
gdt_end:

gdtr:   dw gdt_end - gdt - 1
        dd gdt
idtr:   dw 32 * 8 - 1
        dd IDT_BASE

seltab:
        dw 0x0000          ; null
        dw 0x0010          ; plain data
        dw 0x0008          ; readable code into a data register
        dw 0x0018          ; data DPL3, RPL 0
        dw 0x001B          ; data DPL3, RPL 3
        dw 0x0020          ; not present
        dw 0x0028          ; execute-only code
        dw 0x0030          ; expand-down
        dw 0x0038          ; short limit
        dw 0x0003          ; null with RPL 3
        dw 0x0080          ; index past the GDT limit
        dw 0x0084          ; TI=1: an LDT selector, with LDTR never loaded
        dw 0x0088          ; index 17, past the GDT limit
        dw 0x000C          ; TI=1, index 1, LDTR never loaded
seltab_end:

; Self-describing footer: the harness reads these rather than a symbol file.
        times 496 - ($ - $$) db 0
        dw 0x5350                          ; 'PS' magic
        dw under_test
        dw after_test
        dw fault
        dw (seltab_end - seltab) / 2
        times 510 - ($ - $$) db 0
        dw 0xAA55
