#!/usr/bin/env python3
"""fpugen.py — the x87 oracle image: generates fputest.asm, a real-mode
boot image that runs a table of FPU cases and writes one line per case to
port E9 (fpurun.py runs it under dos-monster, Bochs and QEMU and diffs).

Each case: FNINIT, FLDCW, up to three FLD m80 from a table of awkward
values (the last is ST(0)), ten bytes of memory operand at DS:BX, the
instruction bytes copied into a slot and CALLed, then FNSAVE. The line
is the status word, the tag word, every register the tags call in use
(ST order, sign+exponent:significand) and the ten memory bytes.

    python3 fpugen.py > fputest.asm      (deterministic: no randomness)
"""
import struct, sys

# ---- the values: (name, sign+exponent, significand) -----------------------
V = [
    ("+0",      0x0000, 0x0000000000000000),
    ("-0",      0x8000, 0x0000000000000000),
    ("+1",      0x3FFF, 0x8000000000000000),
    ("-1",      0xBFFF, 0x8000000000000000),
    ("1.5",     0x3FFF, 0xC000000000000000),
    ("-2.5",    0xC000, 0xA000000000000000),
    ("3",       0x4000, 0xC000000000000000),
    ("10",      0x4002, 0xA000000000000000),
    ("pi",      0x4000, 0xC90FDAA22168C235),
    ("1/3",     0x3FFD, 0xAAAAAAAAAAAAAAAB),
    ("-1/3",    0xBFFD, 0xAAAAAAAAAAAAAAAB),
    ("0.5",     0x3FFE, 0x8000000000000000),
    ("0.1",     0x3FFB, 0xCCCCCCCCCCCCCCCD),
    ("2^70+1",  0x4045, 0x8000000000000001),  # beyond 64-bit integers? no: 2^70 needs rounding for m64 int
    ("1e18",    0x403A, 0xDE0B6B3A76400000),
    ("-12345.6",0xC00C, 0xC0E6CCCCCCCCCCCD),
    ("2^-70",   0x3FB9, 0x8000000000000000),
    ("big",     0x7FFE, 0xFFFFFFFFFFFFFFFF),  # largest finite
    ("tiny",    0x0001, 0x8000000000000000),  # smallest normal
    ("den",     0x0000, 0x0000000000000001),  # smallest denormal
    ("den2",    0x0000, 0x4000000000000000),
    ("pden",    0x0000, 0x8000000000000001),  # pseudo-denormal
    ("+inf",    0x7FFF, 0x8000000000000000),
    ("-inf",    0xFFFF, 0x8000000000000000),
    ("qnan",    0x7FFF, 0xC000000000001234),
    ("snan",    0x7FFF, 0x8000000000005678),
    ("-qnan",   0xFFFF, 0xC000000000009ABC),
    ("indef",   0xFFFF, 0xC000000000000000),
    ("unnorm",  0x3FFF, 0x4000000000000000),
    ("pinf",    0x7FFF, 0x0000000000000000),  # pseudo-infinity
    ("7",       0x4001, 0xE000000000000000),
    ("-7",      0xC001, 0xE000000000000000),
    ("1.0000001",0x3FFF, 0x8000000000000001),
    ("2^63",    0x403E, 0x8000000000000000),
    ("-2^15-0.5",0xC00E, 0x8001000000000000),
    ("0.75",    0x3FFE, 0xC000000000000000),
    ("-0.3",    0xBFFD, 0x9999999999999999),
    ("100.5",   0x4005, 0xC900000000000000),
    ("2^16383", 0x7FFE, 0x8000000000000000),
    ("2^-16382x1.1", 0x0001, 0xC000000000000000),
]
IDX = {n: i for i, (n, _, _) in enumerate(V)}

CW_DEF = 0x037F
RC = {"near": 0x000, "down": 0x400, "up": 0x800, "chop": 0xC00}
PC = {"24": 0x000, "53": 0x200, "64": 0x300}

cases = []   # (comment, cw, [stack names], mem bytes, insn bytes)

