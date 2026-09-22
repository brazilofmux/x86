; dpmi.asm — a DPMI client that exercises the host from both widths.
;
; Assembled twice: dpmi.com is a 16-bit client (AX=0 at the mode switch),
; dpmi32.com (-DCLIENT32) a 32-bit one (AX=1). The code is 16-bit either way
; — the host always returns into the segment the client far-called it from —
; but a 32-bit client is handed 32-bit gates and frames, and that difference
; is what the two builds are for. Every stage prints a line, so a failure
; names itself; the exit code is 0 only if every check held.
        cpu 386
        org 100h

%ifdef CLIENT32
  %define W 4
%else
  %define W 2
%endif

        mov     ah, 4Ah                 ; a .COM owns all of memory: keep our
        mov     bx, 1000h               ; 64 KB (the stack is at its top) and
        int     21h                     ; give back the rest, or 0100 has none
        mov     ax, 1687h
        int     2Fh
        test    ax, ax
        jz      have
        mov     dx, msg_none
        jmp     bail
have:   mov     [entry], di
        mov     [entry + 2], es
%ifdef CLIENT32
        mov     ax, 1
%else
        xor     ax, ax
%endif
        call    far [entry]
        jnc     inpm
        mov     dx, msg_fail
        jmp     bail

; ---- from here on, protected mode ----
inpm:   mov     [psp_sel], es           ; ES is the PSP's selector on entry
        mov     ax, ds
        mov     es, ax

; 1. The basics: version, a descriptor, its base read back.
        mov     ax, 0400h
        int     31h
        cmp     ax, 005Ah
        jne     bad
        mov     ax, 0000h
        mov     cx, 1
        int     31h
        jc      bad
        mov     bx, ax
        mov     cx, 0012h               ; base 00123400h
        mov     dx, 3400h
        mov     ax, 0007h
        int     31h
        jc      bad
        xor     cx, cx
        xor     dx, dx
        mov     ax, 0006h
        int     31h
        jc      bad
        cmp     cx, 0012h
        jne     bad
        cmp     dx, 3400h
        jne     bad
        ; DS is a selector now, so this only prints if the DOS layer
        ; resolved the pointer through the descriptor's base.
        mov     dx, msg_pm
        call    print

