/* x86_fpu.c — the x87 (387, and the 486DX's on-chip unit)
 *
 * Arithmetic is Berkeley SoftFloat 3e's extF80, with its 8086 NaN rules;
 * this file is the x87 around it:
 *
 * - The register stack: physical R0-R7, ST(i) = R((TOP + i) & 7), an
 *   empty bit per register (the tag word's other values are the
 *   contents' class, computed when FSTENV/FSAVE report them, which is what
 *   a 387 does on FLDENV/FRSTOR too).
 * - Exceptions in the order the SDM ranks them: stack fault (IE with SF;
 *   C1 says overflow), invalid operand (SNaN; the unsupported encodings —
 *   unnormals, pseudo-infinities, pseudo-NaNs — that the 387 rejects),
 *   denormal operand, then the operation's own (ZE, OE, UE, PE). Masked,
 *   each has its default result. Unmasked, IE/DE/ZE leave the destination
 *   and TOP alone, and OE/UE deliver the result with its exponent wrapped
 *   by 24576 (register destinations) or store nothing (memory). Either
 *   way ES and B go up and the next waiting instruction reports it: #MF
 *   with CR0.NE, else FERR# (IRQ 13, not wired until the slave 8259 is).
 * - C1 after a rounded result: 1 when the rounding went up in magnitude,
 *   found by rounding again toward zero (only when inexact).
 * - Precision control (CW.PC) on FADD/FSUB/FMUL/FDIV/FSQRT only.
 * - The last instruction and operand pointers and opcode, kept by every
 *   instruction but the control ones, and FSTENV/FSAVE's four layouts
 *   (real or V86 / protected, 16- or 32-bit operand size).
 * - Transcendentals (FSIN FCOS FSINCOS FPTAN FPATAN F2XM1 FYL2X FYL2XP1)
 *   evaluated in SoftFloat's 128-bit format and rounded once, by CW.RC, to
 *   64 bits: within an ulp of the true value, not the 486 microcode's own
 *   last-bit behaviour. FSIN/FCOS/FSINCOS/FPTAN reduce their argument with
 *   a 66-bit pi, as the x87 does (Intel documents it), so large arguments
 *   come out as they do on silicon rather than as they do in a libm.
 *
 * Everything reaches memory through x86_fpu_mrd/mwr, the interpreter's
 * own checks (limit, #AC, paging), and faults longjmp out through
 * x86_fault like any other instruction's: nothing here commits FPU state
 * before its last memory access.
 */
#include "x86.h"
#include "x86_decode.h"
#include "x86_fpu.h"

#include "softfloat/platform.h"
#include "internals.h"
#include "specialize.h"

#include <string.h>

#define F (c->fpu)

enum {
    SW_IE = 0x0001, SW_DE = 0x0002, SW_ZE = 0x0004, SW_OE = 0x0008,
    SW_UE = 0x0010, SW_PE = 0x0020, SW_SF = 0x0040, SW_ES = 0x0080,
    SW_C0 = 0x0100, SW_C1 = 0x0200, SW_C2 = 0x0400, SW_C3 = 0x4000,
    SW_B  = 0x8000,
};
#define SW_CC (SW_C0 | SW_C1 | SW_C2 | SW_C3)

/* ---- values ------------------------------------------------------------ */

typedef extFloat80_t fx;

static inline fx mk(uint16_t sexp, uint64_t sig) { fx v; v.signExp = sexp; v.signif = sig; return v; }
static const fx INDEF = { UINT64_C(0xC000000000000000), 0xFFFF };
#define FX_ZERO(s)  mk((uint16_t)((s) ? 0x8000 : 0), 0)
#define FX_INF(s)   mk((uint16_t)((s) ? 0xFFFF : 0x7FFF), UINT64_C(0x8000000000000000))
#define FX_ONE      mk(0x3FFF, UINT64_C(0x8000000000000000))

enum { K_ZERO, K_NORMAL, K_DENORMAL, K_INF, K_QNAN, K_SNAN, K_UNSUP };

static int klass(fx v) {
    unsigned e = v.signExp & 0x7FFF;
    uint64_t s = v.signif;
    if (e == 0) return s == 0 ? K_ZERO : K_DENORMAL;        /* J set: a pseudo-denormal, a denormal all the same */
    if (!(s >> 63)) return K_UNSUP;                          /* unnormal, pseudo-infinity, pseudo-NaN */
    if (e == 0x7FFF) {
        if (!(s << 1)) return K_INF;
        return (s >> 62) & 1 ? K_QNAN : K_SNAN;
    }
    return K_NORMAL;
}
static inline int sign_of(fx v) { return v.signExp >> 15; }
static inline int is_nan(int k) { return k == K_QNAN || k == K_SNAN; }

/* A pseudo-denormal (exponent 0, J set) is worth 2^-16382 x 1.f, the same
 * as exponent 1: say so before SoftFloat sees it. */
static inline fx canon(fx v) {
    if (!(v.signExp & 0x7FFF) && (v.signif >> 63)) v.signExp |= 1;
    return v;
}
static inline fx quiet(fx v) { v.signif |= UINT64_C(0x4000000000000000); return v; }

/* ---- the stack ---------------------------------------------------------- */

static inline int top(const x86_cpu *c) { return (F.sw >> 11) & 7; }
static inline void set_top(x86_cpu *c, int t) { F.sw = (uint16_t)((F.sw & ~0x3800) | ((t & 7) << 11)); }
static inline int phys(const x86_cpu *c, int i) { return (top(c) + i) & 7; }
static inline int empty(const x86_cpu *c, int i) { return (F.empty >> phys(c, i)) & 1; }
static inline fx st(const x86_cpu *c, int i) { int p = phys(c, i); return mk(F.sexp[p], F.sig[p]); }
static inline void set_st(x86_cpu *c, int i, fx v) {
    int p = phys(c, i);
    F.sig[p] = v.signif; F.sexp[p] = v.signExp;
    F.empty &= (uint8_t)~(1u << p);
}
static inline void pop(x86_cpu *c) { F.empty |= (uint8_t)(1u << phys(c, 0)); set_top(c, top(c) + 1); }
static inline void push(x86_cpu *c, fx v) { set_top(c, top(c) - 1); set_st(c, 0, v); }

static inline void set_cc(x86_cpu *c, uint16_t cc) { F.sw = (uint16_t)((F.sw & ~SW_CC) | cc); }
static inline void set_c1(x86_cpu *c, int on) { F.sw = (uint16_t)(on ? F.sw | SW_C1 : F.sw & ~SW_C1); }

/* Sticky flags, and ES/B while any of them is unmasked. */
static void flag(x86_cpu *c, uint16_t ex) {
    F.sw |= (uint16_t)(ex & 0x7F);
    if (F.sw & ~F.cw & 0x3F) F.sw |= SW_ES | SW_B;
}
static inline int unmasked(const x86_cpu *c, uint16_t ex) { return (ex & ~F.cw & 0x3F) != 0; }

/* Stack faults: IE with SF, C1 = 1 for overflow. The caller checks
 * whether IE is masked (then it delivers the indefinite) or not. */
static int underflow(x86_cpu *c) { set_c1(c, 0); flag(c, SW_IE | SW_SF); return unmasked(c, SW_IE); }
static int overflow(x86_cpu *c)  { set_c1(c, 1); flag(c, SW_IE | SW_SF); return unmasked(c, SW_IE); }

/* A push: overflow if ST(7) is in use; masked, the indefinite goes in. */
static void push_checked(x86_cpu *c, fx v) {
    if (!empty(c, 7)) { if (overflow(c)) return; v = INDEF; }
    push(c, v);
}

/* ---- SoftFloat's modes and flags ---------------------------------------- */

static const uint8_t rc_mode[4] = { softfloat_round_near_even, softfloat_round_min, softfloat_round_max, softfloat_round_minMag };
static const uint8_t pc_bits[4] = { 32, 80, 64, 80 };     /* 01 is reserved: full precision */

static inline uint8_t rmode(const x86_cpu *c) { return rc_mode[(F.cw >> 10) & 3]; }

static void sf_begin(const x86_cpu *c, int pc) {
    softfloat_roundingMode = rmode(c);
    extF80_roundingPrecision = pc ? pc_bits[(F.cw >> 8) & 3] : 80;
    softfloat_detectTininess = softfloat_tininess_afterRounding;
    softfloat_exceptionFlags = 0;
}
static uint16_t sf_flags(void) {
    uint8_t f = softfloat_exceptionFlags;
    return (uint16_t)(((f & softfloat_flag_invalid) ? SW_IE : 0) | ((f & softfloat_flag_infinite) ? SW_ZE : 0)
                    | ((f & softfloat_flag_overflow) ? SW_OE : 0) | ((f & softfloat_flag_underflow) ? SW_UE : 0)
                    | ((f & softfloat_flag_inexact) ? SW_PE : 0));
}
static inline int same(fx a, fx b) { return a.signExp == b.signExp && a.signif == b.signif; }

/* ---- two-operand arithmetic --------------------------------------------- */

enum { A_ADD, A_MUL, A_COM, A_COMP, A_SUB, A_SUBR, A_DIV, A_DIVR };

static fx sf_op(int op, fx a, fx b) {
    switch (op) {
    case A_ADD:  return extF80_add(a, b);
    case A_MUL:  return extF80_mul(a, b);
    case A_SUB:  return extF80_sub(a, b);
    case A_SUBR: return extF80_sub(b, a);
    case A_DIV:  return extF80_div(a, b);
    default:     return extF80_div(b, a);          /* A_DIVR */
    }
}

/* Scale by 2^n, exactly, where the result stays a normal number. */
static fx scale_exact(fx v, int n) {
    int e = (v.signExp & 0x7FFF) + n;
    return mk((uint16_t)((v.signExp & 0x8000) | (e & 0x7FFF)), v.signif);
}

