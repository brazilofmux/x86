/* x86_sse.c — SSE (the Pentium III's) and SSE2 (the Pentium 4's): XMM0-7
 * and MXCSR, in the x87's state (x86_fpu.h) beside what FXSAVE covers.
 *
 * One decoded op (OP_SSE) per 0F opcode; the prefix picks the form: none
 * packed single, F3 scalar single, 66 packed double (or, on the MMX
 * opcodes, the 128-bit integer form), F2 scalar double — the last two
 * SSE2's, #UD on a Pentium III. Floating point is SoftFloat's binary32 and
 * binary64 under MXCSR: the rounding control, flush-to-zero, the six flags
 * and their masks. What SoftFloat does not do the x86 way is done here:
 * NaN propagation (the first source's, quieted, else the second's —
 * SoftFloat's 8086 specialisation keeps the x87's larger-significand
 * rule), the denormal-operand flag, flush-to-zero, and MIN/MAX (the second
 * operand on a NaN or two zeros). An unmasked exception in any element
 * leaves the destination unwritten and is #XM — #UD with CR4.OSXMMEXCPT
 * clear — with every element's flags set. RCPPS/RSQRTPS are the
 * approximations the architecture allows (relative error under 1.5 x
 * 2^-12): the exact value, its low 12 significand bits cleared; silicon's
 * own tables differ in those bits. The 128-bit integer forms run the MMX
 * code (x86_mmx_alu) on each half, but for what crosses the halves.
 * Memory goes through x86_fpu_mrd/mwr; a 16-byte operand of a packed
 * instruction must be 16-aligned (#GP(0)), MOVUPS's, MOVUPD's and
 * MOVDQU's need not be. */
#include "x86.h"
#include "x86_decode.h"
#include "x86_fpu.h"

#include "softfloat/platform.h"
#include "internals.h"
#include "specialize.h"

#include <string.h>

#define F (c->fpu)

enum { M_IE = 1, M_DE = 2, M_ZE = 4, M_OE = 8, M_UE = 16, M_PE = 32 };
#define MX_FTZ 0x8000u

static void ud(x86_cpu *c) { x86_fault(c, X86_EXC_UD, 0); }

/* lanes of width W (32 or 64) in a 128-bit register */
static uint64_t lane(const uint64_t *x, int i, int w) {
    return w == 64 ? x[i] : (uint32_t)(x[i >> 1] >> (32 * (i & 1)));
}
static void set_lane(uint64_t *x, int i, int w, uint64_t v) {
    if (w == 64) { x[i] = v; return; }
    int s = 32 * (i & 1);
    x[i >> 1] = (x[i >> 1] & ~((uint64_t)0xFFFFFFFFu << s)) | ((v & 0xFFFFFFFFu) << s);
}

/* ---- memory ------------------------------------------------------------- */

static void ld(x86_cpu *c, const x86_insn *in, uint32_t ea, int n, int aligned, uint64_t *q) {
    if (aligned && ((c->seg[in->seg].base + ea) & 15)) x86_fault(c, X86_EXC_GP, 0);
    uint8_t b[16] = { 0 };
    x86_fpu_mrd(c, in, ea, n, n >= 8 ? 8 : n, b);
    q[0] = q[1] = 0;
    for (int i = n - 1; i >= 0; i--) q[i >> 3] = (q[i >> 3] << 8) | b[i];
}
static void st(x86_cpu *c, const x86_insn *in, uint32_t ea, int n, int aligned, const uint64_t *q) {
    if (aligned && ((c->seg[in->seg].base + ea) & 15)) x86_fault(c, X86_EXC_GP, 0);
    uint8_t b[16];
    for (int i = 0; i < n; i++) b[i] = (uint8_t)(q[i >> 3] >> (8 * (i & 7)));
    x86_fpu_mwr(c, in, ea, n, n >= 8 ? 8 : n, b);
}

/* ---- floating point under MXCSR, either width ------------------------------- */

static uint64_t expm(int w) { return w == 32 ? 0x7F800000u : UINT64_C(0x7FF0000000000000); }
static uint64_t fracm(int w) { return w == 32 ? 0x007FFFFFu : UINT64_C(0x000FFFFFFFFFFFFF); }
static uint64_t qbit(int w) { return w == 32 ? 0x00400000u : UINT64_C(0x0008000000000000); }
static uint64_t signb(int w) { return w == 32 ? 0x80000000u : UINT64_C(0x8000000000000000); }
static int is_nan(uint64_t x, int w) { return (x & expm(w)) == expm(w) && (x & fracm(w)); }
static int is_snan(uint64_t x, int w) { return is_nan(x, w) && !(x & qbit(w)); }
static int is_den(uint64_t x, int w) { return (x & expm(w)) == 0 && (x & fracm(w)); }
static int is_zero(uint64_t x, int w) { return (x & ~signb(w)) == 0; }

