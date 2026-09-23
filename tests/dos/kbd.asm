; kbd.com — print each INT 16h key (AH=00h) as four hex digits, one per
; line, until Esc. Fed xterm escape sequences on stdin, it checks the
; BIOS's INT 9 translation of modified keys (tests/dos/kbd.out).
        cpu     8086
        org     100h
next:   xor     ah, ah
        int     16h
        push    ax
        call    hex4
        mov     ah, 9
        mov     dx, crlf
        int     21h
        pop     ax
        cmp     ax, 011Bh               ; Esc
        jne     next
        mov     ax, 4C00h
        int     21h

; AX as four hex digits
hex4:   mov     cx, 4
.d:     push    cx
        mov     cl, 4
        rol     ax, cl
        pop     cx
        push    ax
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .p
        add     al, 7
.p:     mov     dl, al
        mov     ah, 2
        int     21h
        pop     ax
        loop    .d
        ret

crlf    db      13, 10, '$'