def case(comment, insn, stack=(), cw=CW_DEF, mem=b"\0" * 10):
    mem = bytes(mem) + b"\0" * (10 - len(mem))
    cases.append((comment, cw, [IDX[s] for s in stack], mem, bytes(insn)))

def f32(x): return struct.pack("<f", x)
def f64(x): return struct.pack("<d", x)
def raw32(v): return struct.pack("<I", v)
def raw64(v): return struct.pack("<Q", v)

SOME = ["+0", "-0", "+1", "-1", "1.5", "pi", "1/3", "-1/3", "0.1", "big", "tiny", "den",
        "pden", "+inf", "-inf", "qnan", "snan", "-qnan", "indef", "unnorm", "pinf", "7", "-2.5", "2^16383",
        "2^-70", "1.0000001"]
FEW = ["+0", "-0", "+1", "1/3", "pi", "big", "tiny", "den", "pden", "+inf", "-inf", "qnan", "snan",
       "unnorm", "-7", "2^-16382x1.1"]

# ---- two-operand register arithmetic: ST(0) op ST(1) (D8 C0+i form, i=1)
ARITH = [("fadd", 0xC1), ("fmul", 0xC9), ("fsub", 0xE1), ("fsubr", 0xE9), ("fdiv", 0xF1), ("fdivr", 0xF9)]
for name, op in ARITH:
    for a in FEW:
        for b in FEW:
            case("%s %s,%s" % (name, b, a), [0xD8, op], (a, b))
# rounding and precision control on the ones that round
PAIRS = [("1/3", "pi"), ("+1", "1/3"), ("0.1", "7"), ("big", "big"), ("tiny", "0.5"), ("1.0000001", "-1/3"),
         ("den", "0.5"), ("2^16383", "7"), ("tiny", "tiny")]
for name, op in ARITH:
    for rcn, rc in RC.items():
        for pcn, pc in PC.items():
            for a, b in PAIRS:
                case("%s %s,%s rc=%s pc=%s" % (name, b, a, rcn, pcn), [0xD8, op], (a, b), cw=0x007F | rc | pc)
# the P forms, DC forms and a pop
for a, b in [("1/3", "pi"), ("+1", "+inf"), ("snan", "1.5")]:
    case("faddp %s,%s" % (b, a), [0xDE, 0xC1], (a, b))
    case("fsubp st1 %s,%s" % (b, a), [0xDE, 0xE9], (a, b))
    case("fsubrp st1 %s,%s" % (b, a), [0xDE, 0xE1], (a, b))
    case("fdivp st1 %s,%s" % (b, a), [0xDE, 0xF9], (a, b))
    case("fdivrp st1 %s,%s" % (b, a), [0xDE, 0xF1], (a, b))
    case("fsub st1,st0 (DC E9) %s,%s" % (b, a), [0xDC, 0xE9], (a, b))
    case("fdiv st1,st0 (DC F9) %s,%s" % (b, a), [0xDC, 0xF9], (a, b))

# ---- memory operands
for x in [0.0, -0.0, 1.0, 1.0 / 3, 1e-40, 3.4e38, float("inf")]:
    for s in ["pi", "+1", "tiny"]:
        case("fadd m32 %s+%r" % (s, x), [0xD8, 0x07], (s,), mem=f32(x))
        case("fmul m64 %s*%r" % (s, x), [0xDC, 0x0F], (s,), mem=f64(x))
