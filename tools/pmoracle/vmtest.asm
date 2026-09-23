; vmtest.asm — virtual-8086 oracle: a boot image that enters V86 mode by
; IRETD from ring 0, runs a fragment, and reports what came back to the
; ring-0 handlers (vector, error code, the whole V86 frame, EAX) on port
; E9. pgrun.py runs it under QEMU and dos-monster and diffs the two.
;
; Every fragment ends in HLT, which in V86 mode is #GP(0): the handler
; prints the frame and resumes the script. Layout as pgtest.asm: stage 2 at
; 7E00, IDT 6000, TSS 20000 (I/O bitmap at 20068: port 60h denied, 61h
; allowed), ring-0 stack 7000 and 6E00 for the TSS; V86 stack 0000:5000;
; data at 3000:0000; paging tables at 10000 when a case turns it on.
        cpu 386
        org 0x7C00

STAGE2_LMA  equ 0x7E00
STAGE2_SECS equ 10
IDT_BASE    equ 0x6000
TSS_BASE    equ 0x20000            ; with its 8K I/O bitmap, clear of the code
IOPB        equ 0x68
STACK0      equ 0x7000
STACK0T     equ 0x6E00
PD          equ 0x10000
PT0         equ 0x11000
SEL_CODE    equ 0x08
SEL_DATA    equ 0x10
SEL_TSS     equ 0x18
RET_VEC     equ 0x30
FL_VM       equ 0x20000
IOPL3       equ 0x3000

; ---------------------------------------------------------------- stage 1
        bits 16
stage1:
        cli
        cld
        xor ax, ax
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 0x7C00
        mov ah, 0x02
        mov al, STAGE2_SECS
        mov ch, 0
        mov cl, 2
        mov dh, 0
        mov bx, STAGE2_LMA
        int 0x13
        jc  .h
        jmp stage2
.h:     hlt
        jmp .h
        times 510 - ($ - $$) db 0
        dw 0xAA55

; ---------------------------------------------------------------- stage 2
stage2:
        in al, 0x92
        or al, 2
        out 0x92, al
        lgdt [gdtr]
        mov eax, cr0
        or al, 1
        mov cr0, eax
        jmp SEL_CODE:pm32

        bits 32
pm32:
        mov ax, SEL_DATA
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax
        mov ss, ax
        mov esp, STACK0
        mov edi, IDT_BASE
        xor ecx, ecx
.idt:   mov eax, fault_stubs
        mov edx, ecx
        shl edx, 3
        add eax, edx
        mov [edi], ax
        mov word [edi+2], SEL_CODE
        mov word [edi+4], 0x8E00            ; interrupt gate, DPL 0
        shr eax, 16
        mov [edi+6], ax
        add edi, 8
        inc ecx
        cmp ecx, 0x31
        jb .idt
        mov word [IDT_BASE + 3 * 8 + 4], 0xEE00       ; INT3: DPL 3
        mov word [IDT_BASE + RET_VEC * 8 + 4], 0xEE00 ; INT 30h: DPL 3
        lidt [idtr]
        mov dword [TSS_BASE + 4], STACK0T
        mov dword [TSS_BASE + 8], SEL_DATA
        mov word [TSS_BASE + 0x66], IOPB
        mov edi, TSS_BASE + IOPB                      ; I/O bitmap: all allowed...
        mov ecx, 8192 / 4
        xor eax, eax
        rep stosd
        mov byte [TSS_BASE + IOPB + 8192], 0xFF       ; (the terminating byte)
        or byte [TSS_BASE + IOPB + 0x60 / 8], 1       ; ...but port 60h
        mov ax, SEL_TSS
        ltr ax
        mov dword [0x30010], 0x5A5A1234
        mov dword [0x31000], 0x55667788
        mov dword [0x32000], 0x99AABBCC
        mov esi, msg_start
        call puts

%macro V86 3                                        ; test number, fragment, extra EFLAGS
        mov byte [tno], %1
        mov ebx, %2
        mov edx, %3
        call v86
