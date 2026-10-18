/* x86_sse.c — SSE, the Pentium III's (Katmai): XMM0-7 and MXCSR (in the
 * x87's state, x86_fpu.h, beside the registers FXSAVE also covers).
 *
 * One decoded op (OP_SSE) per 0F opcode; the prefix picks the form — none
 * the packed one, F3 the scalar one; 66 and F2 are SSE2's (#UD here).
 * Floating point is SoftFloat's binary32 under MXCSR: the rounding control,
 * flush-to-zero, the six flags and their masks. What SoftFloat does not do
 * the x86 way is done here: NaN propagation (the first source's, quieted,
 * else the second's — SoftFloat's 8086 specialisation keeps the x87's
 * larger-significand rule), the denormal-operand flag, flush-to-zero, and
 * MIN/MAX (the second operand on a NaN or two zeros). An unmasked
 * exception in any element leaves the destination unwritten and is #XM —
 * #UD with CR4.OSXMMEXCPT clear — with every element's flags set.
 * RCPPS/RSQRTPS are the approximations the architecture allows (relative
 * error under 1.5 x 2^-12): the exact value, its low 12 significand bits
 * cleared; silicon's own tables differ in those bits. Memory goes through
 * x86_fpu_mrd/mwr; a 16-byte operand of a packed instruction must be
 * 16-aligned (#GP(0)), MOVUPS's need not be. */
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

static uint32_t lane(const uint64_t *x, int i) { return (uint32_t)(x[i >> 1] >> (32 * (i & 1))); }
static void set_lane(uint64_t *x, int i, uint32_t v) {
    int s = 32 * (i & 1);
    x[i >> 1] = (x[i >> 1] & ~((uint64_t)0xFFFFFFFFu << s)) | ((uint64_t)v << s);
}

static void ud(x86_cpu *c) { x86_fault(c, X86_EXC_UD, 0); }

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

/* ---- single precision under MXCSR ------------------------------------------ */

static int is_nan(uint32_t x) { return (x & 0x7F800000u) == 0x7F800000u && (x & 0x007FFFFFu); }
static int is_snan(uint32_t x) { return is_nan(x) && !(x & 0x00400000u); }
static int is_den(uint32_t x) { return (x & 0x7F800000u) == 0 && (x & 0x007FFFFFu); }
static int is_zero(uint32_t x) { return (x & 0x7FFFFFFFu) == 0; }

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
static uint32_t sf_end(x86_cpu *c, uint32_t r, uint32_t *fl) {
    uint32_t f = sf_flags();
    int tiny = is_den(r);
    if (tiny && !(F.mxcsr & (M_UE << 7))) f |= M_UE;                 /* unmasked: tiny is enough */
    if (tiny && (F.mxcsr & MX_FTZ) && (F.mxcsr & (M_UE << 7))) { r &= 0x80000000u; f |= M_UE | M_PE; }
    *fl |= f;
    return r;
}

enum { O_ADD, O_SUB, O_MUL, O_DIV, O_MIN, O_MAX, O_SQRT };

static uint32_t f32op(x86_cpu *c, int op, uint32_t a, uint32_t b, uint32_t *fl) {
    if (op == O_MIN || op == O_MAX) {
        if (is_nan(a) || is_nan(b)) { *fl |= M_IE; return b; }
        if (is_den(a) || is_den(b)) *fl |= M_DE;
        if (is_zero(a) && is_zero(b)) return b;
        sf_begin(c);
        int lt = f32_lt((float32_t){ a }, (float32_t){ b });
        return (op == O_MIN) == lt ? a : b;
    }
    int una = op == O_SQRT;                                          /* SQRT: the source alone */
    if (una) a = b;
    if (is_snan(a) || (!una && is_snan(b))) *fl |= M_IE;
    if (is_nan(a)) return a | 0x00400000u;
    if (!una && is_nan(b)) return b | 0x00400000u;
    if (is_den(a) || (!una && is_den(b))) *fl |= M_DE;
    sf_begin(c);
    float32_t x = { a }, y = { b }, r;
    switch (op) {
    case O_ADD: r = f32_add(x, y); break;
    case O_SUB: r = f32_sub(x, y); break;
    case O_MUL: r = f32_mul(x, y); break;
    case O_DIV: r = f32_div(x, y); break;
    default:    r = f32_sqrt(x); break;
    }
    return sf_end(c, r.v, fl);
}