/* The unmasked OE/UE result: the same rounding, the exponent wrapped by
 * 24576 — computed on operands scaled by that much so the result lands
 * in range. The operand scaled is the one with room for it. */
static void wrap_ops(int op, fx a, fx b, int down, fx *pa, fx *pb) {
    int n = down ? -24576 : 24576;
    fx sa = a, sb = b;
    int ea = a.signExp & 0x7FFF, eb = b.signExp & 0x7FFF;
    int pick_a;
    switch (op) {
    case A_ADD: case A_SUB: case A_SUBR:
        sa = scale_exact(a, n); sb = scale_exact(b, n);
        break;
    case A_MUL:
        pick_a = down ? ea >= eb : ea <= eb;
        if (pick_a) sa = scale_exact(a, n); else sb = scale_exact(b, n);
        break;
    case A_DIV:
        if (down ? ea > 24576 : ea + 24576 < 0x7FFF) sa = scale_exact(a, n); else sb = scale_exact(b, -n);
        break;
    default:   /* A_DIVR: b / a */
        if (down ? eb > 24576 : eb + 24576 < 0x7FFF) sb = scale_exact(b, n); else sa = scale_exact(a, -n);
        break;
    }
    *pa = sa; *pb = sb;
}

/* dst = dst OP src as the x87 does it; returns 0 when an unmasked
 * exception keeps the result from being written. *out gets the result
 * and C1 is set. */
static int arith(x86_cpu *c, int op, fx a, fx b, fx *out) {
    int ka = klass(a), kb = klass(b);
    if (ka == K_UNSUP || kb == K_UNSUP) {
        flag(c, SW_IE); set_c1(c, 0);
        if (unmasked(c, SW_IE)) return 0;
        *out = INDEF; return 1;
    }
    if (is_nan(ka) || is_nan(kb)) {
        sf_begin(c, 1);
        fx r = sf_op(op, a, b);                    /* the 8086 rules pick and quiet the NaN */
        uint16_t ex = sf_flags() & SW_IE;
        set_c1(c, 0);
        flag(c, ex);
        if (unmasked(c, ex)) return 0;
        *out = r; return 1;
    }
    /* a denormal divided by zero is only ZE (Bochs and QEMU agree) */
    int by_zero = (op == A_DIV && kb == K_ZERO) || (op == A_DIVR && ka == K_ZERO);
    if ((ka == K_DENORMAL || kb == K_DENORMAL) && !by_zero) {
        flag(c, SW_DE);
        if (unmasked(c, SW_DE)) { set_c1(c, 0); return 0; }
    }
    a = canon(a); b = canon(b);
    sf_begin(c, 1);
    fx r = sf_op(op, a, b);
    uint16_t ex = sf_flags();
    /* x87 underflow unmasked is any tiny result, exact or not */
    if (!(F.cw & SW_UE) && !(ex & SW_UE) && (r.signExp & 0x7FFF) == 0 && r.signif && !(ex & (SW_IE | SW_ZE)))
        ex |= SW_UE;
    int up = 0;
    if ((ex & SW_PE) && !(ex & (SW_IE | SW_ZE))) {
        uint8_t save = softfloat_exceptionFlags;
        softfloat_roundingMode = softfloat_round_minMag;
        fx t = sf_op(op, a, b);
        up = !same(t, r);
        softfloat_exceptionFlags = save;
    }
    set_c1(c, 0);
    if (unmasked(c, ex & (SW_IE | SW_ZE))) { flag(c, ex & (SW_IE | SW_ZE)); return 0; }
    int wrap_oe = (ex & SW_OE) && unmasked(c, SW_OE), wrap_ue = !wrap_oe && (ex & SW_UE) && unmasked(c, SW_UE);
    if (wrap_oe || wrap_ue) {
        /* the wrapped result is rounded afresh: its own PE and C1 */
        fx sa, sb;
        wrap_ops(op, a, b, wrap_oe, &sa, &sb);
        sf_begin(c, 1);
        r = sf_op(op, sa, sb);
        uint16_t pe = sf_flags() & SW_PE;
        up = 0;
        if (pe) { softfloat_roundingMode = softfloat_round_minMag; up = !same(sf_op(op, sa, sb), r); }
        ex = (uint16_t)((ex & ~(SW_OE | SW_UE | SW_PE)) | (wrap_oe ? SW_OE : SW_UE) | pe);
    }
    set_c1(c, up);
    flag(c, ex);
    *out = r;
    return 1;
}

/* ---- comparisons -------------------------------------------------------- */

/* C3 C2 C0: 000 greater, 001 less, 100 equal, 111 unordered. QUIET: FUCOM
 * (only SNaN and unsupported are invalid); else any NaN is. Returns 0
 * when an unmasked IE or DE stops it (then nothing pops). */
static int compare(x86_cpu *c, fx a, fx b, int quiet_cmp) {
    int ka = klass(a), kb = klass(b);
    uint16_t ex = 0;
    set_c1(c, 0);
    if (ka == K_UNSUP || kb == K_UNSUP || ka == K_SNAN || kb == K_SNAN
        || (!quiet_cmp && (ka == K_QNAN || kb == K_QNAN)))
        ex |= SW_IE;
    else if (!is_nan(ka) && !is_nan(kb) && (ka == K_DENORMAL || kb == K_DENORMAL))
        ex |= SW_DE;
    flag(c, ex);
    if (unmasked(c, ex)) { set_cc(c, SW_C0 | SW_C2 | SW_C3); return 0; }   /* (the SDM: unordered, no pop) */
    uint16_t cc;
    if (ka == K_UNSUP || kb == K_UNSUP || is_nan(ka) || is_nan(kb)) cc = SW_C0 | SW_C2 | SW_C3;
    else {
        a = canon(a); b = canon(b);
        uint8_t save = softfloat_exceptionFlags;
        if (extF80_eq(a, b)) cc = SW_C3;
        else if (extF80_lt_quiet(a, b)) cc = SW_C0;
        else cc = 0;
        softfloat_exceptionFlags = save;
    }
    set_cc(c, cc);
    return 1;
}

/* ---- memory operands ---------------------------------------------------- */

static uint64_t rd_n(x86_cpu *c, const x86_insn *in, uint32_t ea, int n, int align) {
    uint8_t b[8] = { 0 };
    x86_fpu_mrd(c, in, ea, n, align, b);
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | b[i];
    return v;
}
static void wr_n(x86_cpu *c, const x86_insn *in, uint32_t ea, int n, int align, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < n; i++) b[i] = (uint8_t)(v >> (8 * i));
    x86_fpu_mwr(c, in, ea, n, align, b);
}
static fx rd_fx(x86_cpu *c, const x86_insn *in, uint32_t ea) {
    uint8_t b[10];
    x86_fpu_mrd(c, in, ea, 10, 8, b);
    uint64_t s = 0;
    for (int i = 7; i >= 0; i--) s = (s << 8) | b[i];
    return mk((uint16_t)(b[8] | (b[9] << 8)), s);
}
static void fx_bytes(fx v, uint8_t *b) {
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v.signif >> (8 * i));
    b[8] = (uint8_t)v.signExp; b[9] = (uint8_t)(v.signExp >> 8);
}
static void wr_fx(x86_cpu *c, const x86_insn *in, uint32_t ea, fx v) {
    uint8_t b[10];
    fx_bytes(v, b);
    x86_fpu_mwr(c, in, ea, 10, 8, b);
}

/* A memory operand as an 80-bit value: m32/m64 real (with their own
 * denormal and SNaN checks), m16/m32 integer. *ok = 0 when an unmasked
 * exception on the load itself stops the instruction. */
static fx load_mem(x86_cpu *c, const x86_insn *in, uint32_t ea, int kind, int *ok) {
    *ok = 1;
    switch (kind) {
    case 0: {                                          /* m32 real */
        float32_t f; f.v = (uint32_t)rd_n(c, in, ea, 4, 4);
        uint32_t e = (f.v >> 23) & 0xFF, m = f.v & 0x7FFFFF;
        sf_begin(c, 0);
        fx r = f32_to_extF80(f);
        if (e == 0 && m) { flag(c, SW_DE); if (unmasked(c, SW_DE)) *ok = 0; }
        uint16_t ex = sf_flags() & SW_IE;
        if (ex) { flag(c, ex); if (unmasked(c, ex)) *ok = 0; }
        return r;
    }
    case 1: {                                          /* m64 real */
        float64_t f; f.v = rd_n(c, in, ea, 8, 8);
        uint64_t e = (f.v >> 52) & 0x7FF, m = f.v & UINT64_C(0xFFFFFFFFFFFFF);
        sf_begin(c, 0);
        fx r = f64_to_extF80(f);
        if (e == 0 && m) { flag(c, SW_DE); if (unmasked(c, SW_DE)) *ok = 0; }
        uint16_t ex = sf_flags() & SW_IE;
        if (ex) { flag(c, ex); if (unmasked(c, ex)) *ok = 0; }
        return r;
    }
    case 2: return i32_to_extF80((int32_t)(uint32_t)rd_n(c, in, ea, 4, 4));   /* m32 int */
    default: return i32_to_extF80((int16_t)(uint16_t)rd_n(c, in, ea, 2, 2));   /* m16 int */
    }
}

/* ---- conversions out ---------------------------------------------------- */

/* ST(0) to m32/m64 real. Returns 0 if nothing may be stored (unmasked IE,
 * DE, OE or UE); C1 as for any rounding. */
