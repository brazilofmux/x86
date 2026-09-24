; ps2.asm — the PS/2 mouse on the 8042's auxiliary port. First as a
; driver that owns the hardware sees it (NT's i8042prt): the interface
; test, the device's reset, identify, status request and sample rate,
; wrap mode's echo, the controller's loopback (D3h) — every byte read
; with the status register's "from the mouse" bit. Then as a DOS driver
; through the BIOS (FreeDOS's CTMOUSE): INT 15h AH=C2h to initialise,
; install a routine and enable, and the packets INT 74h hands it for
; SGR mouse reports on stdin (the keyboard is polled so they are read).
; Esc ends it. tests/dos/ps2.out.
;
;   nasm -f bin -o ps2.com ps2.asm
        cpu 386
        org 0x100

start:
        cli                             ; the port-level half runs polled
        mov al, 0xA8                    ; aux port on
        call cmd
        mov al, 0xA9                    ; its interface test: 00
        call cmd
        mov dx, s_test
        call line1
        mov al, 0xFF                    ; reset: FA AA 00
        call aux
        mov dx, s_reset
        call line3
        mov al, 0xF2                    ; identify: FA 00
        call aux
        mov dx, s_id
        call line2
        mov al, 0xE9                    ; status: FA, status, resolution, rate
        call aux
        mov dx, s_stat
        call line4
        mov al, 0xF3                    ; rate 40: FA FA
        call aux
        mov al, 40
        call aux
        mov dx, s_rate
        call line2
        mov al, 0xE9
        call aux
        mov dx, s_stat
        call line4
        mov al, 0xEE                    ; wrap: FA, then 55h comes back, EC: FA
        call aux
        mov al, 0x55
        call aux
        mov al, 0xEC
        call aux
        mov dx, s_wrap
        call line3
        mov al, 0xD3                    ; the controller's loopback
        out 0x64, al
        call ibf
        mov al, 0x5A
        out 0x60, al
        mov dx, s_loop
        call line1
        sti

        ; ---- the BIOS's way
        mov ax, 0xC205                  ; initialise, 3-byte packets
        mov bh, 3
        int 0x15
        push cs
        pop es
        mov bx, routine
        mov ax, 0xC207                  ; the routine INT 74h calls
        int 0x15
        mov ax, 0xC200                  ; enable
        mov bh, 1
        int 0x15
        mov dx, s_bios
        call puts
        mov al, ah
        call hex8
        call crlf

.loop:  mov si, [done]                  ; print what the routine logged
        cmp si, [count]
        je .key
        imul si, si, 6
        add si, log
        mov dx, s_p
        call puts
        mov al, [si]
        call hex8
        mov dx, s_x
        call puts
        mov al, [si+2]
        call hex8
        mov dx, s_y
        call puts
        mov al, [si+4]
        call hex8
        call crlf
        inc word [done]
        jmp .loop
.key:   mov ah, 1
        int 0x16
        jz .loop
        mov ah, 0
        int 0x16
        cmp al, 27
        jne .loop
        mov ax, 0xC200                  ; disable
        mov bh, 0
        int 0x15
        mov ax, 0x4C00
        int 0x21

; the BIOS calls this with status, X, Y and a zero word pushed
routine:
        push bp
        mov bp, sp
        push ax
        push bx
        mov bx, [cs:count]
        cmp bx, 32
        jae .full
        imul bx, bx, 6
        add bx, log
        mov ax, [bp+12]
        mov [cs:bx], ax
        mov ax, [bp+10]
        mov [cs:bx+2], ax
        mov ax, [bp+8]
        mov [cs:bx+4], ax
        inc word [cs:count]
.full:  pop bx
        pop ax
        pop bp
        retf

; a controller command
cmd:    push ax
        call ibf
        pop ax
        out 0x64, al
        ret
; a byte to the mouse (D4h)
aux:    push ax
        mov al, 0xD4
        call cmd
        call ibf
        pop ax
        out 0x60, al
        ret
ibf:    in al, 0x64                     ; the input buffer empty
        test al, 2
        jnz ibf
        ret
; the next byte from the 8042 in AL, the "mouse's" bit in AH (0/1); FFh
; and AH=EE if nothing comes
obf:    push cx
        mov cx, 0xFFFF
.w:     in al, 0x64
        test al, 1
        jnz .got
        loop .w
        pop cx
        mov ax, 0xEEFF
        ret
.got:   mov ah, al
        shr ah, 5
        and ah, 1
        in al, 0x60
        pop cx
        ret

line4:  call puts
        call byte1
        call byte1
        call byte1
        call byte1
        jmp crlf
line3:  call puts
        call byte1
        call byte1
        call byte1
        jmp crlf
line2:  call puts
        call byte1
        call byte1
        jmp crlf
line1:  call puts
        call byte1
        jmp crlf
byte1:  call obf                        ; " XX" or " XX*" (* the mouse's)
        push ax
        mov al, ' '
        call putc
        pop ax
        call hex8
        cmp ah, 1
        jne .n
        mov al, '*'
        call putc
.n:     ret

hex8:   push ax
        shr al, 4
        call hex4
        pop ax
hex4:   push ax
        and al, 15
        add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     call putc
        pop ax
        ret
putc:   push ax
        push dx
        mov dl, al
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

s_test  db "aux interface test:$"
s_reset db "reset:$"
s_id    db "identify:$"
s_stat  db "status request:$"
s_rate  db "rate 40:$"
s_wrap  db "wrap 55h, leave:$"
s_loop  db "loopback 5Ah:$"
s_bios  db "INT 15h C2h: enabled, AH=$"
s_p     db "packet $"
s_x     db " X=$"
s_y     db " Y=$"
s_crlf  db 13, 10, "$"
count   dw 0
done    dw 0
log     times 32*6 db 0
