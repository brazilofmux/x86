; fileio.com — file I/O semantics the DOS layer's buffering must keep:
; overwrite inside written data, seek back and read, size by seeking to
; the end with writes pending, the same file open twice (each handle sees
; the other's writes at once), a dup handle sharing the file position,
; truncation by a zero-length write, reading past the end. One line per
; check; the expected output is tests/dos/fileio.out.
        cpu     8086                    ; no 386 near Jcc: the default model is the 286
        org 100h
%macro  jcfail 0
        jnc     %%ok
        jmp     fail
%%ok:
%endmacro
        ; create FIO.TMP, write "ABCDEFGH"
        mov     ah, 3Ch
        xor     cx, cx
        mov     dx, fname
        int     21h
        jcfail
        mov     [h1], ax
        mov     bx, ax
        mov     ah, 40h
        mov     cx, 8
        mov     dx, s8
        int     21h
        ; seek to 2, write "xy"
        mov     ax, 4200h
        mov     bx, [h1]
        xor     cx, cx
        mov     dx, 2
        int     21h
        mov     ah, 40h
        mov     bx, [h1]
        mov     cx, 2
        mov     dx, sxy
        int     21h
        ; seek to 0, read 8: ABxyEFGH
        mov     ax, 4200h
        mov     bx, [h1]
        xor     cx, cx
        xor     dx, dx
        int     21h
        call    read8_h1
        call    show                    ; line 1
        ; seek end: size 8
        mov     ax, 4202h
        mov     bx, [h1]
        xor     cx, cx
        xor     dx, dx
        int     21h
        call    showax                  ; line 2
        ; open it again, read 8 there: ABxyEFGH
        mov     ax, 3D02h
        mov     dx, fname
        int     21h
        jcfail
        mov     [h2], ax
        mov     ah, 3Fh
        mov     bx, [h2]
        mov     cx, 8
        mov     dx, rbuf
        int     21h
        call    show                    ; line 3
        ; h1 (at 8) writes "Z"; h2 (at 8) reads it: Z
        mov     ah, 40h
        mov     bx, [h1]
        mov     cx, 1
        mov     dx, sz
        int     21h
        mov     byte [rbuf], '-'
        mov     ah, 3Fh
        mov     bx, [h2]
        mov     cx, 8
        mov     dx, rbuf
        int     21h
        call    showax                  ; line 4: 1 byte read
        mov     ah, 3Fh                 ; (count shown, then the byte)
        mov     al, [rbuf]
        mov     [one], al
        mov     ah, 9
        mov     dx, one
        int     21h
        ; dup h1; the dup seeks to 1; h1 reads 2 from there: Bx
        mov     ah, 45h
        mov     bx, [h1]
        int     21h
        mov     [h3], ax
        mov     ax, 4200h
        mov     bx, [h3]
        xor     cx, cx
        mov     dx, 1
        int     21h
        mov     ah, 3Fh
        mov     bx, [h1]
        mov     cx, 2
        mov     dx, rbuf
        int     21h
        mov     byte [rbuf+2], 13
        mov     byte [rbuf+3], 10
        mov     byte [rbuf+4], '$'
        mov     ah, 9
        mov     dx, rbuf
        int     21h                     ; line 6
        ; truncate at 4 (write 0 bytes there), size by seeking to the end: 4
        mov     ax, 4200h
        mov     bx, [h1]
        xor     cx, cx
        mov     dx, 4
        int     21h
        mov     ah, 40h
        mov     bx, [h1]
        xor     cx, cx
        int     21h
        mov     ax, 4202h
        mov     bx, [h2]
        xor     cx, cx
        xor     dx, dx
        int     21h
        call    showax                  ; line 7
        ; read past the end: 0
        mov     ah, 3Fh
        mov     bx, [h2]
        mov     cx, 8
        mov     dx, rbuf
        int     21h
        call    showax                  ; line 8
        ; many small writes then read them back through the other handle
        mov     ax, 4200h
        mov     bx, [h1]
        xor     cx, cx
        xor     dx, dx
        int     21h
        mov     si, 500
.w:     mov     ah, 40h
        mov     bx, [h1]
        mov     cx, 8
        mov     dx, s8
        int     21h
        dec     si
        jnz     .w
        mov     ax, 4200h
        mov     bx, [h2]
        xor     cx, cx
        mov     dx, 3992
        int     21h
        call    read8_h2
        call    show                    ; line 9: ABCDEFGH (the 500th record)
        mov     ah, 3Eh
        mov     bx, [h1]
        int     21h
        mov     ah, 3Eh
        mov     bx, [h2]
        int     21h
        mov     ah, 3Eh
        mov     bx, [h3]
        int     21h
        ; reopen: size 4000, then delete
        mov     ax, 3D00h
        mov     dx, fname
        int     21h
        jcfail
        mov     bx, ax
        mov     ax, 4202h
        xor     cx, cx
        xor     dx, dx
        int     21h
        call    showax                  ; line 10
        mov     ah, 41h
        mov     dx, fname
        int     21h
        mov     ax, 4C00h
        int     21h
fail:   mov     ah, 9
        mov     dx, mfail
        int     21h
        mov     ax, 4C01h
        int     21h

read8_h1:
        mov     bx, [h1]
        jmp     read8
read8_h2:
        mov     bx, [h2]
read8:  mov     ah, 3Fh
        mov     cx, 8
        mov     dx, rbuf
        int     21h
        ret
show:   mov     byte [rbuf+8], 13
        mov     byte [rbuf+9], 10
        mov     byte [rbuf+10], '$'
        mov     ah, 9
        mov     dx, rbuf
        int     21h
        ret
; AX in decimal, then CR LF
showax: mov     di, num+5
        mov     cx, 10
.d:     xor     dx, dx
        div     cx
        add     dl, '0'
        dec     di
        mov     [di], dl
        test    ax, ax
        jnz     .d
        mov     ah, 9
        mov     dx, di
        int     21h
        ret

fname   db      'FIO.TMP', 0
s8      db      'ABCDEFGH'
sxy     db      'xy'
sz      db      'Z'
one     db      '?', 13, 10, '$'
mfail   db      'FAIL', 13, 10, '$'
num     db      '     ', 13, 10, '$'
h1      dw      0
h2      dw      0
h3      dw      0
rbuf    times 16 db 0