case("fadd m32 snan", [0xD8, 0x07], ("pi",), mem=raw32(0x7F800001))
case("fadd m32 qnan", [0xD8, 0x07], ("pi",), mem=raw32(0x7FC00123))
case("fadd m64 snan", [0xDC, 0x07], ("pi",), mem=raw64(0x7FF0000000000001))
case("fadd m32 den", [0xD8, 0x07], ("pi",), mem=raw32(0x00000001))
case("fcom m32 1", [0xD8, 0x17], ("pi",), mem=f32(1.0))
case("fcomp m64 pi", [0xDC, 0x1F], ("pi",), mem=f64(3.141592653589793))
case("ficom m16 3", [0xDE, 0x17], ("3",), mem=struct.pack("<h", 3))
case("fiadd m16 -5", [0xDE, 0x07], ("1.5",), mem=struct.pack("<h", -5))
case("fidivr m32 7", [0xDA, 0x3F], ("3",), mem=struct.pack("<i", 7))
case("fisub m32 max", [0xDA, 0x27], ("+0",), mem=struct.pack("<i", -2**31))
for s in ["+1", "den", "snan", "qnan"]:
    case("fld m32 %s" % s, [0xD9, 0x07], (), mem={"+1": f32(1.0), "den": raw32(1), "snan": raw32(0x7F800001), "qnan": raw32(0xFFC00000)}[s])
    case("fld m64 %s" % s, [0xDD, 0x07], (), mem={"+1": f64(1.0), "den": raw64(1), "snan": raw64(0x7FF0000000000001), "qnan": raw64(0xFFF8000000000000)}[s])
for s in SOME:
    for rcn in ("near", "up", "chop"):
        cw = 0x037F & ~0xC00 | RC[rcn]
        case("fst m32 %s rc=%s" % (s, rcn), [0xD9, 0x17], (s,), cw=cw)
        case("fstp m64 %s rc=%s" % (s, rcn), [0xDD, 0x1F], (s,), cw=cw)
    case("fstp m80 %s" % s, [0xDB, 0x3F], (s,))
    case("fld m80 %s" % s, [0xDB, 0x2F], (), mem=struct.pack("<QH", V[IDX[s]][2], V[IDX[s]][1]))
INTS = SOME + ["1e18", "-12345.6", "2^70+1", "2^63", "-2^15-0.5", "0.75", "-0.3", "100.5", "0.5"]
for s in INTS:
    for rcn in RC:
        cw = 0x037F & ~0xC00 | RC[rcn]
        case("fist m16 %s rc=%s" % (s, rcn), [0xDF, 0x17], (s,), cw=cw)
        case("fistp m32 %s rc=%s" % (s, rcn), [0xDB, 0x1F], (s,), cw=cw)
        case("fistp m64 %s rc=%s" % (s, rcn), [0xDF, 0x3F], (s,), cw=cw)
        case("frndint %s rc=%s" % (s, rcn), [0xD9, 0xFC], (s,), cw=cw)
    case("fbstp %s" % s, [0xDF, 0x37], (s,))
for v in [0, -1, 32767, -32768]:
    case("fild m16 %d" % v, [0xDF, 0x07], (), mem=struct.pack("<h", v))
for v in [0, 2**31 - 1, -2**31]:
    case("fild m32 %d" % v, [0xDB, 0x07], (), mem=struct.pack("<i", v))
for v in [0, 2**63 - 1, -2**63, 12345678901234567]:
    case("fild m64 %d" % v, [0xDF, 0x2F], (), mem=struct.pack("<q", v))
for bcd, sign in [(b"\x21\x43\x65\x87\x09\x21\x43\x65\x87", 0x00), (b"\x00" * 9, 0x80), (b"\x99" * 9, 0x80)]:
    case("fbld", [0xDF, 0x27], (), mem=bcd + bytes([sign]))

# ---- one-operand and the rest
ONE = [("fchs", [0xD9, 0xE0]), ("fabs", [0xD9, 0xE1]), ("ftst", [0xD9, 0xE4]), ("fxam", [0xD9, 0xE5]),
       ("fsqrt", [0xD9, 0xFA]), ("fxtract", [0xD9, 0xF4]), ("f2xm1", [0xD9, 0xF0]),
       ("fsin", [0xD9, 0xFE]), ("fcos", [0xD9, 0xFF]), ("fptan", [0xD9, 0xF2]), ("fsincos", [0xD9, 0xFB])]
# F2XM1 is defined for |x| <= 1 only; outside, emulators (and chips) differ
F2XM1_DOM = {"+0", "-0", "+1", "-1", "1/3", "-1/3", "0.1", "tiny", "den", "pden", "+inf", "-inf", "qnan", "snan",
             "-qnan", "indef", "unnorm", "pinf", "2^-70", "0.5", "-0.3", "0.75", "1.0000001"}
for name, insn in ONE:
    for s in SOME + ["0.5", "-0.3", "0.75", "100.5", "1e18", "2^63"]:
        if name == "f2xm1" and s not in F2XM1_DOM: continue
        case("%s %s" % (name, s), insn, (s,))
for rcn in RC:
    for pcn in PC:
        for s in ["1/3", "7", "0.1", "big"]:
            case("fsqrt %s rc=%s pc=%s" % (s, rcn, pcn), [0xD9, 0xFA], (s,), cw=0x007F | RC[rcn] | PC[pcn])
TWO = [("fscale", [0xD9, 0xFD]), ("fprem", [0xD9, 0xF8]), ("fprem1", [0xD9, 0xF5]), ("fpatan", [0xD9, 0xF3]),
       ("fyl2x", [0xD9, 0xF1]), ("fyl2xp1", [0xD9, 0xF9]), ("fcom", [0xD8, 0xD1]), ("fucom", [0xDD, 0xE1]),
       ("fucompp", [0xDA, 0xE9]), ("fcompp", [0xDE, 0xD9]), ("fxch", [0xD9, 0xC9])]
TWOV = ["+0", "-0", "+1", "-1", "0.5", "pi", "10", "-7", "big", "tiny", "den", "+inf", "-inf", "qnan",
        "snan", "unnorm", "1e18"]
# FYL2XP1 is defined for |x| < 1 - sqrt(2)/2 only
YL2XP1_DOM = {"+0", "-0", "tiny", "den", "qnan", "snan", "unnorm", "+inf", "-inf", "0.1", "-0.3"}
for name, insn in TWO:
    for a in TWOV + (["0.1", "-0.3"] if name == "fyl2xp1" else []):
        if name == "fyl2xp1" and a not in YL2XP1_DOM: continue
        for b in TWOV:
            case("%s st0=%s st1=%s" % (name, a, b), insn, (b, a))
for name, insn in [("fsin", [0xD9, 0xFE]), ("fcos", [0xD9, 0xFF]), ("fptan", [0xD9, 0xF2]), ("fpatan", [0xD9, 0xF3]),
                   ("f2xm1", [0xD9, 0xF0]), ("fyl2x", [0xD9, 0xF1])]:
    for rcn in RC:
        stk = ("0.5", "pi") if name in ("fpatan", "fyl2x") else ("0.1",) if name == "f2xm1" else ("pi",)
        case("%s %s rc=%s" % (name, "st0=%s st1=%s" % (stk[1], stk[0]) if len(stk) > 1 else stk[0], rcn),
             insn, stk, cw=0x037F & ~0xC00 | RC[rcn])
for k, n in enumerate(["fld1", "fldl2t", "fldl2e", "fldpi", "fldlg2", "fldln2", "fldz"]):
    for rcn in RC:
        case("%s rc=%s" % (n, rcn), [0xD9, 0xE8 + k], (), cw=0x037F & ~0xC00 | RC[rcn])
# the stack itself
case("fld st1 (empty)", [0xD9, 0xC1], ("+1",))
case("fadd (st1 empty)", [0xD8, 0xC1], ("+1",))
case("fsqrt (empty)", [0xD9, 0xFA], ())
case("fxch (st1 empty)", [0xD9, 0xC9], ("pi",))
case("fdecstp", [0xD9, 0xF6], ("pi", "+1"))
case("fincstp", [0xD9, 0xF7], ("pi", "+1"))
case("ffree st0", [0xDD, 0xC0], ("pi", "+1"))
case("ffreep st0", [0xDF, 0xC0], ("pi", "+1"))
case("fst st2", [0xDD, 0xD2], ("pi", "+1"))
case("fstp st1 (DF D9)", [0xDF, 0xD9], ("pi", "+1"))
case("fnop", [0xD9, 0xD0], ("pi",))
case("fnclex after ie", [0xDB, 0xE2], ("snan",))
# eight pushes, then a ninth: FDECSTP x5 then FLD1 x... done by stacking three and decrementing
case("push onto full (fld1 with st7 used)", [0xD9, 0xE8], ("+1", "pi", "7"), cw=CW_DEF)
# unmasked: nothing is written, ES and B go up
for name, op in [("fdiv", 0xF1), ("fadd", 0xC1), ("fmul", 0xC9)]:
    for a, b in [("+0", "+1"), ("snan", "+1"), ("den", "+1"), ("big", "big"), ("tiny", "tiny"), ("1/3", "+1")]:
        case("%s %s,%s unmasked" % (name, b, a), [0xD8, op], (a, b), cw=0x0340)
for s in ["big", "tiny", "1/3", "snan"]:
    case("fst m32 %s unmasked" % s, [0xD9, 0x17], (s,), cw=0x0340)
    case("fistp m16 %s unmasked" % s, [0xDF, 0x1F], (s,), cw=0x0340)
case("fsqrt -1 unmasked", [0xD9, 0xFA], ("-1",), cw=0x0340)
case("fscale big by 2^16383 unmasked", [0xD9, 0xFD], ("2^16383" if False else "10", "big"), cw=0x0340)
case("fxtract 0 unmasked", [0xD9, 0xF4], ("+0",), cw=0x0340)
case("fprem big/tiny", [0xD9, 0xF8], ("tiny", "big"))
case("fprem1 big/tiny", [0xD9, 0xF5], ("tiny", "big"))

