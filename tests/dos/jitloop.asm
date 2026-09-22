; jitloop.com — the translator's hot-path shape: a memory read-modify-write,
; a register increment feeding a conditional branch, and a near call/ret.
; ~590M guest instructions, long enough to measure without noise.
        cpu 8086
        org 100h
        mov     si, 200h
        mov     bp, 9                   ; outer repeats
outer:  xor     cx, cx
        mov     dx, 300
loop1:  add     [si], cx
        inc     cx
        jnz     loop1
        call    sub1
        dec     dx
        jnz     loop1
        dec     bp
        jnz     outer
        mov     ax, 4C00h
        int     21h
sub1:   mov     ax, [si]
        xor     bx, ax
        ret
