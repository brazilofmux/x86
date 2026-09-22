; a20.com — the A20 gate through INT 15h/24h, port 92h and the 8042,
; checked by the aliasing of FFFF:0510 onto 0000:0500. Prints one hex
; byte per probe; see a20.out.
        cpu 8086
        org 100h
        mov     ax, 2402h               ; query: expect 00 (off at boot)
        int     15h
        call    hex2
        mov     ax, 2403h               ; support: BX=3
        int     15h
        mov     al, bl
        call    hex2

        xor     ax, ax
        mov     ds, ax
        mov     ax, 0FFFFh
        mov     es, ax
        mov     byte [0500h], 11h
        mov     byte [es:0510h], 22h
        mov     al, [0500h]             ; aliased: 22
        call    hex2

        in      al, 92h
        or      al, 2                   ; port 92h: A20 on
        out     92h, al
        in      al, 92h
        and     al, 2
        call    hex2                    ; 02
        mov     al, [es:0510h]          ; real HMA now: 00
        call    hex2
        mov     byte [es:0510h], 33h
        mov     al, [0500h]             ; low memory untouched: 22
        call    hex2

        mov     al, 0D1h                ; 8042: write output port, A20 off
        out     64h, al
        mov     al, 0DDh
        out     60h, al
        mov     ax, 2402h
        int     15h
        call    hex2                    ; 00
        mov     al, [es:0510h]          ; aliased again: 22
        call    hex2
        mov     al, 0D0h                ; read output port back
        out     64h, al
.w:     in      al, 64h
        test    al, 1
        jz      .w
        in      al, 60h
        and     al, 2
        call    hex2                    ; 00

        mov     ax, 2401h               ; INT 15h: A20 on
        int     15h
        mov     ax, 2402h
        int     15h
        call    hex2                    ; 01
        mov     al, [es:0510h]          ; 33
        call    hex2

        mov     al, 0DFh                ; 8042 shortcut: off? no — DF = on, DD = off
        out     64h, al
        mov     al, 0DDh
        out     64h, al
        mov     al, [es:0510h]          ; 22
        call    hex2

        ; Translated code must not keep block keys or far-transfer
        ; targets computed under the other gate state: one loop body
        ; calls FFFF:0910 three times while the gate flips under it.
        push    cs
        pop     ds
        xor     ax, ax
        mov     es, ax
        mov     si, rtn_a               ; routine A at 0000:0900 (mov al,'A'; retf)
        mov     di, 0900h
        mov     cx, 4
        rep     movsb
        mov     ax, 0FFFFh
        mov     es, ax
        mov     al, 0DFh                ; A20 on: the real HMA at FFFF:0910
        out     64h, al
        mov     si, rtn_b
        mov     di, 0910h
        mov     cx, 4
        rep     movsb
        mov     al, 0DDh                ; off again
        out     64h, al
        mov     cx, 3
        mov     bl, 0
.loop:  call    far [cs:hma_ptr]        ; A (aliased), then B, then A
        call    hex2
        xor     bl, 2                   ; flip the gate: off→on→off
        in      al, 92h
        and     al, 0FDh
        or      al, bl
        out     92h, al
        loop    .loop

        mov     dx, crlf
        mov     ah, 9
        int     21h
        mov     ax, 4C00h
        int     21h

hma_ptr dw      0910h, 0FFFFh
rtn_a   db      0B0h, 'A', 0CBh, 90h    ; mov al,'A' / retf / nop
rtn_b   db      0B0h, 'B', 0CBh, 90h

hex2:   push    ax
        push    cx
        mov     cl, 4
        shr     al, cl
        call    hex1
        pop     cx
        pop     ax
hex1:   push    ax
        and     al, 15
        add     al, '0'
        cmp     al, '9'
        jbe     .p
        add     al, 7
.p:     mov     dl, al
        mov     ah, 2
        int     21h
        pop     ax
        ret
crlf    db      13, 10, '$'
