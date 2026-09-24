; rtc.asm — the MC146818 real-time clock behind ports 70h/71h and IRQ 8,
; as a program sees it: the power-on registers; the periodic interrupt at
; 1024 Hz through the program's own INT 70h; no second interrupt for a
; handler that never reads register C; the update-ended interrupt; UIP
; before the second changes; INT 15h AH=83h's flag byte, posted by the
; BIOS's INT 70h; and setting the time through the registers, read back
; by the registers and by INT 1Ah. Timings are checked against the BIOS
; tick (the PIT), so the verdicts come out the same at any speed.
;
;   nasm -f bin -o rtc.com rtc.asm
        cpu 386
        org 0x100

start:
        ; ---- registers A, B, D as POST left them
        mov dx, s_regs
        call puts
        mov al, 0x0A
        call rd
        call hex8
        call space
        mov al, 0x0B
        call rd
        call hex8
        call space
        mov al, 0x0D
        call rd
        call hex8
        call crlf

        ; ---- our own INT 70h
        xor ax, ax
        mov es, ax
        mov eax, [es:0x70*4]
        mov [old70], eax
        cli
        mov word [es:0x70*4], irq8
        mov [es:0x70*4+2], cs
        sti

        ; ---- periodic, 1024 Hz, over 18 ticks (about a second)
        mov dword [count], 0
        mov byte [readc], 1
        mov al, 0x0C
        call rd                         ; clear any flag
        mov ah, 0x26
        mov al, 0x0A
        call wr
        mov al, 0x0B
        call rd
        or al, 0x40                     ; PIE
        mov ah, al
        mov al, 0x0B
        call wr
        mov cx, 18
        call ticks
        call pie_off
        mov eax, [count]                ; 1024 Hz x 18/18.2 s = ~1013
        mov dx, s_pf_ok
        cmp eax, 700
        jb .pfbad
        cmp eax, 1300
        jbe .pfp
.pfbad: mov dx, s_pf_bad
.pfp:   call puts

        ; ---- a handler that never reads C: one interrupt, then none
        mov dword [count], 0
        mov byte [readc], 0
        mov al, 0x0C
        call rd
        mov al, 0x0B
        call rd
        or al, 0x40
        mov ah, al
        mov al, 0x0B
        call wr
        mov cx, 4
        call ticks
        call pie_off
        mov al, 0x0C
        call rd                         ; (let it go)
        mov dx, s_noc
        call puts
        mov eax, [count]
        call hex8
        call crlf

        ; ---- the update-ended interrupt: within 40 ticks (two seconds)
        mov dword [count], 0
        mov byte [readc], 1
        mov al, 0x0C
        call rd
        mov al, 0x0B
        call rd
        or al, 0x10                     ; UIE
        mov ah, al
        mov al, 0x0B
        call wr
        mov cx, 40
.uf:    cmp dword [count], 0
        jne .ufok
        push cx
        mov cx, 1
        call ticks
        pop cx
        loop .uf
        mov dx, s_uf_bad
        jmp .ufp
.ufok:  mov dx, s_uf_ok
.ufp:   call puts
        mov al, 0x0B
        call rd
        and al, ~0x10
        mov ah, al
        mov al, 0x0B
        call wr

        ; ---- UIP: seen, and then the seconds move. Just after that update
        ; the next is a second away: sleep 17 ticks (934 ms), then poll the
        ; rest, so a slow run (-V on the instruction clock) need not spin
        ; through a whole second
        mov cx, 17
        call ticks
        mov al, 0x00
        call rd
        mov [sec0], al
        mov ecx, 50000000
.uip:   mov al, 0x0A
        call rd
        test al, 0x80
        jnz .uipok
        dec ecx
        jnz .uip
        mov dx, s_nouip
        jmp .uipp
.uipok: mov ecx, 50000000
.sec:   mov al, 0x00
        call rd
        cmp al, [sec0]
        jne .secok
        dec ecx
        jnz .sec
        mov dx, s_nosec
        jmp .uipp