static uint32_t sf_flags(void) {
    uint8_t f = softfloat_exceptionFlags;
    return ((f & softfloat_flag_invalid) ? M_IE : 0) | ((f & softfloat_flag_infinite) ? M_ZE : 0)
         | ((f & softfloat_flag_overflow) ? M_OE : 0) | ((f & softfloat_flag_underflow) ? M_UE : 0)
         | ((f & softfloat_flag_inexact) ? M_PE : 0);
}
static void sf_begin(x86_cpu *c) {
    static const uint8_t rm[4] = { softfloat_round_near_even, softfloat_round_min, softfloat_round_max, softfloat_round_minMag };
    softfloat_roundingMode = rm[(F.mxcsr >> 13) & 3];
    softfloat_detectTininess = softfloat_tininess_afterRounding;
    softfloat_exceptionFlags = 0;
}
/* a rounded result: flush-to-zero, and the underflow flag as the mask has it */
static uint64_t sf_end(x86_cpu *c, uint64_t r, int w, uint32_t *fl) {
    uint32_t f = sf_flags();
    int tiny = is_den(r, w);
    if (tiny && !(F.mxcsr & (M_UE << 7))) f |= M_UE;                 /* unmasked: tiny is enough */
    if (tiny && (F.mxcsr & MX_FTZ) && (F.mxcsr & (M_UE << 7))) { r &= signb(w); f |= M_UE | M_PE; }
    *fl |= f;
    return r;
}

/* DAZ (MXCSR bit 6, the Pentium 4's): a denormal source reads as a zero
 * of its sign, and flags no DE */
#define MX_DAZ 0x40u
static uint64_t daz(const x86_cpu *c, uint64_t x, int w) {
    return (F.mxcsr & MX_DAZ) && is_den(x, w) ? x & signb(w) : x;
}

enum { O_ADD, O_SUB, O_MUL, O_DIV, O_MIN, O_MAX, O_SQRT };

static uint64_t fop(x86_cpu *c, int op, uint64_t a, uint64_t b, int w, uint32_t *fl) {
    a = daz(c, a, w); b = daz(c, b, w);
    if (op == O_MIN || op == O_MAX) {
        if (is_nan(a, w) || is_nan(b, w)) { *fl |= M_IE; return b; }
        if (is_den(a, w) || is_den(b, w)) *fl |= M_DE;
        if (is_zero(a, w) && is_zero(b, w)) return b;
        int lt = w == 32 ? f32_lt((float32_t){ (uint32_t)a }, (float32_t){ (uint32_t)b })
                         : f64_lt((float64_t){ a }, (float64_t){ b });
        return (op == O_MIN) == lt ? a : b;
    }
    int una = op == O_SQRT;                                          /* SQRT: the source alone */
    if (una) a = b;
    if (is_snan(a, w) || (!una && is_snan(b, w))) *fl |= M_IE;
    if (is_nan(a, w)) return a | qbit(w);
    if (!una && is_nan(b, w)) return b | qbit(w);
    if (is_den(a, w) || (!una && is_den(b, w))) *fl |= M_DE;
    sf_begin(c);
    uint64_t r;
    if (w == 32) {
        float32_t x = { (uint32_t)a }, y = { (uint32_t)b };
        switch (op) {
        case O_ADD: r = f32_add(x, y).v; break;
        case O_SUB: r = f32_sub(x, y).v; break;
        case O_MUL: r = f32_mul(x, y).v; break;
        case O_DIV: r = f32_div(x, y).v; break;
        default:    r = f32_sqrt(x).v; break;
        }
    } else {
        float64_t x = { a }, y = { b };
        switch (op) {
        case O_ADD: r = f64_add(x, y).v; break;
        case O_SUB: r = f64_sub(x, y).v; break;
        case O_MUL: r = f64_mul(x, y).v; break;
        case O_DIV: r = f64_div(x, y).v; break;
        default:    r = f64_sqrt(x).v; break;
        }
    }
    return sf_end(c, r, w, fl);
}

/* RCP / RSQRT (single only): no exceptions; a denormal reads as a zero */
static uint32_t approx(uint32_t b, int rsqrt) {
    uint32_t s = b & 0x80000000u;
    if (is_nan(b, 32)) return b | 0x00400000u;
    if (is_zero(b, 32) || is_den(b, 32)) return s | 0x7F800000u;
    if (rsqrt && s) return 0xFFC00000u;
    if ((b & 0x7FFFFFFFu) == 0x7F800000u) return s;                    /* 1/inf, 1/sqrt(inf) */
    union { float f; uint32_t u; } in = { .u = b }, out;
    double v = rsqrt ? 1.0 / __builtin_sqrt((double)in.f) : 1.0 / (double)in.f;
    out.f = (float)v;
    if (is_den(out.u, 32)) return s;                                  /* (as silicon: tiny results are zero) */
    return out.u & ~0xFFFu;
}