static int to_real(x86_cpu *c, fx v, int dbl, uint64_t *bits) {
    int k = klass(v);
    set_c1(c, 0);
    if (k == K_UNSUP) {
        flag(c, SW_IE);
        if (unmasked(c, SW_IE)) return 0;
        *bits = dbl ? UINT64_C(0xFFF8000000000000) : 0xFFC00000u;
        return 1;
    }
    v = canon(v);
    sf_begin(c, 0);
    uint64_t r = dbl ? extF80_to_f64(v).v : extF80_to_f32(v).v;
    uint16_t ex = sf_flags();
    if (unmasked(c, SW_UE) && !(ex & SW_UE) && k != K_ZERO && !is_nan(k)) {
        /* x87 underflow unmasked: any tiny result, exact or not */
        uint64_t e = dbl ? (r >> 52) & 0x7FF : (r >> 23) & 0xFF;
        if (e == 0) ex |= SW_UE;
    }
    int up = 0;
    if ((ex & SW_PE) && !(ex & SW_IE)) {
        softfloat_roundingMode = softfloat_round_minMag;
        uint64_t t = dbl ? extF80_to_f64(v).v : extF80_to_f32(v).v;
        up = t != r;
    }
    if (unmasked(c, ex & (SW_IE | SW_OE | SW_UE))) { flag(c, ex & (SW_IE | SW_OE | SW_UE)); return 0; }
    flag(c, ex);
    set_c1(c, up);
    *bits = r;
    return 1;
}

/* ST(0) to a signed integer of N bytes, by CW.RC. Out of range, a NaN or
 * an infinity is IE: the integer indefinite, masked. */
static int to_int(x86_cpu *c, fx v, int n, int64_t *out) {
    int k = klass(v);
    set_c1(c, 0);
    int64_t indef = n == 2 ? INT16_MIN : n == 4 ? INT32_MIN : INT64_MIN;
    if (k == K_UNSUP || is_nan(k) || k == K_INF) {
        flag(c, SW_IE);
        if (unmasked(c, SW_IE)) return 0;
        *out = indef; return 1;
    }
    v = canon(v);
    sf_begin(c, 0);
    int64_t r = extF80_to_i64(v, rmode(c), true);
    uint16_t ex = sf_flags();
    if (!(ex & SW_IE) && ((n == 2 && (r < INT16_MIN || r > INT16_MAX)) || (n == 4 && (r < INT32_MIN || r > INT32_MAX))))
        ex = SW_IE;
    if (ex & SW_IE) {
        flag(c, SW_IE);
        if (unmasked(c, SW_IE)) return 0;
        *out = indef; return 1;
    }
    int up = 0;
    if (ex & SW_PE) {
        int64_t t = extF80_to_i64(v, softfloat_round_minMag, false);
        up = t != r;
    }
    flag(c, ex & SW_PE);
    set_c1(c, up);
    *out = r;
    return 1;
}

/* ---- the exponent and significand as parts ------------------------------ */

/* A finite nonzero value as sig (bit 63 set) x 2^(exp - 16383 - 63). */
static void parts(fx v, int *exp, uint64_t *sig) {
    int e = v.signExp & 0x7FFF;
    uint64_t s = v.signif;
    if (e == 0) e = 1;                                  /* denormals (and pseudo-denormals) */
    while (!(s >> 63)) { s <<= 1; e--; }
    *exp = e; *sig = s;
}

/* ---- FPREM / FPREM1 ----------------------------------------------------- */

static void fprem(x86_cpu *c, int ieee) {
    if (empty(c, 0) || empty(c, 1)) {
        if (underflow(c)) return;
        set_st(c, 0, INDEF); return;
    }
    fx a = st(c, 0), b = st(c, 1);
    int ka = klass(a), kb = klass(b);
    uint16_t cc = 0;
    if (ka == K_UNSUP || kb == K_UNSUP || ka == K_INF || kb == K_ZERO
        || (is_nan(ka) || is_nan(kb))) {
        if (is_nan(ka) || is_nan(kb)) {
            if (ka != K_UNSUP && kb != K_UNSUP) {
                sf_begin(c, 0);
                fx r = extF80_add(a, b);
                uint16_t ex = sf_flags() & SW_IE;
                flag(c, ex);
                set_cc(c, 0);
                if (!unmasked(c, ex)) set_st(c, 0, r);
                return;
            }
        }
        flag(c, SW_IE);
        set_cc(c, 0);
        if (!unmasked(c, SW_IE)) set_st(c, 0, INDEF);
        return;
    }
    if (ka == K_DENORMAL || kb == K_DENORMAL) {
        flag(c, SW_DE);
        if (unmasked(c, SW_DE)) return;
    }
    if (ka == K_ZERO || kb == K_INF) { set_cc(c, 0); return; }   /* ST(0) stands, quotient 0 */
    int ea, eb; uint64_t sa, sb;
    parts(a, &ea, &sa); parts(b, &eb, &sb);
    int d = ea - eb, sign = sign_of(a);
    unsigned __int128 num, den = sb, q, r;
    int rexp;
    if (d >= 64) {                                     /* partial: reduce by 32..63, C2 says more to do */
        int n = (d & 31) | 32;
        num = (unsigned __int128)sa << n;
        q = num / den; r = num % den;
        rexp = ea - n;
        cc = SW_C2;
    } else if (d >= 0) {
        num = (unsigned __int128)sa << d;
        q = num / den; r = num % den;
        rexp = eb;
        if (ieee && (2 * r > den || (2 * r == den && (q & 1)))) { q++; r = den - r; sign ^= 1; }
    } else {
        q = 0; r = sa; rexp = ea;
        if (ieee && d == -1 && (unsigned __int128)sa > den) {  /* |a| > |b|/2: one more b */
            q = 1; r = 2 * den - sa; rexp = ea; sign ^= 1;
        }
    }
    if (!(cc & SW_C2)) {
        if (q & 4) cc |= SW_C0;
        if (q & 2) cc |= SW_C3;
        if (q & 1) cc |= SW_C1;
    }
    fx res;
    if (r == 0) res = FX_ZERO(sign_of(a));
    else {
        sf_begin(c, 0);
        res = softfloat_normRoundPackToExtF80(sign, rexp, (uint64_t)r, 0, 80);   /* exact */
        uint16_t ex = sf_flags() & SW_UE;
        if (!(ex) && unmasked(c, SW_UE) && !(res.signExp & 0x7FFF)) ex = SW_UE;
        if (ex) { flag(c, ex); if (unmasked(c, ex)) res = softfloat_normRoundPackToExtF80(sign, rexp + 24576, (uint64_t)r, 0, 80); }
    }
    set_cc(c, cc);
    set_st(c, 0, res);
}

/* ---- FSCALE, FXTRACT, FRNDINT, FSQRT ------------------------------------ */

static void fscale(x86_cpu *c) {
    if (empty(c, 0) || empty(c, 1)) { if (underflow(c)) return; set_st(c, 0, INDEF); return; }
    fx a = st(c, 0), b = st(c, 1);
    int ka = klass(a), kb = klass(b);
    set_c1(c, 0);
    if (ka == K_UNSUP || kb == K_UNSUP) { flag(c, SW_IE); if (!unmasked(c, SW_IE)) set_st(c, 0, INDEF); return; }
    if (is_nan(ka) || is_nan(kb)) {
        sf_begin(c, 0);
        fx r = extF80_add(a, b);
        uint16_t ex = sf_flags() & SW_IE;
        flag(c, ex);
        if (!unmasked(c, ex)) set_st(c, 0, r);
        return;
    }
    if (ka == K_DENORMAL || kb == K_DENORMAL) { flag(c, SW_DE); if (unmasked(c, SW_DE)) return; }
    if (kb == K_INF) {
        int neg = sign_of(b);
        if ((ka == K_ZERO && !neg) || (ka == K_INF && neg)) {
            flag(c, SW_IE); if (!unmasked(c, SW_IE)) set_st(c, 0, INDEF); return;
        }
        if (ka == K_ZERO || ka == K_INF) return;
        set_st(c, 0, neg ? FX_ZERO(sign_of(a)) : FX_INF(sign_of(a)));
        return;
    }
    if (ka == K_ZERO || ka == K_INF) return;
    /* the scale: ST(1) toward zero, clamped well past any result's range */
    int64_t n;
    int eb = b.signExp & 0x7FFF;
    if (kb == K_ZERO || eb < 0x3FFF) n = 0;
    else if (eb > 0x3FFF + 20) n = sign_of(b) ? -0x100000 : 0x100000;
    else { n = (int64_t)(canon(b).signif >> (63 - (eb - 0x3FFF))); if (sign_of(b)) n = -n; }
    int ea; uint64_t sa;
    parts(a, &ea, &sa);
    sf_begin(c, 0);
    fx r = softfloat_normRoundPackToExtF80(sign_of(a), (int32_t)(ea + n), sa, 0, 80);
    uint16_t ex = sf_flags();
    if (!(ex & SW_UE) && unmasked(c, SW_UE) && !(r.signExp & 0x7FFF)) ex |= SW_UE;
    int up = 0;
    if (ex & SW_PE) {
        softfloat_roundingMode = softfloat_round_minMag;
        fx t = softfloat_normRoundPackToExtF80(sign_of(a), (int32_t)(ea + n), sa, 0, 80);
        up = !same(t, r);
    }
    int wrap_oe = (ex & SW_OE) && unmasked(c, SW_OE), wrap_ue = !wrap_oe && (ex & SW_UE) && unmasked(c, SW_UE);
    if (wrap_oe || wrap_ue) {
        int32_t we = (int32_t)(ea + n + (wrap_oe ? -24576 : 24576));
        sf_begin(c, 0);
        r = softfloat_normRoundPackToExtF80(sign_of(a), we, sa, 0, 80);
        uint16_t pe = sf_flags() & SW_PE;
        up = 0;
        if (pe) { softfloat_roundingMode = softfloat_round_minMag; up = !same(softfloat_normRoundPackToExtF80(sign_of(a), we, sa, 0, 80), r); }
        ex = (uint16_t)((ex & ~(SW_OE | SW_UE | SW_PE)) | (wrap_oe ? SW_OE : SW_UE) | pe);
    }
    flag(c, ex);
    set_c1(c, up);
    set_st(c, 0, r);
}

