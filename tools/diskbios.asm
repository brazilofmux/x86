; diskbios.asm — the fixed-disk half of INT 13h, in real instructions, at
; PC_STUB_SEG:PC_STUB_DISKBIOS (F100:1000). An AT's BIOS reaches its hard
; disks by programming the controller; so does this one, so that whatever
; watches the controller — a V86 monitor trapping its ports, Windows'
; WDCTRL checking that the BIOS drives it the way it will — sees what it
; would on the machine. Disks 80h/81h, functions 00h, 02h-04h, 0Ch, 0Dh,
; 10h, 11h and the extensions 42h-44h, 47h; everything else (the
; parameter and type queries, which never touch the controller) and every
; diskette call go on to the host's INT 13h at F000:0013 with the frame
; as it came.
;
; It is the IBM AT's sequence: the caller's CHS, checked against the
; BIOS's geometry (the fixed-disk parameter tables INT 41h/46h point at),
; into the task file as CHS — the drive's translation is that geometry
; (INITIALIZE DEVICE PARAMETERS at reset) — then the command; INT 15h
; AX=9000h and a wait for IRQ 14 (INT 76h sets 40:8Eh) before each
; sector read and after each sector written, the status register read
; after each interrupt (INTRQ drops), 256 words by REP INSW/OUTSW. LBA
; only where the AT had nothing: the extensions, and geometries past the
; controller's 16 heads.
; Errors: the error register to the BIOS's codes, AL the sectors done,
; the status at 40:74 as ever. Reset (00h/0Dh) is a soft reset of the
; channel and INITIALIZE DEVICE PARAMETERS back to the BIOS's geometry.
;
; pc/pc_diskbios.h holds the assembled bytes (make pc/pc_diskbios.h).
        bits 16
        cpu 286
        org 0x1000

; the frame every native function builds (FRAME): locals below BP
%define UNIT    byte [bp-2]         ; 0 or 1
%define CHSMODE byte [bp-1]         ; 1: the controller addressed in CHS
%define OP      byte [bp-4]         ; 0 read, 1 write, 2 verify, 3 seek
%define COUNT   word [bp-6]         ; sectors this command
%define DONE    word [bp-8]         ; ... moved
%define LBALO   word [bp-10]
%define LBAHI   word [bp-12]
%define BUFSEG  word [bp-14]
%define BUFOFF  word [bp-16]
%define STATUS  byte [bp-18]        ; the BIOS status to return
%define CALLAX  word [bp-20]        ; the caller's AX (AL returned from here)
%define SECTOR  word [bp-22]        ; CHS pieces; the extensions reuse these two
%define HEAD    word [bp-24]
%define CYL     word [bp-26]
%define HEADS   word [bp-28]        ; the unit's BIOS geometry
%define SPT     word [bp-30]
%define FLAGS   word [bp+6]         ; the INT frame's flags

%macro FRAME 0
        push bp
        mov bp, sp
        sub sp, 30
        push bx
        push cx
        push dx
        push si
        push di
        push ds
        push es
        mov CALLAX, ax
        mov al, dl
        and al, 1
        mov UNIT, al
        mov CHSMODE, 0
        mov STATUS, 0
%endmacro

int13:  cmp dl, 0x80
        jb hle
        cmp dl, 0x81
        ja hle
        push ds
        push ax
        mov ax, 0x40
        mov ds, ax
        mov al, dl
        and al, 1
        cmp al, [0x75]                  ; a unit the BIOS has?
        pop ax
        pop ds
        jae hle
        cmp ah, 0x02
        je f_read
        cmp ah, 0x03
        je f_write
        cmp ah, 0x04
        je f_verify
        cmp ah, 0x00
        je f_reset
        cmp ah, 0x0D
        je f_reset
        cmp ah, 0x0C
        je f_seek
        cmp ah, 0x10
        je f_ready
        cmp ah, 0x11
        je f_recal
        cmp ah, 0x42
        je f_xread
        cmp ah, 0x43
        je f_xwrite
        cmp ah, 0x44
        je f_xverify
        cmp ah, 0x47
        je f_xseek
hle:    jmp 0xF000:0x0013

; ---- CHS: read, write, verify, seek ---------------------------------------
f_read: FRAME
        mov OP, 0
        jmp chs_common
f_write:
        FRAME
        mov OP, 1
        jmp chs_common
f_verify:
        FRAME
        mov OP, 2
        jmp chs_common