static int feq(uint64_t a, uint64_t b, int w) {
    return w == 32 ? f32_eq((float32_t){ (uint32_t)a }, (float32_t){ (uint32_t)b }) : f64_eq((float64_t){ a }, (float64_t){ b });
}
static int flt(uint64_t a, uint64_t b, int w) {
    return w == 32 ? f32_lt_quiet((float32_t){ (uint32_t)a }, (float32_t){ (uint32_t)b }) : f64_lt_quiet((float64_t){ a }, (float64_t){ b });
}
static int cmp_pred(const x86_cpu *c, uint64_t a, uint64_t b, int p, int w, uint32_t *fl) {
    a = daz(c, a, w); b = daz(c, b, w);
    int un = is_nan(a, w) || is_nan(b, w);
    int sig = p == 1 || p == 2 || p == 5 || p == 6;                  /* LT LE NLT NLE: any NaN is invalid */
    if (is_snan(a, w) || is_snan(b, w) || (sig && un)) *fl |= M_IE;
    if (!un && (is_den(a, w) || is_den(b, w))) *fl |= M_DE;
    if (un) return p == 3 || p == 4 || p == 5 || p == 6;              /* UNORD NEQ NLT NLE */
    int eq = feq(a, b, w), lt = flt(a, b, w);
    switch (p) {
    case 0: return eq;
    case 1: return lt;
    case 2: return lt || eq;
    case 3: return 0;
    case 4: return !eq;
    case 5: return !lt;
    case 6: return !(lt || eq);
    default: return 1;
    }
}

/* to int32 under MXCSR (or truncated); out of range or NaN: the integer
 * indefinite. Invalid and precision only — no denormal flag (the SDM;
 * Bochs agrees). */
static uint32_t f2i(x86_cpu *c, uint64_t a, int w, int trunc, uint32_t *fl) {
    a = daz(c, a, w);
    sf_begin(c);
    uint8_t rm = trunc ? softfloat_round_minMag : softfloat_roundingMode;
    int32_t r = w == 32 ? f32_to_i32((float32_t){ (uint32_t)a }, rm, 1) : f64_to_i32((float64_t){ a }, rm, 1);
    uint32_t f = sf_flags();
    if (f & M_IE) { r = INT32_MIN; f &= ~(uint32_t)M_PE; }
    *fl |= f;
    return (uint32_t)r;
}
static uint64_t i2f(x86_cpu *c, uint32_t v, int w, uint32_t *fl) {
    if (w == 64) return i32_to_f64((int32_t)v).v;                     /* exact */
    sf_begin(c);
    float32_t r = i32_to_f32((int32_t)v);
    *fl |= sf_flags();
    return r.v;
}
/* single to double (exact) and back (rounded): a NaN keeps its sign and
 * its payload's top bits, quieted; an SNaN is invalid; a denormal source
 * flags DE */
static uint64_t s2d(x86_cpu *c, uint32_t a, uint32_t *fl) {
    a = (uint32_t)daz(c, a, 32);
    if (is_snan(a, 32)) *fl |= M_IE;
    if (is_nan(a, 32)) return ((uint64_t)(a & 0x80000000u) << 32) | UINT64_C(0x7FF8000000000000) | ((uint64_t)(a & 0x003FFFFFu) << 29);
    if (is_den(a, 32)) *fl |= M_DE;
    sf_begin(c);
    return f32_to_f64((float32_t){ a }).v;
}
static uint32_t d2s(x86_cpu *c, uint64_t a, uint32_t *fl) {
    a = daz(c, a, 64);
    if (is_snan(a, 64)) *fl |= M_IE;
    if (is_nan(a, 64)) return (uint32_t)((a >> 32) & 0x80000000u) | 0x7FC00000u | (uint32_t)((a >> 29) & 0x003FFFFFu);
    if (is_den(a, 64)) *fl |= M_DE;
    sf_begin(c);
    return (uint32_t)sf_end(c, f64_to_f32((float64_t){ a }).v, 32, fl);
}

/* the flags into MXCSR; an unmasked one is #XM (or #UD), and 1 says the
 * caller must not write its result */
static int raise(x86_cpu *c, uint32_t fl) {
    F.mxcsr |= fl & 0x3F;
    if (!(fl & ~(F.mxcsr >> 7) & 0x3F)) return 0;
    x86_fault(c, (c->cr4 & X86_CR4_OSXMMEXCPT) ? X86_EXC_XM : X86_EXC_UD, 0);
    return 1;
}

/* MXCSR's writable bits: DAZ (bit 6) is the Pentium 4's (Willamette's, as
 * Bochs has it) */
uint32_t x86_mxcsr_mask(const x86_cpu *c) { return c->model >= X86_MODEL_P4 ? 0xFFFFu : 0xFFBFu; }

/* ---- SSE2's 128-bit integer forms (66 on the MMX opcodes, and their F2/F3 kin) ---- */