%endmacro
        V86 1,  f_segs,    IOPL3
        V86 2,  f_read,    IOPL3
        V86 3,  f_cli,     0
        V86 4,  f_pushf,   0
        V86 5,  f_popf,    0
        V86 6,  f_intn,    0
        V86 7,  f_iret,    0
        V86 8,  f_int3,    0
        V86 9,  f_cli,     IOPL3
        V86 10, f_pushf3,  IOPL3
        V86 11, f_in61,    0
        V86 12, f_in60,    0
        V86 13, f_in60,    IOPL3
        V86 14, f_lar,     IOPL3
        V86 15, f_div0,    IOPL3
        V86 16, f_farjmp,  IOPL3
        V86 17, f_popfiopl, IOPL3
        V86 18, f_iret3,   IOPL3
        V86 19, f_intn,    IOPL3
        V86 20, f_movcr,   IOPL3
        ; paging on: identity, user; 31000 supervisor, 32000 read-only
        mov edi, PD
        mov ecx, 1024
        xor eax, eax
        rep stosd
        mov dword [PD], PT0 | 7
        mov edi, PT0
        mov eax, 7
        mov ecx, 1024
.pt:    stosd
        add eax, 0x1000
        loop .pt
        mov dword [PT0 + 0x31 * 4], 0x31000 | 3
        mov dword [PT0 + 0x32 * 4], 0x32000 | 5
        mov dword [PT0 + 0x40 * 4], 0x34000 | 7         ; linear 40000 -> 34000 (as JEMM maps UMBs)
        mov dword [0x34010], 0x11112222
        mov eax, PD
        mov cr3, eax
        mov eax, cr0
        or eax, 0x80000000
        mov cr0, eax
        V86 32, f_read,    IOPL3
        V86 33, f_pgsuper, IOPL3
        V86 34, f_pgro,    IOPL3
        V86 35, f_pgroread, IOPL3
        V86 36, f_segload, IOPL3
        V86 37, f_remap,   IOPL3

        mov esi, msg_done
        call puts
        mov al, 0
        out 0xF4, al
.h:     hlt
        jmp .h

; v86: enter virtual-8086 mode at 0000:EBX with EFLAGS = VM | EDX | 2,
; SS:SP 0000:5000, DS 3000, ES 3100, FS 3200, GS 3300. Comes back through
; a handler, which resumes at .back with this stack.
v86:
        mov [saved_esp], esp
        mov dword [resume], .back
        push dword 0x3300                   ; GS
        push dword 0x3200                   ; FS
        push dword 0x3000                   ; DS
        push dword 0x3100                   ; ES
        push dword 0                        ; SS
        push dword 0x5000                   ; ESP
        or edx, FL_VM | 2
        push edx                            ; EFLAGS
        push dword 0                        ; CS
        push ebx                            ; EIP
        mov eax, 0x0BADF00D
        iretd
.back:  ret

; ---- V86 fragments (16-bit code, CS = 0)
        bits 16
f_segs:     mov ax, cs
            shl eax, 16
            mov ax, ds
            hlt
f_read:     mov eax, [0x0010]               ; 3000:0010
            hlt
f_cli:      cli
            mov eax, 0x11
            hlt
f_pushf:    pushf
            hlt
f_popf:     push word 0
            popf
            hlt
f_intn:     int 0x30
            hlt
f_iret:     iret
            hlt
f_int3:     int3
            hlt
f_pushf3:   pushf
            pop ax
            and ax, 0x0FD5                  ; the bits both machines agree on
            hlt
f_in61:     in al, 0x61
            mov eax, 0x61
            hlt
f_in60:     in al, 0x60
            mov eax, 0x60
            hlt
f_lar:      lar ax, bx
            hlt
f_div0:     xor cx, cx
            mov ax, 5
            div cl
            hlt
f_farjmp:   jmp 0x0700:(f_far2 - 0x7000)
f_far2:     mov ax, cs
            hlt
f_popfiopl: push word 0x0202                ; IOPL 0 in the image
            popf
            pushf
            pop ax
            and ax, 0x3200                  ; IOPL and IF
            hlt
f_iret3:    push word 0x0202
            push word 0
            push word f_iret3b
            iret
f_iret3b:   pushf
            pop ax
            and ax, 0x3200
            hlt
f_movcr:    mov eax, cr0
            hlt
f_pgsuper:  mov ax, 0x3100
            mov ds, ax
            mov eax, [0]                    ; linear 31000: supervisor page
            hlt
f_pgro:     mov ax, 0x3200
            mov ds, ax
            mov dword [0], 1                ; linear 32000: read-only to the user
            hlt
f_pgroread: mov ax, 0x3200
            mov ds, ax
            mov eax, [0]
            hlt
f_segload:  mov bx, 3                       ; the FreeDOS pattern, a few times round
.again:     call .cmp
            dec bx
            jnz .again
            mov ax, ds
            shl eax, 16
            mov ax, es
            hlt
