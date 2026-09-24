; ide.asm — the primary IDE channel as a driver that owns it sees it
; (NT's atdisk, Windows' 32-bit disk access): IDENTIFY; READ and WRITE
; SECTORS in LBA; the controller and INT 13h agreeing about the same
; sectors, each writing what the other reads; a CHS read under a
; translation set by INITIALIZE DEVICE PARAMETERS; the errors (a sector
; past the end, a command it does not know); the missing slave; READ
; MULTIPLE with an interrupt per block through the program's own INT
; 76h; soft reset's signature. Run with -hda on the image run.sh makes:
; 1 MB, every sector starting with its own LBA.
;
;   nasm -f bin -o ide.com ide.asm
        cpu 386
        org 0x100

start:
        push cs
        pop es
        mov dx, 0x3F6                   ; nIEN: polled until the interrupt test
        mov al, 0x02
        out dx, al

        ; ---- IDENTIFY
        mov al, 0xA0
        call devsel
        mov al, 0xEC
        call cmd
        call drq
        mov di, buf
        call insect
        mov dx, s_model
        call puts
        mov si, buf + 27*2
        mov cx, 20
.m:     lodsw
        xchg al, ah
        call putc
        mov al, ah
        call putc
        loop .m
        call crlf
        mov dx, s_geom
        call puts
        mov ax, [buf + 1*2]
        call hex16
        call space
        mov ax, [buf + 3*2]
        call hex16
        call space
        mov ax, [buf + 6*2]
        call hex16
        mov dx, s_lba
        call puts
        mov ax, [buf + 61*2]
        call hex16
        mov ax, [buf + 60*2]
        call hex16
        mov dx, s_mult
        call puts
        mov al, [buf + 47*2]
        call hex8
        call crlf

        ; ---- READ SECTORS, LBA 3, two of them
        mov eax, 3
        mov cl, 2
        call lba_regs
        mov al, 0x20
        call cmd
        mov dx, s_read
        call puts
        mov bx, 2
.rd:    call drq
        mov di, buf
        call insect
        mov eax, [buf]
        call hex32
        call space
        dec bx
        jnz .rd
        call status
        call crlf

        ; ---- WRITE SECTORS LBA 10, then INT 13h reads it
        mov di, buf
        mov cx, 512
        mov al, 0x5A
        rep stosb
        mov dword [buf], 0x31544449      ; "IDT1"
        mov eax, 10
        mov cl, 1
        call lba_regs
        mov al, 0x30
        call cmd
        call drq
        mov si, buf
        mov dx, 0x1F0
        mov cx, 256
        rep outsw
        call wait_ready
        mov eax, 10
        call int13_read
        mov dx, s_w13
        call puts
        call compare
        call crlf

        ; ---- INT 13h writes LBA 11, the controller reads it
        mov di, buf
        mov cx, 512
        mov al, 0xA5
        rep stosb
        mov dword [buf], 0x32544449      ; "IDT2"
        mov si, dap
        mov word [si+2], 1
        mov word [si+4], buf
        mov [si+6], cs
        mov dword [si+8], 11
        mov ah, 0x43
        xor al, al
        mov dl, 0x80
        int 0x13
        mov eax, 11
        mov cl, 1
        call lba_regs
        mov al, 0x20
        call cmd
        call drq
        mov di, buf2
        call insect
        mov dx, s_13w
        call puts
        call compare
        call crlf

        ; ---- CHS: 4 heads of 17 sectors; cylinder 1, head 2, sector 5 = LBA 106
        mov al, 0xA3                    ; heads - 1 in the low nibble
        call devsel
        mov dx, 0x1F2
        mov al, 17
        out dx, al
        mov al, 0x91
        call cmd
        call wait_ready
        mov al, 0xA2                    ; CHS, head 2
        call devsel
        mov dx, 0x1F2
        mov al, 1
        out dx, al
        inc dx
        mov al, 5                       ; sector
        out dx, al
        inc dx
        mov al, 1                       ; cylinder 1
        out dx, al
        inc dx
        xor al, al
        out dx, al
        mov al, 0x20
        call cmd
        call drq
        mov di, buf
        call insect
        mov dx, s_chs
        call puts
        mov eax, [buf]
        call hex32
        call crlf

        ; ---- past the end: IDNF; a command it does not have: ABRT
        mov eax, 2048
        mov cl, 1
        call lba_regs
        mov al, 0x20
        call cmd
        mov dx, s_idnf
        call puts
        call status
        call crlf
        mov al, 0xA1                    ; IDENTIFY PACKET DEVICE: not ATAPI
        call cmd
        mov dx, s_abrt
        call puts
        call status
        call crlf

        ; ---- the missing slave
        mov al, 0xB0
        call devsel
        mov dx, s_slave
        call puts
        mov dx, 0x1F7
        in al, dx
        call hex8
        call crlf
        mov al, 0xA0
        call devsel

        ; ---- READ MULTIPLE (4 a block), 8 sectors from LBA 20, interrupt-driven
        mov dx, 0x1F2
        mov al, 4
        out dx, al
        mov al, 0xC6                    ; SET MULTIPLE MODE
        call cmd
        call wait_ready
        xor ax, ax
        mov es, ax
        mov eax, [es:0x76*4]
        mov [old76], eax
        cli
        mov word [es:0x76*4], irq14
        mov [es:0x76*4+2], cs
        push cs
        pop es
        mov dx, 0x3F6                   ; interrupts on
        xor al, al
        out dx, al
        sti
        mov eax, 20
        mov cl, 8
        call lba_regs
        mov al, 0xC4
        call cmd
        mov dx, s_mul
        call puts
        mov bx, 2