static void fxtract(x86_cpu *c) {
    if (empty(c, 0)) {
        if (underflow(c)) return;
        set_st(c, 0, INDEF); push_checked(c, INDEF); return;
    }
    if (!empty(c, 7)) {
        if (overflow(c)) return;
        set_st(c, 0, INDEF); push(c, INDEF); return;
    }
    fx a = st(c, 0);
    int k = klass(a);
    set_c1(c, 0);
    fx e, s;
    switch (k) {
    case K_UNSUP:
        flag(c, SW_IE); if (unmasked(c, SW_IE)) return;
        e = s = INDEF; break;
    case K_SNAN: case K_QNAN:
        if (k == K_SNAN) { flag(c, SW_IE); if (unmasked(c, SW_IE)) return; }
        e = s = quiet(a); break;
    case K_ZERO:
        flag(c, SW_ZE); if (unmasked(c, SW_ZE)) return;
        e = FX_INF(1); s = a; break;
    case K_INF:
        e = FX_INF(0); s = a; break;
    default: {
        if (k == K_DENORMAL) { flag(c, SW_DE); if (unmasked(c, SW_DE)) return; }
        int ea; uint64_t sa;
        parts(a, &ea, &sa);
        e = i32_to_extF80(ea - 16383);
        s = mk((uint16_t)((a.signExp & 0x8000) | 0x3FFF), sa);
        break;
    }
    }
    set_st(c, 0, e);
    push(c, s);
}

static void frndint(x86_cpu *c) {
    if (empty(c, 0)) { if (underflow(c)) return; set_st(c, 0, INDEF); return; }
    fx a = st(c, 0);
    int k = klass(a);
    set_c1(c, 0);
    if (k == K_UNSUP) { flag(c, SW_IE); if (!unmasked(c, SW_IE)) set_st(c, 0, INDEF); return; }
    if (k == K_DENORMAL) { flag(c, SW_DE); if (unmasked(c, SW_DE)) return; }
    a = canon(a);
    sf_begin(c, 0);
    fx r = extF80_roundToInt(a, rmode(c), true);
    uint16_t ex = sf_flags();
    if (unmasked(c, ex & SW_IE)) { flag(c, ex); return; }
    int up = 0;
    if (ex & SW_PE) {
        fx t = extF80_roundToInt(a, softfloat_round_minMag, false);
        up = !same(t, r);
    }
    flag(c, ex);
    set_c1(c, up);
    set_st(c, 0, r);
}

static void fsqrt(x86_cpu *c) {
    if (empty(c, 0)) { if (underflow(c)) return; set_st(c, 0, INDEF); return; }
    fx a = st(c, 0);
    int k = klass(a);
    set_c1(c, 0);
    if (k == K_UNSUP) { flag(c, SW_IE); if (!unmasked(c, SW_IE)) set_st(c, 0, INDEF); return; }
    if (k == K_DENORMAL) { flag(c, SW_DE); if (unmasked(c, SW_DE)) return; }
    a = canon(a);
    sf_begin(c, 1);
    fx r = extF80_sqrt(a);
    uint16_t ex = sf_flags();
    if (unmasked(c, ex & SW_IE)) { flag(c, ex); return; }
    int up = 0;
    if ((ex & SW_PE) && !(ex & SW_IE)) {
        softfloat_roundingMode = softfloat_round_minMag;
        fx t = extF80_sqrt(a);
        up = !same(t, r);
    }
    flag(c, ex);
    set_c1(c, up);
    set_st(c, 0, r);
}

/* ---- transcendentals, in 128 bits --------------------------------------- */

typedef float128_t fq;

static fq q_i(int64_t v) { return i64_to_f128(v); }
static fq q_add(fq a, fq b) { return f128_add(a, b); }
static fq q_sub(fq a, fq b) { return f128_sub(a, b); }
static fq q_mul(fq a, fq b) { return f128_mul(a, b); }
static fq q_div(fq a, fq b) { return f128_div(a, b); }
static int q_zero(fq a) { return !(a.v[1] & UINT64_C(0x7FFFFFFFFFFFFFFF)) && !a.v[0]; }
static fq q_neg(fq a) { a.v[1] ^= UINT64_C(0x8000000000000000); return a; }
static int q_sign(fq a) { return (int)(a.v[1] >> 63); }
static fq q_abs(fq a) { a.v[1] &= UINT64_C(0x7FFFFFFFFFFFFFFF); return a; }
static int q_exp(fq a) { return (int)((a.v[1] >> 48) & 0x7FFF) - 16383; }
static int q_lt(fq a, fq b) { return f128_lt(a, b); }
/* below 2^-120 of a reference: the series is done */
static int q_tiny(fq t, fq ref) { return q_zero(t) || q_exp(t) < q_exp(ref) - 120; }

/* atanh-style series: sum x^(2k+1)/(2k+1) */
static fq q_atanh_series(fq x, int alternate) {
    fq x2 = q_mul(x, x), term = x, sum = x;
    for (int k = 1; k < 200; k++) {
        term = q_mul(term, x2);
        fq t = q_div(term, q_i(2 * k + 1));
        if (alternate && (k & 1)) t = q_neg(t);
        if (q_tiny(t, sum)) break;
        sum = q_add(sum, t);
    }
    return sum;
}
static fq q_atan_small(fq x) { return q_atanh_series(x, 1); }

static fq Q_PI, Q_PI2, Q_LN2, Q_ONE, Q_TWO, Q_HALF;
static fq P66_HI, P66_LO;           /* pi/2 from a 66-bit pi, in two exact parts */
static int q_ready;

static fq q_pow2(int n) { fq v; v.v[1] = (uint64_t)(16383 + n) << 48; v.v[0] = 0; return v; }

static void q_init(void) {
    if (q_ready) return;
    uint8_t rm = softfloat_roundingMode, fl = softfloat_exceptionFlags;
    softfloat_roundingMode = softfloat_round_near_even;
    Q_ONE = q_i(1); Q_TWO = q_i(2); Q_HALF = q_div(Q_ONE, Q_TWO);
    /* pi = 16 atan(1/5) - 4 atan(1/239); ln 2 = 2 atanh(1/3) */
    Q_PI = q_sub(q_mul(q_i(16), q_atan_small(q_div(Q_ONE, q_i(5)))),
                 q_mul(q_i(4), q_atan_small(q_div(Q_ONE, q_i(239)))));
    Q_PI2 = q_div(Q_PI, Q_TWO);
    Q_LN2 = q_mul(Q_TWO, q_atanh_series(q_div(Q_ONE, q_i(3)), 0));
    /* The x87's pi: 66 bits, the top 64 of pi's significand and the next
     * two (11; the bit after them is 0, so this is also pi rounded to 66).
     * pi/2 = N66 x 2^-65, split 33 + 33 so k x each part is exact. */
    unsigned __int128 n66 = ((unsigned __int128)UINT64_C(0xC90FDAA22168C234) << 2) | 3;
    P66_HI = q_mul(q_i((int64_t)(uint64_t)(n66 >> 33)), q_pow2(-32));
    P66_LO = q_mul(q_i((int64_t)(uint64_t)(n66 & ((UINT64_C(1) << 33) - 1))), q_pow2(-65));
    softfloat_roundingMode = rm; softfloat_exceptionFlags = fl;
    q_ready = 1;
}

/* sin and cos of |r| <= pi/4 */
static void q_sincos_small(fq r, fq *s, fq *co) {
    fq r2 = q_mul(r, r), t = r, sum = r;
    for (int k = 1; k < 60; k++) {
        t = q_neg(q_div(q_mul(t, r2), q_i((2 * k) * (2 * k + 1))));
        if (q_tiny(t, sum)) break;
        sum = q_add(sum, t);
    }
    *s = sum;
    t = Q_ONE; sum = Q_ONE;
    for (int k = 1; k < 60; k++) {
        t = q_neg(q_div(q_mul(t, r2), q_i((2 * k - 1) * (2 * k))));
        if (q_tiny(t, sum)) break;
        sum = q_add(sum, t);
    }
    *co = sum;
}

/* sin and cos of x, |x| < 2^63, reduced by the x87's pi/2 */
static void q_sincos(fq x, fq *s, fq *co) {
    fq pi2 = q_add(P66_HI, P66_LO);
    fq kq = f128_roundToInt(q_div(x, pi2), softfloat_round_near_even, false);
    fq r = q_sub(q_sub(x, q_mul(kq, P66_HI)), q_mul(kq, P66_LO));
    int64_t k = f128_to_i64(kq, softfloat_round_near_even, false);
    fq sr, cr;
    q_sincos_small(r, &sr, &cr);
    switch (k & 3) {
    case 0: *s = sr; *co = cr; break;
    case 1: *s = cr; *co = q_neg(sr); break;
    case 2: *s = q_neg(sr); *co = q_neg(cr); break;
    default: *s = q_neg(cr); *co = sr; break;
    }
}

/* atan2(y, x) for finite x, y not both zero */
static fq q_atan2(fq y, fq x) {
    fq ay = q_abs(y), ax = q_abs(x), t;
    int swap = q_lt(ax, ay);
    t = swap ? q_div(ax, ay) : q_div(ay, ax);          /* 0 <= t <= 1 */
    /* halve the angle three times: atan t = 2 atan(t / (1 + sqrt(1 + t^2))) */
    for (int i = 0; i < 3; i++) t = q_div(t, q_add(Q_ONE, f128_sqrt(q_add(Q_ONE, q_mul(t, t)))));
    fq a = q_mul(q_i(8), q_atan_small(t));
    if (swap) a = q_sub(Q_PI2, a);
    if (q_sign(x)) a = q_sub(Q_PI, a);
    return q_sign(y) ? q_neg(a) : a;
}