static uint64_t mmx_half(uint8_t op, uint64_t a, uint64_t b) {
    uint64_t r = 0;
    (void)x86_mmx_alu(op, a, b, &r);
    return r;
}

static void sse_int(x86_cpu *c, const x86_insn *in, uint32_t ea, int pf) {
    uint8_t op = in->opcode2;
    int mem = in->mod != 3, d = in->reg, s = in->rm;
    uint64_t *x = F.xmm[d & 7], src[2] = { 0, 0 }, r[2] = { 0, 0 };
    uint32_t im = in->ops[1].imm;
    switch (op) {
    case 0x6E:                                                         /* MOVD xmm, r/m32 */
        if (pf != 1) ud(c);
        if (mem) ld(c, in, ea, 4, 0, src); else src[0] = c->r[s];
        x[0] = (uint32_t)src[0]; x[1] = 0;
        return;
    case 0x7E:
        if (pf == 1) {                                                 /* MOVD r/m32, xmm */
            if (mem) st(c, in, ea, 4, 0, x); else c->r[s] = (uint32_t)x[0];
            return;
        }
        if (pf != 3) ud(c);                                            /* MOVQ xmm, xmm/m64 */
        if (mem) ld(c, in, ea, 8, 0, src); else src[0] = F.xmm[s & 7][0];
        x[0] = src[0]; x[1] = 0;
        return;
    case 0x6F:                                                         /* MOVDQA, MOVDQU: load */
        if (pf != 1 && pf != 3) ud(c);
        if (mem) ld(c, in, ea, 16, pf == 1, src); else { src[0] = F.xmm[s & 7][0]; src[1] = F.xmm[s & 7][1]; }
        x[0] = src[0]; x[1] = src[1];
        return;
    case 0x7F:                                                         /* MOVDQA, MOVDQU: store */
        if (pf != 1 && pf != 3) ud(c);
        if (mem) st(c, in, ea, 16, pf == 1, x); else { F.xmm[s & 7][0] = x[0]; F.xmm[s & 7][1] = x[1]; }
        return;
    case 0xD6:
        if (pf == 1) {                                                 /* MOVQ xmm/m64, xmm */
            if (mem) st(c, in, ea, 8, 0, x); else { F.xmm[s & 7][0] = x[0]; F.xmm[s & 7][1] = 0; }
            return;
        }
        if (mem) ud(c);
        x86_mmx_enter(c);
        if (pf == 3) { x[0] = F.sig[s & 7]; x[1] = 0; return; }       /* MOVQ2DQ xmm, mm */
        if (pf == 2) { F.sig[d & 7] = F.xmm[s & 7][0]; F.sexp[d & 7] = 0xFFFF; return; }   /* MOVDQ2Q mm, xmm */
        ud(c); return;
    case 0xE7:                                                         /* MOVNTDQ m128, xmm */
        if (pf != 1 || !mem) ud(c);
        st(c, in, ea, 16, 1, x);
        return;
    case 0xD7: {                                                       /* PMOVMSKB r32, xmm */
        if (pf != 1 || mem) ud(c);
        uint32_t m = 0;
        for (int i = 0; i < 16; i++) m |= (uint32_t)((F.xmm[s & 7][i >> 3] >> (8 * (i & 7) + 7)) & 1) << i;
        c->r[d] = m;
        return;
    }
    case 0xC5:                                                         /* PEXTRW r32, xmm, Ib */
        if (pf != 1 || mem) ud(c);
        c->r[d] = (uint32_t)(F.xmm[s & 7][(im >> 2) & 1] >> (16 * (im & 3))) & 0xFFFF;
        return;
    case 0xC4: {                                                       /* PINSRW xmm, r32/m16, Ib */
        if (pf != 1) ud(c);
        uint64_t w = mem ? (ld(c, in, ea, 2, 0, src), src[0]) : (c->r[s] & 0xFFFF);
        int k = im & 7, sh = 16 * (k & 3);
        x[k >> 2] = (x[k >> 2] & ~((uint64_t)0xFFFF << sh)) | (w << sh);
        return;
    }
    case 0xF7: {                                                       /* MASKMOVDQU xmm, xmm: to [EDI] */
        if (pf != 1 || mem) ud(c);
        uint32_t di = in->adsize == 4 ? c->r[R_DI] : (c->r[R_DI] & 0xFFFF);
        for (int i = 0; i < 16; i++)
            if ((F.xmm[s & 7][i >> 3] >> (8 * (i & 7) + 7)) & 1) {
                uint8_t byte = (uint8_t)(x[i >> 3] >> (8 * (i & 7)));
                x86_fpu_mwr(c, in, di + (uint32_t)i, 1, 1, &byte);
            }
        return;
    }
    case 0x71: case 0x72: case 0x73: {                                 /* the shifts by Ib (and PSRLDQ, PSLLDQ) */
        if (pf != 1 || mem) ud(c);
        uint64_t *y = F.xmm[s & 7];
        int bits = op == 0x71 ? 16 : op == 0x72 ? 32 : 64, kind, n = im & 0xFF;
        if (op == 0x73 && (in->reg == 3 || in->reg == 7)) {            /* PSRLDQ, PSLLDQ: bytes, the whole register */
            uint64_t lo = y[0], hi = y[1];                             /* (as two halves: MSVC has no __int128) */
            int sh = 8 * n;
            if (n > 15) lo = hi = 0;
            else if (in->reg == 3) {
                if (sh >= 64) { lo = hi >> (sh - 64); hi = 0; }
                else if (sh) { lo = (lo >> sh) | (hi << (64 - sh)); hi >>= sh; }
            } else {
                if (sh >= 64) { hi = lo << (sh - 64); lo = 0; }
                else if (sh) { hi = (hi << sh) | (lo >> (64 - sh)); lo <<= sh; }
            }
            y[0] = lo; y[1] = hi;
            return;
        }
        switch (in->reg) {
        case 2: kind = 1; break;
        case 4: if (op == 0x73) { ud(c); return; } kind = 2; break;
        case 6: kind = 0; break;
        default: ud(c); return;
        }
        y[0] = x86_mmx_shift(y[0], (uint64_t)n, bits, kind);
        y[1] = x86_mmx_shift(y[1], (uint64_t)n, bits, kind);
        return;
    }
    default: break;
    }
    /* xmm op= xmm/m128 (16-aligned) */
    if (mem) ld(c, in, ea, 16, 1, src); else { src[0] = F.xmm[s & 7][0]; src[1] = F.xmm[s & 7][1]; }
    switch (op) {
    case 0x70:                                                         /* PSHUFD, PSHUFHW, PSHUFLW */
        if (pf == 1) {
            for (int i = 0; i < 4; i++) set_lane(r, i, 32, lane(src, (im >> (2 * i)) & 3, 32));
        } else if (pf == 3 || pf == 2) {
            int hi = pf == 3;
            uint64_t h = src[hi], o = 0;
            for (int i = 0; i < 4; i++) o |= ((h >> (16 * ((im >> (2 * i)) & 3))) & 0xFFFF) << (16 * i);
            r[hi] = o; r[!hi] = src[!hi];
        } else ud(c);
        break;
    case 0x60: case 0x61: case 0x62:                                   /* PUNPCKL BW WD DQ */
        if (pf != 1) ud(c);
        r[0] = mmx_half(op, x[0], src[0]); r[1] = mmx_half((uint8_t)(op + 8), x[0], src[0]);
        break;
    case 0x68: case 0x69: case 0x6A:                                   /* PUNPCKH BW WD DQ */
        if (pf != 1) ud(c);
        r[0] = mmx_half((uint8_t)(op - 8), x[1], src[1]); r[1] = mmx_half(op, x[1], src[1]);
        break;
    case 0x6C: if (pf != 1) ud(c); r[0] = x[0]; r[1] = src[0]; break;  /* PUNPCKLQDQ */
    case 0x6D: if (pf != 1) ud(c); r[0] = x[1]; r[1] = src[1]; break;  /* PUNPCKHQDQ */
    case 0x63: case 0x67: case 0x6B:                                   /* the packs: a's lanes low, b's high */
        if (pf != 1) ud(c);
        r[0] = mmx_half(op, x[0], x[1]); r[1] = mmx_half(op, src[0], src[1]);
        break;
    case 0xD1: case 0xD2: case 0xD3: case 0xE1: case 0xE2: case 0xF1: case 0xF2: case 0xF3:   /* shifts by xmm/m128's low quadword */
        if (pf != 1) ud(c);
        r[0] = mmx_half(op, x[0], src[0]); r[1] = mmx_half(op, x[1], src[0]);
        break;
    default:
        if (pf != 1) ud(c);
        if (!x86_mmx_alu(op, x[0], src[0], &r[0]) || !x86_mmx_alu(op, x[1], src[1], &r[1])) { ud(c); return; }
        break;
    }
    x[0] = r[0]; x[1] = r[1];
}

