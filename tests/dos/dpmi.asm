; dpmi.com — a minimal DPMI client: detect the host, switch to protected
; mode, allocate and shape a descriptor, read it back, and leave.
        cpu 386
        org 100h
        mov     ax, 1687h
        int     2Fh
        test    ax, ax
        jz      have
        mov     dx, msg_none
        jmp     bail
have:   mov     [entry], di
        mov     [entry + 2], es
        xor     ax, ax                  ; a 16-bit client
        call    far [entry]
        jnc     inpm
        mov     dx, msg_fail
        jmp     bail

; ---- from here on we are in 32-bit protected mode ----
inpm:
        mov     ax, 0400h               ; version
        int     31h
        cmp     ax, 005Ah
        jne     bad
        mov     ax, 0000h               ; one descriptor
        mov     cx, 1
        int     31h
        jc      bad
        mov     bx, ax
        mov     cx, 0012h               ; base 00123400h
        mov     dx, 3400h
        mov     ax, 0007h
        int     31h
        jc      bad
        xor     cx, cx                  ; read it back
        xor     dx, dx
        mov     ax, 0006h
        int     31h
        jc      bad
        cmp     cx, 0012h
        jne     bad
        cmp     dx, 3400h
        jne     bad
        ; DS here is a protected-mode selector, so this only prints if the
        ; DOS layer resolved the pointer through the descriptor's base.
        mov     dx, msg_pm
        mov     ah, 9
        int     21h
        mov     ax, 4C00h               ; success
        int     21h
bad:    mov     ax, 4C01h
        int     21h

bail:   mov     ah, 9
        int     21h
        mov     ax, 4C02h
        int     21h

entry   dd      0
msg_pm   db     'hello from protected mode', 13, 10, '$'
msg_none db     'no DPMI host', 13, 10, '$'
msg_fail db     'mode switch failed', 13, 10, '$'
