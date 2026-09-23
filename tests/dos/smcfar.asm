; smcfar.com — INT n and CALL FAR [mem] whose frame pushes land on
; translated code. The JIT held the far target in scratch registers
; across the pushes, and the store's code-bitmap check (SMC) called out
; and clobbered them: the transfer went to garbage. Seen as FreeDOS's
; installer crashing in XCOPY after SLICEREX, whose freed code became
; the next program's stack. Prints "I F ok".
        cpu     8086
        org     100h
        call    victim                  ; run it once: its bytes are now translated code
        xor     ax, ax
        mov     es, ax
        mov     word [es:60h*4], handler
        mov     [es:60h*4+2], cs
        mov     [farptr+2], cs
        mov     [savesp], sp
        cli
        mov     sp, victim_end          ; the frames below overwrite victim
        sti
        int     60h
        call    far [farptr]
        cli
        mov     sp, [savesp]
        sti
        mov     dx, okmsg
        mov     ah, 9
        int     21h
        mov     ax, 4C00h
        int     21h

handler:
        push    ax
        push    dx
        mov     dl, 'I'
        mov     ah, 2
        int     21h
        mov     dl, ' '
        int     21h
        pop     dx
        pop     ax
        iret

farproc:
        push    ax
        push    dx
        mov     dl, 'F'
        mov     ah, 2
        int     21h
        mov     dl, ' '
        int     21h
        pop     dx
        pop     ax
        retf

farptr  dw      farproc, 0
savesp  dw      0
okmsg   db      'ok', 13, 10, '$'

        align   16
victim:
        times   64 nop
        ret
        times   15 nop
victim_end:
