; smcflag.com — a memory-destination ADD whose store lands inside the block
; running it. Its AF and PF are dead in that block (the ADD after it writes
; them again before anything reads them), so the AArch64 backend defers the
; two to the code-bitmap guard's cold path — and the sweep then abandons the
; block at exactly that guard. -V compares every flag at the exit, so this
; is where a wrong deferred AF or PF shows up. The patched immediate is
; printed too, so the interpreter and the JIT have to agree on the result.
        cpu 8086
        org 100h
        jmp     start

run:    mov     dx, 3
        mov     ax, 0708h
        mov     bx, ptgt
        add     word [bx], ax   ; 1119 + 0708 = 1821: AF and PF both set
        add     dx, dx          ; kills AF and PF before anything reads them
        mov     cx, 1119h       ; B9 19 11 — its immediate is the target
ptgt    equ     $ - 2
        ret

start:  call    run
        mov     ax, cx          ; the immediate as it was fetched: 1821
        call    hex16
        mov     dl, 13
        mov     ah, 2
        int     21h
        mov     dl, 10
        mov     ah, 2
        int     21h
        mov     ax, 4C00h
        int     21h

hex16:  push    ax
        mov     al, ah
        call    hex8
        pop     ax
        call    hex8
        ret
hex8:   push    ax
        mov     cl, 4
        shr     al, cl
        call    nib
        pop     ax
        and     al, 0Fh
        call    nib
        ret
nib:    add     al, '0'
        cmp     al, '9'
        jbe     .p
        add     al, 7
.p:     mov     dl, al
        mov     ah, 2
        int     21h
        ret
