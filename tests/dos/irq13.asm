; irq13.asm — the coprocessor's error line, as a PC/AT wires it: an
; unmasked x87 exception with CR0.NE clear raises FERR#, the slave 8259's
; IRQ 13 (INT 75h), whose BIOS handler clears the latch at port F0h, EOIs
; both 8259s and calls INT 2 — where DOS programs hook floating-point
; errors. This hooks INT 2 and checks: the handler runs, by the next
; instruction, with the exception in the status word; with interrupts
; off it waits for STI (and then comes soon: how soon after STI is the
; translator's block boundary, so this waits for it); and the slave's
; mask reads as POST left it (IRQs 8 and 13 open).
;
;   nasm -f bin -o irq13.com irq13.asm
        cpu 486
        org 0x100

start:
        xor ax, ax
        mov es, ax
        mov ax, [es:2*4]
        mov [old2], ax
        mov ax, [es:2*4+2]
        mov [old2+2], ax
        cli
        mov word [es:2*4], nmi
        mov [es:2*4+2], cs
        sti

        in al, 0xA1                     ; slave mask: all but IRQ 8 (the RTC) and IRQ 13
        mov dx, s_mask
        call puts
        call hex8
        call crlf

        ; ---- 1/0 with ZE unmasked
        fninit
        fldcw [cw_ze]
        fld1
        fldz
        fdivp st1, st0                  ; FERR#: IRQ 13 at the next boundary
        mov al, [count]                 ; the handler has run by now
        mov dx, s_one
        call puts
        call hex8
        mov dx, s_sw
        call puts
        mov ax, [sw_seen]
        call hex16
        call crlf

        ; ---- the same with interrupts off: nothing until STI
        fninit
        fldcw [cw_ze]
        fld1
        fldz
        cli
        fdivp st1, st0
        nop
        mov al, [count]
        mov [before_sti], al
        sti
        mov cx, 0xFFFF                  ; then soon: at most a timer tick or so
.wait:  cmp byte [count], 2
        jae .came
        loop .wait
.came:  mov al, [count]
        mov [after_sti], al
        mov dx, s_cli
        call puts
        mov al, [before_sti]
        call hex8
        mov dx, s_sti
        call puts
        mov al, [after_sti]
        call hex8
        call crlf

        fninit
        xor ax, ax
        mov es, ax
        cli
        mov ax, [old2]
        mov [es:2*4], ax
        mov ax, [old2+2]
        mov [es:2*4+2], ax
        sti
        mov ax, 0x4C00
        int 0x21

; INT 2: what the BIOS's INT 75h calls — take the status word, clear the
; exception, count it
nmi:    fnstsw [cs:sw_seen]
        fnclex
        inc byte [cs:count]
        iret

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

s_mask  db "slave mask: $"
s_one   db "1/0: INT 2 count $"
s_sw    db ", status word there $"
s_cli   db "with IF clear: count before STI $"
s_sti   db ", after $"
s_crlf  db 13, 10, "$"
cw_ze   dw 0x037B
old2    dd 0
sw_seen dw 0
count   db 0
before_sti db 0
after_sti  db 0
