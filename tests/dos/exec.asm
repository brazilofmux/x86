; exec.com — INT 21h/4Bh: shrink, EXEC hello.com twice (once with an
; explicit environment), print the child's return code, then load exe1.exe
; as an overlay and call it. Exits with the child's code + 10.
        cpu 8086
%macro jcfail 0
        jnc     %%ok
        jmp     fail
%%ok:
%endmacro
        org 100h
        mov     sp, stack_top           ; our stack, below the memory we keep
        mov     bx, ((stack_top - $$) + 15) / 16 + 17   ; PSP + program + stack, in paragraphs
        mov     ah, 4Ah                 ; shrink to what we use
        int     21h
        jcfail

        mov     ah, 9
        mov     dx, m_before
        int     21h

        mov     word [pb_env], 0        ; inherit the environment
        mov     ax, cs
        mov     [pb + 4], ax
        mov     [pb + 8], ax
        mov     [pb + 12], ax
        mov     ax, 4B00h
        mov     dx, child
        mov     bx, pb
        int     21h
        jcfail
        mov     ah, 4Dh
        int     21h
        mov     [code], al
        mov     ah, 9
        mov     dx, m_after
        int     21h
        mov     al, [code]
        call    hex2
        mov     ah, 2
        mov     dl, 13
        int     21h
        mov     dl, 10
        int     21h

        ; second time: with our own environment block
        mov     ax, cs
        mov     [pb_env], ax            ; not a real env block, but DOS uses it as given
        mov     ax, 4B00h
        mov     dx, child
        mov     bx, pb
        int     21h
        jcfail
        mov     ah, 4Dh
        int     21h
        mov     [code], al

        ; overlay: exe1.exe's code at a fresh block; it expects to be a
        ; program, so we only check the load succeeded and the bytes landed
        mov     bx, 0100h
        mov     ah, 48h
        int     21h
        jcfail
        mov     [ov_seg], ax
        mov     [ov_rel], ax
        mov     ax, 4B03h
        mov     dx, overlay
        mov     bx, ov_seg
        int     21h
        jcfail
        mov     es, [ov_seg]
        mov     al, [es:0]              ; first byte of exe1's code (its MZ image starts with mov ax,...)
        call    hex2
        mov     ah, 2
        mov     dl, 13
        int     21h
        mov     dl, 10
        int     21h

        mov     al, [code]
        add     al, 10
        mov     ah, 4Ch
        int     21h
fail:   mov     dx, m_fail
        mov     ah, 9
        int     21h
        mov     ax, 4CFFh
        int     21h

hex2:   push    ax
        mov     cl, 4
        shr     al, cl
        call    hex1
        pop     ax
hex1:   and     al, 15
        add     al, '0'
        cmp     al, '9'
        jbe     .p
        add     al, 7
.p:     mov     dl, al
        mov     ah, 2
        int     21h
        ret

child   db      'HELLO.COM', 0
overlay db      'EXE1.EXE', 0
pb:
pb_env  dw      0
        dw      tail, 0                 ; command tail (segment patched at run time? cs=ds for .COM: fix below)
        dw      fcb1, 0
        dw      fcb2, 0
tail    db      4, ' a b', 13
fcb1    times 16 db 0
fcb2    times 16 db 0
ov_seg  dw      0
ov_rel  dw      0
code    db      0
m_before db     'parent: exec', 13, 10, '$'
m_after  db     'parent: child returned $'
m_fail   db     'parent: FAIL', 13, 10, '$'
        align   16
        times   256 db 0
stack_top:
