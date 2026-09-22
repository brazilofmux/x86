/* x86_interp.c — the oracle
 *
 * Straightforward, eager-flags, byte-exact interpreter over decoded
 * instructions. It exists to be right, not fast: it is validated
 * against the SingleStepTests 8088 suite and then serves as the -V
 * lockstep reference for the DBT. Every undefined-flag choice made
 * here is therefore a contract the JIT must honour too; they are
 * called out in comments as "CONTRACT".
 */

#include "x86.h"
#include "x86_decode.h"
#include <string.h>

/* parity_even[b] = 1 if b has an even number of set bits (PF semantics) */
#define P2(n) n, n ^ 1, n ^ 1, n
#define P4(n) P2(n), P2(n ^ 1), P2(n ^ 1), P2(n)
#define P6(n) P4(n), P4(n ^ 1), P4(n ^ 1), P4(n)
static const uint8_t parity_even[256] = { P6(1), P6(0), P6(0), P6(1) };

#define RAISE(v) do { c->exc = (v); return; } while (0)

static inline uint32_t szmask(int size) { return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFu; }
static inline uint32_t signbit(int size) { return size == 1 ? 0x80 : size == 2 ? 0x8000 : 0x80000000u; }
static inline int32_t sext(uint32_t v, int size) { return size == 1 ? (int8_t)v : size == 2 ? (int16_t)v : (int32_t)v; }

/* ------------------------------------------------------------------------
 * Flags
 * --------------------------------------------------------------------- */
static inline void set_szp(x86_cpu *c, uint32_t res, int size) {
    res &= szmask(size);
    c->eflags &= ~(X86_SF | X86_ZF | X86_PF);
    if (res & signbit(size)) c->eflags |= X86_SF;
    if (res == 0) c->eflags |= X86_ZF;
    if (parity_even[res & 0xFF]) c->eflags |= X86_PF;
}

static inline uint32_t do_add(x86_cpu *c, uint32_t a, uint32_t b, int cin, int size) {
    uint32_t m = szmask(size);
    a &= m; b &= m;
    uint64_t r64 = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)r64 & m;
    c->eflags &= ~X86_ARITH_FLAGS;
    if (r64 >> (size * 8)) c->eflags |= X86_CF;
    if ((a ^ b ^ r) & 0x10) c->eflags |= X86_AF;
    if (((a ^ r) & (b ^ r)) & signbit(size)) c->eflags |= X86_OF;
    set_szp(c, r, size);
    return r;
}

static inline uint32_t do_sub(x86_cpu *c, uint32_t a, uint32_t b, int cin, int size) {
    uint32_t m = szmask(size);
    a &= m; b &= m;
    uint64_t r64 = (uint64_t)a - b - cin;
    uint32_t r = (uint32_t)r64 & m;
    c->eflags &= ~X86_ARITH_FLAGS;
    if (r64 >> (size * 8)) c->eflags |= X86_CF;
    if ((a ^ b ^ r) & 0x10) c->eflags |= X86_AF;
    if (((a ^ b) & (a ^ r)) & signbit(size)) c->eflags |= X86_OF;
    set_szp(c, r, size);
    return r;
}

/* CONTRACT: logic ops clear CF, OF and AF. */
static inline uint32_t do_logic(x86_cpu *c, uint32_t r, int size) {
    c->eflags &= ~X86_ARITH_FLAGS;
    set_szp(c, r, size);
    return r & szmask(size);
}

/* ------------------------------------------------------------------------
 * Memory and operands
 * --------------------------------------------------------------------- */
static inline uint32_t admask(const x86_insn *in) { return in->adsize == 2 ? 0xFFFF : 0xFFFFFFFFu; }
static inline uint32_t stkmask(x86_cpu *c) { return c->seg[S_SS].big ? 0xFFFFFFFFu : 0xFFFF; }

/* CONTRACT: a word straddling offset FFFF wraps within the segment on
 * the 8086/186 (silicon-verified by the 8088 suite). A 286/386 would
 * raise #GP there; we access linearly instead, which is what the JIT's
 * unchecked host-pointer loads do, and no program that survives real
 * hardware can tell the difference. */
static inline uint32_t wrapmask(const x86_cpu *c, uint32_t m) { return c->model >= X86_MODEL_286 ? 0xFFFFFFFFu : m; }

static uint32_t calc_ea(x86_cpu *c, const x86_insn *in) {
    uint32_t ea = (uint32_t)in->disp;
    if (in->base >= 0) ea += c->r[in->base];
    if (in->index >= 0) ea += c->r[in->index] << in->scale;
    return ea & admask(in);
}

static inline uint32_t mrd(x86_cpu *c, const x86_insn *in, int seg, uint32_t off, int size) {
    return x86_rd(c, c->seg[seg].base, off, wrapmask(c, admask(in)), size);
}
static inline void mwr(x86_cpu *c, const x86_insn *in, int seg, uint32_t off, int size, uint32_t v) {
    x86_wr(c, c->seg[seg].base, off, wrapmask(c, admask(in)), size, v);
}

static uint32_t rd_op(x86_cpu *c, const x86_insn *in, int i, uint32_t ea) {
    const x86_operand *o = &in->ops[i];
    switch (o->kind) {
    case OPK_REG:  return x86_get_reg(c, o->reg, o->size);
    case OPK_SREG: return c->seg[o->reg].sel;
    case OPK_IMM:  return o->imm;
    case OPK_MEM:  return mrd(c, in, in->seg, ea, o->size);
    }
    return 0;
}

static void wr_op(x86_cpu *c, const x86_insn *in, int i, uint32_t ea, uint32_t v) {
    const x86_operand *o = &in->ops[i];
    switch (o->kind) {
    case OPK_REG:  x86_set_reg(c, o->reg, o->size, v); break;
    case OPK_MEM:  mwr(c, in, in->seg, ea, o->size, v); break;
    }
}