/* RCP / RSQRT: no exceptions; a denormal reads as a zero */
static uint32_t approx(uint32_t b, int rsqrt) {
    uint32_t s = b & 0x80000000u;
    if (is_nan(b)) return b | 0x00400000u;
    if (is_zero(b) || is_den(b)) return s | 0x7F800000u;
    if (rsqrt && s) return 0xFFC00000u;
    if ((b & 0x7FFFFFFFu) == 0x7F800000u) return s;                    /* 1/inf, 1/sqrt(inf) */
    union { float f; uint32_t u; } in = { .u = b }, out;
    double v = rsqrt ? 1.0 / __builtin_sqrt((double)in.f) : 1.0 / (double)in.f;
    out.f = (float)v;
    if (is_den(out.u)) return s;                                      /* (as silicon: tiny results are zero) */
    return out.u & ~0xFFFu;
}

static int cmp_pred(uint32_t a, uint32_t b, int p, uint32_t *fl) {
    int un = is_nan(a) || is_nan(b);
    int sig = p == 1 || p == 2 || p == 5 || p == 6;                  /* LT LE NLT NLE: any NaN is invalid */
    if (is_snan(a) || is_snan(b) || (sig && un)) *fl |= M_IE;
    if (!un && (is_den(a) || is_den(b))) *fl |= M_DE;
    if (un) return p == 3 || p == 4 || p == 5 || p == 6;              /* UNORD NEQ NLT NLE */
    int eq = f32_eq((float32_t){ a }, (float32_t){ b }), lt = f32_lt_quiet((float32_t){ a }, (float32_t){ b });
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
static uint32_t f2i(x86_cpu *c, uint32_t a, int trunc, uint32_t *fl) {
    sf_begin(c);
    int32_t r = f32_to_i32((float32_t){ a }, trunc ? softfloat_round_minMag : softfloat_roundingMode, 1);
    uint32_t f = sf_flags();
    if (f & M_IE) { r = INT32_MIN; f &= ~(uint32_t)M_PE; }
    *fl |= f;
    return (uint32_t)r;
}
static uint32_t i2f(x86_cpu *c, uint32_t v, uint32_t *fl) {
    sf_begin(c);
    float32_t r = i32_to_f32((int32_t)v);
    *fl |= sf_flags();
    return r.v;
}

/* the flags into MXCSR; an unmasked one is #XM (or #UD), and 1 says the
 * caller must not write its result */
static int raise(x86_cpu *c, uint32_t fl) {
    F.mxcsr |= fl & 0x3F;
    if (!(fl & ~(F.mxcsr >> 7) & 0x3F)) return 0;
    x86_fault(c, (c->cr4 & X86_CR4_OSXMMEXCPT) ? X86_EXC_XM : X86_EXC_UD, 0);
    return 1;
}

/* ---- dispatch --------------------------------------------------------------- */

void x86_sse_exec(x86_cpu *c, const x86_insn *in, uint32_t ea) {
    uint8_t op = in->opcode2;
    int pf = in->rep == 0xF3 ? 3 : in->rep == 0xF2 ? 2 : in->p66 ? 1 : 0;
    int mem = in->mod != 3, d = in->reg, s = in->rm;
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
            if ((uint32_t)q[0] & ~0xFFBFu) x86_fault(c, X86_EXC_GP, 0);
            F.mxcsr = (uint32_t)q[0];
            return;
        }
        case 7:                                                        /* SFENCE (mod 3) */
            if (mem) ud(c);
            return;
        default: ud(c); return;
        }
    }

    if ((c->cr0 & X86_CR0_EM) || !(c->cr4 & X86_CR4_OSFXSR) || pf == 1 || pf == 2) ud(c);
    if (c->cr0 & X86_CR0_TS) x86_fault(c, X86_EXC_NM, 0);
    int sc = pf == 3;                                                  /* the scalar form */
    uint64_t src[2] = { 0, 0 }, r[2] = { x[0], x[1] };
    uint32_t fl = 0;

    switch (op) {
    /* ---- moves ---- */
    case 0x10: case 0x28:                                              /* MOVUPS, MOVSS, MOVAPS: load */
        if (op == 0x28 && sc) ud(c);
        if (sc) {
            if (mem) { ld(c, in, ea, 4, 0, src); x[0] = src[0]; x[1] = 0; }
            else set_lane(x, 0, lane(F.xmm[s & 7], 0));
            return;
        }
        if (mem) ld(c, in, ea, 16, op == 0x28, src);
        else { src[0] = F.xmm[s & 7][0]; src[1] = F.xmm[s & 7][1]; }
        x[0] = src[0]; x[1] = src[1];
        return;
    case 0x11: case 0x29: case 0x2B: {                                 /* MOVUPS, MOVSS, MOVAPS, MOVNTPS: store */
        if ((op != 0x11 && sc) || (op == 0x2B && !mem)) ud(c);
        if (sc) {
            if (mem) st(c, in, ea, 4, 0, x);
            else set_lane(F.xmm[s & 7], 0, lane(x, 0));
            return;
        }
        if (mem) st(c, in, ea, 16, op != 0x11, x);
        else { F.xmm[s & 7][0] = x[0]; F.xmm[s & 7][1] = x[1]; }
        return;
    }
    case 0x12: case 0x16:                                              /* MOVLPS/MOVHLPS, MOVHPS/MOVLHPS */
        if (sc) ud(c);
        if (mem) { ld(c, in, ea, 8, 0, src); x[op == 0x16] = src[0]; }
        else x[op == 0x16] = F.xmm[s & 7][op == 0x12];
        return;
    case 0x13: case 0x17:                                              /* MOVLPS m64, MOVHPS m64 */
        if (sc || !mem) ud(c);
        src[0] = x[op == 0x17];
        st(c, in, ea, 8, 0, src);
        return;
    case 0x50: {                                                       /* MOVMSKPS r32, xmm */
        if (sc || mem) ud(c);
        uint32_t m = 0;
        for (int i = 0; i < 4; i++) m |= (lane(F.xmm[s & 7], i) >> 31) << i;
        c->r[d] = m;
        return;
    }
    /* ---- conversions with MMX and general registers ---- */
    case 0x2A:
        if (sc) {                                                      /* CVTSI2SS xmm, r/m32 */
            uint32_t v = mem ? (ld(c, in, ea, 4, 0, src), (uint32_t)src[0]) : c->r[s];
            uint32_t f = i2f(c, v, &fl);
            if (raise(c, fl)) return;
            set_lane(x, 0, f);
            return;
        }
        if (mem) ld(c, in, ea, 8, 0, src);                             /* CVTPI2PS xmm, mm/m64 */
        else { x86_mmx_enter(c); src[0] = F.sig[s & 7]; }
        r[0] = (uint64_t)i2f(c, (uint32_t)src[0], &fl) | (uint64_t)i2f(c, (uint32_t)(src[0] >> 32), &fl) << 32;
        if (raise(c, fl)) return;
        x[0] = r[0];
        return;
    case 0x2C: case 0x2D: {                                            /* CVT(T)PS2PI mm, xmm/m64; CVT(T)SS2SI r32, xmm/m32 */
        int t = op == 0x2C;
        if (mem) ld(c, in, ea, sc ? 4 : 8, 0, src);
        else { src[0] = F.xmm[s & 7][0]; }
        if (sc) {
            uint32_t v = f2i(c, (uint32_t)src[0], t, &fl);
            if (raise(c, fl)) return;
            c->r[d] = v;
            return;
        }
        x86_mmx_enter(c);
        uint64_t q = (uint64_t)f2i(c, (uint32_t)src[0], t, &fl) | (uint64_t)f2i(c, (uint32_t)(src[0] >> 32), t, &fl) << 32;
        if (raise(c, fl)) return;
        F.sig[d & 7] = q; F.sexp[d & 7] = 0xFFFF;
        return;
    }
    case 0x2E: case 0x2F: {                                            /* UCOMISS, COMISS */
        if (sc) ud(c);
        if (mem) ld(c, in, ea, 4, 0, src); else src[0] = F.xmm[s & 7][0];
        uint32_t a = lane(x, 0), b = (uint32_t)src[0];
        int un = is_nan(a) || is_nan(b);
        if (is_snan(a) || is_snan(b) || (op == 0x2F && un)) fl |= M_IE;
        if (!un && (is_den(a) || is_den(b))) fl |= M_DE;
        if (raise(c, fl)) return;
        uint32_t z;
        if (un) z = X86_ZF | X86_PF | X86_CF;
        else if (f32_eq((float32_t){ a }, (float32_t){ b })) z = X86_ZF;
        else z = f32_lt_quiet((float32_t){ a }, (float32_t){ b }) ? X86_CF : 0;
        c->eflags = (c->eflags & ~(uint32_t)(X86_ZF | X86_PF | X86_CF | X86_OF | X86_SF | X86_AF)) | z;
        return;
    }
    default: break;
    }

    /* the rest: xmm op= xmm/m128 (packed, 16-aligned) or xmm/m32 (scalar) */
    if (mem) {
        int sh = op == 0xC6 || op == 0x14 || op == 0x15 || (op >= 0x54 && op <= 0x57);   /* no scalar form */
        if (sc && sh) ud(c);
        ld(c, in, ea, sc ? 4 : 16, !sc, src);
    } else { src[0] = F.xmm[s & 7][0]; src[1] = F.xmm[s & 7][1]; }
    int n = sc ? 1 : 4;
    switch (op) {
    case 0x51: case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        int o = op == 0x51 ? O_SQRT : op == 0x58 ? O_ADD : op == 0x59 ? O_MUL : op == 0x5C ? O_SUB
              : op == 0x5D ? O_MIN : op == 0x5E ? O_DIV : O_MAX;
        for (int i = 0; i < n; i++) set_lane(r, i, f32op(c, o, lane(x, i), lane(src, i), &fl));
        break;
    }
    case 0x52: case 0x53:                                              /* RSQRTPS/SS, RCPPS/SS */
        for (int i = 0; i < n; i++) set_lane(r, i, approx(lane(src, i), op == 0x52));
        break;
    case 0x54: if (sc) ud(c); r[0] = x[0] & src[0]; r[1] = x[1] & src[1]; break;      /* ANDPS */
    case 0x55: if (sc) ud(c); r[0] = ~x[0] & src[0]; r[1] = ~x[1] & src[1]; break;    /* ANDNPS */
    case 0x56: if (sc) ud(c); r[0] = x[0] | src[0]; r[1] = x[1] | src[1]; break;      /* ORPS */
    case 0x57: if (sc) ud(c); r[0] = x[0] ^ src[0]; r[1] = x[1] ^ src[1]; break;      /* XORPS */
    case 0x14:                                                         /* UNPCKLPS */
        if (sc) ud(c);
        set_lane(r, 0, lane(x, 0)); set_lane(r, 1, lane(src, 0)); set_lane(r, 2, lane(x, 1)); set_lane(r, 3, lane(src, 1));
        break;
    case 0x15:                                                         /* UNPCKHPS */
        if (sc) ud(c);
        set_lane(r, 0, lane(x, 2)); set_lane(r, 1, lane(src, 2)); set_lane(r, 2, lane(x, 3)); set_lane(r, 3, lane(src, 3));
        break;
    case 0xC6: {                                                       /* SHUFPS */
        if (sc) ud(c);
        uint32_t im = in->ops[1].imm;
        set_lane(r, 0, lane(x, im & 3)); set_lane(r, 1, lane(x, (im >> 2) & 3));
        set_lane(r, 2, lane(src, (im >> 4) & 3)); set_lane(r, 3, lane(src, (im >> 6) & 3));
        break;
    }
    case 0xC2: {                                                       /* CMPPS, CMPSS */
        int p = in->ops[1].imm & 7;
        for (int i = 0; i < n; i++) set_lane(r, i, cmp_pred(lane(x, i), lane(src, i), p, &fl) ? 0xFFFFFFFFu : 0);
        break;
    }
    default: ud(c); return;
    }
    if (raise(c, fl)) return;
    x[0] = r[0]; x[1] = r[1];
}
