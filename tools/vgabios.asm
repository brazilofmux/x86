; vgabios.asm — the native half of INT 10h AH=00 (pc_video.c), at
; PC_STUB_SEG:PC_STUB_VGAPROG. The host sets the mode up and then continues
; here, with the INT's frame still on the stack, so that the mode's
; registers are programmed the way a VGA BIOS does it: by OUT instructions
; the CPU executes. Under a V86 monitor that virtualizes the VGA (Windows'
; VDD in 386 enhanced mode) those are trapped and recorded; the host
; writing the registers itself went unseen, and the VDD's copy kept the
; previous mode's.
;
; ROM variables (same segment): [F0h] the mode's parameter table (misc,
; sequencer 1-4, CRTC 0-18h, graphics controller 0-8, attribute
; controller 0-14h), [F2h] the DAC data, [F4h] its length in bytes.
;
; Assembled bytes are in pc/pc_video.c (vgaprog[]): nasm -f bin -l ...
        bits 16
        org 0x100
vga_prog:
        push ax
        push bx
        push cx
        push dx
        push si
        push ds
        push cs
        pop ds
        cld
        mov si, [0xF0]
        mov dx, 0x3C4
        mov ax, 0x0100
        out dx, ax              ; sequencer: synchronous reset
        mov dx, 0x3C2
        lodsb
        out dx, al              ; miscellaneous output
        mov dx, 0x3C4
        mov bl, 1
.seq:   mov al, bl
        mov ah, [si]
        inc si
        out dx, ax
        inc bl
        cmp bl, 5
        jb .seq
        mov ax, 0x0300
        out dx, ax              ; end of reset
        mov dx, 0x3D4
        mov ax, 0x0011
        out dx, ax              ; CRTC 0-7 writable
        xor bl, bl
.crtc:  mov al, bl
        mov ah, [si]
        inc si
        out dx, ax
        inc bl
        cmp bl, 25
        jb .crtc
        mov dx, 0x3CE
        xor bl, bl
.gc:    mov al, bl
        mov ah, [si]
        inc si
        out dx, ax
        inc bl
        cmp bl, 9
        jb .gc
        mov dx, 0x3DA
        in al, dx               ; attribute flip-flop to "index"
        mov dx, 0x3C0
        xor bl, bl
.ac:    mov al, bl
        out dx, al
        lodsb
        out dx, al
        inc bl
        cmp bl, 21
        jb .ac
        mov al, 0x20
        out dx, al              ; palette address source: display on
        mov si, [0xF2]
        mov cx, [0xF4]
        mov dx, 0x3C8
        xor al, al
        out dx, al
        inc dx
.dac:   lodsb
        out dx, al
        loop .dac
        pop ds
        pop si
        pop dx
        pop cx
        pop bx
        pop ax
        iret