static void push(x86_cpu *c, int size, uint32_t v) {
    uint32_t m = stkmask(c);
    uint32_t sp = (c->r[R_SP] - size) & m;
    c->r[R_SP] = m == 0xFFFF ? ((c->r[R_SP] & 0xFFFF0000u) | sp) : sp;
    x86_wr(c, c->seg[S_SS].base, sp, wrapmask(c, m), size, v);
}

static uint32_t pop(x86_cpu *c, int size) {
    uint32_t m = stkmask(c);
    uint32_t sp = c->r[R_SP] & m;
    uint32_t v = x86_rd(c, c->seg[S_SS].base, sp, wrapmask(c, m), size);
    sp = (sp + size) & m;
    c->r[R_SP] = m == 0xFFFF ? ((c->r[R_SP] & 0xFFFF0000u) | sp) : sp;
    return v;
}

static inline void set_ip(x86_cpu *c, uint32_t ip) {
    c->eip = c->seg[S_CS].big ? ip : (ip & 0xFFFF);
}

/* Segment register load. Real mode only for now; Phase B hooks PM here. */
static void load_seg(x86_cpu *c, int s, uint32_t sel) {
    x86_load_seg(c, s, (uint16_t)sel);
    if (s == S_SS) c->int_inhibit = 1;
}

/* Real-mode interrupt delivery: 16-bit pushes, vector from the IVT at 0. */
void x86_interrupt(x86_cpu *c, int vector, int is_sw) {
    (void)is_sw;
    push(c, 2, c->eflags & 0xFFFF);
    c->eflags &= ~(X86_IF | X86_TF);
    push(c, 2, c->seg[S_CS].sel);
    push(c, 2, c->eip & 0xFFFF);
    uint32_t off = x86_rd(c, 0, vector * 4, 0xFFFFFFFFu, 2);
    uint32_t sel = x86_rd(c, 0, vector * 4 + 2, 0xFFFFFFFFu, 2);
    load_seg(c, S_CS, sel);
    c->int_inhibit = 0;
    set_ip(c, off);
}

/* ------------------------------------------------------------------------
 * Shifts and rotates. Counts are applied one bit at a time so the
 * flags after a multi-bit shift are exactly what the 8086 microcode
 * produces (OF from the final step, etc.). 186+ masks the count to 5
 * bits; the 8086 does not. CONTRACT: count 0 leaves all flags alone.
 * --------------------------------------------------------------------- */
static uint32_t do_shift(x86_cpu *c, int op, uint32_t v, uint32_t cnt, int size) {
    if (c->model > X86_MODEL_8086) cnt &= 0x1F;
    uint32_t m = szmask(size), sb = signbit(size);
    int bits = size * 8;
    v &= m;
    if (cnt == 0) return v;
    uint32_t cf = (c->eflags & X86_CF) != 0, of = (c->eflags & X86_OF) != 0;
    switch (op) {
    case OP_ROL:
        for (uint32_t i = 0; i < cnt; i++) { cf = (v & sb) != 0; v = ((v << 1) | cf) & m; }
        of = ((v & sb) != 0) ^ cf;
        break;
    case OP_ROR:
        for (uint32_t i = 0; i < cnt; i++) { cf = v & 1; v = (v >> 1) | (cf << (bits - 1)); }
        of = ((v & sb) != 0) ^ ((v >> (bits - 2)) & 1);
        break;
    case OP_RCL:
        for (uint32_t i = 0; i < cnt; i++) { uint32_t nc = (v & sb) != 0; v = ((v << 1) | cf) & m; cf = nc; }
        of = ((v & sb) != 0) ^ cf;
        break;
    case OP_RCR:
        for (uint32_t i = 0; i < cnt; i++) {
            of = ((v & sb) != 0) ^ cf;
            uint32_t nc = v & 1; v = (v >> 1) | (cf << (bits - 1)); cf = nc;
        }
        break;
    case OP_SHL: case OP_SAL:
        for (uint32_t i = 0; i < cnt; i++) { cf = (v & sb) != 0; v = (v << 1) & m; }
        of = ((v & sb) != 0) ^ cf;
        set_szp(c, v, size);
        c->eflags &= ~X86_AF;                       /* CONTRACT: AF cleared by shifts */
        break;
    case OP_SHR:
        for (uint32_t i = 0; i < cnt; i++) { of = (v & sb) != 0; cf = v & 1; v >>= 1; }
        set_szp(c, v, size);
        c->eflags &= ~X86_AF;
        break;
    case OP_SAR:
        for (uint32_t i = 0; i < cnt; i++) { cf = v & 1; v = (v >> 1) | (v & sb); }
        of = 0;
        set_szp(c, v, size);
        c->eflags &= ~X86_AF;
        break;
    }
    c->eflags = (c->eflags & ~(X86_CF | X86_OF)) | (cf ? X86_CF : 0) | (of ? X86_OF : 0);
    return v;
}

/* ------------------------------------------------------------------------
 * Multiply / divide
 * --------------------------------------------------------------------- */
/* CONTRACT: MUL/IMUL set SF/ZF/PF from the low half of the result and
 * clear AF (Intel leaves them undefined). */
static void do_mul(x86_cpu *c, uint32_t src, int size) {
    uint32_t a = x86_get_reg(c, R_AX, size);
    uint64_t r = (uint64_t)a * (src & szmask(size));
    uint32_t lo = (uint32_t)r & szmask(size), hi = (uint32_t)(r >> (size * 8)) & szmask(size);
    if (size == 1) x86_set_r16(c, R_AX, (uint16_t)r);
    else { x86_set_reg(c, R_AX, size, lo); x86_set_reg(c, R_DX, size, hi); }
    c->eflags &= ~X86_ARITH_FLAGS;
    set_szp(c, lo, size);
    if (hi) c->eflags |= X86_CF | X86_OF;
}

