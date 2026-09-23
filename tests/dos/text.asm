; text.com — a text screen that exercises the VGA text renderer: all 256
; attributes (blink off through INT 10h AX=1003h, so the high background
; colours are bright), a row of line-drawing characters (the ninth-column
; rule), and a block cursor set with INT 10h AH=01h. tests/dos/run.sh
; renders it with -G and compares the pixels.
        cpu     8086
        org     100h
        mov     ax, 0003h
        int     10h
        mov     ax, 1003h               ; attribute bit 7 = bright background
        xor     bl, bl
        int     10h
        mov     ax, 0B800h
        mov     es, ax
        ; 16 rows of 16 attributes, each cell pair 'Ab'
        xor     bx, bx                  ; bl = attribute
.a:     mov     al, bl
        mov     cl, 4
        shr     al, cl                  ; row = attr >> 4
        mov     ah, 160
        mul     ah
        mov     di, ax
        mov     al, bl
        and     al, 0Fh
        shl     al, 1
        shl     al, 1                   ; col = (attr & 15) * 2 cells = 4 bytes
        xor     ah, ah
        add     di, ax
        mov     ah, bl
        mov     al, 'A'
        stosw
        mov     al, 'b'
        stosw
        inc     bl
        jnz     .a
        ; row 18: line drawing, attribute 1Fh
        mov     di, 18 * 160
        mov     si, boxes
        mov     ah, 1Fh
.b:     lodsb
        or      al, al
        jz      .c
        stosw
        jmp     .b
.c:     ; a block cursor at row 20, column 10
        mov     ah, 01h
        mov     cx, 0007h
        int     10h
        mov     ah, 02h
        xor     bh, bh
        mov     dx, 140Ah
        int     10h
        mov     ax, 4C00h
        int     21h
boxes   db      0DAh, 0C4h, 0C2h, 0C4h, 0BFh, ' ', 0C9h, 0CDh, 0CBh, 0CDh, 0BBh, ' '
        db      0C3h, 0C4h, 0C5h, 0C4h, 0B4h, ' ', 0CCh, 0CDh, 0CEh, 0CDh, 0B9h, ' '
        db      0C0h, 0C4h, 0C1h, 0C4h, 0D9h, ' ', 0C8h, 0CDh, 0CAh, 0CDh, 0BCh, ' '
        db      0B0h, 0B1h, 0B2h, 0DBh, 0DCh, 0DDh, 0DEh, 0DFh, ' ', 'W', 'P', '5', '1', 0