.secok: mov dx, s_uip
.uipp:  call puts

        ; ---- back to the BIOS's INT 70h, and INT 15h AH=83h through it
        xor ax, ax
        mov es, ax
        cli
        mov eax, [old70]
        mov [es:0x70*4], eax
        sti
        mov byte [flag83], 0
        push ds
        pop es
        mov bx, flag83
        mov cx, 0x0003                  ; 200000 us: about 3.6 ticks
        mov dx, 0x0D40
        mov ax, 0x8300
        int 0x15
        jc .e83bad
        mov al, [flag83]                ; not yet
        mov [flag83_0], al
        mov cx, 1
        call ticks
        mov al, [flag83]
        mov [flag83_1], al              ; still not, after one tick
        mov cx, 60
.w83:   test byte [flag83], 0x80
        jnz .w83d
        push cx
        mov cx, 1
        call ticks
        pop cx
        loop .w83
.w83d:  mov dx, s_83
        call puts
        mov al, [flag83_0]
        call hex8
        call space
        mov al, [flag83_1]
        call hex8
        call space
        mov al, [flag83]
        call hex8
        call crlf
        jmp .set
.e83bad:
        mov dx, s_83bad
        call puts

.set:   ; ---- set the time through the registers: SET, 12:34:56, clear SET
        mov al, 0x0B
        call rd
        or al, 0x80
        mov ah, al
        mov al, 0x0B
        call wr
        mov ax, 0x5600
        call wr
        mov ax, 0x3402
        call wr
        mov ax, 0x1204
        call wr
        mov al, 0x0B
        call rd
        and al, 0x7F
        mov ah, al
        mov al, 0x0B
        call wr
        mov dx, s_set
        call puts
        mov al, 0x04
        call rd
        call hex8
        mov al, ':'
        call putc
        mov al, 0x02
        call rd
        call hex8
        mov dx, s_1a
        call puts
        mov ah, 0x02
        int 0x1A
        mov al, ch
        call hex8
        mov al, ':'
        call putc
        mov al, cl
        call hex8
        call crlf

        mov ax, 0x4C00
        int 0x21

; our IRQ 8: count; read register C (unless told not to); EOI both
irq8:   push ax
        inc dword [cs:count]
        cmp byte [cs:readc], 0
        je .noc
        mov al, 0x0C
        out 0x70, al
        in al, 0x71
.noc:   mov al, 0x20
        out 0xA0, al
        out 0x20, al
        pop ax
        iret

pie_off:
        mov al, 0x0B
        call rd
        and al, ~0x40
        mov ah, al
        mov al, 0x0B
        jmp wr

; wait CX BIOS ticks (the tick count at 40:6C)
ticks:  push es
        push ax
        push bx
        mov ax, 0x40
        mov es, ax
.t:     mov bx, [es:0x6C]
.same:  sti
        hlt
        cmp bx, [es:0x6C]
        je .same
        loop .t
        pop bx
        pop ax
        pop es
        ret

rd:     out 0x70, al                    ; register AL -> AL
        in al, 0x71
        ret
wr:     out 0x70, al                    ; register AL <- AH
        mov al, ah
        out 0x71, al
        ret

hex8:   push ax
        shr al, 4
        call hex4
        pop ax
hex4:   push ax
        and al, 15
        add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     call putc
        pop ax
        ret
putc:   push ax
        push dx
        mov dl, al
        mov ah, 2
        int 0x21
        pop dx
        pop ax
        ret
space:  push ax
        mov al, ' '
        call putc
        pop ax
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

s_regs  db "registers A B D: $"
s_uip   db "UIP seen, then the second changed", 13, 10, "$"
s_nouip db "UIP never seen", 13, 10, "$"
s_nosec db "the second never changed", 13, 10, "$"
s_pf_ok db "periodic 1024 Hz: about 1000 interrupts a second", 13, 10, "$"
s_pf_bad db "periodic 1024 Hz: wrong count", 13, 10, "$"
s_noc   db "without reading C: interrupts $"
s_uf_ok db "update-ended interrupt: came", 13, 10, "$"
s_uf_bad db "update-ended interrupt: never", 13, 10, "$"
s_83    db "INT 15h AH=83h flag: at once, a tick later, in the end: $"
s_83bad db "INT 15h AH=83h: refused", 13, 10, "$"
s_set   db "set 12:34 through the registers: reads $"
s_1a    db ", INT 1Ah says $"
s_crlf  db 13, 10, "$"
old70   dd 0
count   dd 0
readc   db 0
sec0    db 0
flag83  db 0
flag83_0 db 0
flag83_1 db 0
