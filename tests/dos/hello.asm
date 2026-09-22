; hello.com — INT 21h/09 string, INT 10h teletype, INT 21h/4C exit
        org 100h
        mov     ah, 9
        mov     dx, msg
        int     21h
        mov     ah, 0Eh
        mov     al, '*'
        int     10h
        mov     ah, 2
        mov     dl, 13
        int     21h
        mov     dl, 10
        int     21h
        mov     ax, 4C07h
        int     21h
msg     db      'Hello from dos-monster!', 13, 10, '$'
