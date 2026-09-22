; jitcmp.com — register compare-and-branch bound: no memory in the loop,
; so the critical path is the flag producer feeding the conditional.
        cpu 8086
        org 100h
        mov     bp, 1200                ; outer repeats
        mov     ax, 4321h
outer:  mov     bx, 0FFFFh
inner:  cmp     ax, bx
        je      skip
        add     dx, bx
        xor     si, bx
skip:   dec     bx
        jnz     inner
        dec     bp
        jnz     outer
        mov     ax, 4C00h
        int     21h
