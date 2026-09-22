; intbench.com — N x (INT 16h AH=01, keyboard status poll): the tightest
; DOS-service loop WordPerfect's idle path runs.
        cpu 8086
        org 100h
        mov     cx, 2000
        mov     dx, 100                 ; 200,000 calls
outer:  mov     bx, dx
inner:  mov     ah, 1
        int     16h
        dec     bx
        jnz     inner
        loop    outer
        mov     ax, 4C00h
        int     21h
