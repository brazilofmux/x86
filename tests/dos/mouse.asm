; mouse.com — the INT 33h driver's event handler and polling calls. It
; resets the driver, shows the cursor, installs a handler for every event
; (AX=0Ch, mask 7Fh) that logs AX/BX/CX/DX, and polls the keyboard (so
; guest code runs between host events); each logged call is printed as
; "E=ev B=buttons X=x Y=y". Esc ends it with AX=03h (position, buttons)
; and AX=05h (left presses). Fed SGR mouse reports on stdin with
; X86_MOUSE=1 (tests/dos/mouse.out).
        cpu     8086
        org     100h
        xor     ax, ax                  ; reset: AX=FFFF if installed
        int     33h
        call    hex4
        call    nl
        mov     ax, 1                   ; show the cursor
        int     33h
        push    cs
        pop     es
        mov     ax, 0Ch                 ; handler for every event
        mov     cx, 7Fh
        mov     dx, handler
        int     33h
.loop:  mov     si, [done]              ; print what the handler logged
        cmp     si, [count]
        je      .key
        shl     si, 1
        shl     si, 1
        shl     si, 1
        add     si, log
        mov     dl, 'E'
        call    field
        mov     dl, 'B'
        call    field
        mov     dl, 'X'
        call    field
        mov     dl, 'Y'
        call    field
        call    nl
        inc     word [done]
        jmp     .loop
.key:   mov     ah, 1
        int     16h
        jz      .loop
        xor     ah, ah
        int     16h
        cmp     al, 27
        jne     .loop
        mov     ax, 3                   ; position and buttons
        int     33h
        push    dx
        push    cx
        mov     ax, bx
        call    hex4
        call    sp_
        pop     ax
        call    hex4
        call    sp_
        pop     ax
        call    hex4
        call    nl
        mov     ax, 5                   ; left presses since the last ask
        xor     bx, bx
        int     33h
        mov     ax, bx
        call    hex4
        call    nl
        mov     ax, 4C00h
        int     21h

; the handler: AX event mask, BX buttons, CX x, DX y (far call, RETF)
handler:
        push    si
        push    ds
        push    cs
        pop     ds
        mov     si, [count]
        cmp     si, 64
        jae     .full
        shl     si, 1
        shl     si, 1
        shl     si, 1
        mov     [log + si], ax
        mov     [log + si + 2], bx
        mov     [log + si + 4], cx
        mov     [log + si + 6], dx
        inc     word [count]
.full:  pop     ds
        pop     si
        retf

; "<dl>=" then the word at [si] in hex and a space; si += 2
field:  mov     ah, 2
        int     21h
        mov     dl, '='
        int     21h
        lodsw
        call    hex4
        call    sp_
        ret

sp_:    mov     ah, 2
        mov     dl, ' '
        int     21h
        ret
nl:     mov     ah, 2
        mov     dl, 13
        int     21h
        mov     dl, 10
        int     21h
        ret

; AX as four hex digits
hex4:   push    si
        mov     cx, 4
.d:     push    cx
        mov     cl, 4
        rol     ax, cl
        pop     cx
        push    ax
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .p
        add     al, 7
.p:     mov     dl, al
        mov     ah, 2
        int     21h
        pop     ax
        loop    .d
        pop     si
        ret

count   dw      0
done    dw      0
log     times 64 * 8 db 0