static void do_imul1(x86_cpu *c, uint32_t src, int size) {
    int64_t r = (int64_t)sext(x86_get_reg(c, R_AX, size), size) * sext(src, size);
    uint32_t lo = (uint32_t)r & szmask(size), hi = (uint32_t)((uint64_t)r >> (size * 8)) & szmask(size);
    if (size == 1) x86_set_r16(c, R_AX, (uint16_t)r);
    else { x86_set_reg(c, R_AX, size, lo); x86_set_reg(c, R_DX, size, hi); }
    c->eflags &= ~X86_ARITH_FLAGS;
    set_szp(c, lo, size);
    if (r != (int64_t)sext(lo, size)) c->eflags |= X86_CF | X86_OF;
}

static uint32_t do_imul2(x86_cpu *c, uint32_t a, uint32_t b, int size) {
    int64_t r = (int64_t)sext(a, size) * sext(b, size);
    uint32_t lo = (uint32_t)r & szmask(size);
    c->eflags &= ~X86_ARITH_FLAGS;
    set_szp(c, lo, size);
    if (r != (int64_t)sext(lo, size)) c->eflags |= X86_CF | X86_OF;
    return lo;
}

/* ------------------------------------------------------------------------
 * 8086/8088 division, microcode-exact.
 *
 * A translation of the CORD / PREIDIV / POSTIDIV microcode routines (after
 * MartyPC's muldiv.rs by Daniel Balsom, MIT; the microcode itself per
 * reenigne's disassembly). It exists so that the flags left behind —
 * including the ones pushed on a #DE trap — and the two genuine quirks
 * (IDIV rejects the most negative quotient; a REP prefix negates the
 * IDIV quotient because it sets the same internal F1 flag NEGATE
 * toggles) match the silicon. Returns 0 or -1 (trap, flags set).
 * --------------------------------------------------------------------- */
static int div_8086(x86_cpu *c, uint32_t hi, uint32_t lo, uint32_t divisor,
                    int size, int is_signed, int negate, uint32_t *q_out, uint32_t *r_out) {
    uint32_t m = szmask(size), sb = signbit(size);
    uint32_t tmpa = hi & m, tmpc = lo & m, tmpb = divisor & m;
    int carry;

    if (is_signed) {                                   /* PREIDIV → NEGATE */
        if (tmpa & sb) {                               /* dividend negative */
            int cy = tmpc != 0;                        /* 1b6: NEG tmpc */
            tmpc = (0 - tmpc) & m;
            tmpa = cy ? (~tmpa & m) : ((0 - tmpa) & m);/* 1b8-1ba */
            negate = !negate;
        }
        carry = (tmpb & sb) != 0;                      /* 1bb: LRCY tmpb */
        c->eflags = (c->eflags & ~X86_CF) | (carry ? X86_CF : 0);
        if (carry) { tmpb = (0 - tmpb) & m; negate = !negate; }   /* 1be */
    }

    /* CORD */
    do_sub(c, tmpa, tmpb, 0, size);                    /* 188: SUBT tmpa, flags */
    carry = (c->eflags & X86_CF) != 0;
    if (!carry) return -1;                             /* 18a: NCY INT0 */
    for (int counter = size * 8; counter > 0; counter--) {
        uint32_t nc;
        nc = (tmpc & sb) != 0; tmpc = ((tmpc << 1) | carry) & m; carry = nc;   /* 18c: RCL tmpc */
        nc = (tmpa & sb) != 0; tmpa = ((tmpa << 1) | carry) & m; carry = nc;   /* 18d: RCL tmpa */
        if (carry) {                                   /* 18e → 195/196 */
            carry = 0;
            tmpa = (tmpa - tmpb) & m;
        } else {
            do_sub(c, tmpa, tmpb, 0, size);            /* 18f: SUBT, flags */
            carry = (c->eflags & X86_CF) != 0;
            if (!carry) tmpa = (tmpa - tmpb) & m;      /* 190 → 196 */
        }
    }
    {
        uint32_t nc = (tmpc & sb) != 0;                /* 192/193: RCL tmpc */
        tmpc = ((tmpc << 1) | carry) & m; carry = nc;
    }
    carry = (tmpc & sb) != 0;                          /* 194: RCL (no dest) → CF */
    c->eflags = (c->eflags & ~X86_CF) | (carry ? X86_CF : 0);

    uint32_t quot = ~tmpc & m;                         /* 164: COM1 tmpc */
    if (is_signed) {                                   /* POSTIDIV */
        if (!carry) return -1;                         /* 1c4: NCY INT0 */
        if (hi & sb) tmpa = (0 - tmpa) & m;            /* 1c5-1c8: remainder takes dividend sign */
        quot = negate ? ((tmpc + 1) & m) : (~tmpc & m);/* 1c9-1cb */
        c->eflags &= ~(X86_CF | X86_OF);               /* 1cc: CCOF */
    }
    *q_out = quot; *r_out = tmpa;
    return 0;
}

/* CONTRACT (186+): DIV/IDIV leave all flags unchanged. */
static void do_div(x86_cpu *c, uint32_t src, int size, int is_signed, int negate) {
    uint32_t hi = size == 1 ? x86_get_r8(c, R_AH) : x86_get_reg(c, R_DX, size);
    uint32_t lo = size == 1 ? x86_get_r8(c, R_AL) : x86_get_reg(c, R_AX, size);
    uint32_t q, r;

    if (c->model == X86_MODEL_8086) {
        if (div_8086(c, hi, lo, src, size, is_signed, negate, &q, &r) < 0) RAISE(X86_EXC_DE);
    } else if (!is_signed) {
        src &= szmask(size);
        if (src == 0) RAISE(X86_EXC_DE);
        uint64_t dividend = ((uint64_t)hi << (size * 8)) | lo;
        uint64_t q64 = dividend / src;
        if (q64 > szmask(size)) RAISE(X86_EXC_DE);
        q = (uint32_t)q64; r = (uint32_t)(dividend % src);
    } else {
        int64_t divisor = sext(src, size);
        if (divisor == 0) RAISE(X86_EXC_DE);
        int64_t dividend = size == 1 ? (int16_t)((hi << 8) | lo)
                         : size == 2 ? (int32_t)((hi << 16) | lo)
                         : (int64_t)(((uint64_t)hi << 32) | lo);
        int64_t q64 = dividend / divisor, r64 = dividend % divisor;
        int64_t qmax = (int64_t)(signbit(size) - 1);
        if (q64 > qmax || q64 < -qmax - 1) RAISE(X86_EXC_DE);
        q = (uint32_t)q64; r = (uint32_t)r64;
    }
    if (size == 1) x86_set_r16(c, R_AX, (uint16_t)(((r & 0xFF) << 8) | (q & 0xFF)));
    else { x86_set_reg(c, R_AX, size, q); x86_set_reg(c, R_DX, size, r); }
}

