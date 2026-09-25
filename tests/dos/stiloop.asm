; stiloop.asm — a loop that takes interrupts only in a STI shadow.
;
; "sti; nop; cli" and the test that ends the loop: interrupts are enabled
; for one instruction boundary per pass, and every translated block of it
; ends with IF clear. The timer's INT 8 (and the BIOS's INT 1Ch after it)
; can only be taken there — as Windows 2000's idle loop taught us (issue
; #1). Three ticks counted by an INT 1Ch hook, then "ticks: 3". A
; translator that delivers only at block boundaries never gets there.
;
;   nasm -f bin -o stiloop.com stiloop.asm
        cpu 8086
        org 0x100

start:  xor ax, ax
        mov es, ax
        mov ax, [es:0x1C*4]
        mov [old1c], ax
        mov ax, [es:0x1C*4+2]
        mov [old1c+2], ax
        cli
        mov word [es:0x1C*4], tick
        mov [es:0x1C*4+2], cs
.wait:  sti
        nop
        cli
        cmp word [ticks], 3
        jb .wait
        mov ax, [old1c]                 ; (interrupts still off)
        mov [es:0x1C*4], ax
        mov ax, [old1c+2]
        mov [es:0x1C*4+2], ax
        sti
        mov dx, msg
        mov ah, 9
        int 0x21
        mov ax, 0x4C00
        int 0x21

tick:   inc word [cs:ticks]
        iret

msg     db "ticks: 3", 13, 10, "$"
ticks   dw 0
old1c   dd 0
