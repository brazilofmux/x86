; cpu486.asm — the 486 against the 386, as CPU-detection code sees them.
;
; EFLAGS.AC (bit 18) can be toggled on a 486 and not on a 386 (except
; that our 386 keeps reserved bits, as the 386EX the SingleStepTests
; suite was measured on does: see x86_flags_fixup); EFLAGS.ID (bit 21)
; only where CPUID exists. Then the 486's additions under a #UD handler
; of our own: BSWAP, XADD, CMPXCHG (both outcomes) and INVD, each with
; its result and the arithmetic flags the SDM defines (OF SF ZF AF PF
; CF, mask 8D5h), or "#UD". Expected outputs (cpu486-386.out,
; cpu486-486.out) are worked from the instruction set reference.
;
;   nasm -f bin -o cpu486.com cpu486.asm
        cpu 586                 ; (NASM files XADD and CMPXCHG under the 586; the 486 has them)
        org 0x100

start:
        ; ---- EFLAGS.AC: 386 or 486
        mov edx, 0x40000
        call toggles
        mov dx, s_ac0
        jz .ac
        mov dx, s_ac1
.ac:    call puts
        ; ---- EFLAGS.ID: CPUID
        mov edx, 0x200000
        call toggles
        mov dx, s_id0
        jz .id
        mov dx, s_id1
.id:    call puts

        ; ---- our #UD handler
        xor ax, ax
        mov es, ax
        mov ax, [es:6*4]
        mov [old6], ax
        mov ax, [es:6*4+2]
        mov [old6+2], ax
        cli
        mov word [es:6*4], ud_handler
        mov [es:6*4+2], cs
        sti

        ; ---- BSWAP EAX (2 bytes)
        mov dx, s_bswap
        call puts
        mov byte [skip], bs_e - bs_s
        mov byte [ud], 0
        mov eax, 0x12345678
bs_s:   bswap eax
bs_e:    cmp byte [ud], 0
        jne .bsud
        call hex32
        call crlf
        jmp .xadd
.bsud:  call ud_line

.xadd:  ; ---- XADD EAX, EBX (3 bytes): EAX = 5 + 7, EBX = 5
        mov dx, s_xadd
        call puts
        mov byte [skip], xa_e - xa_s
        mov byte [ud], 0
        mov eax, 5
        mov ebx, 7
xa_s:   xadd eax, ebx
xa_e:    pushf
        cmp byte [ud], 0
        jne .xaud
        call hex32
        call space
        mov eax, ebx
        call hex32
        call space
        pop ax
        call flags
        call crlf
        jmp .cx1
.xaud:  pop ax
        call ud_line

.cx1:   ; ---- CMPXCHG EBX, ECX (3 bytes), EAX = EBX: store ECX, ZF
        mov dx, s_cx1
        call puts
        mov byte [skip], c1_e - c1_s
        mov byte [ud], 0
        mov eax, 5
        mov ebx, 5
        mov ecx, 9
c1_s:   cmpxchg ebx, ecx
c1_e:    pushf
        cmp byte [ud], 0
        jne .c1ud
        call hex32
        call space
        mov eax, ebx
        call hex32
        call space
        pop ax
        call flags
        call crlf
        jmp .cx2
.c1ud:  pop ax
        call ud_line

.cx2:   ; ---- CMPXCHG EBX, ECX, EAX != EBX: EAX = EBX, flags of 4 - 9
        mov dx, s_cx2
        call puts
        mov byte [skip], c2_e - c2_s
        mov byte [ud], 0
        mov eax, 4
        mov ebx, 9
        mov ecx, 1
c2_s:   cmpxchg ebx, ecx
c2_e:    pushf
        cmp byte [ud], 0
        jne .c2ud
        call hex32
        call space
        mov eax, ebx
        call hex32
        call space
        pop ax
        call flags
        call crlf
        jmp .invd
.c2ud:  pop ax
        call ud_line

.invd:  ; ---- INVD (2 bytes): real mode is CPL 0
        mov dx, s_invd
        call puts
        mov byte [skip], iv_e - iv_s
        mov byte [ud], 0
iv_s:   invd
iv_e:    cmp byte [ud], 0
        jne .inud
        mov dx, s_ok
        call puts
        jmp .done
.inud:  call ud_line

.done:  xor ax, ax
        mov es, ax
        cli
        mov ax, [old6]
        mov [es:6*4], ax
        mov ax, [old6+2]
        mov [es:6*4+2], ax
        sti
        mov ax, 0x4C00
        int 0x21

; ZF clear if the EFLAGS bits in EDX can be flipped; EFLAGS as it was
toggles:
        pushfd
        pop eax
        mov ecx, eax
        xor eax, edx
        push eax
        popfd
        pushfd
        pop eax
        push ecx
        popfd
        xor eax, ecx
        test eax, edx
        ret

; #UD: skip the instruction ([skip] bytes), note it
ud_handler:
        push bp
        mov bp, sp
        push ax
        xor ax, ax
        mov al, [cs:skip]
        add [bp+2], ax
        mov byte [cs:ud], 1
        pop ax
        pop bp
        iret

ud_line:
        mov dx, s_ud
        jmp puts

; AX's low bits: the defined arithmetic flags
flags:
        and ax, 0x8D5
        push ax
        mov al, ah
        call hex4
        pop ax
        push ax
        shr al, 4
        call hex4
        pop ax
        jmp hex4

hex32:  push eax
        shr eax, 16
        call hex16
        pop eax
hex16:  push ax
        mov al, ah
        call hex8
        pop ax
hex8:   push ax
        shr al, 4
        call hex4
        pop ax
hex4:   push ax
        push dx
        and al, 15
        add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     mov dl, al
        mov ah, 2
        int 0x21
        pop dx
        pop ax
        ret

space:  push dx
        mov dx, s_sp
        call puts
        pop dx
        ret
crlf:   push dx
        mov dx, s_crlf
        call puts
        pop dx
        ret
puts:   push ax
        mov ah, 9
        int 0x21
        pop ax
        ret

s_ac0   db "EFLAGS.AC: fixed (386)", 13, 10, "$"
s_ac1   db "EFLAGS.AC: toggles (486)", 13, 10, "$"
s_id0   db "EFLAGS.ID: fixed (no CPUID)", 13, 10, "$"
s_id1   db "EFLAGS.ID: toggles (CPUID)", 13, 10, "$"
s_bswap db "bswap 12345678: $"
s_xadd  db "xadd 5,7 -> eax ebx flags: $"
s_cx1   db "cmpxchg eax=5 ebx=5 ecx=9 -> eax ebx flags: $"
s_cx2   db "cmpxchg eax=4 ebx=9 ecx=1 -> eax ebx flags: $"
s_invd  db "invd: $"
s_ok    db "ok", 13, 10, "$"
s_ud    db "#UD", 13, 10, "$"
s_sp    db " $"
s_crlf  db 13, 10, "$"
skip    db 0
ud      db 0
old6    dd 0
