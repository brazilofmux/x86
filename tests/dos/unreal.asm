; unreal.com — unreal mode, the way HIMEMX sets it up: from real mode
; load a GDT, set PE (CS still holds a real-mode selector, whose low bits
; are not a CPL: the processor is at CPL 0 until a far jump), load DS with
; a flat 4 GB descriptor, clear PE. Real-mode loads of DS after that keep
; the 4 GB limit, so a 32-bit offset above 1 MB reads and writes memory.
; Prints "CPL0 unreal ok". 386 only.
        cpu     386
        org     100h
        in      al, 92h                 ; A20 on
        or      al, 2
        out     92h, al
        xor     eax, eax
        mov     ax, cs
        shl     eax, 4
        add     eax, gdt
        mov     [gdtr+2], eax
        mov     dx, cs                  ; low bits of CS: whatever the load gave us
        cli
        lgdt    [gdtr]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax                ; PE: CPL is 0 whatever CS's low bits say
        mov     bx, 8
        mov     ds, bx                  ; #GP here if CPL came from CS
        and     al, 0FEh
        mov     cr0, eax
        sti
        mov     ds, dx                  ; real mode again; reload DS (base only)
        xor     ax, ax
        mov     ds, ax                  ; and again, base 0: the 4 GB limit stays
        mov     esi, 110000h            ; above 1 MB and the HMA
        mov     dword [esi], 0CAFEF00Dh
        mov     eax, [esi]
        push    cs
        pop     ds
        cmp     eax, 0CAFEF00Dh
        jne     .bad
        mov     dx, okmsg
        jmp     .out
.bad:   mov     dx, badmsg
.out:   mov     ah, 9
        int     21h
        mov     ax, 4C00h
        int     21h

        align   8
gdt     dq      0
        dq      00CF92000000FFFFh       ; 1: data, base 0, 4 GB
gdtr    dw      15
        dd      0
okmsg   db      'CPL0 unreal ok', 13, 10, '$'
badmsg  db      'unreal FAILED', 13, 10, '$'