/* ln of x > 0 */
static fq q_ln(fq x) {
    int bias = 0;
    if (!((x.v[1] >> 48) & 0x7FFF)) { x = q_mul(x, q_pow2(256)); bias = -256; }   /* a subnormal: an 80-bit denormal is one here too */
    int e = q_exp(x) + bias;
    fq m = x;
    m.v[1] = (m.v[1] & ~(UINT64_C(0x7FFF) << 48)) | ((uint64_t)16383 << 48);   /* 1 <= m < 2 */
    if (q_lt(q_mul(m, m), Q_TWO) == 0) { m = q_div(m, Q_TWO); e++; }         /* sqrt(1/2) <= m < sqrt 2 */
    fq s = q_div(q_sub(m, Q_ONE), q_add(m, Q_ONE));
    fq l = q_mul(Q_TWO, q_atanh_series(s, 0));
    return q_add(l, q_mul(q_i(e), Q_LN2));
}
/* ln(1 + x), |x| small */
static fq q_ln1p(fq x) {
    fq s = q_div(x, q_add(Q_TWO, x));
    return q_mul(Q_TWO, q_atanh_series(s, 0));
}
/* e^y - 1, |y| < 1 */
static fq q_expm1(fq y) {
    fq t = y, sum = y;
    for (int k = 2; k < 80; k++) {
        t = q_div(q_mul(t, y), q_i(k));
        if (q_tiny(t, sum)) break;
        sum = q_add(sum, t);
    }
    return sum;
}

/* A 128-bit result into an 80-bit register by CW.RC; C1, PE, UE, OE.
 * PE always: the value is an approximation even where it lands exactly
 * on 64 bits (sin x = x for tiny x), and the x87 says so too. */
static int q_out(x86_cpu *c, fq v, fx *out) {
    sf_begin(c, 0);
    fx r = f128_to_extF80(v);
    uint16_t ex = sf_flags() | SW_PE;
    if (!(r.signExp & 0x7FFF) && !q_zero(v)) ex |= SW_UE;          /* tiny, and (always) inexact */
    int up = 0;
    if (softfloat_exceptionFlags & softfloat_flag_inexact) {
        softfloat_roundingMode = softfloat_round_minMag;
        fx t = f128_to_extF80(v);
        up = !same(t, r);
    }
    flag(c, ex);
    if (unmasked(c, ex & (SW_OE | SW_UE))) return 0;
    set_c1(c, up);
    *out = r;
    return 1;
}
static fq to_q(fx v) {
    uint8_t rm = softfloat_roundingMode;
    fq r = extF80_to_f128(canon(v));
    softfloat_roundingMode = rm;
    return r;
}

/* An operand check for the one-operand transcendentals: 1 = go on */
static int trans_arg(x86_cpu *c, fx a, int *k) {
    *k = klass(a);
    set_c1(c, 0);
    if (*k == K_UNSUP) { flag(c, SW_IE); if (!unmasked(c, SW_IE)) set_st(c, 0, INDEF); return 0; }
    if (is_nan(*k)) {
        if (*k == K_SNAN) { flag(c, SW_IE); if (unmasked(c, SW_IE)) return 0; }
        set_st(c, 0, quiet(a));
        return 0;
    }
    if (*k == K_DENORMAL) { flag(c, SW_DE); if (unmasked(c, SW_DE)) return 0; }
    return 1;
}

/* FSIN, FCOS, FSINCOS, FPTAN */
enum { T_SIN, T_COS, T_SINCOS, T_TAN };
static void ftrig(x86_cpu *c, int which) {
    int two = which == T_SINCOS || which == T_TAN;
    if (empty(c, 0)) {
        if (underflow(c)) return;
        set_st(c, 0, INDEF);
        if (two) push_checked(c, INDEF);
        return;
    }
    if (two && !empty(c, 7)) {
        if (overflow(c)) return;
        set_st(c, 0, INDEF); push(c, INDEF); return;
    }
    fx a = st(c, 0);
    int k;
    if (klass(a) == K_INF) {
        set_c1(c, 0);
        F.sw &= (uint16_t)~SW_C2;
        flag(c, SW_IE);
        if (unmasked(c, SW_IE)) return;
        set_st(c, 0, INDEF);
        if (two) push(c, INDEF);
        return;
    }
    if (!trans_arg(c, a, &k)) {
        F.sw &= (uint16_t)~SW_C2;
        if (two && is_nan(k) && !(k == K_SNAN && unmasked(c, SW_IE))) push(c, st(c, 0));
        if (two && k == K_UNSUP && !unmasked(c, SW_IE)) push(c, INDEF);
        return;
    }
    if (k != K_ZERO && (a.signExp & 0x7FFF) >= 0x3FFF + 63) { F.sw |= SW_C2; return; }   /* out of range: untouched */
    F.sw &= (uint16_t)~SW_C2;
    q_init();
    fx r0, r1;
    if (k == K_ZERO) {
        switch (which) {
        case T_SIN: set_st(c, 0, a); return;
        case T_COS: set_st(c, 0, FX_ONE); return;
        case T_SINCOS: set_st(c, 0, a); push(c, FX_ONE); return;
        default: set_st(c, 0, a); push(c, FX_ONE); return;
        }
    }
    fq s, co;
    q_sincos(to_q(a), &s, &co);
    switch (which) {
    case T_SIN: if (q_out(c, s, &r0)) set_st(c, 0, r0); break;
    case T_COS: if (q_out(c, co, &r0)) set_st(c, 0, r0); break;
    case T_SINCOS:
        if (!q_out(c, s, &r0)) break;
        if (!q_out(c, co, &r1)) break;
        set_st(c, 0, r0); push(c, r1);
        break;
    default:
        if (!q_out(c, q_div(s, co), &r0)) break;
        set_st(c, 0, r0); push(c, FX_ONE);
        break;
    }
}

/* F2XM1: 2^x - 1 */
static void f2xm1(x86_cpu *c) {
    if (empty(c, 0)) { if (underflow(c)) return; set_st(c, 0, INDEF); return; }
    fx a = st(c, 0);
    int k;
    if (!trans_arg(c, a, &k)) return;
    if (k == K_ZERO) return;
    if (k == K_INF) { set_st(c, 0, sign_of(a) ? mk(0xBFFF, UINT64_C(0x8000000000000000)) : a); return; }
    if ((a.signExp & 0x7FFF) == 0x3FFF && a.signif == UINT64_C(0x8000000000000000)) {
        flag(c, SW_PE);                         /* 2^1 - 1 and 2^-1 - 1: exact values, reported inexact */
        set_st(c, 0, sign_of(a) ? mk(0xBFFE, UINT64_C(0x8000000000000000)) : FX_ONE);
        return;
    }
    q_init();
    fx r;
    if (q_out(c, q_expm1(q_mul(to_q(a), Q_LN2)), &r)) set_st(c, 0, r);
}

/* FPATAN: ST(1) = atan(ST(1) / ST(0)), pop */
static void fpatan(x86_cpu *c) {
    if (empty(c, 0) || empty(c, 1)) {
        if (underflow(c)) return;
        set_st(c, 1, INDEF); pop(c); return;
    }
    fx x = st(c, 0), y = st(c, 1);
    int kx = klass(x), ky = klass(y);
    set_c1(c, 0);
    fx r;
    if (kx == K_UNSUP || ky == K_UNSUP) { flag(c, SW_IE); if (unmasked(c, SW_IE)) return; r = INDEF; }
    else if (is_nan(kx) || is_nan(ky)) {
        sf_begin(c, 0);
        r = extF80_add(y, x);
        uint16_t ex = sf_flags() & SW_IE;
        flag(c, ex);
        if (unmasked(c, ex)) return;
    } else {
        if (kx == K_DENORMAL || ky == K_DENORMAL) { flag(c, SW_DE); if (unmasked(c, SW_DE)) return; }
        q_init();
        int sx = sign_of(x), sy = sign_of(y);
        fq v;
        if (ky == K_ZERO && kx != K_INF && kx != K_ZERO) v = sx ? Q_PI : q_i(0);
        else if (ky == K_ZERO) v = kx == K_INF ? (sx ? Q_PI : q_i(0)) : (sx ? Q_PI : q_i(0));
        else if (ky == K_INF && kx == K_INF) v = sx ? q_mul(q_i(3), q_div(Q_PI, q_i(4))) : q_div(Q_PI, q_i(4));
        else if (ky == K_INF) v = Q_PI2;
        else if (kx == K_INF) v = sx ? Q_PI : q_i(0);
        else if (kx == K_ZERO) v = Q_PI2;
        else {
            v = q_abs(q_atan2(to_q(y), to_q(x)));
            /* |y/x| below even 128-bit range: the angle is that small, not 0 */
            if (q_zero(v)) { v.v[1] = 0; v.v[0] = 1; }
        }
        if (q_zero(v)) r = FX_ZERO(sy);
        else {
            if (sy) v = q_neg(v);
            if (!q_out(c, v, &r)) return;
        }
    }
    set_st(c, 1, r);
    pop(c);
}

