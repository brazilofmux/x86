; scan.com — print every code the keyboard sends (port 60h, read in an
; INT 9 handler that then chains to the BIOS's), two hex digits a line,
; until Esc's break code (81h); then each key the BIOS put in its buffer
; (INT 16h), as "k" and four hex digits. Fed held keys on stdin (kitty
; keyboard protocol press/release events), it checks that a key goes
; down and comes up when the host says, and what the BIOS made of it
; (tests/dos/scan.out).
        cpu     8086
        org     100h
        mov     ax, 3509h               ; the BIOS's INT 9
        int     21h
        mov     [old9], bx
        mov     [old9+2], es
        mov     ax, 2509h
        mov     dx, int9
        int     21h
main:   mov     bx, [tail]
        cmp     bx, [head]
        jne     .got
        sti
        hlt                             ; until the next code
        jmp     main
.got:
        mov     al, [ring+bx]
        inc     bx
        and     bx, 63
        mov     [tail], bx
        push    ax
        call    hex2
        call    newline
        pop     ax
        cmp     al, 81h                 ; Esc up: done
        jne     main
        push    ds
        lds     dx, [old9]
        mov     ax, 2509h
        int     21h
        pop     ds
keys:   mov     ah, 1                   ; what the BIOS buffered
        int     16h
        jz      done
        xor     ah, ah
        int     16h
        push    ax
        mov     dl, 'k'
        mov     ah, 2
        int     21h
        pop     ax
        push    ax
        mov     al, ah
        call    hex2
        pop     ax
        call    hex2
        call    newline
        jmp     keys
done:   mov     ax, 4C00h
        int     21h

int9:   push    ax
        push    bx
        in      al, 60h
        mov     bx, [cs:head]
        mov     [cs:ring+bx], al
        inc     bx
        and     bx, 63
        mov     [cs:head], bx
        pop     bx
        pop     ax
        jmp     far [cs:old9]           ; the BIOS reads the same code and sends the EOI

; AL as two hex digits
hex2:   push    ax
        mov     cl, 4
        shr     al, cl
        call    digit
        pop     ax
        and     al, 0Fh
digit:  add     al, '0'
        cmp     al, '9'
        jbe     .p
        add     al, 7
.p:     mov     dl, al
        mov     ah, 2
        int     21h
        ret
newline:
        mov     ah, 9
        mov     dx, crlf
        int     21h
        ret

crlf    db      13, 10, '$'
old9    dd      0
head    dw      0
tail    dw      0
ring    times 64 db 0