/* ---- dispatch --------------------------------------------------------------- */

static int is_int_op(uint8_t op) {
    return (op >= 0x60 && op <= 0x7F) || (op >= 0xD1 && op <= 0xFE && op != 0xE6) || op == 0xC4 || op == 0xC5;
}

void x86_sse_exec(x86_cpu *c, const x86_insn *in, uint32_t ea) {
    uint8_t op = in->opcode2;
    int pf = in->rep == 0xF3 ? 3 : in->rep == 0xF2 ? 2 : in->p66 ? 1 : 0;
    int mem = in->mod != 3, d = in->reg, s = in->rm;
    int sse2 = c->model >= X86_MODEL_P4;
    uint64_t *x = F.xmm[d & 7];

    if (op == 0xAE) {                                                  /* the group: by reg */
        if (pf) ud(c);
        switch (in->reg) {
        case 0: case 1:                                                /* FXSAVE, FXRSTOR */
            if (!mem) ud(c);
            if (c->cr0 & (X86_CR0_EM | X86_CR0_TS)) x86_fault(c, X86_EXC_NM, 0);
            if (in->reg == 0) x86_fxsave(c, in, ea); else x86_fxrstor(c, in, ea);
            return;
        case 2: case 3: {                                              /* LDMXCSR, STMXCSR */
            if (!mem || (c->cr0 & X86_CR0_EM) || !(c->cr4 & X86_CR4_OSFXSR)) ud(c);
            if (c->cr0 & X86_CR0_TS) x86_fault(c, X86_EXC_NM, 0);
            uint64_t q[2] = { F.mxcsr, 0 };
            if (in->reg == 3) { st(c, in, ea, 4, 0, q); return; }
            ld(c, in, ea, 4, 0, q);
            if ((uint32_t)q[0] & ~x86_mxcsr_mask(c)) x86_fault(c, X86_EXC_GP, 0);
            F.mxcsr = (uint32_t)q[0];
            return;
        }
        case 5: case 6:                                                /* LFENCE, MFENCE (Pentium 4; mod 3) */
            if (!sse2 || mem) ud(c);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            return;
        case 7:
            if (!mem) { __atomic_thread_fence(__ATOMIC_SEQ_CST); return; }   /* SFENCE */
            if (!sse2) ud(c);
            { uint8_t b; x86_fpu_mrd(c, in, ea, 1, 1, &b); }          /* CLFLUSH: as a byte read (#PF, #GP); no cache to flush */
            return;
        default: ud(c); return;
        }
    }
    if (op == 0xC3) {                                                  /* MOVNTI m32, r32 (Pentium 4) */
        if (!sse2 || !mem || pf) ud(c);
        uint64_t q[2] = { c->r[d], 0 };
        st(c, in, ea, 4, 0, q);
        return;
    }

    if ((c->cr0 & X86_CR0_EM) || !(c->cr4 & X86_CR4_OSFXSR) || (!sse2 && (pf == 1 || pf == 2))) ud(c);
    if (c->cr0 & X86_CR0_TS) x86_fault(c, X86_EXC_NM, 0);
    if (is_int_op(op)) { sse_int(c, in, ea, pf); return; }

    int dbl = pf == 1 || pf == 2, sc = pf == 2 || pf == 3, w = dbl ? 64 : 32, sz = w / 8;
    uint64_t src[2] = { 0, 0 }, r[2] = { x[0], x[1] };
    uint32_t fl = 0;

    switch (op) {
    /* ---- moves ---- */
    case 0x10: case 0x28:                                              /* MOVUPS/PD, MOVSS/SD, MOVAPS/PD: load */
        if (op == 0x28 && sc) ud(c);
        if (sc) {
            if (mem) { ld(c, in, ea, sz, 0, src); x[0] = src[0]; x[1] = 0; }
            else set_lane(x, 0, w, lane(F.xmm[s & 7], 0, w));
            return;
        }
        if (mem) ld(c, in, ea, 16, op == 0x28, src);
        else { src[0] = F.xmm[s & 7][0]; src[1] = F.xmm[s & 7][1]; }
        x[0] = src[0]; x[1] = src[1];
        return;
    case 0x11: case 0x29: case 0x2B: {                                 /* MOVUPS/PD, MOVSS/SD, MOVAPS/PD, MOVNTPS/PD: store */
        if ((op != 0x11 && sc) || (op == 0x2B && !mem)) ud(c);
        if (sc) {
            if (mem) st(c, in, ea, sz, 0, x);
            else set_lane(F.xmm[s & 7], 0, w, lane(x, 0, w));
            return;
        }
        if (mem) st(c, in, ea, 16, op != 0x11, x);
        else { F.xmm[s & 7][0] = x[0]; F.xmm[s & 7][1] = x[1]; }
        return;
    }
    case 0x12: case 0x16:                                              /* MOVLPS/MOVHLPS, MOVHPS/MOVLHPS; MOVLPD, MOVHPD */
        if (sc || (pf == 1 && !mem)) ud(c);
        if (mem) { ld(c, in, ea, 8, 0, src); x[op == 0x16] = src[0]; }
        else x[op == 0x16] = F.xmm[s & 7][op == 0x12];
        return;
    case 0x13: case 0x17:                                              /* MOVLPS/PD m64, MOVHPS/PD m64 */
        if (sc || !mem) ud(c);
        src[0] = x[op == 0x17];
        st(c, in, ea, 8, 0, src);
        return;
    case 0x50: {                                                       /* MOVMSKPS/PD r32, xmm */
        if (sc || mem) ud(c);
        uint32_t m = 0;
        for (int i = 0; i < 128 / w; i++) m |= (uint32_t)((lane(F.xmm[s & 7], i, w) >> (w - 1)) & 1) << i;
        c->r[d] = m;
        return;
    }
    /* ---- conversions with MMX and general registers ---- */
    case 0x2A:
        if (sc) {                                                      /* CVTSI2SS/SD xmm, r/m32 */
            uint32_t v = mem ? (ld(c, in, ea, 4, 0, src), (uint32_t)src[0]) : c->r[s];
            uint64_t f = i2f(c, v, w, &fl);
            if (raise(c, fl)) return;
            set_lane(x, 0, w, f);
            return;
        }
        if (mem) ld(c, in, ea, 8, 0, src);                             /* CVTPI2PS/PD xmm, mm/m64 */
        else { x86_mmx_enter(c); src[0] = F.sig[s & 7]; }
        for (int i = 0; i < 2; i++) set_lane(r, i, w, i2f(c, (uint32_t)(src[0] >> (32 * i)), w, &fl));
        if (raise(c, fl)) return;
        if (dbl) { x[0] = r[0]; x[1] = r[1]; } else x[0] = r[0];
        return;
    case 0x2C: case 0x2D: {                                            /* CVT(T)PS/PD2PI mm, xmm/m; CVT(T)SS/SD2SI r32, xmm/m */
        int t = op == 0x2C;
        if (mem) ld(c, in, ea, sc ? sz : (dbl ? 16 : 8), pf == 1, src);
        else { src[0] = F.xmm[s & 7][0]; src[1] = F.xmm[s & 7][1]; }
        if (sc) {
            uint32_t v = f2i(c, lane(src, 0, w), w, t, &fl);
            if (raise(c, fl)) return;
            c->r[d] = v;
            return;
        }
        x86_mmx_enter(c);
        uint64_t q = (uint64_t)f2i(c, lane(src, 0, w), w, t, &fl) | (uint64_t)f2i(c, lane(src, 1, w), w, t, &fl) << 32;
        if (raise(c, fl)) return;
        F.sig[d & 7] = q; F.sexp[d & 7] = 0xFFFF;
        return;
    }
    case 0x2E: case 0x2F: {                                            /* UCOMISS/SD, COMISS/SD */
        if (sc) ud(c);
        if (mem) ld(c, in, ea, sz, 0, src); else src[0] = F.xmm[s & 7][0];
        uint64_t a = daz(c, lane(x, 0, w), w), b = daz(c, lane(src, 0, w), w);
        int un = is_nan(a, w) || is_nan(b, w);
        if (is_snan(a, w) || is_snan(b, w) || (op == 0x2F && un)) fl |= M_IE;
        if (!un && (is_den(a, w) || is_den(b, w))) fl |= M_DE;
        if (raise(c, fl)) return;
        uint32_t z;
        if (un) z = X86_ZF | X86_PF | X86_CF;
        else if (feq(a, b, w)) z = X86_ZF;
        else z = flt(a, b, w) ? X86_CF : 0;
        c->eflags = (c->eflags & ~(uint32_t)(X86_ZF | X86_PF | X86_CF | X86_OF | X86_SF | X86_AF)) | z;
        return;
    }
    case 0x5A: {                                                       /* the precision conversions */
        if (!sse2) ud(c);
        int n_in = pf == 0 ? 8 : pf == 1 ? 16 : pf == 2 ? 8 : 4;       /* CVTPS2PD m64, CVTPD2PS m128, CVTSD2SS m64, CVTSS2SD m32 */
        if (mem) ld(c, in, ea, n_in, pf == 1, src); else { src[0] = F.xmm[s & 7][0]; src[1] = F.xmm[s & 7][1]; }
        switch (pf) {
        case 0: r[0] = s2d(c, (uint32_t)src[0], &fl); r[1] = s2d(c, (uint32_t)(src[0] >> 32), &fl); break;
        case 1: r[0] = (uint64_t)d2s(c, src[0], &fl) | (uint64_t)d2s(c, src[1], &fl) << 32; r[1] = 0; break;
        case 2: set_lane(r, 0, 32, d2s(c, src[0], &fl)); break;
        default: r[0] = s2d(c, (uint32_t)src[0], &fl); break;
        }
        if (raise(c, fl)) return;
        x[0] = r[0]; x[1] = r[1];
        return;
    }
    case 0x5B: case 0xE6: {                                            /* the integer conversions */
        if (!sse2) ud(c);
        if (op == 0x5B && pf == 2) ud(c);
        if (op == 0xE6 && pf == 0) ud(c);
        int n_in = (op == 0xE6 && pf == 3) ? 8 : 16;                   /* CVTDQ2PD reads m64 */
        if (mem) ld(c, in, ea, n_in, n_in == 16, src); else { src[0] = F.xmm[s & 7][0]; src[1] = F.xmm[s & 7][1]; }
        if (op == 0x5B) {
            for (int i = 0; i < 4; i++)
                set_lane(r, i, 32, pf == 0 ? i2f(c, (uint32_t)lane(src, i, 32), 32, &fl)    /* CVTDQ2PS */
                                           : f2i(c, lane(src, i, 32), 32, pf == 3, &fl));   /* CVTPS2DQ, CVTTPS2DQ */
        } else if (pf == 3) {                                          /* CVTDQ2PD */
            r[0] = i2f(c, (uint32_t)src[0], 64, &fl); r[1] = i2f(c, (uint32_t)(src[0] >> 32), 64, &fl);
        } else {                                                       /* CVTPD2DQ, CVTTPD2DQ */
            r[0] = (uint64_t)f2i(c, src[0], 64, pf == 1, &fl) | (uint64_t)f2i(c, src[1], 64, pf == 1, &fl) << 32;
            r[1] = 0;
        }
        if (raise(c, fl)) return;
        x[0] = r[0]; x[1] = r[1];
        return;
    }
    default: break;
    }

    /* the rest: xmm op= xmm/m128 (packed, 16-aligned) or xmm/m32, m64 (scalar) */
    int nosc = op == 0xC6 || op == 0x14 || op == 0x15 || (op >= 0x54 && op <= 0x57);   /* no scalar form */
    if (sc && nosc) ud(c);
    if ((op == 0x52 || op == 0x53) && dbl) ud(c);                      /* no RCPPD, RSQRTPD */
    if (mem) ld(c, in, ea, sc ? sz : 16, !sc, src);
    else { src[0] = F.xmm[s & 7][0]; src[1] = F.xmm[s & 7][1]; }
    int n = sc ? 1 : 128 / w;
    switch (op) {
    case 0x51: case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        int o = op == 0x51 ? O_SQRT : op == 0x58 ? O_ADD : op == 0x59 ? O_MUL : op == 0x5C ? O_SUB
              : op == 0x5D ? O_MIN : op == 0x5E ? O_DIV : O_MAX;
        for (int i = 0; i < n; i++) set_lane(r, i, w, fop(c, o, lane(x, i, w), lane(src, i, w), w, &fl));
        break;
    }
    case 0x52: case 0x53:                                              /* RSQRTPS/SS, RCPPS/SS */
        for (int i = 0; i < n; i++) set_lane(r, i, 32, approx((uint32_t)lane(src, i, 32), op == 0x52));
        break;
    case 0x54: r[0] = x[0] & src[0]; r[1] = x[1] & src[1]; break;      /* ANDPS/PD */
    case 0x55: r[0] = ~x[0] & src[0]; r[1] = ~x[1] & src[1]; break;    /* ANDNPS/PD */
    case 0x56: r[0] = x[0] | src[0]; r[1] = x[1] | src[1]; break;      /* ORPS/PD */
    case 0x57: r[0] = x[0] ^ src[0]; r[1] = x[1] ^ src[1]; break;      /* XORPS/PD */
    case 0x14:                                                         /* UNPCKLPS/PD */
        if (dbl) { r[0] = x[0]; r[1] = src[0]; break; }
        set_lane(r, 0, 32, lane(x, 0, 32)); set_lane(r, 1, 32, lane(src, 0, 32));
        set_lane(r, 2, 32, lane(x, 1, 32)); set_lane(r, 3, 32, lane(src, 1, 32));
        break;
    case 0x15:                                                         /* UNPCKHPS/PD */
        if (dbl) { r[0] = x[1]; r[1] = src[1]; break; }
        set_lane(r, 0, 32, lane(x, 2, 32)); set_lane(r, 1, 32, lane(src, 2, 32));
        set_lane(r, 2, 32, lane(x, 3, 32)); set_lane(r, 3, 32, lane(src, 3, 32));
        break;
    case 0xC6: {                                                       /* SHUFPS/PD */
        uint32_t im = in->ops[1].imm;
        if (dbl) { r[0] = x[im & 1]; r[1] = src[(im >> 1) & 1]; break; }
        set_lane(r, 0, 32, lane(x, im & 3, 32)); set_lane(r, 1, 32, lane(x, (im >> 2) & 3, 32));
        set_lane(r, 2, 32, lane(src, (im >> 4) & 3, 32)); set_lane(r, 3, 32, lane(src, (im >> 6) & 3, 32));
        break;
    }
    case 0xC2: {                                                       /* CMPPS/PD/SS/SD */
        int p = in->ops[1].imm & 7;
        for (int i = 0; i < n; i++) set_lane(r, i, w, cmp_pred(c, lane(x, i, w), lane(src, i, w), p, w, &fl) ? ~UINT64_C(0) : 0);
        break;
    }
    default: ud(c); return;
    }
    if (raise(c, fl)) return;
    x[0] = r[0]; x[1] = r[1];
}