.cmp:       push ds
            push es
            push ax
            mov ds, [cs:seg_pair]
            mov es, [cs:seg_pair + 2]
            mov ax, [ds:0x10]
            cmp ax, [es:0x10]
            jnz .out
            mov ax, [ds:0x12]
            cmp ax, [es:0x12]
.out:       pop ax
            pop es
            pop ds
            ret
seg_pair:   dw 0x3000, 0x3001
f_remap:    mov ax, 0x4000                  ; stack and data in the remapped page
            mov ss, ax
            mov sp, 0x0200
            mov ds, ax
            mov bx, 3
.again:     call f_segload.cmp
            push word [0x10]
            pop cx
            dec bx
            jnz .again
            mov ax, [0x10]
            shl eax, 16
            mov ax, cx
            hlt
        bits 32

; fault stubs: push the vector, go to fault
fault_stubs:
%assign v 0
%rep 0x31
        push dword v
        jmp near fault
        nop
%assign v v+1
%endrep

; fault: "Tnn v=VV [e=EEEE] cs:ip=CCCC:IIII fl=FFFFFFFF ss:sp=SSSS:PPPP
;         es=.. ds=.. fs=.. gs=.. eax=XXXXXXXX", then resume
fault:
        mov [ss:saved_eax], eax             ; DS is null when the fault came from V86
        mov ax, SEL_DATA
        mov ds, ax
        mov es, ax
        pop ecx                             ; vector
        mov esi, msg_t
        call puts
        movzx eax, byte [tno]
        call hex8
        mov esi, msg_v
        call puts
        mov eax, ecx
        call hex8
        cmp ecx, 8
        je .err
        cmp ecx, 10
        jb .noerr
        cmp ecx, 14
        ja .noerr
.err:   mov esi, msg_e
        call puts
        pop eax
        call hex16
.noerr: mov esi, msg_csip
        call puts
        mov eax, [esp + 4]                  ; CS
        call hex16
        mov al, ':'
        out 0xE9, al
        mov eax, [esp]                      ; EIP
        call hex16
        mov esi, msg_fl
        call puts
        mov eax, [esp + 8]
        call hex32
        test dword [esp + 8], FL_VM
        jz .nov86
        mov esi, msg_sssp
        call puts
        mov eax, [esp + 16]
        call hex16
        mov al, ':'
        out 0xE9, al
        mov eax, [esp + 12]
        call hex16
        mov esi, msg_es
        call puts
        mov eax, [esp + 20]
        call hex16
        mov esi, msg_ds
        call puts
        mov eax, [esp + 24]
        call hex16
        mov esi, msg_fs
        call puts
        mov eax, [esp + 28]
        call hex16
        mov esi, msg_gs
        call puts
        mov eax, [esp + 32]
        call hex16
.nov86: mov esi, msg_eax
        call puts
        mov eax, [saved_eax]
        call hex32
        mov al, 10
        out 0xE9, al
        mov esp, [saved_esp]
        jmp [resume]

puts:   lodsb
        test al, al
        jz .d
        out 0xE9, al
        jmp puts
.d:     ret
hex32:  push eax
        shr eax, 16
        call hex16
        pop eax
hex16:  push eax
        shr eax, 8
        call hex8
        pop eax
hex8:   push eax
        shr al, 4
        call .n
        pop eax
.n:     and al, 15
        add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     out 0xE9, al
        ret

msg_start db "vmtest", 10, 0
msg_done  db "done", 10, 0
msg_t     db "T", 0
msg_v     db " v=", 0
msg_e     db " e=", 0
msg_csip  db " cs:ip=", 0
msg_fl    db " fl=", 0
msg_sssp  db " ss:sp=", 0
msg_es    db " es=", 0
msg_ds    db " ds=", 0
msg_fs    db " fs=", 0
msg_gs    db " gs=", 0
msg_eax   db " eax=", 0
tno       db 0
          align 4
resume    dd 0
saved_esp dd 0
saved_eax dd 0

gdt:    dq 0
        dq 0x00CF9A000000FFFF       ; 08 code, ring 0
        dq 0x00CF92000000FFFF       ; 10 data, ring 0
        dw 0x2069, TSS_BASE & 0xFFFF, 0x8900 | (TSS_BASE >> 16), 0 ; 18 386 TSS, limit 68h + the I/O bitmap
gdt_end:
gdtr:   dw gdt_end - gdt - 1
        dd gdt
idtr:   dw 0x31 * 8 - 1
        dd IDT_BASE

        times (STAGE2_SECS + 1) * 512 - ($ - $$) db 0