# ---- emit -------------------------------------------------------------------
out = sys.stdout
N = len(cases)
REC = 32
out.write("""; fputest.asm — generated by fpugen.py: %d x87 cases, see there.
        cpu 486
        org 0x7C00
STAGE2_SEG  equ 0x1000
        bits 16
stage1:
        cli
        cld
        xor ax, ax
        mov ds, ax
        mov ss, ax
        mov sp, 0x7C00
        mov [drive], dl
        ; read sectors 2.. into 1000:0000, one at a time (CHS for 1.44 MB)
        mov ax, STAGE2_SEG
        mov es, ax
        xor bx, bx
        mov word [lba], 1
.rd:    mov ax, [lba]
        xor dx, dx
        mov cx, 18
        div cx                      ; ax = track*2+head, dx = sector-1
        inc dx
        mov cl, dl                  ; sector
        xor dx, dx
        mov si, 2
        div si                      ; ax = cylinder, dx = head
        mov ch, al
        mov dh, dl
        mov dl, [drive]
        mov ax, 0x0201
        int 0x13
        jc .rd
        add bx, 512
        jnz .same
        mov ax, es
        add ax, 0x1000
        mov es, ax
.same:  inc word [lba]
        cmp word [lba], STAGE2_SECS + 1
        jbe .rd
        jmp STAGE2_SEG:0
drive   db 0
lba     dw 0
        times 510 - ($ - $$) db 0
        dw 0xAA55

        section stage2 vstart=0
s2:     cli                         ; (the BIOS's INT 13h left interrupts on: no timer ticks in the middle)
        mov ax, cs
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 0xFFF0
        mov si, msg_start
        call puts
        xor bp, bp                  ; case number
.next:  cmp bp, NCASES
        jae .done
        mov eax, [cur]              ; the record, by linear address: FS:BX
        mov bx, ax
        and bx, 15
        shr eax, 4
        mov fs, ax
        fninit
        fldcw [fs:bx]
        movzx cx, byte [fs:bx+2]    ; stack count
        lea di, [bx+3]
.ld:    jcxz .ldd
        movzx ax, byte [fs:di]
        imul ax, ax, 10
        add ax, values
        mov si, ax
        fld tword [si]
        inc di
        dec cx
        jmp .ld
.ldd:   xor si, si                  ; the memory operand
.cm:    mov al, [fs:bx+si+6]
        mov [membuf+si], al
        inc si
        cmp si, 10
        jb .cm
        movzx cx, byte [fs:bx+16]   ; the instruction, then RET
        xor si, si
.ci:    cmp si, cx
        jae .cid
        mov al, [fs:bx+si+17]
        mov [slot+si], al
        inc si
        jmp .ci
.cid:   mov byte [slot+si], 0xC3
        mov bx, membuf
        call slot
        fnsave [save]
        ; the line: Cnnnn sw tw regs... m=bytes
        mov al, 'C'
        out 0xE9, al
        mov ax, bp
        call hex16
        mov al, ' '
        out 0xE9, al
        mov ax, [save+2]
        call hex16
        mov al, ' '
        out 0xE9, al
        mov ax, [save+4]
        call hex16
        xor cx, cx                  ; ST0..7 that the tags call in use (physical = top + i)
.reg:   mov ax, [save+2]
        shr ax, 11
        add ax, cx
        and ax, 7
        shl ax, 1
        push cx
        mov cx, ax
        mov ax, [save+4]
        shr ax, cl
        pop cx
        and ax, 3
        cmp ax, 3
        je .skip
        mov al, ' '
        out 0xE9, al
        mov bx, cx
        imul bx, bx, 10
        add bx, save+14
        mov ax, [bx+8]
        call hex16
        mov al, ':'
        out 0xE9, al
        mov ax, [bx+6]
        call hex16
        mov ax, [bx+4]
        call hex16
        mov ax, [bx+2]
        call hex16
        mov ax, [bx]
        call hex16
.skip:  inc cx
        cmp cx, 8
        jb .reg
        mov al, ' '
        out 0xE9, al
        mov al, 'm'
        out 0xE9, al
        mov al, '='
        out 0xE9, al
        mov bx, membuf + 9
.mb:    mov al, [bx]
        call hex8
        dec bx
        cmp bx, membuf
        jae .mb
        mov al, 10
        out 0xE9, al
        add dword [cur], %d
        inc bp
        jmp .next
.done:  mov si, msg_done
        call puts
        mov al, 0
        out 0xF4, al                ; QEMU isa-debug-exit / dos-monster: end
        mov dx, 0x8900              ; Bochs: shutdown
        mov si, msg_shut
.sh:    lodsb
        test al, al
        jz .h
        out dx, al
        jmp .sh
.h:     hlt
        jmp .h

puts:   lodsb
        test al, al
        jz .d
        out 0xE9, al
        jmp puts
.d:     ret
hex16:  push ax
        mov al, ah
        call hex8
        pop ax
hex8:   push ax
        shr al, 4
        call .n
        pop ax
.n:     and al, 15
        add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     out 0xE9, al
        ret

msg_start db "fputest", 10, 0
msg_done  db "done", 10, 0
msg_shut  db "Shutdown", 0
        align 16
save    times 108 db 0
membuf  times 16 db 0
slot    times 16 db 0
cur     dd STAGE2_SEG * 16 + cases
        align 16
values:
""" % (N, REC))
for n, se, s in V:
    out.write("        dq 0x%016X\n        dw 0x%04X            ; %s\n" % (s, se, n))
out.write("cases:\n")
for i, (comment, cw, stk, mem, insn) in enumerate(cases):
    assert len(stk) <= 3 and len(insn) <= 15
    rec = struct.pack("<HB", cw, len(stk)) + bytes(stk + [0] * (3 - len(stk))) + mem + bytes([len(insn)]) + insn
    rec += b"\0" * (REC - len(rec))
    out.write("        db " + ",".join("0x%02X" % b for b in rec) + "   ; C%04X %s\n" % (i, comment))
out.write("NCASES equ %d\ns2_end:\nSTAGE2_SECS equ (s2_end - s2 + 511) / 512\n" % N)
out.write("        times (2880 * 512 - 512) - (s2_end - s2) db 0\n")
