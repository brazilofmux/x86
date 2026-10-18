; c686test.asm — the Pentium Pro's additions, as a transcript image
; (c586test.asm's frame; run by pgrun.py --m686 against QEMU's pentium2,
; or Bochs's p2_klamath — neither has a Pentium Pro, and a Pentium II is
; the same here but for CPUID's model and its MMX and SEP bits): CPUID;
; CMOVcc on every condition over eight flag states, its 16-bit form, a
; memory source taken and not taken, and a not-taken one on a page not
; present (still #PF); FCMOVcc, all eight, both ways, and on an empty
; register; FCOMI, FUCOMI, FCOMIP, FUCOMIP: less, greater, equal, a quiet
; and a signalling NaN, an empty register, an unmasked invalid; global
; pages (CR4.PGE): kept through a CR3 load, dropped by INVLPG and by
; toggling PGE, a page without G dropped; RDPMC at ring 0, at ring 3
; without and with CR4.PCE, on a counter that does not exist; the P5's
; counter MSR (gone on a P6); UD2.
;
; Layout as c586test.asm; linear 5A0000 and 5A1000 are pages of PT1
; pointed at 34000 (DEADBEEF at 34010) and 36000 (FEEDFACE at 36010) by
; the tests themselves.
        cpu 686
        org 0x7C00

STAGE2_LMA  equ 0x7E00
STAGE2_SECS equ 16
IDT_BASE    equ 0x6000
TSS_BASE    equ 0x6800
STACK0      equ 0x7000
STACK3      equ 0x5000
STACK0T     equ 0x6E00
PD          equ 0x10000
PT0         equ 0x11000
PT1         equ 0x12000
SEL_CODE    equ 0x08
SEL_DATA    equ 0x10
SEL_CODE3   equ 0x18 | 3
SEL_DATA3   equ 0x20 | 3
SEL_TSS     equ 0x28
RET_VEC     equ 0x30
TRAP_VEC    equ 0x32                ; a 32-bit trap gate: IF stays as it was
INTR_VEC    equ 0x33                ; a 32-bit interrupt gate: IF cleared
CR4_TSD     equ 0x04
CR4_PSE     equ 0x10
CR4_PGE     equ 0x80
CR4_PCE     equ 0x100

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
        mov eax, cr0                ; ET as the machine came up
        mov [cr0_boot], eax
        mov al, 0xFF                ; every IRQ masked at the PIC: ring 3 runs with IF
        out 0x21, al                ; set, and a timer tick there reads as vector 8 (a
        out 0xA1, al                ; slow -V run on the host clock took one in T23)
        in al, 0x92                 ; A20 on
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
        mov word [edi+4], 0x8E00
        shr eax, 16
        mov [edi+6], ax
        add edi, 8
        inc ecx
        cmp ecx, 32
        jb .idt
        mov edi, IDT_BASE + RET_VEC * 8
        mov eax, ret_trap
        mov [edi], ax
        mov word [edi+2], SEL_CODE
        mov word [edi+4], 0xEE00
        shr eax, 16
        mov [edi+6], ax
        mov edi, IDT_BASE + TRAP_VEC * 8
        mov eax, if_probe
        mov [edi], ax
        mov word [edi+2], SEL_CODE
        mov word [edi+4], 0x8F00
        shr eax, 16
        mov [edi+6], ax
        mov edi, IDT_BASE + INTR_VEC * 8
        mov eax, if_probe
        mov [edi], ax
        mov word [edi+2], SEL_CODE
        mov word [edi+4], 0x8E00
        shr eax, 16
        mov [edi+6], ax
        lidt [idtr]
        mov dword [TSS_BASE + 4], STACK0T
        mov dword [TSS_BASE + 8], SEL_DATA
        mov word [TSS_BASE + 0x66], 0x68
        mov ax, SEL_TSS
        ltr ax
        mov dword [0x30000], 0x11223344
        mov dword [0x30004], 0x55667788
        mov dword [0x30020], 5
        mov dword [0x32000], 0x99AABBCC
        mov dword [0x34010], 0xDEADBEEF
        mov dword [0x36010], 0xFEEDFACE
        mov dword [0x30FFC], 0xAAAAAAAA     ; a CMPXCHG8B across into a read-only page
        mov dword [0x31000], 0xBBBBBBBB
        mov dword [0x400000], 0             ; (as a page table, with PSE off: nothing present)
        mov dword [0x400010], 0xCAFEBABE    ; in the 4 MB page at 400000
        mov dword [0x5FF000], 0x13579BDF
        mov edi, PD
        mov ecx, 1024
        xor eax, eax
        rep stosd
        mov dword [PD + 0], PT0 | 7
        mov dword [PD + 4], PT1 | 7
        mov edi, PT0
        mov eax, 7
        mov ecx, 1024
.pt0:   stosd
        add eax, 0x1000
        loop .pt0
        mov dword [PT0 + 0x32 * 4], 0x32000 | 5    ; user, read-only
        mov dword [PT0 + 0x31 * 4], 0x31000 | 5    ; user, read-only
        mov dword [PD + 8], 0x400000 | 0x87        ; linear 800000: a 4 MB page, user r/w (PS)
        mov dword [PD + 12], 0x400000 | 0x83       ; linear C00000: the same, supervisor
        mov edi, PT1
        mov ecx, 1024
        xor eax, eax
        rep stosd
        mov dword [PT1 + 0], 0x34000 | 7           ; linear 400000 -> 34000
        mov eax, PD
        mov cr3, eax
        mov eax, cr0
        or eax, 0x80000000
        mov cr0, eax
        jmp .paged
.paged:
        mov esi, msg_start
        call puts

        ; ---- CPUID: family 6; the P6 features (PSE TSC MSR CX8 PGE CMOV)
        mov byte [tno], 1
        mov eax, 1
        cpuid
        and eax, 0xF00
        call ok_eax
        mov byte [tno], 2
        mov eax, 1
        cpuid
        mov eax, edx
        and eax, 0xA138
        call ok_eax

        ; ---- CMOVcc: for each flag state, bit cc of EAX says CMOVcc moved
%macro CMOV_ALL 2
        mov byte [tno], %1
        xor ecx, ecx
%assign cc 15
%rep 16
        xor eax, eax
        mov ebx, 1
        push dword %2
        popfd
        db 0x0F, 0x40 + cc, 0xC3            ; cmovcc eax, ebx
        shl ecx, 1
        or ecx, eax
%assign cc cc-1
%endrep
        push dword 0x002
        popfd
        mov eax, ecx
        call ok_eax
%endmacro
        CMOV_ALL 0x10, 0x002
        CMOV_ALL 0x11, 0x8D7
        CMOV_ALL 0x12, 0x043
        CMOV_ALL 0x13, 0x882
        CMOV_ALL 0x14, 0x082
        CMOV_ALL 0x15, 0x802
        CMOV_ALL 0x16, 0x006
        CMOV_ALL 0x17, 0x042
        mov byte [tno], 0x18                ; 16 bits: the low word only
        mov eax, 0x11112222
        mov ebx, 0x33334444
        push dword 0x042
        popfd
        cmovz ax, bx
        call ok_eax
        mov byte [tno], 0x19
        mov eax, 0x11112222
        push dword 0x042
        popfd
        cmovnz ax, bx
        call ok_eax
        mov byte [tno], 0x1A                ; memory, taken
        xor eax, eax
        push dword 0x042
        popfd
        cmovz eax, [0x30000]
        call ok_eax
        mov byte [tno], 0x1B                ; memory, not taken
        mov eax, 0x5555AAAA
        push dword 0x042
        popfd
        cmovnz eax, [0x30000]
        call ok_eax
        mov byte [tno], 0x1C                ; not taken, not present: #PF all the same
        mov dword [resume], .t20
        mov eax, 0x5555AAAA
        push dword 0x042
        popfd
        cmovnz eax, [0x1000000]
        call ok_eax

        ; ---- FCMOVcc: bit k of EAX says form k (B E BE U NB NE NBE NU) moved
.t20:   finit
%macro FCMOV_ALL 2
        mov byte [tno], %1
        xor ecx, ecx
%assign k 0
%rep 8
        finit
        fld dword [f_two]
        fld1
        push dword %2
        popfd
%if k < 4
        db 0xDA, 0xC1 + k * 8               ; fcmov<cc> st0, st1
%else
        db 0xDB, 0xC1 + (k - 4) * 8
%endif
        fistp dword [tmp]
        cmp dword [tmp], 2
        jne %%n %+ k
        or ecx, 1 << k
%%n %+ k:
%assign k k+1
%endrep
        push dword 0x002
        popfd
        mov eax, ecx
        call ok_eax
%endmacro
        FCMOV_ALL 0x20, 0x002
        FCMOV_ALL 0x21, 0x8D7
        FCMOV_ALL 0x22, 0x043
        mov byte [tno], 0x23                ; ST(1) empty: #IS, masked — the indefinite in ST(0)
        finit
        fld1
        push dword 0x002
        popfd
        fcmovb st0, st1
        fxam
        fnstsw ax
        and eax, 0x4740
        call ok_eax

        ; ---- FCOMI and friends: EAX = FSW << 16 | the EFLAGS that are theirs and OF SF AF
%macro COMI 4                                ; tno, op bytes (two), ST(0) from, ST(1) from
        mov byte [tno], %1
        finit
        fld tword [%4]
        fld tword [%3]
        push dword 0x8D7
        popfd
        db %2
        pushfd
        pop ebx
        fnstsw ax
        shl eax, 16
        and ebx, 0x8D5
        or eax, ebx
        push dword 0x002
        popfd
        call ok_eax
%endmacro
        COMI 0x30, {0xDB, 0xF1}, t_one, t_two    ; fcomi: less
        COMI 0x31, {0xDB, 0xF1}, t_two, t_one    ; greater
        COMI 0x32, {0xDB, 0xF1}, t_one, t_one    ; equal
        COMI 0x33, {0xDB, 0xF1}, t_qnan, t_one   ; a QNaN: #IA, masked
        COMI 0x34, {0xDB, 0xF1}, t_snan, t_one   ; an SNaN
        COMI 0x35, {0xDB, 0xE9}, t_qnan, t_one   ; fucomi: a QNaN, quietly
        COMI 0x36, {0xDB, 0xE9}, t_snan, t_one   ; an SNaN: #IA
        COMI 0x37, {0xDB, 0xE9}, t_two, t_one
        COMI 0x38, {0xDF, 0xF1}, t_one, t_two    ; fcomip: pops
        COMI 0x39, {0xDF, 0xE9}, t_qnan, t_two   ; fucomip
        COMI 0x3A, {0xDF, 0xF1}, t_qnan, t_two   ; fcomip, #IA masked: still pops
        mov byte [tno], 0x3B                     ; ST(1) empty: #IS, masked: unordered
        finit
        fld1
        push dword 0x8D7
        popfd
        fcomi st0, st1
        pushfd
        pop ebx
        fnstsw ax
        shl eax, 16
        and ebx, 0x8D5
        or eax, ebx
        push dword 0x002
        popfd
        call ok_eax
        mov byte [tno], 0x3C                     ; #IA unmasked: EFLAGS and the stack untouched
        finit
        fldcw [cw_ie]
        fld tword [t_two]
        fld tword [t_qnan]
        push dword 0x002
        popfd
        fcomip st0, st1
        pushfd
        pop ebx
        fnstsw ax
        fnclex
        shl eax, 16
        and ebx, 0x8D5
        or eax, ebx
        call ok_eax
        finit

        ; ---- global pages
        mov byte [tno], 0x40
        mov eax, CR4_PSE | CR4_PGE
        mov cr4, eax
        mov eax, cr4
        call ok_eax
        mov dword [PT1 + 0x1A0 * 4], 0x34000 | 0x107   ; 5A0000 -> 34000, G
        mov dword [PT1 + 0x1A1 * 4], 0x34000 | 0x007   ; 5A1000 -> 34000, not G
        invlpg [0x5A0000]
        invlpg [0x5A1000]
        mov byte [tno], 0x41
        mov eax, [0x5A0010]                     ; DEADBEEF, into the TLB
        mov ebx, [0x5A1010]
        call ok_eax
        mov dword [PT1 + 0x1A0 * 4], 0x36000 | 0x107   ; both now 36000, no INVLPG
        mov dword [PT1 + 0x1A1 * 4], 0x36000 | 0x007
        mov eax, cr3
        mov cr3, eax                            ; a CR3 load
        mov byte [tno], 0x42
        mov eax, [0x5A0010]                     ; global: still DEADBEEF
        call ok_eax
        mov byte [tno], 0x43
        mov eax, [0x5A1010]                     ; not: FEEDFACE
        call ok_eax
        mov byte [tno], 0x44
        invlpg [0x5A0000]
        mov eax, [0x5A0010]                     ; INVLPG drops a global entry: FEEDFACE
        call ok_eax
        mov dword [PT1 + 0x1A0 * 4], 0x34000 | 0x107   ; back to 34000 (the TLB holds 36000)
        mov byte [tno], 0x45
        mov eax, cr3
        mov cr3, eax
        mov eax, [0x5A0010]                     ; still FEEDFACE
        call ok_eax
        mov byte [tno], 0x46
        mov eax, CR4_PSE
        mov cr4, eax                            ; PGE off: everything goes
        mov eax, [0x5A0010]                     ; DEADBEEF
        call ok_eax
        mov byte [tno], 0x47
        mov eax, [PT1 + 0x1A0 * 4]              ; the PTE: accessed, G kept
        call ok_eax

        ; ---- RDPMC and the P6 MSRs
        mov byte [tno], 0x50
        mov dword [resume], .t51
        mov ecx, 0xC1
        mov edx, 0x12
        mov eax, 0x34567890
        wrmsr
        xor ecx, ecx
        rdpmc
        mov ebx, edx
        call ok_eax
.t51:   mov byte [tno], 0x51
        mov eax, ebx
        call ok_eax
        mov byte [tno], 0x52
        mov dword [resume], .t53
        mov ecx, 2
        rdpmc
        call ok_eax
.t53:   mov byte [tno], 0x53
        mov ebx, r3_rdpmc                       ; ring 3, PCE clear: #GP
        call ring3
        mov byte [tno], 0x54
        mov eax, CR4_PSE | CR4_PCE
        mov cr4, eax
        mov ebx, r3_rdpmc                       ; PCE set
        call ring3
        mov eax, CR4_PSE
        mov cr4, eax
        mov byte [tno], 0x55
        mov dword [resume], .t56
        mov ecx, 0x11                           ; the P5's CESR: gone
        rdmsr
        call ok_eax
.t56:   mov byte [tno], 0x56
        mov dword [resume], .t60
        ud2
        call ok_eax

.t60:   xor eax, eax
        mov cr4, eax
        mov esi, msg_done
        call puts
        mov al, 0
        out 0xF4, al
.h:     hlt
        jmp .h
ring3:
        mov [saved_esp], esp
        mov dword [resume], .back
        mov ax, SEL_DATA3
        mov ds, ax
        mov es, ax
        push dword SEL_DATA3
        push dword STACK3
        push dword [r3flags]
        push dword SEL_CODE3
        push ebx
        iretd
.back:  ret

r3_rdpmc:       xor ecx, ecx
                rdpmc
                int RET_VEC
if_probe:                                   ; EFLAGS.IF inside the handler
        pushfd
        pop eax
        and eax, 0x200
        iretd

ret_trap:
        mov bx, SEL_DATA
        mov ds, bx
        mov es, bx
        mov esp, [saved_esp]
        call ok_eax
        jmp [resume]

fault_stubs:
%assign v 0
%rep 32
        push dword v
        jmp near fault
        nop
%assign v v+1
%endrep

; fault: report vector, error code (if any) and CR2, resume the script
fault:
        mov bx, SEL_DATA
        mov ds, bx
        mov es, bx
        pop ecx
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
        cmp ecx, 17
        je .err
        cmp ecx, 10
        jb .noerr
        cmp ecx, 14
        ja .noerr
.err:   mov esi, msg_e
        call puts
        pop eax
        call hex16
.noerr: mov esi, msg_cr2
        call puts
        mov eax, cr2
        call hex32
        mov al, 10
        out 0xE9, al
        mov esp, [saved_esp]
        cmp dword [saved_esp], 0
        jne .go
        mov esp, STACK0
.go:    jmp [resume]

ok_eax:
        push eax
        mov esi, msg_t
        call puts
        movzx eax, byte [tno]
        call hex8
        mov esi, msg_ok
        call puts
        pop eax
        call hex32
        mov al, 10
        out 0xE9, al
        ret

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

msg_start db "c686test", 10, 0
msg_done  db "done", 10, 0
msg_t     db "T", 0
msg_ok    db " ok eax=", 0
msg_v     db " v=", 0
msg_e     db " e=", 0
msg_cr2   db " cr2=", 0
tno       db 0
          align 4
resume    dd 0
saved_esp dd 0
cr0_boot  dd 0
r3flags   dd 0x202
flg       dd 0
tmp       dd 0
f_two     dd 2.0
cw_ie     dw 0x037E                       ; IE unmasked
t_one     dt 1.0
t_two     dt 2.0
t_qnan    dq 0xC000000000000000
          dw 0x7FFF
t_snan    dq 0xA000000000000000
          dw 0x7FFF

gdt:    dq 0
        dq 0x00CF9A000000FFFF       ; 08 code, ring 0
        dq 0x00CF92000000FFFF       ; 10 data, ring 0
        dq 0x00CFFA000000FFFF       ; 18 code, ring 3
        dq 0x00CFF2000000FFFF       ; 20 data, ring 3
        dw 0x67, TSS_BASE, 0x8900, 0 ; 28 386 TSS
gdt_end:
gdtr:   dw gdt_end - gdt - 1
        dd gdt
idtr:   dw 0x34 * 8 - 1
        dd IDT_BASE

        times (STAGE2_SECS + 1) * 512 - ($ - $$) db 0
