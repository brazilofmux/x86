; exe1.exe — hand-built MZ (nasm -f bin): header with a relocation for the
; data segment, code at load:0, data at load+40h:0, stack at load+80h:0.
; Exercises the loader, relocations, INT 21h files/dirs/memory, INT 1Ah,
; and a JIT-friendly loop.
        cpu     8086
        org     0
DATA_PARA equ 40h
STACK_PARA equ 80h
%define D(x) ((x) - 420h)   ; file offset → load offset (header is 20h)
header:
        db      'MZ'
        dw      (filesize % 512)        ; bytes in last page
        dw      (filesize + 511) / 512  ; pages
        dw      1                       ; relocations
        dw      2                       ; header paragraphs (32 bytes)
        dw      64                      ; min alloc
        dw      0FFFFh                  ; max alloc
        dw      STACK_PARA              ; SS (relocated by loader)
        dw      512                     ; SP
        dw      0                       ; checksum
        dw      0                       ; IP
        dw      0                       ; CS
        dw      1Ch                     ; reloc table offset
        dw      0                       ; overlay
        dw      reloc1 - 20h, 0         ; reloc: offset of the immediate in mov ax, DATA_PARA
        times 20h - ($ - $$) db 0
start:
        mov     ax, DATA_PARA
reloc1  equ     $ - 2
        mov     ds, ax
        ; shrink our block to what we use (PSP:0 .. end of stack)
        mov     bx, STACK_PARA + 32 + 16
        mov     ah, 4Ah
        int     21h
        jc      fail
        ; announce
        mov     ah, 9
        mov     dx, D(banner)
        int     21h
        ; D(ticks)
        mov     ah, 0
        int     1Ah
        mov     [D(ticks)], dx
        ; create a file
        mov     ah, 3Ch
        xor     cx, cx
        mov     dx, D(fname)
        int     21h
        jc      fail
        mov     [D(fh)], ax
        mov     bx, ax
        mov     ah, 40h
        mov     cx, 13
        mov     dx, D(content)
        int     21h
        jc      fail
        mov     bx, [D(fh)]
        mov     ah, 3Eh
        int     21h
        ; read it back
        mov     ax, 3D00h
        mov     dx, D(fname)
        int     21h
        jc      fail
        mov     bx, ax
        mov     ah, 3Fh
        mov     cx, 64
        mov     dx, D(buf)
        int     21h
        jc      fail
        mov     [D(got)], ax
        mov     ah, 3Eh
        int     21h
        ; print it (AX bytes)
        mov     cx, [D(got)]
        mov     si, D(buf)
.pr:    lodsb
        mov     dl, al
        mov     ah, 2
        int     21h
        loop    .pr
        ; findfirst *.TXT
        mov     ah, 4Eh
        xor     cx, cx
        mov     dx, D(spec)
        int     21h
        jc      fail
        mov     ah, 9
        mov     dx, D(found)
        int     21h
        mov     ah, 2Fh
        int     21h                 ; ES:BX = DTA
        lea     si, [bx+1Eh]
.nm:    mov     dl, [es:si]
        or      dl, dl
        jz      .nmd
        mov     ah, 2
        int     21h
        inc     si
        jmp     .nm
.nmd:   mov     ah, 9
        mov     dx, D(crlf)
        int     21h
        ; delete
        mov     ah, 41h
        mov     dx, D(fname)
        int     21h
        jc      fail
        ; alloc 4K, free it
        mov     ah, 48h
        mov     bx, 256
        int     21h
        jc      fail
        mov     es, ax
        mov     ah, 49h
        int     21h
        jc      fail
        ; some arithmetic for the JIT: 100000 iterations
        mov     cx, 0
        xor     ax, ax
        mov     bx, 1
.lp:    add     ax, bx
        adc     dx, 0
        inc     bx
        dec     cx
        jnz     .lp
        push    ax
        mov     ah, 9
        mov     dx, D(sum)
        int     21h
        pop     ax
        call    hex16
        mov     ah, 9
        mov     dx, D(crlf)
        int     21h
        mov     ax, 4C00h
        int     21h
fail:   push    ax
        mov     ah, 9
        mov     dx, D(failed)
        int     21h
        pop     ax
        call    hex16
        mov     ax, 4C01h
        int     21h

; print AX as 4 hex digits
hex16:  mov     cx, 4
.h:     push    cx
        mov     cl, 4
        rol     ax, cl              ; 8086-compatible (C1 /0 is 186+)
        pop     cx
        push    ax
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .d
        add     al, 7
.d:     mov     dl, al
        mov     ah, 2
        int     21h
        pop     ax
        loop    .h
        ret


        times 420h - ($ - $$) db 0
banner  db      'exe1: MZ loaded, data segment relocated', 13, 10, '$'
fname   db      'TEST1.TXT', 0
spec    db      '*.TXT', 0
content db      'file content', 10
found   db      'found: $'
crlf    db      13, 10, '$'
sum     db      'sum: $'
failed  db      'FAILED, error $'
ticks   dw      0
fh      dw      0
got     dw      0
buf     times 64 db 0


        times 820h - ($ - $$) db 0
        resb 512
filesize equ $ - $$