; 2. The host turned the environment segment in the PSP into a selector
;    (DJGPP's crt1 hands PSP:2Ch straight to movedata). LSL proves it is
;    a live descriptor; the bytes prove it is the environment.
        push    es
        mov     es, [psp_sel]
        mov     ax, [es:2Ch]
        pop     es
        lsl     cx, ax
        jnz     bad
        push    ds
        mov     ds, ax
        cmp     dword [0], 'PATH'
        pop     ds
        jne     bad
        mov     dx, msg_env
        call    print

; 3. The timer interrupts protected mode and comes back intact. IRQ0 is
;    INT 8, which is also #DF; it once arrived with #DF's error code on top
;    and every IRET came back one slot short.
        mov     ax, 0002h               ; a selector for the BIOS data area
        mov     bx, 0040h
        int     31h
        jc      bad
        mov     fs, ax
        mov     [esp_before], esp
        mov     ebx, [fs:6Ch]
        sti
tick:   hlt
        cmp     ebx, [fs:6Ch]
        je      tick
        cmp     esp, [esp_before]
        jne     bad
        mov     dx, msg_tick
        call    print

; 4. A real-mode excursion (0300): INT 21h AH=09 in real mode, printing a
;    string that lives in a DOS block (0100) we filled through its selector.
        mov     ax, 0100h
        mov     bx, 2                   ; two paragraphs
        int     31h
        jc      bad
        mov     [dos_seg], ax
        mov     [dos_sel], dx
        push    es
        mov     es, dx
        mov     si, msg_rm
        xor     di, di
        mov     cx, msg_rm_len
        cld
        rep     movsb
        pop     es
        mov     di, rmcs
        xor     ax, ax
        mov     cx, 32h / 2
        rep     stosw
        mov     dword [rmcs + 1Ch], 0900h       ; EAX
        mov     ax, [dos_seg]
        mov     [rmcs + 24h], ax                ; DS; DX stays 0
        mov     ax, 0300h
        mov     bx, 0021h
        xor     cx, cx
        mov     di, rmcs
        int     31h
        jc      bad
        mov     ax, 0101h
        mov     dx, [dos_sel]
        int     31h
        jc      bad

; 5. Extended memory: allocate 64 KB above the HMA, put a descriptor on it,
;    write and read its last dword, give it back.
        mov     ax, 0501h
        mov     bx, 1                   ; BX:CX = 10000h bytes
        xor     cx, cx
        int     31h
        jc      bad
        cmp     bx, 0011h               ; above 1 MB + HMA
        jb      bad
        mov     [xms_lin], cx
        mov     [xms_lin + 2], bx
        mov     [xms_h], di
        mov     [xms_h + 2], si
        mov     ax, 0000h
        mov     cx, 1
        int     31h
        jc      bad
        mov     [xms_sel], ax
        mov     bx, ax
        mov     cx, [xms_lin + 2]
        mov     dx, [xms_lin]
        mov     ax, 0007h
        int     31h
        jc      bad
        mov     bx, [xms_sel]
        xor     cx, cx
        mov     dx, 0FFFFh
        mov     ax, 0008h
        int     31h
        jc      bad
        mov     gs, [xms_sel]
        mov     dword [gs:0FFFCh], 12345678h
        cmp     dword [gs:0FFFCh], 12345678h
        jne     bad
        mov     ax, 0502h
        mov     si, [xms_h + 2]
        mov     di, [xms_h]
        int     31h
        jc      bad
        mov     dx, msg_xms
        call    print

; 6. An exception handler (0203) gets DPMI's frame, as wide as the client,
;    on a stack of the host's — and resumes wherever it rewrites the frame
;    to say.
        mov     ax, 0203h
        mov     bl, 0Dh
        mov     cx, cs
%ifdef CLIENT32
        mov     edx, gp_handler
%else
        mov     dx, gp_handler
%endif
        int     31h
        jc      bad
        mov     [esp_before], esp
        mov     ax, 0FFF8h              ; GDT index 1FFFh: far past the table
        mov     es, ax                  ; #GP; the handler steps over it
gp_after:
        cmp     esp, [esp_before]
        jne     bad
        cmp     byte [gp_seen], 1
        jne     bad
        mov     dx, msg_exc
        call    print

; 7. An interrupt handler of the client's own (0205), entered through a gate
;    as wide as the client and left with the matching IRET.
        mov     ax, 0205h
        mov     bl, 60h
        mov     cx, cs
%ifdef CLIENT32
        mov     edx, int60
%else
        mov     dx, int60
%endif
        int     31h
        jc      bad
        mov     [esp_before], esp
        int     60h
        cmp     esp, [esp_before]
        jne     bad
        cmp     byte [int60_seen], 1
        jne     bad
        mov     dx, msg_int
        call    print

; 8. A hook on INT 21h that chains to the host's handler with PUSHF / CALL
;    FAR, the way every TSR does. The host must size that frame by how it
;    was entered; the line below is printed through the chain.
        mov     ax, 0204h
        mov     bl, 21h
        int     31h
%ifdef CLIENT32
        mov     [old21], edx
%else
        mov     [old21], dx
%endif
        mov     [old21 + W], cx
        mov     ax, 0205h
        mov     bl, 21h
        mov     cx, cs
%ifdef CLIENT32
        mov     edx, hook21
%else
        mov     dx, hook21
%endif
        int     31h
        jc      bad
        mov     dx, msg_chain
        call    print
        cmp     byte [hook21_seen], 1
        jne     bad
        mov     ax, 0205h               ; put the host's back
        mov     bl, 21h
        mov     cx, [old21 + W]
%ifdef CLIENT32
        mov     edx, [old21]
%else
        mov     dx, [old21]
%endif
        int     31h
        jc      bad

        mov     ax, 4C00h
        int     21h

bad:    mov     dx, msg_bad
        mov     ah, 9
        int     21h
        mov     ax, 4C01h
        int     21h

bail:   mov     ah, 9
        int     21h
        mov     ax, 4C02h
        int     21h

print:  mov     ah, 9
        int     21h
        ret

gp_handler:
%ifdef CLIENT32
        push    ebp
        mov     ebp, esp
        cmp     dword [ebp + 4 + 08h], 0FFF8h   ; error code: the selector
        jne     .skip
        mov     byte [gp_seen], 1
.skip:  mov     dword [ebp + 4 + 0Ch], gp_after ; resume past the faulting MOV
        pop     ebp
        o32 retf
%else
        push    bp
        mov     bp, sp
        cmp     word [bp + 2 + 04h], 0FFF8h
        jne     .skip
        mov     byte [gp_seen], 1
.skip:  mov     word [bp + 2 + 06h], gp_after
        pop     bp
        retf
%endif

int60:  mov     byte [int60_seen], 1
%ifdef CLIENT32
        iretd
%else
        iret
%endif

hook21: mov     byte [hook21_seen], 1
%ifdef CLIENT32
        pushfd
        call    far dword [old21]
        iretd
%else
        pushf
        call    far [old21]
        iret
%endif

entry       dd  0
psp_sel     dw  0
esp_before  dd  0
dos_seg     dw  0
dos_sel     dw  0
xms_lin     dd  0
xms_h       dd  0
xms_sel     dw  0
old21       dd  0, 0
gp_seen     db  0
int60_seen  db  0
hook21_seen db  0
rmcs        times 32h db 0

msg_pm     db  'hello from protected mode', 13, 10, '$'
msg_env    db  'environment selector ok', 13, 10, '$'
msg_tick   db  'timer tick ok', 13, 10, '$'
msg_rm     db  'real-mode excursion ok', 13, 10, '$'
msg_rm_len equ $ - msg_rm
msg_xms    db  'extended memory ok', 13, 10, '$'
msg_exc    db  'exception handler ok', 13, 10, '$'
msg_int    db  'interrupt handler ok', 13, 10, '$'
msg_chain  db  'chained INT 21h ok', 13, 10, '$'
msg_bad    db  'check failed', 13, 10, '$'
msg_none   db  'no DPMI host', 13, 10, '$'
msg_fail   db  'mode switch failed', 13, 10, '$'