/* ------------------------------------------------------------------------
 * String instructions
 * --------------------------------------------------------------------- */
static void do_string(x86_cpu *c, const x86_insn *in) {
    int size = in->ops[0].size;
    uint32_t am = admask(in);
    int32_t delta = (c->eflags & X86_DF) ? -size : size;
    int rep = in->rep;
    int test_z = (in->op == OP_CMPS || in->op == OP_SCAS);

    for (;;) {
        if (rep && (c->r[R_CX] & am) == 0) break;
        uint32_t si = c->r[R_SI] & am, di = c->r[R_DI] & am;
        uint32_t a, b;
        switch (in->op) {
        case OP_MOVS:
            a = mrd(c, in, in->seg, si, size);
            mwr(c, in, S_ES, di, size, a);
            si += delta; di += delta;
            break;
        case OP_CMPS:
            a = mrd(c, in, in->seg, si, size);
            b = mrd(c, in, S_ES, di, size);
            do_sub(c, a, b, 0, size);
            si += delta; di += delta;
            break;
        case OP_STOS:
            mwr(c, in, S_ES, di, size, x86_get_reg(c, R_AX, size));
            di += delta;
            break;
        case OP_LODS:
            x86_set_reg(c, R_AX, size, mrd(c, in, in->seg, si, size));
            si += delta;
            break;
        case OP_SCAS:
            b = mrd(c, in, S_ES, di, size);
            do_sub(c, x86_get_reg(c, R_AX, size), b, 0, size);
            di += delta;
            break;
        case OP_INS:
            a = c->io_read ? c->io_read(c, x86_get_r16(c, R_DX), size) : szmask(size);
            mwr(c, in, S_ES, di, size, a);
            di += delta;
            break;
        case OP_OUTS:
            a = mrd(c, in, in->seg, si, size);
            if (c->io_write) c->io_write(c, x86_get_r16(c, R_DX), a, size);
            si += delta;
            break;
        }
        c->r[R_SI] = am == 0xFFFF ? ((c->r[R_SI] & 0xFFFF0000u) | (si & 0xFFFF)) : si;
        c->r[R_DI] = am == 0xFFFF ? ((c->r[R_DI] & 0xFFFF0000u) | (di & 0xFFFF)) : di;
        if (!rep) break;
        uint32_t cx = (c->r[R_CX] - 1) & am;
        c->r[R_CX] = am == 0xFFFF ? ((c->r[R_CX] & 0xFFFF0000u) | cx) : cx;
        if (test_z) {
            int zf = (c->eflags & X86_ZF) != 0;
            if (rep == 0xF3 && !zf) break;
            if (rep == 0xF2 && zf) break;
        }
    }
}

/* ------------------------------------------------------------------------
 * Bit test helpers: for a memory operand with a register bit index the
 * address is adjusted by the signed bit offset / operand width.
 * --------------------------------------------------------------------- */
static uint32_t bt_common(x86_cpu *c, const x86_insn *in, uint32_t ea, uint32_t *bitpos, uint32_t *addr) {
    int size = in->ops[0].size;
    uint32_t idx = rd_op(c, in, 1, ea);
    if (in->ops[0].kind == OPK_MEM && in->ops[1].kind == OPK_REG) {
        int32_t s = sext(idx, size);
        int32_t elem = (s >= 0) ? s / (size * 8) : -((-s + size * 8 - 1) / (size * 8));
        *addr = (ea + (uint32_t)(elem * size)) & admask(in);
        *bitpos = (uint32_t)(s - elem * size * 8);
    } else {
        *addr = ea;
        *bitpos = idx & (size * 8 - 1);
    }
    return in->ops[0].kind == OPK_MEM ? mrd(c, in, in->seg, *addr, size) : rd_op(c, in, 0, ea);
}

/* ------------------------------------------------------------------------
 * Execute one decoded instruction. c->eip already points past it.
 * --------------------------------------------------------------------- */
