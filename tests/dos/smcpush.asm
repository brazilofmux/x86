; smcpush.com — a PUSH that overwrites an already-translated instruction.
; The JIT's push path has no code-bitmap check, so it should miss this.
        cpu 8086
        org 100h
        jmp     start
target: mov     al, 1           ; B0 01 — overwritten below with B0 02
        ret
start:  call    target          ; translate the target block
        mov     sp, target + 2  ; a push lands exactly on "mov al,1"
        mov     bx, 02B0h       ; little-endian: B0 02  = "mov al,2"
        push    bx
        mov     sp, 0FFFEh
        call    target          ; must now yield AL=2
        add     al, '0'
        mov     dl, al
        mov     ah, 2
        int     21h
        mov     ax, 4C00h
        int     21h