f_seek: FRAME
        mov OP, 3
        mov byte [bp-20], 1             ; (one sector's worth of task file)
chs_common:
        mov BUFOFF, bx
        mov BUFSEG, es
        mov al, [bp-20]
        xor ah, ah
        test ax, ax
        jz .badcmd
        mov COUNT, ax
        call chs_lba
        jc .notfound
        cmp HEADS, 16                   ; the WD1003's 16 heads: CHS, as the AT BIOS
        ja .x
        mov CHSMODE, 1
.x:     call xfer
        mov al, [bp-8]
        mov [bp-20], al                 ; AL: sectors moved
        jmp leave_fn
.notfound:
        mov STATUS, 0x04
        mov byte [bp-20], 0
        jmp leave_fn
.badcmd:
        mov STATUS, 0x01
        jmp leave_fn

; CX/DH through the unit's BIOS geometry to LBALO/LBAHI; CF if it names
; no sector
chs_lba:
        mov al, cl
        and al, 0x3F
        xor ah, ah
        mov SECTOR, ax
        mov al, dh
        mov HEAD, ax
        mov al, ch
        mov ah, cl
        shr ah, 6
        mov CYL, ax
        push ds
        call fdpt                       ; DS:BX = the parameter table
        mov al, [bx+2]
        xor ah, ah
        mov HEADS, ax
        mov al, [bx+14]
        mov SPT, ax
        mov ax, [bx]
        pop ds
        cmp CYL, ax
        jae .bad
        mov ax, SECTOR
        test ax, ax
        jz .bad
        cmp ax, SPT
        ja .bad
        mov ax, HEAD
        cmp ax, HEADS
        jae .bad
        mov ax, CYL                     ; (cyl x heads + head) x spt + sector - 1
        mul HEADS
        add ax, HEAD
        adc dx, 0
        mov bx, ax
        mov ax, dx
        mul SPT
        mov cx, ax
        mov ax, bx
        mul SPT
        add dx, cx
        mov cx, SECTOR
        dec cx
        add ax, cx
        adc dx, 0
        mov LBALO, ax
        mov LBAHI, dx
        clc
        ret
.bad:   stc
        ret

; DS:BX = UNIT's fixed-disk parameter table (INT 41h or INT 46h)
fdpt:   xor bx, bx
        mov ds, bx
        mov bx, 0x41*4
        cmp UNIT, 0
        je .t
        mov bx, 0x46*4
.t:     lds bx, [bx]
        ret

; ---- the transfer ---------------------------------------------------------
; OP on COUNT sectors at LBAHI:LBALO, BUFSEG:BUFOFF; DONE, STATUS
xfer:   mov DONE, 0
        mov ax, BUFOFF                  ; the buffer normalised: whole sectors fit
        shr ax, 4
        add BUFSEG, ax
        and BUFOFF, 0x000F
        call wait_idle
        jc .ret
        push ds                         ; the control byte, as the AT BIOS sends it
        call fdpt                       ; each command: IRQ 14 on (nIEN clear),
        mov al, [bx+8]                  ; bit 3 for more than 8 heads
        pop ds
        and al, 0x08
        mov dx, 0x3F6
        out dx, al
        cmp CHSMODE, 0
        je .lba
        mov dx, 0x1F6                   ; CHS through the drive's translation: the AT's way
        mov al, [bp-24]                 ; head
        and al, 0x0F
        or al, 0xA0
        mov ah, UNIT
        shl ah, 4
        or al, ah
        out dx, al
        mov dx, 0x1F2
        mov al, [bp-6]
        out dx, al
        inc dx
        mov al, [bp-22]                 ; sector
        out dx, al
        inc dx
        mov al, [bp-26]                 ; cylinder
        out dx, al
        inc dx
        mov al, [bp-25]
        out dx, al
        jmp .cmd
.lba:   mov dx, 0x1F6                   ; LBA (the extensions, and geometries past 16 heads)
        mov al, [bp-11]
        and al, 0x0F
        or al, 0xE0
        mov ah, UNIT
        shl ah, 4
        or al, ah
        out dx, al
        mov dx, 0x1F2
        mov al, [bp-6]                  ; (256 goes out as 0)
        out dx, al
        inc dx
        mov al, [bp-10]
        out dx, al
        inc dx
        mov al, [bp-9]
        out dx, al
        inc dx
        mov al, [bp-12]
        out dx, al
.cmd:   push ds                         ; no interrupt seen yet
        mov ax, 0x40
        mov ds, ax
        mov byte [0x8E], 0
        pop ds
        mov bl, OP
        mov al, 0x20                    ; READ SECTORS
        cmp bl, 1
        jne .c1
        mov al, 0x30                    ; WRITE SECTORS
.c1:    cmp bl, 2
        jne .c2
        mov al, 0x40                    ; READ VERIFY SECTORS
.c2:    cmp bl, 3
        jne .c3
        mov al, 0x70                    ; SEEK
.c3:    mov dx, 0x1F7
        out dx, al
        cld
        cmp bl, 1
        je .write
        cmp bl, 2
        jae .nodata
.rsec:  call wait_int                   ; a sector ready: its interrupt, then the status
        jc .ret
        test al, 0x08
        jz .nodrq
        mov es, BUFSEG
        mov di, BUFOFF
        mov cx, 256
        mov dx, 0x1F0
        rep insw
        add BUFSEG, 0x20                ; 512 bytes on
        inc DONE
        mov ax, DONE
        cmp ax, COUNT
        jb .rsec
        mov dx, 0x1F7                   ; after the last, no interrupt: the status
        in al, dx
        test al, 0x01
        jnz .err
        mov STATUS, 0
.ret:   ret
.write: call wait_drq                   ; the first sector: DRQ, no interrupt
        jc .ret
.wsec:  push ds
        mov si, BUFOFF
        mov ds, BUFSEG
        mov cx, 256
        mov dx, 0x1F0
        rep outsw
        pop ds
        add BUFSEG, 0x20
        call wait_int                   ; taken: its interrupt
        jc .ret
        inc DONE
        mov cx, DONE
        cmp cx, COUNT
        jae .wdone
        test al, 0x08
        jnz .wsec
.nodrq: mov STATUS, 0x20                ; the controller stopped short
        ret
.wdone: mov STATUS, 0
        ret
.nodata:
        call wait_int                   ; verify, seek: the interrupt at the end
        jc .ret
        mov ax, COUNT
        mov DONE, ax
        mov STATUS, 0
        ret
.err:   jmp err_status

; the drive's interrupt, as the AT BIOS waits for it: INT 15h AX=9000h
; (the disk is busy: a multitasker may run something else), then the
; flag INT 76h sets at 40:8Eh; then the status register itself (INTRQ
; drops), in AL; CF with the error's status, or a timeout's (80h)
wait_int:
        sti
        mov ax, 0x9000
        int 0x15
        push ds
        mov ax, 0x40
        mov ds, ax
        mov bx, 64
.o:     xor cx, cx
.l:     cmp byte [0x8E], 0
        jne .got
        loop .l
        dec bx
        jnz .o
        pop ds
        mov STATUS, 0x80
        stc
        ret
.got:   mov byte [0x8E], 0
        pop ds
        mov dx, 0x1F7
        in al, dx
        test al, 0x01
        jnz err_status
        clc
        ret

; the channel not busy (before a command); CF, status 80h, on a timeout
wait_idle:
        xor cx, cx
        mov dx, 0x3F6
.l:     in al, dx
        test al, 0x80
        jz .ok
        loop .l
        mov STATUS, 0x80
        stc
        ret
.ok:    clc
        ret
; data ready: CF with the error's status, or a timeout's
wait_drq:
        xor cx, cx
        mov dx, 0x3F6
.l:     in al, dx
        test al, 0x80
        jnz .n
        test al, 0x01
        jnz err_status
        test al, 0x08
        jnz .ok
.n:     loop .l
        mov STATUS, 0x80
        stc
        ret
.ok:    clc
        ret
; the command done: not busy, then the status register itself (INTRQ
; drops); CF with the error's status
wait_done:
        xor cx, cx
        mov dx, 0x3F6
.l:     in al, dx
        test al, 0x80
        jz .r
        loop .l
        mov STATUS, 0x80
        stc
        ret
.r:     mov dx, 0x1F7
        in al, dx
        test al, 0x01
        jnz err_status
        clc
        ret
; the error register as the BIOS reports it
err_status:
        mov dx, 0x1F1
        in al, dx
        mov ah, 0x04                    ; ID not found: sector not found
        test al, 0x10
        jnz .s
        mov ah, 0x10                    ; uncorrectable: bad ECC
        test al, 0x40
        jnz .s
        mov ah, 0x0A                    ; bad block
        test al, 0x80
        jnz .s
        mov ah, 0x02                    ; address mark not found
        test al, 0x01
        jnz .s
        mov ah, 0x01                    ; aborted: bad command
        test al, 0x04
        jnz .s
        mov ah, 0x20                    ; the controller failed
.s:     mov STATUS, ah
        mov dx, 0x1F7
        in al, dx
        stc
        ret

; ---- reset, ready, recalibrate --------------------------------------------
f_reset:
        FRAME
        mov dx, 0x3F6
        mov al, 0x04                    ; SRST
        out dx, al
        xor al, al                      ; released, interrupts on
        out dx, al
        call wait_idle
        jc .done
        mov UNIT, 0
        call init_params
        jc .done
        push ds
        mov ax, 0x40
        mov ds, ax
        cmp byte [0x75], 2
        pop ds
        jb .ok
        mov UNIT, 1
        call init_params
        jc .done
.ok:    mov STATUS, 0
.done:  jmp leave_fn

; INITIALIZE DEVICE PARAMETERS: UNIT back to the BIOS's heads and sectors
; (where they fit the command: up to 16 heads)
init_params:
        push ds
        call fdpt
        mov cl, [bx+2]
        mov ch, [bx+14]
        pop ds
        cmp cl, 16
        ja .skip
        mov dx, 0x1F6
        mov al, cl
        dec al
        or al, 0xA0
        mov ah, UNIT
        shl ah, 4
        or al, ah
        out dx, al
        mov dx, 0x1F2
        mov al, ch
        out dx, al
        mov dx, 0x1F7
        mov al, 0x91
        out dx, al
        jmp wait_done
.skip:  clc
        ret

f_ready:
        FRAME
        mov dx, 0x1F6
        mov al, UNIT
        shl al, 4
        or al, 0xA0
        out dx, al
        mov dx, 0x1F7
        in al, dx
        test al, 0x40                   ; DRDY
        jnz .ok
        mov STATUS, 0xAA                ; not ready
.ok:    jmp leave_fn

f_recal:
        FRAME
        call wait_idle
        jc .done
        mov dx, 0x1F6
        mov al, UNIT
        shl al, 4
        or al, 0xA0
        out dx, al
        mov dx, 0x1F7
        mov al, 0x10                    ; RECALIBRATE
        out dx, al
        call wait_done
        jc .done
        mov STATUS, 0
.done:  jmp leave_fn

; ---- the extensions: the disk address packet at DS:SI ----------------------
f_xread:
        FRAME
        mov OP, 0
        jmp ext_common
f_xwrite:
        FRAME
        mov OP, 1
        jmp ext_common
f_xverify:
        FRAME
        mov OP, 2
        jmp ext_common
f_xseek:
        FRAME
        mov OP, 3
ext_common:
        mov ax, [si+4]
        mov BUFOFF, ax
        mov ax, [si+6]
        mov BUFSEG, ax
        mov ax, [si+8]
        mov LBALO, ax
        mov ax, [si+10]
        mov LBAHI, ax
        mov ax, [si+2]
        cmp OP, 3
        jne .c
        mov ax, 1
.c:     mov SECTOR, ax                  ; (the extensions: sectors left)
        mov HEAD, 0                     ; ... and moved
        cmp word [si+12], 0             ; past 28 bits: not this controller's
        jne .bad
        cmp word [si+14], 0
        jne .bad
        test byte [bp-11], 0xF0
        jnz .bad
.chunk: mov ax, SECTOR
        test ax, ax
        jz .ok
        cmp ax, 128
        jbe .n
        mov ax, 128
.n:     mov COUNT, ax
        push si
        call xfer
        pop si
        mov ax, DONE
        add HEAD, ax
        sub SECTOR, ax
        add LBALO, ax
        adc LBAHI, 0
        cmp STATUS, 0
        jne .end
        jmp .chunk
.ok:    mov STATUS, 0
.end:   cmp OP, 3
        je .x
        mov ax, HEAD
        mov [si+2], ax                  ; blocks transferred
.x:     jmp leave_fn
.bad:   mov STATUS, 0x01
        mov word [si+2], 0
        jmp leave_fn

; ---- return: status at 40:74 and in AH, AL from CALLAX's low byte, CF in
; the frame's flags ---------------------------------------------------------
leave_fn:
        mov ax, 0x40
        mov ds, ax
        mov al, STATUS
        mov [0x74], al
        mov ah, al
        mov al, [bp-20]
        pop es
        pop ds
        pop di
        pop si
        pop dx
        pop cx
        pop bx
        and FLAGS, 0xFFFE
        test ah, ah
        jz .ok
        or FLAGS, 1
.ok:    mov sp, bp
        pop bp
        iret