static void execute(x86_cpu *c, const x86_insn *in, uint32_t start_ip) {
    uint32_t ea = in->ea_valid ? calc_ea(c, in) : 0;
    int size = in->ops[0].size;
    uint32_t a, b, r;

    switch (in->op) {
    /* ---- arithmetic ------------------------------------------------ */
    case OP_ADD: case OP_ADC: case OP_SUB: case OP_SBB: case OP_CMP: {
        a = rd_op(c, in, 0, ea); b = rd_op(c, in, 1, ea);
        int cin = (in->op == OP_ADC || in->op == OP_SBB) ? (c->eflags & X86_CF) : 0;
        r = (in->op == OP_ADD || in->op == OP_ADC) ? do_add(c, a, b, cin, size) : do_sub(c, a, b, cin, size);
        if (in->op != OP_CMP) wr_op(c, in, 0, ea, r);
        break;
    }
    case OP_AND: case OP_OR: case OP_XOR: case OP_TEST:
        a = rd_op(c, in, 0, ea); b = rd_op(c, in, 1, ea);
        r = in->op == OP_AND || in->op == OP_TEST ? a & b : in->op == OP_OR ? a | b : a ^ b;
        r = do_logic(c, r, size);
        if (in->op != OP_TEST) wr_op(c, in, 0, ea, r);
        break;
    case OP_NOT:
        wr_op(c, in, 0, ea, ~rd_op(c, in, 0, ea));
        break;
    case OP_NEG:
        a = rd_op(c, in, 0, ea);
        r = do_sub(c, 0, a, 0, size);
        wr_op(c, in, 0, ea, r);
        break;
    case OP_INC: case OP_DEC: {
        uint32_t cf = c->eflags & X86_CF;
        a = rd_op(c, in, 0, ea);
        r = in->op == OP_INC ? do_add(c, a, 1, 0, size) : do_sub(c, a, 1, 0, size);
        c->eflags = (c->eflags & ~X86_CF) | cf;
        wr_op(c, in, 0, ea, r);
        break;
    }
    case OP_ROL: case OP_ROR: case OP_RCL: case OP_RCR:
    case OP_SHL: case OP_SHR: case OP_SAL: case OP_SAR:
        a = rd_op(c, in, 0, ea); b = rd_op(c, in, 1, ea);
        r = do_shift(c, in->op, a, b & 0xFF, size);
        wr_op(c, in, 0, ea, r);
        break;
    case OP_SETMO:
        /* CONTRACT (8086 only): count 0 does nothing; otherwise the
         * operand becomes all ones. Flags are undefined; we leave them. */
        if ((rd_op(c, in, 1, ea) & 0xFF) != 0) wr_op(c, in, 0, ea, 0xFFFFFFFFu);
        break;
    case OP_MUL:
        do_mul(c, rd_op(c, in, 0, ea), size);
        break;
    case OP_IMUL:
        if (in->opcode2 == 0xAF) {                       /* IMUL Gv,Ev */
            a = rd_op(c, in, 0, ea); b = rd_op(c, in, 1, ea);
            wr_op(c, in, 0, ea, do_imul2(c, a, b, size));
        } else {
            do_imul1(c, rd_op(c, in, 0, ea), size);
        }
        break;
    case OP_IMUL3:
        b = rd_op(c, in, 1, ea);
        wr_op(c, in, 0, ea, do_imul2(c, b, in->imm2, size));
        break;
    case OP_DIV:
        do_div(c, rd_op(c, in, 0, ea), size, 0, 0);
        break;
    case OP_IDIV:
        do_div(c, rd_op(c, in, 0, ea), size, 1, in->rep != 0);
        break;

    /* ---- BCD -------------------------------------------------------- */
    case OP_DAA: case OP_DAS: {
        uint32_t al = x86_get_r8(c, R_AL), old_al = al;
        int old_cf = (c->eflags & X86_CF) != 0, af = 0;
        int sub = in->op == OP_DAS;
        if ((al & 0xF) > 9 || (c->eflags & X86_AF)) {
            al = (sub ? al - 6 : al + 6) & 0xFF;
            af = 1;
        }
        /* Intel: high adjust if old_AL > 99 or CF. The 8086/8088 skips
         * the 9A..9F carry-through case when AF was already set (its
         * microcode never performs the low-nibble compare then), so it
         * adjusts only for old_AL >= A0. Measured from the 8088 tests. */
        int cf = old_cf || old_al >= 0xA0 ||
                 (old_al > 0x99 && !(c->model == X86_MODEL_8086 && (c->eflags & X86_AF)));
        if (cf) al = (sub ? al - 0x60 : al + 0x60) & 0xFF;
        x86_set_r8(c, R_AL, al);
        c->eflags &= ~(X86_CF | X86_AF | X86_OF);      /* CONTRACT: OF cleared */
        if (cf) c->eflags |= X86_CF;
        if (af) c->eflags |= X86_AF;
        set_szp(c, al, 1);
        break;
    }
    case OP_AAA: case OP_AAS: {
        uint32_t al = x86_get_r8(c, R_AL), ah = x86_get_r8(c, R_AH);
        int adjust = (al & 0xF) > 9 || (c->eflags & X86_AF);
        c->eflags &= ~X86_ARITH_FLAGS;                 /* CONTRACT: OF/SF/ZF/PF cleared */
        if (adjust) {
            if (in->op == OP_AAA) { al += 6; ah += 1; } else { al -= 6; ah -= 1; }
            c->eflags |= X86_AF | X86_CF;
        }
        x86_set_r8(c, R_AL, al & 0xF);
        x86_set_r8(c, R_AH, ah);
        break;
    }
    case OP_AAM: {
        uint32_t d = in->ops[0].imm;
        if (d == 0) {
            /* 8088: the microcode has already set flags for a zero result
             * by the time it traps, and they are what INT 0 pushes. */
            c->eflags &= ~X86_ARITH_FLAGS;
            set_szp(c, 0, 1);
            RAISE(X86_EXC_DE);
        }
        uint32_t al = x86_get_r8(c, R_AL);
        x86_set_r8(c, R_AH, al / d);
        x86_set_r8(c, R_AL, al % d);
        c->eflags &= ~X86_ARITH_FLAGS;                 /* CONTRACT: OF/AF/CF cleared */
        set_szp(c, al % d, 1);
        break;
    }
    case OP_AAD: {
        uint32_t d = in->ops[0].imm;
        uint32_t al = (x86_get_r8(c, R_AH) * d + x86_get_r8(c, R_AL)) & 0xFF;
        x86_set_r8(c, R_AL, al);
        x86_set_r8(c, R_AH, 0);
        c->eflags &= ~X86_ARITH_FLAGS;
        set_szp(c, al, 1);
        break;
    }
    case OP_SALC:
        x86_set_r8(c, R_AL, (c->eflags & X86_CF) ? 0xFF : 0);
        break;

    /* ---- data movement ---------------------------------------------- */
    case OP_MOV:
        wr_op(c, in, 0, ea, rd_op(c, in, 1, ea));
        break;
    case OP_MOVSEG:
        if (in->ops[0].kind == OPK_SREG) {
            if (in->ops[0].reg == S_CS && c->model > X86_MODEL_8086) RAISE(X86_EXC_UD);
            load_seg(c, in->ops[0].reg, rd_op(c, in, 1, ea));
        } else {
            wr_op(c, in, 0, ea, c->seg[in->ops[1].reg].sel);
        }
        break;
    case OP_XCHG:
        a = rd_op(c, in, 0, ea); b = rd_op(c, in, 1, ea);
        wr_op(c, in, 0, ea, b); wr_op(c, in, 1, ea, a);
        break;
    case OP_LEA:
        wr_op(c, in, 0, ea, ea);
        break;
    case OP_XLAT: {
        uint32_t off = (c->r[R_BX] + x86_get_r8(c, R_AL)) & admask(in);
        x86_set_r8(c, R_AL, mrd(c, in, in->seg, off, 1));
        break;
    }
    case OP_CBW:
        if (in->opsize == 2) x86_set_r16(c, R_AX, (uint16_t)(int8_t)x86_get_r8(c, R_AL));
        else x86_set_r32(c, R_AX, (uint32_t)(int16_t)x86_get_r16(c, R_AX));
        break;
    case OP_CWD:
        if (in->opsize == 2) x86_set_r16(c, R_DX, (x86_get_r16(c, R_AX) & 0x8000) ? 0xFFFF : 0);
        else x86_set_r32(c, R_DX, (c->r[R_AX] & 0x80000000u) ? 0xFFFFFFFFu : 0);
        break;
    case OP_SAHF:
        c->eflags = (c->eflags & ~0xD5u) | (x86_get_r8(c, R_AH) & 0xD5);
        break;
    case OP_LAHF:
        x86_set_r8(c, R_AH, (c->eflags & 0xD7) | 0x02);
        break;
    case OP_MOVZX:
        wr_op(c, in, 0, ea, rd_op(c, in, 1, ea));
        break;
    case OP_MOVSX:
        wr_op(c, in, 0, ea, (uint32_t)sext(rd_op(c, in, 1, ea), in->ops[1].size));
        break;
    case OP_SETCC:
        wr_op(c, in, 0, ea, x86_cond(c->eflags, in->cond));
        break;
    case OP_LES: case OP_LDS: case OP_LSS: case OP_LFS: case OP_LGS: {
        static const uint8_t which[] = { S_ES, S_DS, S_SS, S_FS, S_GS };
        a = mrd(c, in, in->seg, ea, in->opsize);
        b = mrd(c, in, in->seg, (ea + in->opsize) & admask(in), 2);
        wr_op(c, in, 0, ea, a);
        load_seg(c, which[in->op - OP_LES], b);
        break;
    }
    case OP_BSWAP:
        a = rd_op(c, in, 0, ea);
        wr_op(c, in, 0, ea, (a >> 24) | ((a >> 8) & 0xFF00) | ((a << 8) & 0xFF0000) | (a << 24));
        break;
    case OP_XADD:
        a = rd_op(c, in, 0, ea); b = rd_op(c, in, 1, ea);
        r = do_add(c, a, b, 0, size);
        wr_op(c, in, 1, ea, a); wr_op(c, in, 0, ea, r);
        break;
    case OP_CMPXCHG: {
        a = rd_op(c, in, 0, ea);
        uint32_t acc = x86_get_reg(c, R_AX, size);
        do_sub(c, acc, a, 0, size);
        if (c->eflags & X86_ZF) wr_op(c, in, 0, ea, rd_op(c, in, 1, ea));
        else x86_set_reg(c, R_AX, size, a);
        break;
    }

    /* ---- stack ------------------------------------------------------ */
    case OP_PUSH:
        a = rd_op(c, in, 0, ea);
        /* 8086/8088 PUSH SP stores the already-decremented value */
        if (c->model == X86_MODEL_8086 && in->ops[0].kind == OPK_REG && in->ops[0].reg == R_SP && size == 2)
            a = (a - 2) & 0xFFFF;
        push(c, in->opsize, a);
        break;
    case OP_POP:
        a = pop(c, in->opsize);
        if (in->ops[0].kind == OPK_SREG) load_seg(c, in->ops[0].reg, a);
        else {
            /* POP [mem]: EA is computed after SP is incremented */
            if (in->ops[0].kind == OPK_MEM) ea = calc_ea(c, in);
            wr_op(c, in, 0, ea, a);
        }
        break;
    case OP_PUSHA: {
        uint32_t sp = x86_get_reg(c, R_SP, in->opsize);
        for (int i = 0; i < 8; i++) push(c, in->opsize, i == R_SP ? sp : x86_get_reg(c, i, in->opsize));
        break;
    }
    case OP_POPA:
        for (int i = 7; i >= 0; i--) {
            a = pop(c, in->opsize);
            if (i != R_SP) x86_set_reg(c, i, in->opsize, a);
        }
        break;
    case OP_PUSHF:
        push(c, in->opsize, in->opsize == 2 ? (c->eflags & 0xFFFF) : (c->eflags & ~(X86_RF | X86_VM)));
        break;
    case OP_POPF: {
        a = pop(c, in->opsize);
        uint32_t m = in->opsize == 2 ? 0xFFFF : 0xFFFFFFFFu;
        c->eflags = x86_flags_fixup(c, (c->eflags & ~m) | (a & m));
        break;
    }
    case OP_ENTER: {
        uint32_t fsize = in->ops[0].imm, level = in->ops[1].imm & 31;
        int os = in->opsize;
        uint32_t sm = stkmask(c);
        push(c, os, x86_get_reg(c, R_BP, os));
        uint32_t frame = c->r[R_SP] & sm;
        if (level > 0) {
            for (uint32_t i = 1; i < level; i++) {
                uint32_t bp = (x86_get_reg(c, R_BP, os) - os) & sm;
                x86_set_reg(c, R_BP, os, bp);
                push(c, os, x86_rd(c, c->seg[S_SS].base, bp, wrapmask(c, sm), os));
            }
            push(c, os, frame);
        }
        x86_set_reg(c, R_BP, os, frame);
        x86_set_reg(c, R_SP, os, (c->r[R_SP] - fsize) & sm);
        break;
    }
    case OP_LEAVE: {
        int os = in->opsize;
        x86_set_reg(c, R_SP, os, x86_get_reg(c, R_BP, os));
        x86_set_reg(c, R_BP, os, pop(c, os));
        break;
    }

    /* ---- control flow ---------------------------------------------- */
    case OP_JMP:
        a = rd_op(c, in, 0, ea);
        if (in->ops[0].kind == OPK_IMM) a += c->eip;
        set_ip(c, a);
        break;
    case OP_JCC:
        if (x86_cond(c->eflags, in->cond)) set_ip(c, c->eip + in->ops[0].imm);
        break;
    case OP_JCXZ:
        if ((c->r[R_CX] & admask(in)) == 0) set_ip(c, c->eip + in->ops[0].imm);
        break;
    case OP_LOOP: case OP_LOOPE: case OP_LOOPNE: {
        uint32_t am = admask(in);
        uint32_t cx = (c->r[R_CX] - 1) & am;
        c->r[R_CX] = am == 0xFFFF ? ((c->r[R_CX] & 0xFFFF0000u) | cx) : cx;
        int zf = (c->eflags & X86_ZF) != 0;
        int go = cx != 0 && (in->op == OP_LOOP || (in->op == OP_LOOPE ? zf : !zf));
        if (go) set_ip(c, c->eip + in->ops[0].imm);
        break;
    }
    case OP_CALL:
        a = rd_op(c, in, 0, ea);
        if (in->ops[0].kind == OPK_IMM) a += c->eip;
        push(c, in->opsize, c->eip);
        set_ip(c, a);
        break;
    case OP_CALLF: case OP_JMPF: {
        uint32_t off, sel;
        if (in->ops[0].kind == OPK_IMM) { off = in->ops[0].imm; sel = in->imm2; }
        else {
            off = mrd(c, in, in->seg, ea, in->opsize);
            sel = mrd(c, in, in->seg, (ea + in->opsize) & admask(in), 2);
        }
        if (in->op == OP_CALLF) {
            push(c, in->opsize, c->seg[S_CS].sel);
            push(c, in->opsize, c->eip);
        }
        load_seg(c, S_CS, sel);
        set_ip(c, off);
        break;
    }
    case OP_RET:
        a = pop(c, in->opsize);
        if (in->ops[0].kind == OPK_IMM) x86_set_reg(c, R_SP, stkmask(c) == 0xFFFF ? 2 : 4, c->r[R_SP] + in->ops[0].imm);
        set_ip(c, a);
        break;
    case OP_RETF: {
        a = pop(c, in->opsize);
        b = pop(c, in->opsize);
        if (in->ops[0].kind == OPK_IMM) x86_set_reg(c, R_SP, stkmask(c) == 0xFFFF ? 2 : 4, c->r[R_SP] + in->ops[0].imm);
        load_seg(c, S_CS, b);
        set_ip(c, a);
        break;
    }
    case OP_IRET: {
        a = pop(c, in->opsize);
        b = pop(c, in->opsize);
        uint32_t f = pop(c, in->opsize);
        uint32_t m = in->opsize == 2 ? 0xFFFF : 0xFFFFFFFFu;
        load_seg(c, S_CS, b);
        set_ip(c, a);
        c->eflags = x86_flags_fixup(c, (c->eflags & ~m) | (f & m));
        break;
    }
    case OP_INT:
        x86_interrupt(c, in->ops[0].imm, 1);
        break;
    case OP_INT3:
        x86_interrupt(c, 3, 1);
        break;
    case OP_INTO:
        if (c->eflags & X86_OF) x86_interrupt(c, 4, 1);
        break;
    case OP_INT1:
        x86_interrupt(c, 1, 1);
        break;
    case OP_BOUND: {
        int32_t idx = sext(rd_op(c, in, 0, ea), size);
        int32_t lo = sext(mrd(c, in, in->seg, ea, size), size);
        int32_t hi = sext(mrd(c, in, in->seg, (ea + size) & admask(in), size), size);
        if (idx < lo || idx > hi) RAISE(X86_EXC_BR);
        break;
    }

    /* ---- strings, I/O ---------------------------------------------- */
    case OP_MOVS: case OP_CMPS: case OP_STOS: case OP_LODS: case OP_SCAS: case OP_INS: case OP_OUTS:
        do_string(c, in);
        break;
    case OP_IN: {
        uint32_t port = rd_op(c, in, 1, ea) & 0xFFFF;
        a = c->io_read ? c->io_read(c, port, size) : szmask(size);
        wr_op(c, in, 0, ea, a);
        break;
    }
    case OP_OUT: {
        uint32_t port = rd_op(c, in, 0, ea) & 0xFFFF;
        int sz = in->ops[1].size;
        if (c->io_write) c->io_write(c, port, rd_op(c, in, 1, ea), sz);
        break;
    }

    /* ---- flags ------------------------------------------------------ */
    case OP_CLC: c->eflags &= ~X86_CF; break;
    case OP_STC: c->eflags |= X86_CF; break;
    case OP_CMC: c->eflags ^= X86_CF; break;
    case OP_CLD: c->eflags &= ~X86_DF; break;
    case OP_STD: c->eflags |= X86_DF; break;
    case OP_CLI: c->eflags &= ~X86_IF; break;
    case OP_STI: c->eflags |= X86_IF; c->int_inhibit = 1; break;

    /* ---- bit ops ---------------------------------------------------- */
    case OP_BT: case OP_BTS: case OP_BTR: case OP_BTC: {
        uint32_t bitpos, addr;
        a = bt_common(c, in, ea, &bitpos, &addr);
        uint32_t bit = (a >> bitpos) & 1;
        c->eflags = (c->eflags & ~X86_CF) | (bit ? X86_CF : 0);
        if (in->op == OP_BT) break;
        r = in->op == OP_BTS ? a | (1u << bitpos) : in->op == OP_BTR ? a & ~(1u << bitpos) : a ^ (1u << bitpos);
        if (in->ops[0].kind == OPK_MEM) mwr(c, in, in->seg, addr, size, r);
        else wr_op(c, in, 0, ea, r);
        break;
    }
    case OP_BSF: case OP_BSR:
        b = rd_op(c, in, 1, ea) & szmask(size);
        if (b == 0) { c->eflags |= X86_ZF; break; }      /* CONTRACT: dest unchanged */
        c->eflags &= ~X86_ZF;
        if (in->op == OP_BSF) { r = 0; while (!(b & (1u << r))) r++; }
        else { r = size * 8 - 1; while (!(b & (1u << r))) r--; }
        wr_op(c, in, 0, ea, r);
        break;
    case OP_SHLD: case OP_SHRD: {
        uint32_t cnt = (in->imm2 == 0xFFFFFFFFu ? x86_get_r8(c, R_CL) : in->imm2) & 0x1F;
        if (cnt == 0) break;
        int bits = size * 8;
        a = rd_op(c, in, 0, ea) & szmask(size); b = rd_op(c, in, 1, ea) & szmask(size);
        if (cnt > (uint32_t)bits) break;                 /* CONTRACT: undefined → no-op */
        uint32_t cf;
        if (in->op == OP_SHLD) {
            cf = (a >> (bits - cnt)) & 1;
            r = (cnt == (uint32_t)bits) ? b : ((a << cnt) | (b >> (bits - cnt))) & szmask(size);
        } else {
            cf = (a >> (cnt - 1)) & 1;
            r = (cnt == (uint32_t)bits) ? b : ((a >> cnt) | (b << (bits - cnt))) & szmask(size);
        }
        c->eflags &= ~X86_ARITH_FLAGS;
        if (cf) c->eflags |= X86_CF;
        if ((a ^ r) & signbit(size)) c->eflags |= X86_OF;
        set_szp(c, r, size);
        wr_op(c, in, 0, ea, r);
        break;
    }

    /* ---- misc ------------------------------------------------------- */
    case OP_NOP: case OP_WAIT: case OP_LOCK_ONLY:
        break;
    case OP_ESC:
        /* No coprocessor: the 8088 still performs the operand bus read */
        if (in->ops[0].kind == OPK_MEM) (void)mrd(c, in, in->seg, ea, 1);
        break;
    case OP_HLT:
        c->halted = 1;
        break;
    case OP_ARPL:
        if (!c->pmode) RAISE(X86_EXC_UD);
        a = rd_op(c, in, 0, ea); b = rd_op(c, in, 1, ea);
        if ((a & 3) < (b & 3)) { c->eflags |= X86_ZF; wr_op(c, in, 0, ea, (a & ~3u) | (b & 3)); }
        else c->eflags &= ~X86_ZF;
        break;

    /* System instructions: Phase B (DPMI host). */
    case OP_LAR: case OP_LSL: case OP_CLTS: case OP_SGDT: case OP_SIDT: case OP_LGDT: case OP_LIDT:
    case OP_SLDT: case OP_STR: case OP_LLDT: case OP_LTR: case OP_VERR: case OP_VERW:
    case OP_SMSW: case OP_LMSW: case OP_MOVCR: case OP_MOVDR: case OP_MOVTR:
    case OP_UD:
    default:
        (void)start_ip;
        RAISE(X86_EXC_UD);
    }
}

