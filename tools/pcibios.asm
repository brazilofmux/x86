; pcibios.asm — the BIOS32 Service Directory and the 32-bit PCI BIOS, in
; real instructions, at physical F3000h (PC_STUB_SEG:PC_STUB_PCIBIOS), when
; the machine has a PCI bus (-pci; pc/pc_pci.c).
;
; The directory is the 16-byte "_32_" header protected-mode software finds
; by scanning E0000h-FFFFFh (Linux 2.x's pcibios_init, Xinu's copy of it):
; its entry, far-called from a flat 32-bit code segment with EAX = a service
; name, answers "$PCI" with where the PCI BIOS is. The PCI BIOS, far-called
; the same way with AH = B1h, does what the PCI BIOS Specification 2.1 asks
; of it through configuration mechanism #1 (ports CF8h/CFCh) — nothing is
; answered from the host: installation check (01h), find device (02h), find
; class (03h), configuration reads and writes of a byte, word, dword
; (08h-0Dh); anything else is FUNC_NOT_SUPPORTED. CF clear and AH = 0 on
; success, CF set and AH = the error otherwise. One bus: bus 0.
;
; The directory's checksum byte is filled in when it is installed
; (pc/pc_pci.c): the sum of its 16 bytes is 0.
;
; pc/pc_pcibios.h holds the assembled bytes (make pc/pc_pcibios.h).
        bits 32
        org 0xF3000

SUCCESSFUL      equ 0x00
FUNC_NOT_SUPP   equ 0x81
BAD_VENDOR_ID   equ 0x83
DEVICE_NOT_FOUND equ 0x86
BAD_REGISTER    equ 0x87

; ---- the BIOS32 Service Directory (16 bytes, 16-byte aligned) ----------
directory:
        db "_32_"
        dd bios32
        db 0                    ; revision
        db 1                    ; length, in 16-byte units
        db 0                    ; checksum (installed)
        times 5 db 0

; ---- BIOS32: EAX = service identifier, EBX = 0 -------------------------
; found: AL = 0, EBX = the service's base, ECX = its length, EDX = its entry
; offset from the base; else AL = 80h
bios32:
        cmp eax, "$PCI"
        jne .none
        mov ebx, 0xF0000
        mov ecx, 0x10000
        mov edx, pcibios - 0xF0000
        xor al, al
        retf
.none:  mov al, 0x80
        retf

; ---- the PCI BIOS: AH = B1h, AL = the function ---------------------------
pcibios:
        cmp ah, 0xB1
        jne .unsupp
        cmp al, 0x01
        je present
        cmp al, 0x02
        je find_device
        cmp al, 0x03
        je find_class
        cmp al, 0x08
        jb .unsupp
        cmp al, 0x0D
        jbe config
.unsupp:
        mov ah, FUNC_NOT_SUPP
        stc
        retf

; 01h: installed — EDX = "PCI ", AL = mechanisms (#1), BX = version 2.10,
; CL = the last bus
present:
        mov edx, "PCI "
        mov ax, 0x0001
        mov bx, 0x0210
        xor cl, cl
        clc
        retf

; the configuration dword at bus 0, device/function BL, register (low byte
; of EDI, dword-aligned) — into EAX; clobbers DX
cfg_read:
        push ebx
        and ebx, 0xFF
        shl ebx, 8
        mov eax, edi
        and eax, 0xFC
        or eax, ebx
        or eax, 0x80000000
        mov dx, 0xCF8
        out dx, eax
        mov dx, 0xCFC
        in eax, dx
        pop ebx
        ret

; 02h: find device CX (device ID) of vendor DX, SI-th instance
find_device:
        cmp dx, 0xFFFF
        je .badv
        push ebp
        push eax
        push ecx
        push edx
        push esi
        push edi
        mov ebp, ecx
        shl ebp, 16
        mov bp, dx              ; EBP = device:vendor, as configuration dword 0 reads
        and esi, 0xFFFF
        xor ebx, ebx            ; BH = bus 0, BL = device/function
        xor edi, edi            ; register 0
.next:  call cfg_read
        cmp eax, ebp
        jne .skip
        test esi, esi
        jz .found
        dec esi
.skip:  inc bl
        jnz .next
        pop edi
        pop esi
        pop edx
        pop ecx
        pop eax
        pop ebp
        mov ah, DEVICE_NOT_FOUND
        stc
        retf
.found: pop edi
        pop esi
        pop edx
        pop ecx
        pop eax
        pop ebp
        mov ah, SUCCESSFUL
        clc
        retf
.badv:  mov ah, BAD_VENDOR_ID
        stc
        retf

; 03h: find class ECX (24 bits: class, subclass, interface), SI-th instance
find_class:
        push ebp
        push eax
        push ecx
        push edx
        push esi
        push edi
        mov ebp, ecx
        and ebp, 0x00FFFFFF
        and esi, 0xFFFF
        xor ebx, ebx
.next:  xor edi, edi
        call cfg_read
        cmp ax, 0xFFFF          ; nothing there
        je .skip
        mov edi, 0x08
        call cfg_read
        shr eax, 8              ; class code: bits 31:8 of register 08h
        cmp eax, ebp
        jne .skip
        test esi, esi
        jz .found
        dec esi
.skip:  inc bl
        jnz .next
        pop edi
        pop esi
        pop edx
        pop ecx
        pop eax
        pop ebp
        mov ah, DEVICE_NOT_FOUND
        stc
        retf
.found: pop edi
        pop esi
        pop edx
        pop ecx
        pop eax
        pop ebp
        mov ah, SUCCESSFUL
        clc
        retf

; 08h-0Dh: read byte, word, dword into CL/CX/ECX; write byte, word, dword
; from CL/CX/ECX. BH = bus, BL = device/function, DI = register.
config:
        test bh, bh
        jnz .bad                ; one bus
        push eax
        push edx
        push edi
        mov ah, al              ; AH = the function
        movzx edi, di
        cmp ah, 0x09            ; words (09h, 0Ch) are word-aligned,
        je .w
        cmp ah, 0x0C
        je .w
        cmp ah, 0x0A            ; dwords (0Ah, 0Dh) dword-aligned
        je .d
        cmp ah, 0x0D
        jne .addr
.d:     test di, 3
        jnz .badpop
        jmp .addr
.w:     test di, 1
        jnz .badpop
.addr:  movzx eax, bl
        shl eax, 8
        mov edx, edi
        and edx, 0xFC
        or eax, edx
        or eax, 0x80000000
        mov dx, 0xCF8
        out dx, eax
        mov edx, edi
        and edx, 3
        add edx, 0xCFC          ; the byte lane
        mov eax, [esp+8]        ; (the saved EAX: the function in AL)
        cmp al, 0x08
        je .rb
        cmp al, 0x09
        je .rw
        cmp al, 0x0A
        je .rd
        cmp al, 0x0B
        je .wb
        cmp al, 0x0C
        je .ww
        mov eax, ecx            ; 0Dh
        out dx, eax
        jmp .ok
.wb:    mov al, cl
        out dx, al
        jmp .ok
.ww:    mov ax, cx
        out dx, ax
        jmp .ok
.rb:    in al, dx
        mov cl, al
        jmp .ok
.rw:    in ax, dx
        mov cx, ax
        jmp .ok
.rd:    in eax, dx
        mov ecx, eax
.ok:    pop edi
        pop edx
        pop eax
        mov ah, SUCCESSFUL
        clc
        retf
.badpop:
        pop edi
        pop edx
        pop eax
.bad:   mov ah, BAD_REGISTER
        stc
        retf