.blk:   mov ecx, 5000000
.w:     cmp byte [irqs_seen], 0
        jne .got
        dec ecx
        jnz .w
        mov dx, s_noirq
        call puts
        jmp .mdone
.got:   dec byte [irqs_seen]
        mov di, buf
        mov dx, 0x1F0
        mov cx, 4*256
        rep insw
        mov eax, [buf]
        call hex32
        call space
        mov eax, [buf + 3*512]
        call hex32
        call space
        dec bx
        jnz .blk
.mdone: mov dx, s_irqs
        call puts
        mov al, [irqs]
        call hex8
        call crlf
        mov dx, 0x3F6
        mov al, 0x02
        out dx, al
        xor ax, ax
        mov es, ax
        cli
        mov eax, [old76]
        mov [es:0x76*4], eax
        sti
        push cs
        pop es

        ; ---- soft reset: the ATA signature
        mov dx, 0x3F6
        mov al, 0x06
        out dx, al
        mov al, 0x02
        out dx, al
        mov dx, s_srst
        call puts
        mov dx, 0x1F1
.sig:   in al, dx
        call hex8
        call space
        inc dx
        cmp dx, 0x1F6
        jb .sig
        mov dx, 0x1F7
        in al, dx
        call hex8
        call crlf

        mov ax, 0x4C00
        int 0x21

; our IRQ 14: note it, read the status (INTRQ off), EOI both
irq14:  push ax
        push dx
        inc byte [cs:irqs]
        inc byte [cs:irqs_seen]
        mov dx, 0x1F7
        in al, dx
        mov al, 0x20
        out 0xA0, al
        out 0x20, al
        pop dx
        pop ax
        iret

devsel: mov dx, 0x1F6
        out dx, al
        ret
cmd:    mov dx, 0x1F7
        out dx, al
        ret
; LBA EAX, count CL, device 0
lba_regs:
        push eax
        mov dx, 0x1F2
        mov al, cl
        out dx, al
        pop eax
        inc dx
        out dx, al                      ; 1F3
        shr eax, 8
        inc dx
        out dx, al                      ; 1F4
        shr eax, 8
        inc dx
        out dx, al                      ; 1F5
        shr eax, 8
        and al, 0x0F
        or al, 0xE0
        inc dx
        out dx, al                      ; 1F6: LBA, master
        ret
drq:    mov dx, 0x1F7
.w:     in al, dx
        test al, 0x80
        jnz .w
        test al, 0x08
        jz .w
        ret
wait_ready:
        mov dx, 0x1F7
.w:     in al, dx
        test al, 0x80
        jnz .w
        ret
insect: mov dx, 0x1F0
        mov cx, 256
        rep insw
        ret
; "status XX error YY"
status: mov dx, s_st
        call puts
        mov dx, 0x1F7
        in al, dx
        call hex8
        mov dx, s_er
        call puts
        mov dx, 0x1F1
        in al, dx
        jmp hex8
; LBA EAX through INT 13h AH=42h into buf2
int13_read:
        mov si, dap
        mov word [si+2], 1
        mov word [si+4], buf2
        mov [si+6], cs
        mov [si+8], eax
        mov ah, 0x42
        mov dl, 0x80
        int 0x13
        ret
compare:
        mov si, buf
        mov di, buf2
        mov cx, 512
        repe cmpsb
        mov dx, s_same
        je .p
        mov dx, s_diff
.p:     jmp puts

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

s_model db "model: $"
s_geom  db "cylinders heads sectors: $"
s_lba   db ", LBA sectors $"
s_mult  db ", multiple up to $"
s_read  db "READ SECTORS at 3, two: $"
s_w13   db "WRITE SECTORS at 10, INT 13h reads: $"
s_13w   db "INT 13h writes 11, the controller reads: $"
s_chs   db "CHS 1/2/5 under 4x17: $"
s_idnf  db "past the end:$"
s_abrt  db "IDENTIFY PACKET:$"
s_slave db "no slave: status $"
s_mul   db "READ MULTIPLE 8 at 20, blocks of 4: $"
s_noirq db "no interrupt $"
s_irqs  db "IRQs $"
s_srst  db "after soft reset, error count sector cyl cyl, status: $"
s_st    db " status $"
s_er    db " error $"
s_same  db "the same$"
s_diff  db "different$"
s_crlf  db 13, 10, "$"
old76   dd 0
irqs    db 0
irqs_seen db 0
        align 4
dap     db 0x10, 0
        dw 0, 0, 0
        dq 0
buf     times 4*512 db 0
buf2    times 512 db 0