/* Gather up to 15 code bytes at CS:IP, honouring the 16-bit IP wrap in
 * real mode, into a bounce buffer. The JIT will decode in place when it
 * can; the oracle never needs to be clever. */
static void fetch_bytes(x86_cpu *c, uint8_t *buf) {
    uint32_t base = c->seg[S_CS].base, ip = c->eip;
    uint32_t m = c->seg[S_CS].big ? 0xFFFFFFFFu : 0xFFFF;
    for (int i = 0; i < 16; i++) buf[i] = x86_phys_rd8(c, base + ((ip + i) & m));
}

void x86_exec_decoded(x86_cpu *c, const x86_insn *in) {
    execute(c, in, c->eip - in->len);
}

int x86_step(x86_cpu *c) {
    if (c->halted) return 1;

    uint8_t buf[16];
    x86_insn in;
    fetch_bytes(c, buf);
    x86_dec_ctx ctx = { buf, c->model, c->seg[S_CS].big };
    uint32_t start_ip = c->eip;

    if (!x86_decode(&ctx, &in)) {
        /* > 15 bytes of prefixes: the 8086 just keeps going; 386 #GP.
         * Treat as #UD-ish for now. */
        c->exc = X86_EXC_UD;
    } else {
        set_ip(c, c->eip + in.len);
        c->int_inhibit = 0;
        execute(c, &in, start_ip);
    }
    c->insn_count++;

    if (c->exc >= 0) {
        int vec = c->exc;
        c->exc = -1;
        /* Faults on 286+ push the faulting IP. The 8086/186 divide
         * error (and everything else it can raise) pushes the next IP. */
        int trap_semantics = c->model < X86_MODEL_286;
        if (!trap_semantics) set_ip(c, start_ip);
        if (vec == X86_EXC_UD && c->model == X86_MODEL_8086) return -1;   /* cannot happen; be loud */
        x86_interrupt(c, vec, 0);
    }
    return 0;
}