/* FYL2X: ST(1) = ST(1) x log2 ST(0); FYL2XP1: x log2(ST(0) + 1). Pop. */
static void fyl2x(x86_cpu *c, int p1) {
    if (empty(c, 0) || empty(c, 1)) {
        if (underflow(c)) return;
        set_st(c, 1, INDEF); pop(c); return;
    }
    fx x = st(c, 0), y = st(c, 1);
    int kx = klass(x), ky = klass(y);
    set_c1(c, 0);
    fx r;
    if (kx == K_UNSUP || ky == K_UNSUP) { flag(c, SW_IE); if (unmasked(c, SW_IE)) return; r = INDEF; goto done; }
    if (is_nan(kx) || is_nan(ky)) {
        sf_begin(c, 0);
        r = extF80_add(y, x);
        uint16_t ex = sf_flags() & SW_IE;
        flag(c, ex);
        if (unmasked(c, ex)) return;
        goto done;
    }
    q_init();
    {
        /* the sign and size of log2 of the argument: -1, 0 (log is 0), +1;
         * NEGARG: the argument is below 0 (log undefined); ZEROARG: log is -inf */
        int sx = sign_of(x), sy = sign_of(y);
        fq ly = q_i(0);                         /* log2 of the argument */
        int lsign = 0, lzero = 0, linf = 0, invalid = 0, zdiv = 0;
        if (!p1) {
            if (sx && kx != K_ZERO) invalid = 1;
            else if (kx == K_ZERO) { linf = 1; lsign = 1; }
            else if (kx == K_INF) { linf = 1; lsign = 0; }
            else {
                ly = q_div(q_ln(to_q(x)), Q_LN2);
                lzero = q_zero(ly); lsign = q_sign(ly);
            }
        } else {
            if (kx == K_ZERO) { lzero = 1; lsign = sx; }
            else if (kx == K_INF) { if (sx) invalid = 1; else { linf = 1; lsign = 0; } }
            else {
                fq xq = to_q(x);
                if (q_lt(xq, q_neg(Q_ONE))) invalid = 1;
                else if (f128_eq(xq, q_neg(Q_ONE))) { linf = 1; lsign = 1; }
                else { ly = q_div(q_ln1p(xq), Q_LN2); lzero = q_zero(ly); lsign = q_sign(ly); }
            }
        }
        if (!invalid) {
            if (lzero && ky == K_INF) invalid = 1;
            if (linf && ky == K_ZERO) invalid = 1;
        }
        if (!invalid && !p1 && kx == K_ZERO && ky != K_INF) zdiv = 1;
        if (invalid) { flag(c, SW_IE); if (unmasked(c, SW_IE)) return; r = INDEF; goto done; }
        if (zdiv) { flag(c, SW_ZE); if (unmasked(c, SW_ZE)) return; }
        /* denormal operands count only where there is arithmetic to do
         * (Bochs and QEMU agree: log 0 or an invalid case is ZE or IE alone) */
        else if (kx == K_DENORMAL || ky == K_DENORMAL) { flag(c, SW_DE); if (unmasked(c, SW_DE)) return; }
        int rsign = sy ^ lsign;
        if (linf || ky == K_INF) r = FX_INF(rsign);
        else if (lzero || ky == K_ZERO) r = FX_ZERO(rsign);
        else {
            fq prod = q_mul(to_q(y), ly);
            /* below even 128-bit range: still a nonzero result, rounded as one */
            if (q_zero(prod)) { prod.v[1] = (uint64_t)rsign << 63; prod.v[0] = 1; }
            if (!q_out(c, prod, &r)) return;
        }
    }
done:
    set_st(c, 1, r);
    pop(c);
}

/* ---- environment -------------------------------------------------------- */

static int tag_of(const x86_cpu *c, int p) {
    if ((F.empty >> p) & 1) return 3;
    switch (klass(mk(F.sexp[p], F.sig[p]))) {
    case K_ZERO: return 1;
    case K_NORMAL: return 0;
    default: return 2;
    }
}
static uint16_t tag_word(const x86_cpu *c) {
    uint16_t t = 0;
    for (int p = 0; p < 8; p++) t |= (uint16_t)(tag_of(c, p) << (2 * p));
    return t;
}

static void finit(x86_cpu *c) {
    F.cw = 0x037F; F.sw = 0; F.empty = 0xFF;
    F.fop = 0; F.fip = F.fdp = 0; F.fcs = F.fds = 0;
    F.ferr = 0;
}

void x86_fpu_finit(x86_cpu *c) { finit(c); }

void x86_fpu_reset(x86_cpu *c) {
    memset(&F, 0, sizeof F);
    F.cw = 0x0040;                           /* the 387 and 486 at RESET; the BIOS's FNINIT makes it 037F */
}

static int real_fmt(const x86_cpu *c) { return !c->pmode || (c->eflags & X86_VM); }

