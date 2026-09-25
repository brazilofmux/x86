; uart.asm — COM1's 16550 (-com1), in loopback so it needs no far end.
;
; The BIOS data area's COM1 address and the equipment word's port count;
; the scratch register; a byte sent in loopback coming back (LSR DR, RBR);
; MSR reflecting MCR in loopback; the THRE interrupt identification raised
; by enabling it and taken by reading IIR; the FIFO (IIR bits 7:6) and,
; below its trigger level, the character-timeout identification (0Ch);
; and IRQ 4 itself — an INT 0Ch handler counting received-data
; interrupts, through the master 8259 with its EOI. Without -com1 there
; is no port: "no COM1". Expected output: uart.out (and uart-none.out).
;
;   nasm -f bin -o uart.com uart.asm
        cpu 386
        org 0x100
COM     equ 0x3F8

start:  push ds
        xor ax, ax
        mov ds, ax
        mov ax, [0x400]                 ; COM1's I/O address
        mov bx, [0x410]                 ; equipment word
        pop ds
        test ax, ax
        jnz .have
        mov dx, s_none
        call puts
        jmp exit
.have:  mov dx, s_addr
        call puts
        call hex16
        mov dx, s_ports
        call puts
        mov ax, bx
        shr ax, 9
        and al, 7
        call hex8
        call crlf

        ; ---- scratch
        mov dx, COM+7
        mov al, 0x5A
        out dx, al
        in al, dx
        mov dx, s_scr
        call puts
        call hex8
        call crlf

        ; ---- loopback: MCR = LOOP | OUT2 | DTR; MSR mirrors it (DCD, DSR)
        mov dx, COM+3
        mov al, 0x03                    ; 8N1, DLAB off
        out dx, al
        mov dx, COM+4
        mov al, 0x19
        out dx, al
        mov dx, COM+6
        in al, dx
        mov dx, s_msr
        call puts
        call hex8
        call crlf
        mov dx, COM
        mov al, 'A'
        out dx, al
        mov dx, COM+5
        in al, dx
        mov dx, s_lsr
        call puts
        call hex8
        mov dx, COM
        in al, dx
        mov dx, s_rbr
        call puts
        call hex8
        call crlf

        ; ---- THRE: enabling it with the holding register empty is an
        ; interrupt; reading IIR takes it
        mov dx, COM+2
        in al, dx
        mov bl, al
        mov dx, COM+1
        mov al, 0x02
        out dx, al
        mov dx, COM+2
        in al, dx
        mov bh, al
        in al, dx
        mov cl, al
        mov dx, s_iir
        call puts
        mov al, bl
        call hex8
        call space
        mov al, bh
        call hex8
        call space
        mov al, cl
        call hex8
        call crlf

        ; ---- FIFO, trigger 14: three bytes are below it (timeout, 0Ch)
        mov dx, COM+1
        mov al, 0x01                    ; received data only
        out dx, al
        mov dx, COM+2
        mov al, 0xC7                    ; FIFO on, both cleared, trigger 14
        out dx, al
        mov dx, COM
        mov al, '1'
        out dx, al
        mov al, '2'
        out dx, al
        mov al, '3'
        out dx, al
        mov dx, COM+2
        in al, dx
        mov dx, s_fifo
        call puts
        call hex8
        call space
        mov cx, 3
.rd:    mov dx, COM
        in al, dx
        mov dl, al
        mov ah, 2
        int 0x21
        loop .rd
        mov dx, COM+2
        in al, dx
        call space
        call hex8
        call crlf
        mov dx, COM+2
        mov al, 0x00                    ; FIFO off
        out dx, al

        ; ---- IRQ 4: INT 0Ch counts received-data interrupts
        cli
        push es
        xor ax, ax
        mov es, ax
        mov ax, [es:0x0C*4]
        mov [old0c], ax
        mov ax, [es:0x0C*4+2]
        mov [old0c+2], ax
        mov word [es:0x0C*4], irq4
        mov [es:0x0C*4+2], cs
        pop es
        in al, 0x21
        mov [oldmask], al
        and al, 0xEF                    ; unmask IRQ 4
        out 0x21, al
        mov dx, COM+1
        mov al, 0x01
        out dx, al
        sti
        mov dx, COM
        mov al, 'Z'
        out dx, al                      ; loops back: received data, IRQ 4
        mov cx, 1000
.w:     cmp byte [hits], 0
        jne .got
        loop .w
.got:   cli
        mov dx, COM+1
        xor al, al
        out dx, al
        mov al, [oldmask]
        out 0x21, al
        push es
        xor ax, ax
        mov es, ax
        mov ax, [old0c]
        mov [es:0x0C*4], ax
        mov ax, [old0c+2]
        mov [es:0x0C*4+2], ax
        pop es
        sti
        mov dx, s_irq
        call puts
        mov al, [hits]
        call hex8
        call space
        mov al, [irqiir]
        call hex8
        call space
        mov al, [irqbyte]
        call hex8
        call crlf
        mov dx, COM+4
        xor al, al                      ; loopback off
        out dx, al
exit:   mov ax, 0x4C00
        int 0x21

irq4:   push ax
        push dx
        mov dx, COM+2
        in al, dx
        mov [cs:irqiir], al
        mov dx, COM
        in al, dx
        mov [cs:irqbyte], al
        inc byte [cs:hits]
        mov al, 0x20
        out 0x20, al
        pop dx
        pop ax
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

s_none  db "no COM1", 13, 10, "$"
s_addr  db "COM1 at $"
s_ports db ", ports $"
s_scr   db "scratch $"
s_msr   db "loopback MSR $"
s_lsr   db "sent A: LSR $"
s_rbr   db " RBR $"
s_iir   db "IIR idle, THRE on, after $"
s_fifo  db "FIFO IIR, data, after $"
s_irq   db "IRQ 4 hits, IIR, byte $"
s_sp    db " $"
s_crlf  db 13, 10, "$"
hits    db 0
irqiir  db 0
irqbyte db 0
oldmask db 0
old0c   dd 0