static void put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *b, uint32_t v) { for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t get32(const uint8_t *b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }

/* The environment image: 14 bytes (16-bit) or 28 (32-bit). The real-mode
 * layouts carry 20-bit (32-bit: 32-bit) linear pointers, CS/DS x 16 +
 * offset; reserved halves of the 32-bit layouts read FFFF. */
static int env_image(const x86_cpu *c, int os32, uint8_t *b) {
    uint16_t tw = tag_word(c);
    if (!real_fmt(c)) {
        if (os32) {
            put32(b + 0, 0xFFFF0000u | F.cw); put32(b + 4, 0xFFFF0000u | F.sw); put32(b + 8, 0xFFFF0000u | tw);
            put32(b + 12, F.fip);
            put32(b + 16, F.fcs | ((uint32_t)(F.fop & 0x7FF) << 16));
            put32(b + 20, F.fdp);
            put32(b + 24, 0xFFFF0000u | F.fds);
            return 28;
        }
        put16(b + 0, F.cw); put16(b + 2, F.sw); put16(b + 4, tw);
        put16(b + 6, (uint16_t)F.fip); put16(b + 8, F.fcs);
        put16(b + 10, (uint16_t)F.fdp); put16(b + 12, F.fds);
        return 14;
    }
    uint32_t ip = ((uint32_t)F.fcs << 4) + F.fip, dp = ((uint32_t)F.fds << 4) + F.fdp;
    if (os32) {
        put32(b + 0, 0xFFFF0000u | F.cw); put32(b + 4, 0xFFFF0000u | F.sw); put32(b + 8, 0xFFFF0000u | tw);
        put32(b + 12, 0xFFFF0000u | (ip & 0xFFFF));
        put32(b + 16, ((ip & 0xFFFF0000u) >> 4) | (F.fop & 0x7FF));
        put32(b + 20, 0xFFFF0000u | (dp & 0xFFFF));
        put32(b + 24, (dp & 0xFFFF0000u) >> 4);
        return 28;
    }
    put16(b + 0, F.cw); put16(b + 2, F.sw); put16(b + 4, tw);
    put16(b + 6, (uint16_t)ip);
    put16(b + 8, (uint16_t)(((ip & 0xF0000u) >> 4) | (F.fop & 0x7FF)));
    put16(b + 10, (uint16_t)dp);
    put16(b + 12, (uint16_t)((dp & 0xF0000u) >> 4));
    return 14;
}

static void env_load(x86_cpu *c, int os32, const uint8_t *b) {
    uint16_t tw;
    if (os32) { F.cw = get16(b + 0); F.sw = get16(b + 4); tw = get16(b + 8); }
    else { F.cw = get16(b + 0); F.sw = get16(b + 2); tw = get16(b + 4); }
    if (!real_fmt(c)) {
        if (os32) { F.fip = get32(b + 12); F.fcs = get16(b + 16); F.fop = (uint16_t)(get32(b + 16) >> 16) & 0x7FF; F.fdp = get32(b + 20); F.fds = get16(b + 24); }
        else { F.fip = get16(b + 6); F.fcs = get16(b + 8); F.fdp = get16(b + 10); F.fds = get16(b + 12); }
    } else {
        uint32_t ip, dp;
        if (os32) {
            ip = get16(b + 12) | ((get32(b + 16) << 4) & 0xFFFF0000u);
            F.fop = (uint16_t)(get32(b + 16) & 0x7FF);
            dp = get16(b + 20) | ((get32(b + 24) << 4) & 0xFFFF0000u);
        } else {
            ip = get16(b + 6) | (((uint32_t)get16(b + 8) << 4) & 0xF0000u);
            F.fop = (uint16_t)(get16(b + 8) & 0x7FF);
            dp = get16(b + 10) | (((uint32_t)get16(b + 12) << 4) & 0xF0000u);
        }
        F.fcs = 0; F.fip = ip; F.fds = 0; F.fdp = dp;
    }
    F.cw = (uint16_t)((F.cw & ~0xE0C0) | 0x0040);
    F.empty = 0;
    for (int p = 0; p < 8; p++) if (((tw >> (2 * p)) & 3) == 3) F.empty |= (uint8_t)(1u << p);
    if (F.sw & ~F.cw & 0x3F) F.sw |= SW_ES | SW_B; else F.sw &= (uint16_t)~(SW_ES | SW_B);
}

/* ---- BCD ---------------------------------------------------------------- */

static void fbld(x86_cpu *c, const x86_insn *in, uint32_t ea) {
    uint8_t b[10];
    x86_fpu_mrd(c, in, ea, 10, 8, b);
    if (!empty(c, 7)) { if (overflow(c)) return; push(c, INDEF); return; }
    int64_t v = 0;
    for (int i = 8; i >= 0; i--) v = v * 100 + (b[i] >> 4) * 10 + (b[i] & 15);
    fx r = i64_to_extF80(v);
    if (b[9] & 0x80) r.signExp |= 0x8000;
    set_c1(c, 0);
    push(c, r);
}

static int fbstp(x86_cpu *c, const x86_insn *in, uint32_t ea) {
    uint8_t b[10] = { 0 };
    if (empty(c, 0)) {
        if (underflow(c)) return 0;
        memset(b, 0, 7); b[7] = 0xC0; b[8] = 0xFF; b[9] = 0xFF;
        x86_fpu_mwr(c, in, ea, 10, 8, b);
        return 1;
    }
    fx a = st(c, 0);
    int k = klass(a);
    set_c1(c, 0);
    int64_t v = 0;
    int bad = k == K_UNSUP || is_nan(k) || k == K_INF, up = 0;
    uint16_t ex = 0;
    if (!bad) {
        fx ca = canon(a);
        sf_begin(c, 0);
        fx r = extF80_roundToInt(ca, rmode(c), true);
        ex = sf_flags() & SW_PE;
        if (ex) up = !same(extF80_roundToInt(ca, softfloat_round_minMag, false), r);
        v = extF80_to_i64(r, softfloat_round_minMag, false);
        if (softfloat_exceptionFlags & softfloat_flag_invalid) bad = 1;
        uint64_t mag = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;   /* (INT64_MIN has no signed negation) */
        if (mag > UINT64_C(999999999999999999)) bad = 1;
        else v = (int64_t)mag;
    }
    if (bad) {
        flag(c, SW_IE);
        if (unmasked(c, SW_IE)) return 0;
        memset(b, 0, 7); b[7] = 0xC0; b[8] = 0xFF; b[9] = 0xFF;
    } else {
        for (int i = 0; i < 9; i++) { int lo = (int)(v % 10); v /= 10; int hi = (int)(v % 10); v /= 10; b[i] = (uint8_t)((hi << 4) | lo); }
        b[9] = sign_of(a) ? 0x80 : 0;
    }
    x86_fpu_mwr(c, in, ea, 10, 8, b);
    if (!bad) { flag(c, ex); set_c1(c, up); }
    return 1;
}

/* ---- dispatch ----------------------------------------------------------- */

static void pending(x86_cpu *c) {
    if (!(F.sw & SW_ES)) return;
    if (c->cr0 & X86_CR0_NE) x86_fault(c, X86_EXC_MF, 0);
    F.ferr = 1;                                  /* no IRQ 13 yet: the instruction goes on, as with IGNNE# */
}

void x86_fpu_wait(x86_cpu *c) { pending(c); }

static void last_insn(x86_cpu *c, const x86_insn *in, uint32_t ea, uint32_t start_ip) {
    F.fop = (uint16_t)(((in->opcode & 7) << 8) | in->modrm);
    F.fcs = c->seg[S_CS].sel;
    F.fip = start_ip;
    if (in->mod != 3) { F.fds = c->seg[in->seg].sel; F.fdp = ea; }
}

/* ST(dst) = ST(dst) OP v; POPS: pop afterwards (the P forms) */
static void arith_st(x86_cpu *c, int op, int dst, fx a, fx b, int pops) {
    fx r;
    if (!arith(c, op, a, b, &r)) return;
    set_st(c, dst, r);
    if (pops) pop(c);
}

static void ud(x86_cpu *c) { x86_fault(c, X86_EXC_UD, 0); }

void x86_fpu_exec(x86_cpu *c, const x86_insn *in, uint32_t ea, uint32_t start_ip) {
    int esc = in->opcode & 7, reg = in->reg, rm = in->rm, mem = in->mod != 3;
    int os32 = in->opsize == 4;

    /* the control instructions: no pending-exception check (the FN forms),
     * and the last-instruction pointers stay as they were */
    if (mem) {
        if (esc == 1 && reg >= 4) {
            uint8_t b[28];
            switch (reg) {
            case 4:                                            /* FLDENV */
                pending(c);
                x86_fpu_mrd(c, in, ea, os32 ? 28 : 14, os32 ? 4 : 2, b);
                env_load(c, os32, b);
                return;
            case 5: {                                          /* FLDCW */
                pending(c);
                uint16_t cw = (uint16_t)rd_n(c, in, ea, 2, 2);
                F.cw = (uint16_t)((cw & ~0xE0C0) | 0x0040);
                if (F.sw & ~F.cw & 0x3F) F.sw |= SW_ES | SW_B; else F.sw &= (uint16_t)~(SW_ES | SW_B);
                return;
            }
            case 6: {                                          /* FNSTENV */
                int n = env_image(c, os32, b);
                x86_fpu_mwr(c, in, ea, n, os32 ? 4 : 2, b);
                F.cw |= 0x3F;
                return;
            }
            default:                                           /* FNSTCW */
                wr_n(c, in, ea, 2, 2, F.cw);
                return;
            }
        }
        if (esc == 5 && (reg == 4 || reg == 6 || reg == 7)) {
            if (reg == 7) { wr_n(c, in, ea, 2, 2, F.sw); return; }   /* FNSTSW m16 */
            uint8_t b[28 + 80];
            if (reg == 6) {                                    /* FNSAVE */
                int n = env_image(c, os32, b);
                for (int i = 0; i < 8; i++) fx_bytes(st(c, i), b + n + 10 * i);
                x86_fpu_mwr(c, in, ea, n + 80, os32 ? 4 : 2, b);
                finit(c);
                return;
            }
            pending(c);                                        /* FRSTOR */
            int n = os32 ? 28 : 14;
            x86_fpu_mrd(c, in, ea, n + 80, os32 ? 4 : 2, b);
            env_load(c, os32, b);
            for (int i = 0; i < 8; i++) {
                const uint8_t *p = b + n + 10 * i;
                uint64_t s = 0;
                for (int j = 7; j >= 0; j--) s = (s << 8) | p[j];
                int ph = phys(c, i);
                F.sig[ph] = s; F.sexp[ph] = (uint16_t)(p[8] | (p[9] << 8));
            }
            return;
        }
    } else {
        if (esc == 3 && reg == 4) {
            switch (rm) {
            case 0: case 1: case 4: return;                    /* FENI, FDISI, FSETPM: the 8087's and 287's, nothing on a 387 */
            case 2: F.sw &= (uint16_t)~(0x7F | SW_ES | SW_B); F.ferr = 0; return;   /* FNCLEX */
            case 3: finit(c); return;                          /* FNINIT */
            default: ud(c); return;
            }
        }
        if (esc == 7 && reg == 4) {                            /* FNSTSW AX */
            if (rm) ud(c);
            x86_set_r16(c, R_AX, F.sw);
            return;
        }
    }

    /* reserved encodings */
    if (mem) {
        if ((esc == 1 && reg == 1) || (esc == 3 && (reg == 1 || reg == 4 || reg == 6))
            || (esc == 5 && (reg == 1 || reg == 5)) || (esc == 7 && reg == 1))
            ud(c);
    } else {
        if ((esc == 1 && reg == 2 && rm) || (esc == 1 && reg == 4 && (rm == 2 || rm == 3 || rm == 6 || rm == 7))
            || (esc == 1 && reg == 5 && rm == 7) || (esc == 2 && !(reg == 5 && rm == 1))
            || (esc == 3) || (esc == 5 && reg >= 6) || (esc == 6 && reg == 3 && rm != 1)
            || (esc == 7 && reg >= 5))
            ud(c);
    }

    pending(c);

    /* the memory operand, read before anything is committed */
    fx mv = { 0, 0 };
    int mok = 1;
    if (mem) {
        switch (esc) {
        case 0: mv = load_mem(c, in, ea, 0, &mok); break;               /* m32 real */
        case 2: mv = load_mem(c, in, ea, 2, &mok); break;               /* m32 int */
        case 4: mv = load_mem(c, in, ea, 1, &mok); break;               /* m64 real */
        case 6: mv = load_mem(c, in, ea, 3, &mok); break;               /* m16 int */
        default: break;
        }
    }
    last_insn(c, in, ea, start_ip);

    if (mem) {
        switch (esc) {
        case 0: case 2: case 4: case 6:
            if (reg == 2 || reg == 3) {                                  /* FCOM / FCOMP / FICOM / FICOMP */
                if (empty(c, 0)) {
                    if (underflow(c)) return;
                    set_cc(c, SW_C0 | SW_C2 | SW_C3);
                    if (reg == 3) pop(c);
                    return;
                }
                if (!mok) return;
                if (compare(c, st(c, 0), mv, 0) && reg == 3) pop(c);
                return;
            }
            if (empty(c, 0)) { if (underflow(c)) return; set_st(c, 0, INDEF); return; }
            if (!mok) return;
            arith_st(c, reg, 0, st(c, 0), mv, 0);
            return;
        case 1:
            switch (reg) {
            case 0: {                                                   /* FLD m32 */
                int ok;
                fx v = load_mem(c, in, ea, 0, &ok);
                if (!ok) return;
                set_c1(c, 0);
                push_checked(c, v);
                return;
            }
            case 2: case 3: {                                           /* FST / FSTP m32 */
                uint64_t bits;
                if (empty(c, 0)) {
                    if (underflow(c)) return;
                    wr_n(c, in, ea, 4, 4, 0xFFC00000u);
                } else {
                    if (!to_real(c, st(c, 0), 0, &bits)) return;
                    wr_n(c, in, ea, 4, 4, bits);
                }
                if (reg == 3) pop(c);
                return;
            }
            }
            break;
        case 3:
            switch (reg) {
            case 0: {                                                   /* FILD m32 */
                fx v = i32_to_extF80((int32_t)(uint32_t)rd_n(c, in, ea, 4, 4));
                set_c1(c, 0);
                push_checked(c, v);
                return;
            }
            case 2: case 3: {                                           /* FIST / FISTP m32 */
                int64_t v;
                if (empty(c, 0)) { if (underflow(c)) return; v = INT32_MIN; }
                else if (!to_int(c, st(c, 0), 4, &v)) return;
                wr_n(c, in, ea, 4, 4, (uint64_t)v);
                if (reg == 3) pop(c);
                return;
            }
            case 5: {                                                   /* FLD m80 */
                fx v = rd_fx(c, in, ea);
                set_c1(c, 0);
                push_checked(c, v);
                return;
            }
            case 7:                                                     /* FSTP m80 */
                if (empty(c, 0)) { if (underflow(c)) return; wr_fx(c, in, ea, INDEF); }
                else { wr_fx(c, in, ea, st(c, 0)); set_c1(c, 0); }
                pop(c);
                return;
            }
            break;
        case 5:
            switch (reg) {
            case 0: {                                                   /* FLD m64 */
                int ok;
                fx v = load_mem(c, in, ea, 1, &ok);
                if (!ok) return;
                set_c1(c, 0);
                push_checked(c, v);
                return;
            }
            case 2: case 3: {                                           /* FST / FSTP m64 */
                uint64_t bits;
                if (empty(c, 0)) {
                    if (underflow(c)) return;
                    wr_n(c, in, ea, 8, 8, UINT64_C(0xFFF8000000000000));
                } else {
                    if (!to_real(c, st(c, 0), 1, &bits)) return;
                    wr_n(c, in, ea, 8, 8, bits);
                }
                if (reg == 3) pop(c);
                return;
            }
            }
            break;
        case 7:
            switch (reg) {
            case 0: {                                                   /* FILD m16 */
                fx v = i32_to_extF80((int16_t)(uint16_t)rd_n(c, in, ea, 2, 2));
                set_c1(c, 0);
                push_checked(c, v);
                return;
            }
            case 2: case 3: {                                           /* FIST / FISTP m16 */
                int64_t v;
                if (empty(c, 0)) { if (underflow(c)) return; v = INT16_MIN; }
                else if (!to_int(c, st(c, 0), 2, &v)) return;
                wr_n(c, in, ea, 2, 2, (uint64_t)v);
                if (reg == 3) pop(c);
                return;
            }
            case 4: fbld(c, in, ea); return;                            /* FBLD */
            case 5: {                                                   /* FILD m64 */
                fx v = i64_to_extF80((int64_t)rd_n(c, in, ea, 8, 8));
                set_c1(c, 0);
                push_checked(c, v);
                return;
            }
            case 6: if (fbstp(c, in, ea)) pop(c); return;               /* FBSTP */
            case 7: {                                                   /* FISTP m64 */
                int64_t v;
                if (empty(c, 0)) { if (underflow(c)) return; v = INT64_MIN; }
                else if (!to_int(c, st(c, 0), 8, &v)) return;
                wr_n(c, in, ea, 8, 8, (uint64_t)v);
                pop(c);
                return;
            }
            }
            break;
        }
        ud(c);
        return;
    }

    /* register forms */
    int i = rm;
    switch (esc) {
    case 0:                                        /* D8: ST(0) = ST(0) op ST(i) */
    case 4:                                        /* DC: ST(i) = ST(i) op ST(0), SUB/DIV reversed */
    case 6: {                                      /* DE: the same, then pop */
        if (reg == 2 || reg == 3) {                /* FCOM / FCOMP (and DC/DE's aliases) */
            int pops = (esc == 6 && reg == 3) ? 2 : (esc == 6 || reg == 3);   /* DE D9: FCOMPP */
            if (empty(c, 0) || empty(c, i)) {
                if (underflow(c)) return;
                set_cc(c, SW_C0 | SW_C2 | SW_C3);
                while (pops--) pop(c);
                return;
            }
            if (compare(c, st(c, 0), st(c, i), 0)) while (pops--) pop(c);
            return;
        }
        int dst = esc == 0 ? 0 : i;
        int op = reg;
        if (esc != 0) {                            /* DC/DE: E0 is SUBR, E8 SUB, F0 DIVR, F8 DIV */
            if (op == A_SUB) op = A_SUBR; else if (op == A_SUBR) op = A_SUB;
            else if (op == A_DIV) op = A_DIVR; else if (op == A_DIVR) op = A_DIV;
        }
        if (empty(c, 0) || empty(c, i)) {
            if (underflow(c)) return;
            set_st(c, dst, INDEF);
            if (esc == 6) pop(c);
            return;
        }
        arith_st(c, op, dst, st(c, dst), esc == 0 ? st(c, i) : st(c, 0), esc == 6);
        return;
    }
    case 1:
        switch (reg) {
        case 0: {                                  /* FLD ST(i) */
            if (empty(c, i)) { if (underflow(c)) return; push_checked(c, INDEF); return; }
            fx v = st(c, i);
            set_c1(c, 0);
            push_checked(c, v);
            return;
        }
        case 1: {                                  /* FXCH */
            fx a, b;
            if (empty(c, 0) || empty(c, i)) {
                if (underflow(c)) return;
                a = empty(c, 0) ? INDEF : st(c, 0);
                b = empty(c, i) ? INDEF : st(c, i);
            } else { a = st(c, 0); b = st(c, i); set_c1(c, 0); }
            set_st(c, 0, b); set_st(c, i, a);
            return;
        }
        case 2: return;                            /* FNOP */
        case 3:                                    /* FSTP ST(i), the undocumented D9 D8+i */
            goto fstp_i;
        case 4:
            if (empty(c, 0) && rm != 5) { if (underflow(c)) return; if (rm != 4) set_st(c, 0, INDEF); else set_cc(c, SW_C0 | SW_C2 | SW_C3); return; }
            switch (rm) {
            case 0: { fx v = st(c, 0); v.signExp ^= 0x8000; set_st(c, 0, v); set_c1(c, 0); return; }   /* FCHS */
            case 1: { fx v = st(c, 0); v.signExp &= 0x7FFF; set_st(c, 0, v); set_c1(c, 0); return; }   /* FABS */
            case 4: compare(c, st(c, 0), FX_ZERO(0), 0); return;                                        /* FTST */
            default: {                                                                                  /* FXAM */
                fx v = st(c, 0);
                uint16_t cc = sign_of(v) ? SW_C1 : 0;
                if (empty(c, 0)) cc |= SW_C3 | SW_C0;
                else switch (klass(v)) {
                case K_UNSUP: break;
                case K_QNAN: case K_SNAN: cc |= SW_C0; break;
                case K_NORMAL: cc |= SW_C2; break;
                case K_INF: cc |= SW_C2 | SW_C0; break;
                case K_ZERO: cc |= SW_C3; break;
                default: cc |= SW_C3 | SW_C2; break;
                }
                set_cc(c, cc);
                return;
            }
            }
            return;
        case 5: {                                  /* the constants, rounded by RC (387+) */
            static const struct { uint16_t se; uint64_t s; int8_t adj; } k[7] = {
                { 0x3FFF, UINT64_C(0x8000000000000000), 0 },       /* 1 */
                { 0x4000, UINT64_C(0xD49A784BCD1B8AFE), 1 },       /* log2 10: rounds up only by RC up */
                { 0x3FFF, UINT64_C(0xB8AA3B295C17F0BC), -1 },      /* log2 e: near is the round-up */
                { 0x4000, UINT64_C(0xC90FDAA22168C235), -1 },      /* pi */
                { 0x3FFD, UINT64_C(0x9A209A84FBCFF799), -1 },      /* log10 2 */
                { 0x3FFE, UINT64_C(0xB17217F7D1CF79AC), -1 },      /* ln 2 */
                { 0, 0, 0 },                                       /* +0 */
            };
            fx v = mk(k[rm].se, k[rm].s);
            int rc = (F.cw >> 10) & 3;
            if (k[rm].adj > 0 && rc == 2) v.signif++;
            if (k[rm].adj < 0 && (rc == 1 || rc == 3)) v.signif--;
            set_c1(c, 0);
            push_checked(c, v);
            return;
        }
        case 6:
            switch (rm) {
            case 0: f2xm1(c); return;
            case 1: fyl2x(c, 0); return;
            case 2: ftrig(c, T_TAN); return;
            case 3: fpatan(c); return;
            case 4: fxtract(c); return;
            case 5: fprem(c, 1); return;
            case 6: set_top(c, top(c) - 1); set_c1(c, 0); return;   /* FDECSTP */
            default: set_top(c, top(c) + 1); set_c1(c, 0); return;  /* FINCSTP */
            }
        default:
            switch (rm) {
            case 0: fprem(c, 0); return;
            case 1: fyl2x(c, 1); return;
            case 2: fsqrt(c); return;
            case 3: ftrig(c, T_SINCOS); return;
            case 4: frndint(c); return;
            case 5: fscale(c); return;
            case 6: ftrig(c, T_SIN); return;
            default: ftrig(c, T_COS); return;
            }
        }
    case 2:                                        /* DA E9: FUCOMPP */
        if (empty(c, 0) || empty(c, 1)) {
            if (underflow(c)) return;
            set_cc(c, SW_C0 | SW_C2 | SW_C3); pop(c); pop(c); return;
        }
        if (compare(c, st(c, 0), st(c, 1), 1)) { pop(c); pop(c); }
        return;
    case 5:
        switch (reg) {
        case 0: F.empty |= (uint8_t)(1u << phys(c, i)); return;          /* FFREE */
        case 1: {                                                        /* FXCH alias */
            fx a, b;
            if (empty(c, 0) || empty(c, i)) {
                if (underflow(c)) return;
                a = empty(c, 0) ? INDEF : st(c, 0);
                b = empty(c, i) ? INDEF : st(c, i);
            } else { a = st(c, 0); b = st(c, i); set_c1(c, 0); }
            set_st(c, 0, b); set_st(c, i, a);
            return;
        }
        case 2:                                                          /* FST ST(i) */
            if (empty(c, 0)) { if (underflow(c)) return; set_st(c, i, INDEF); return; }
            set_st(c, i, st(c, 0)); set_c1(c, 0);
            return;
        case 3:
        fstp_i:                                                          /* FSTP ST(i) */
            if (empty(c, 0)) { if (underflow(c)) return; set_st(c, i, INDEF); pop(c); return; }
            set_st(c, i, st(c, 0)); set_c1(c, 0); pop(c);
            return;
        default: {                                                       /* FUCOM / FUCOMP */
            if (empty(c, 0) || empty(c, i)) {
                if (underflow(c)) return;
                set_cc(c, SW_C0 | SW_C2 | SW_C3);
                if (reg == 5) pop(c);
                return;
            }
            if (compare(c, st(c, 0), st(c, i), 1) && reg == 5) pop(c);
            return;
        }
        }
    default:                                       /* DF */
        switch (reg) {
        case 0: F.empty |= (uint8_t)(1u << phys(c, i)); pop(c); return; /* FFREEP */
        case 1: {                                                        /* FXCH alias */
            fx a, b;
            if (empty(c, 0) || empty(c, i)) {
                if (underflow(c)) return;
                a = empty(c, 0) ? INDEF : st(c, 0);
                b = empty(c, i) ? INDEF : st(c, i);
            } else { a = st(c, 0); b = st(c, i); set_c1(c, 0); }
            set_st(c, 0, b); set_st(c, i, a);
            return;
        }
        default: goto fstp_i;                                            /* FSTP aliases (DF D0+i, D8+i) */
        }
    }
}
