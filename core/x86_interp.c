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

void x86_fault(x86_cpu *c, int vector, uint32_t err) {
    c->exc = vector;
    c->exc_err = err;
    if (c->fault_armed) longjmp(c->fault_jb, 1);
}

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
 * the 8086/186 (silicon-verified by the 8088 suite). The 286/386 fault
 * instead (limit_check below: #GP, or #SS through SS on the 386), so
 * their accesses are linear. */
static inline uint32_t wrapmask(const x86_cpu *c, uint32_t m) { return c->model >= X86_MODEL_286 ? 0xFFFFFFFFu : m; }

static uint32_t calc_ea(x86_cpu *c, const x86_insn *in) {
    uint32_t ea = (uint32_t)in->disp;
    if (in->base >= 0) ea += c->r[in->base];
    if (in->index >= 0) ea += c->r[in->index] << in->scale;
    return ea & admask(in);
}

/* 286+: an access past the segment limit (FFFF in real mode) is #GP,
 * with nothing of the instruction committed. The 8086/186 wrap. */
static inline void limit_check(x86_cpu *c, int seg, uint32_t off, int size) {
    if (c->model >= X86_MODEL_286 && (off > c->seg[seg].limit || off + size - 1 > c->seg[seg].limit))
        x86_fault(c, (seg == S_SS && c->model >= X86_MODEL_386) ? X86_EXC_SS : X86_EXC_GP, 0);   /* 386: stack-segment faults are #SS (measured) */
}
static inline uint32_t mrd(x86_cpu *c, const x86_insn *in, int seg, uint32_t off, int size) {
    limit_check(c, seg, off, size);
    return x86_rd(c, c->seg[seg].base, off, wrapmask(c, admask(in)), size);
}
static inline void mwr(x86_cpu *c, const x86_insn *in, int seg, uint32_t off, int size, uint32_t v) {
    limit_check(c, seg, off, size);
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
    limit_check(c, S_SS, sp, size);                 /* before SP moves */
    x86_wr(c, c->seg[S_SS].base, sp, wrapmask(c, m), size, v);
    c->r[R_SP] = m == 0xFFFF ? ((c->r[R_SP] & 0xFFFF0000u) | sp) : sp;
}

static uint32_t pop(x86_cpu *c, int size) {
    uint32_t m = stkmask(c);
    uint32_t sp = c->r[R_SP] & m;
    limit_check(c, S_SS, sp, size);
    uint32_t v = x86_rd(c, c->seg[S_SS].base, sp, wrapmask(c, m), size);
    sp = (sp + size) & m;
    c->r[R_SP] = m == 0xFFFF ? ((c->r[R_SP] & 0xFFFF0000u) | sp) : sp;
    return v;
}

/* Control transfer: a 16-bit operand truncates the target to IP. The
 * 386 keeps EIP whole otherwise, even in a 16-bit code segment — the
 * next fetch is what faults (#GP) when it lies past the limit. The
 * 8086..286 have a 16-bit IP and simply wrap. */
/* Read a stack slot without moving SP (frame prechecks). */
static uint32_t peek(x86_cpu *c, uint32_t off, int size) {
    uint32_t m = stkmask(c);
    uint32_t sp = (c->r[R_SP] + off) & m;
    limit_check(c, S_SS, sp, size);
    return x86_rd(c, c->seg[S_SS].base, sp, wrapmask(c, m), size);
}
/* 386: a 32-bit transfer target past the code limit is #GP before
 * anything is committed. 16-bit targets cannot leave a real-mode segment. */
static inline void check_target(x86_cpu *c, uint32_t ip, int os) {
    if (os == 4 && c->model >= X86_MODEL_386 && ip > c->seg[S_CS].limit) x86_fault(c, X86_EXC_GP, 0);
}

static inline void set_ip(x86_cpu *c, uint32_t ip, int os) {
    c->eip = (os == 2 || c->model < X86_MODEL_386) ? (ip & 0xFFFF) : ip;
}

/* Segment register load. Real mode only for now; Phase B hooks PM here. */
static void load_seg(x86_cpu *c, int s, uint32_t sel) {
    x86_load_seg(c, s, (uint16_t)sel);
    if (s == S_SS) c->int_inhibit = 1;
}

/* Real-mode interrupt delivery: 16-bit pushes, vector from the IVT at 0.
 * The frame pushes are unchecked: a limit fault while delivering a
 * fault is a shutdown on real hardware (SP odd and below 6); we push
 * linearly so the interpreter and the JIT agree on something. */
static void push_raw(x86_cpu *c, uint32_t v) {
    uint32_t m = stkmask(c);
    uint32_t sp = (c->r[R_SP] - 2) & m;
    x86_wr(c, c->seg[S_SS].base, sp, wrapmask(c, m), 2, v);
    c->r[R_SP] = m == 0xFFFF ? ((c->r[R_SP] & 0xFFFF0000u) | sp) : sp;
}
/* Vectors that push an error code. Software interrupts never do. */
static int vec_has_err(int v) {
    return v == 8 || (v >= 10 && v <= 14) || v == 17;
}

/* Load CS for a control transfer. Unlike a data register, the descriptor
 * must be code, and conforming code is reachable from any less privileged
 * level without changing CPL — the RPL written back says which happened. */
static void load_cs_pm(x86_cpu *c, uint16_t sel, int cpl) {
    uint32_t lo, hi;
    if ((sel & 0xFFFC) == 0) x86_fault(c, X86_EXC_GP, 0);
    if (!x86_read_desc(c, sel, &lo, &hi)) x86_fault(c, X86_EXC_GP, sel & 0xFFFC);
    uint16_t attr = (uint16_t)(((hi >> 8) & 0xFF) | (((hi >> 20) & 0x0F) << 8));
    if (!X86_AR_S(attr) || !(X86_AR_TYPE(attr) & X86_TYPE_CODE))
        x86_fault(c, X86_EXC_GP, sel & 0xFFFC);
    int dpl = X86_AR_DPL(attr);
    if (X86_AR_TYPE(attr) & X86_TYPE_CONFORM) {
        if (dpl > cpl) x86_fault(c, X86_EXC_GP, sel & 0xFFFC);
    } else if (dpl != cpl) {
        x86_fault(c, X86_EXC_GP, sel & 0xFFFC);
    }
    if (!X86_AR_P(attr)) x86_fault(c, X86_EXC_NP, sel & 0xFFFC);
    x86_unpack_desc(&c->seg[S_CS], (uint16_t)((sel & 0xFFFC) | cpl), lo, hi);
    x86_set_accessed(c, sel, hi);
}

/* Push through an explicit stack, so a transfer that switches stacks can
 * build the new frame before committing SS:ESP. */
static void push_on(x86_cpu *c, const x86_seg *ss, uint32_t *sp, int size, uint32_t v) {
    *sp = (*sp - size) & (ss->big ? 0xFFFFFFFFu : 0xFFFFu);
    x86_wr(c, ss->base, *sp, 0xFFFFFFFFu, size, v);
}

/* Protected-mode interrupt and exception delivery through the IDT. */
static void deliver_pm(x86_cpu *c, int vector, int is_sw, uint32_t err) {
    uint32_t off = (uint32_t)vector * 8;
    if (off + 7 > c->idtr.limit) x86_fault(c, X86_EXC_GP, (off | 2));   /* IDT error codes set the IDT bit */
    uint32_t lo = x86_rd(c, c->idtr.base, off, 0xFFFFFFFFu, 4);
    uint32_t hi = x86_rd(c, c->idtr.base, off + 4, 0xFFFFFFFFu, 4);
    uint16_t gattr = (uint16_t)((hi >> 8) & 0xFF);
    int type = gattr & 0x1F;
    int gate32 = (type & 0x08) != 0;
    if (!(gattr & 0x80)) x86_fault(c, X86_EXC_NP, (off | 2));
    if ((type & 0x17) != 0x06) x86_fault(c, X86_EXC_GP, (off | 2));     /* not an interrupt/trap gate */
    /* A software INT may only use a gate at or below its own privilege. */
    int cpl = x86_cpl(c);
    if (is_sw && ((gattr >> 5) & 3) < cpl) x86_fault(c, X86_EXC_GP, (off | 2));

    uint16_t gsel = (uint16_t)(lo >> 16);
    uint32_t gip = (lo & 0xFFFF) | (gate32 ? (hi & 0xFFFF0000u) : 0);

    /* Work out the target privilege before touching anything. */
    uint32_t dlo, dhi;
    if (!x86_read_desc(c, gsel, &dlo, &dhi)) x86_fault(c, X86_EXC_GP, gsel & 0xFFFC);
    uint16_t dattr = (uint16_t)(((dhi >> 8) & 0xFF) | (((dhi >> 20) & 0x0F) << 8));
    int dpl = X86_AR_DPL(dattr);
    int conforming = (X86_AR_TYPE(dattr) & X86_TYPE_CONFORM) != 0;
    int newcpl = (conforming || dpl > cpl) ? cpl : dpl;

    uint32_t flags = c->eflags, oldeip = c->eip;
    uint16_t oldcs = c->seg[S_CS].sel, oldss = c->seg[S_SS].sel;
    uint32_t oldsp = c->r[R_SP];
    x86_seg stack = c->seg[S_SS];
    uint32_t sp = c->r[R_SP];

    if (newcpl < cpl) {
        /* Inward: the stack comes from the TSS, and the interrupted one is
         * recorded on it. */
        uint32_t nsp = x86_rd(c, c->tr.base, 4 + newcpl * 8, 0xFFFFFFFFu, 4);
        uint16_t nss = (uint16_t)x86_rd(c, c->tr.base, 8 + newcpl * 8, 0xFFFFFFFFu, 2);
        uint32_t slo, shi;
        if (!x86_read_desc(c, nss, &slo, &shi)) x86_fault(c, X86_EXC_TS, nss & 0xFFFC);
        x86_unpack_desc(&stack, nss, slo, shi);
        sp = nsp;
        push_on(c, &stack, &sp, gate32 ? 4 : 2, oldss);
        push_on(c, &stack, &sp, gate32 ? 4 : 2, oldsp);
    }
    push_on(c, &stack, &sp, gate32 ? 4 : 2, flags);
    push_on(c, &stack, &sp, gate32 ? 4 : 2, oldcs);
    push_on(c, &stack, &sp, gate32 ? 4 : 2, oldeip);
    if (vec_has_err(vector) && !is_sw) push_on(c, &stack, &sp, gate32 ? 4 : 2, err);

    load_cs_pm(c, gsel, newcpl);
    c->seg[S_SS] = stack;
    c->r[R_SP] = sp;
    c->eip = gip;
    c->eflags &= ~(X86_TF | X86_NT);
    if (!(type & 1)) c->eflags &= ~X86_IF;         /* interrupt gate, not trap gate */
    c->int_inhibit = 0;
}


/* A far JMP or CALL in protected mode. The selector may name a code
 * segment directly, or a call gate that names one; a gate is also the only
 * way a CALL can raise privilege, and it brings a new stack with it. */
static void far_transfer_pm(x86_cpu *c, uint16_t sel, uint32_t off, int is_call, int os) {
    int cpl = x86_cpl(c), rpl = sel & 3;
    uint32_t lo, hi;
    if ((sel & 0xFFFC) == 0) x86_fault(c, X86_EXC_GP, 0);
    if (!x86_read_desc(c, sel, &lo, &hi)) x86_fault(c, X86_EXC_GP, sel & 0xFFFC);
    uint16_t attr = (uint16_t)(((hi >> 8) & 0xFF) | (((hi >> 20) & 0x0F) << 8));
    int type = X86_AR_TYPE(attr);

    if (X86_AR_S(attr)) {
        /* Straight to a code segment: no privilege change either way. */
        if (!(type & X86_TYPE_CODE)) x86_fault(c, X86_EXC_GP, sel & 0xFFFC);
        if (type & X86_TYPE_CONFORM) {
            if (X86_AR_DPL(attr) > cpl) x86_fault(c, X86_EXC_GP, sel & 0xFFFC);
        } else {
            if (rpl > cpl || X86_AR_DPL(attr) != cpl) x86_fault(c, X86_EXC_GP, sel & 0xFFFC);
        }
        if (!X86_AR_P(attr)) x86_fault(c, X86_EXC_NP, sel & 0xFFFC);
        if (is_call) {
            push(c, os, c->seg[S_CS].sel);
            push(c, os, c->eip);
        }
        x86_unpack_desc(&c->seg[S_CS], (uint16_t)((sel & 0xFFFC) | cpl), lo, hi);
        x86_set_accessed(c, sel, hi);
        c->eip = os == 2 ? (off & 0xFFFF) : off;
        return;
    }

    /* A system descriptor: the only kind we follow is a call gate. */
    if (type != 0x0C && type != 0x04) x86_fault(c, X86_EXC_GP, sel & 0xFFFC);
    int gate32 = type == 0x0C;
    if (!X86_AR_P(attr)) x86_fault(c, X86_EXC_NP, sel & 0xFFFC);
    /* Software may only use a gate at or below its own privilege. */
    if (X86_AR_DPL(attr) < cpl || X86_AR_DPL(attr) < rpl)
        x86_fault(c, X86_EXC_GP, sel & 0xFFFC);

    uint16_t tsel = (uint16_t)(lo >> 16);
    uint32_t tip = (lo & 0xFFFF) | (gate32 ? (hi & 0xFFFF0000u) : 0);
    int nparams = hi & 0x1F;

    uint32_t tlo, thi;
    if ((tsel & 0xFFFC) == 0) x86_fault(c, X86_EXC_GP, 0);
    if (!x86_read_desc(c, tsel, &tlo, &thi)) x86_fault(c, X86_EXC_GP, tsel & 0xFFFC);
    uint16_t tattr = (uint16_t)(((thi >> 8) & 0xFF) | (((thi >> 20) & 0x0F) << 8));
    if (!X86_AR_S(tattr) || !(X86_AR_TYPE(tattr) & X86_TYPE_CODE))
        x86_fault(c, X86_EXC_GP, tsel & 0xFFFC);    /* the error names the target, not the gate */
    int tdpl = X86_AR_DPL(tattr);
    int conforming = (X86_AR_TYPE(tattr) & X86_TYPE_CONFORM) != 0;
    if (tdpl > cpl) x86_fault(c, X86_EXC_GP, tsel & 0xFFFC);
    if (!X86_AR_P(tattr)) x86_fault(c, X86_EXC_NP, tsel & 0xFFFC);

    int newcpl = (conforming || tdpl == cpl) ? cpl : tdpl;
    if (!is_call && newcpl != cpl) x86_fault(c, X86_EXC_GP, tsel & 0xFFFC);   /* JMP cannot change privilege */

    if (is_call && newcpl < cpl) {
        /* Inward call: a fresh stack out of the TSS, the old one recorded on
         * it, and any parameters copied across. */
        uint32_t nsp = x86_rd(c, c->tr.base, 4 + newcpl * 8, 0xFFFFFFFFu, 4);
        uint16_t nss = (uint16_t)x86_rd(c, c->tr.base, 8 + newcpl * 8, 0xFFFFFFFFu, 2);
        uint32_t slo, shi;
        if (!x86_read_desc(c, nss, &slo, &shi)) x86_fault(c, X86_EXC_TS, nss & 0xFFFC);
        x86_seg stack;
        x86_unpack_desc(&stack, nss, slo, shi);
        uint16_t oldss = c->seg[S_SS].sel;
        uint32_t oldsp = c->r[R_SP], sp = nsp;
        uint32_t params[32];
        for (int i = 0; i < nparams; i++)
            params[i] = x86_rd(c, c->seg[S_SS].base, oldsp + (uint32_t)i * os, 0xFFFFFFFFu, os);
        push_on(c, &stack, &sp, os, oldss);
        push_on(c, &stack, &sp, os, oldsp);
        for (int i = nparams - 1; i >= 0; i--) push_on(c, &stack, &sp, os, params[i]);
        push_on(c, &stack, &sp, os, c->seg[S_CS].sel);
        push_on(c, &stack, &sp, os, c->eip);
        x86_unpack_desc(&c->seg[S_CS], (uint16_t)((tsel & 0xFFFC) | newcpl), tlo, thi);
        x86_set_accessed(c, tsel, thi);
        c->seg[S_SS] = stack;
        c->r[R_SP] = sp;
        c->eip = gate32 ? tip : (tip & 0xFFFF);
        return;
    }
    if (is_call) {
        push(c, os, c->seg[S_CS].sel);
        push(c, os, c->eip);
    }
    x86_unpack_desc(&c->seg[S_CS], (uint16_t)((tsel & 0xFFFC) | newcpl), tlo, thi);
    x86_set_accessed(c, tsel, thi);
    c->eip = gate32 ? tip : (tip & 0xFFFF);
}

void x86_interrupt(x86_cpu *c, int vector, int is_sw) {
    if (c->pmode) { deliver_pm(c, vector, is_sw, c->exc_err); return; }
    (void)is_sw;
    /* The vector is read before the frame is pushed (measured on the
     * 386: a frame landing on the IVT entry does not redirect). */
    uint32_t off = x86_rd(c, 0, vector * 4, 0xFFFFFFFFu, 2);
    uint32_t sel = x86_rd(c, 0, vector * 4 + 2, 0xFFFFFFFFu, 2);
    push_raw(c, c->eflags & 0xFFFF);
    c->eflags &= ~(X86_IF | X86_TF);
    push_raw(c, c->seg[S_CS].sel);
    push_raw(c, c->eip & 0xFFFF);
    load_seg(c, S_CS, sel);
    c->int_inhibit = 0;
    set_ip(c, off, 2);
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
    case OP_SHL: case OP_SAL: {
        uint32_t v0 = v;
        for (uint32_t i = 0; i < cnt; i++) { cf = (v & sb) != 0; v = (v << 1) & m; }
        /* 386 (measured): a byte shifted by exactly 16 or 24 reports the
         * bit a byte-rotate would have dropped (bit 0 / bit 7); every
         * other over-width count shifts out zeros. */
        if (size == 1 && cnt > 8 && !(cnt & 7) && c->model >= X86_MODEL_386) cf = v0 & 1;
        of = ((v & sb) != 0) ^ cf;
        set_szp(c, v, size);
        c->eflags &= ~X86_AF;                       /* CONTRACT: AF cleared by shifts */
        break;
    }
    case OP_SHR: {
        uint32_t v0 = v;
        for (uint32_t i = 0; i < cnt; i++) { of = (v & sb) != 0; cf = v & 1; v >>= 1; }
        if (size == 1 && cnt > 8 && !(cnt & 7) && c->model >= X86_MODEL_386) cf = (v0 >> 7) & 1;
        set_szp(c, v, size);
        c->eflags &= ~X86_AF;
        break;
    }
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

/* CONTRACT (measured on the 286): the two-operand IMUL sets SF/ZF/PF
 * from the HIGH half of the product — the part it throws away — while
 * the one-operand form uses the low half. */
static uint32_t do_imul2(x86_cpu *c, uint32_t a, uint32_t b, int size) {
    int64_t r = (int64_t)sext(a, size) * sext(b, size);
    uint32_t lo = (uint32_t)r & szmask(size);
    c->eflags &= ~X86_ARITH_FLAGS;
    set_szp(c, c->model >= X86_MODEL_286 ? (uint32_t)(r >> (size * 8)) : lo, size);
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
/* 286+: each pointer is committed just before its own access, so a
 * faulting source read leaves SI advanced and DI untouched, a faulting
 * destination write leaves both advanced (measured). REP INS/OUTS
 * faulting additionally show CX two lower than at entry (measured;
 * the microcode's count handling on that path). */
static inline void set_si(x86_cpu *c, uint32_t am, uint32_t v) { c->r[R_SI] = am == 0xFFFF ? ((c->r[R_SI] & 0xFFFF0000u) | (v & 0xFFFF)) : v; }
static inline void set_di(x86_cpu *c, uint32_t am, uint32_t v) { c->r[R_DI] = am == 0xFFFF ? ((c->r[R_DI] & 0xFFFF0000u) | (v & 0xFFFF)) : v; }

static void do_string(x86_cpu *c, const x86_insn *in) {
    int size = in->ops[0].size;
    uint32_t am = admask(in);
    int32_t delta = (c->eflags & X86_DF) ? -size : size;
    int rep = in->rep;
    int test_z = (in->op == OP_CMPS || in->op == OP_SCAS);
    int early = c->model == X86_MODEL_286;   /* the 386 commits nothing on a faulting iteration (measured) */

    for (;;) {
        if (rep && (c->r[R_CX] & am) == 0) break;
        uint32_t si = c->r[R_SI] & am, di = c->r[R_DI] & am;
        uint32_t a, b;
        if (early && rep) {
            /* CX at a faulting iteration (measured): a source-side fault
             * costs one decrement, a destination-side fault two, and the
             * compare ops (CMPS/SCAS) none. */
            int use_si = in->op != OP_STOS && in->op != OP_SCAS && in->op != OP_INS;
            int use_di = in->op != OP_LODS && in->op != OP_OUTS;
            int src_f = use_si && (si > c->seg[in->seg].limit || si + size - 1 > c->seg[in->seg].limit);
            int dst_f = use_di && (di > c->seg[S_ES].limit || di + size - 1 > c->seg[S_ES].limit);
            uint32_t dec = 0;
            switch (in->op) {
            case OP_MOVS: dec = src_f ? 1 : dst_f ? 2 : 0; break;          /* source read first */
            case OP_CMPS: dec = dst_f ? 0 : src_f ? 1 : 0; break;          /* ES:[DI] read first */
            case OP_STOS: case OP_INS: dec = dst_f ? 2 : 0; break;
            case OP_SCAS: dec = dst_f ? 1 : 0; break;
            case OP_LODS: case OP_OUTS: dec = src_f ? 1 : 0; break;
            }
            if (dec) {
                uint32_t cx = (c->r[R_CX] - dec) & am;
                c->r[R_CX] = am == 0xFFFF ? ((c->r[R_CX] & 0xFFFF0000u) | cx) : cx;
            }
        }
        switch (in->op) {
        case OP_MOVS:
            if (early) set_si(c, am, si + delta);
            a = mrd(c, in, in->seg, si, size);
            if (early) set_di(c, am, di + delta);
            mwr(c, in, S_ES, di, size, a);
            break;
        case OP_CMPS:
            /* 286+ fetches ES:[DI] first (measured by which pointer moved) */
            if (early) { set_di(c, am, di + delta); b = mrd(c, in, S_ES, di, size); set_si(c, am, si + delta); a = mrd(c, in, in->seg, si, size); }
            else { a = mrd(c, in, in->seg, si, size); b = mrd(c, in, S_ES, di, size); }
            do_sub(c, a, b, 0, size);
            break;
        case OP_STOS:
            if (early) set_di(c, am, di + delta);
            mwr(c, in, S_ES, di, size, x86_get_reg(c, R_AX, size));
            break;
        case OP_LODS:
            if (early) set_si(c, am, si + delta);
            x86_set_reg(c, R_AX, size, mrd(c, in, in->seg, si, size));
            break;
        case OP_SCAS:
            if (early) set_di(c, am, di + delta);
            b = mrd(c, in, S_ES, di, size);
            do_sub(c, x86_get_reg(c, R_AX, size), b, 0, size);
            break;
        case OP_INS:
            if (early) set_di(c, am, di + delta);
            limit_check(c, S_ES, di, size);
            a = c->io_read ? c->io_read(c, x86_get_r16(c, R_DX), size) : szmask(size);
            mwr(c, in, S_ES, di, size, a);
            break;
        case OP_OUTS:
            if (early) set_si(c, am, si + delta);
            a = mrd(c, in, in->seg, si, size);
            if (c->io_write) c->io_write(c, x86_get_r16(c, R_DX), a, size);
            break;
        }
        if (!early) {
            if (in->op != OP_STOS && in->op != OP_SCAS && in->op != OP_INS) set_si(c, am, si + delta);
            if (in->op != OP_LODS && in->op != OP_OUTS) set_di(c, am, di + delta);
        }
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
        int nib_borrow = 0;
        if ((al & 0xF) > 9 || (c->eflags & X86_AF)) {
            nib_borrow = sub && al < 6;                 /* 286+: the borrow of AL - 6 sets CF */
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
        if (cf || (nib_borrow && c->model >= X86_MODEL_286)) c->eflags |= X86_CF;
        if (af) c->eflags |= X86_AF;
        set_szp(c, al, 1);
        break;
    }
    case OP_AAA: case OP_AAS: {
        uint32_t al = x86_get_r8(c, R_AL), ah = x86_get_r8(c, R_AH);
        int adjust = (al & 0xF) > 9 || (c->eflags & X86_AF);
        c->eflags &= ~X86_ARITH_FLAGS;                 /* CONTRACT: OF/SF/ZF/PF cleared */
        if (adjust) {
            if (c->model >= X86_MODEL_286) {
                /* 286+: AX ± 106h as a word, so AL's carry reaches AH (FFFF → 0105) */
                uint32_t ax = (ah << 8) | al;
                ax = in->op == OP_AAA ? ax + 0x106 : ax - 0x106;
                al = ax & 0xFF; ah = (ax >> 8) & 0xFF;
            } else {
                if (in->op == OP_AAA) { al += 6; ah += 1; } else { al -= 6; ah -= 1; }
            }
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
             * by the time it traps, and they are what INT 0 pushes. The
             * 286 traps with PF set and the rest clear (measured). */
            c->eflags &= ~X86_ARITH_FLAGS;
            if (c->model >= X86_MODEL_286) c->eflags |= X86_PF; else set_szp(c, 0, 1);
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
        } else if (in->ops[0].kind == OPK_REG && in->opsize == 4) {
            x86_set_reg(c, in->ops[0].reg, 4, c->seg[in->ops[1].reg].sel);   /* 386: o32 MOV r32,sreg zero-extends (measured); memory stays a word */
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
        if (in->ops[0].kind == OPK_SREG) {
            /* o32 POP sreg on the 386 reads only the word (no straddle
             * fault for the upper half) and still skips 4 (measured). */
            if (in->opsize == 4 && c->model >= X86_MODEL_386) {
                a = peek(c, 0, 2);
                uint32_t sm = stkmask(c), sp = (c->r[R_SP] + 4) & sm;
                c->r[R_SP] = sm == 0xFFFF ? ((c->r[R_SP] & 0xFFFF0000u) | sp) : sp;
            } else a = pop(c, in->opsize);
            load_seg(c, in->ops[0].reg, a);
        } else {
            uint32_t sp0 = c->r[R_SP];
            a = pop(c, in->opsize);
            /* POP [mem]: EA is computed after SP is incremented; the 386
             * leaves SP where it was if the destination faults (measured). */
            if (in->ops[0].kind == OPK_MEM) {
                ea = calc_ea(c, in);
                if (c->model >= X86_MODEL_386 && (ea > c->seg[in->seg].limit || ea + size - 1 > c->seg[in->seg].limit)) {
                    c->r[R_SP] = sp0;
                    x86_fault(c, in->seg == S_SS ? X86_EXC_SS : X86_EXC_GP, 0);
                }
            }
            wr_op(c, in, 0, ea, a);
        }
        break;
    case OP_PUSHA: {
        uint32_t sp = x86_get_reg(c, R_SP, in->opsize);
        uint32_t sm = stkmask(c), os = in->opsize, lim = c->seg[S_SS].limit;
        if (c->model == X86_MODEL_286) {
            /* 286 (measured): a straddling push faults up front, SP intact */
            for (uint32_t i = 1; i <= 8; i++)
                if (((c->r[R_SP] - os * i) & sm) + os - 1 > lim) x86_fault(c, X86_EXC_GP, 0);
        } else if (c->model >= X86_MODEL_386) {
            /* 386 (measured): every push that fits lands, the straddling
             * one is skipped, and #SS is raised at the end with SP intact */
            uint32_t sp0 = c->r[R_SP];
            int bad = 0;
            for (int i = 0; i < 8; i++) {
                uint32_t p = (c->r[R_SP] - os) & sm;
                if (p + os - 1 > lim) bad = 1;
                else x86_wr(c, c->seg[S_SS].base, p, sm, os, i == R_SP ? sp : x86_get_reg(c, i, os));
                c->r[R_SP] = sm == 0xFFFF ? ((c->r[R_SP] & 0xFFFF0000u) | p) : p;
            }
            if (bad) { c->r[R_SP] = sp0; x86_fault(c, X86_EXC_SS, 0); }
            break;
        }
        for (int i = 0; i < 8; i++) push(c, in->opsize, i == R_SP ? sp : x86_get_reg(c, i, in->opsize));
        break;
    }
    case OP_POPA: {
        uint32_t sm = stkmask(c), os = in->opsize, sp0 = c->r[R_SP];
        if (c->model == X86_MODEL_286) {
            /* 286 (measured): restartable — a straddling pop faults with nothing popped */
            for (uint32_t i = 0; i < 8; i++)
                if (((c->r[R_SP] + os * i) & sm) + os - 1 > c->seg[S_SS].limit) x86_fault(c, X86_EXC_GP, 0);
        }
        for (int i = 7; i >= 0; i--) {
            /* 386 (measured): registers popped before a straddling slot
             * keep their new values; only SP is restored for the #SS */
            if (c->model >= X86_MODEL_386 && (c->r[R_SP] & sm) + os - 1 > c->seg[S_SS].limit) {
                c->r[R_SP] = sp0; x86_fault(c, X86_EXC_SS, 0);
            }
            a = pop(c, in->opsize);
            if (i != R_SP) x86_set_reg(c, i, in->opsize, a);
            /* CONTRACT (386, measured): POPAD on a 16-bit stack leaves the
             * popped image's upper half in ESP; SP itself keeps counting. */
            else if (in->opsize == 4 && stkmask(c) == 0xFFFF && c->model >= X86_MODEL_386)
                c->r[R_SP] = (a & 0xFFFF0000u) | (c->r[R_SP] & 0xFFFF);
        }
        break;
    }
    case OP_PUSHF:
        push(c, in->opsize, in->opsize == 2 ? (c->eflags & 0xFFFF) : (c->eflags & 0x3FFFF & ~(X86_RF | X86_VM)));   /* 386: bits 18-31 push as 0 (measured) */
        break;
    case OP_POPF: {
        a = pop(c, in->opsize);
        uint32_t m = in->opsize == 2 ? 0xFFFF : 0xFFFFFFFFu;
        c->eflags = x86_flags_fixup(c, (c->eflags & ~m) | (a & m));
        break;
    }
    case OP_ENTER: {
        /* One pass, as the microcode does it: each push is limit-checked
         * (286+) right before it lands, so a wrapping frame overwrites
         * the very slots the later levels read (measured). On a fault
         * SP and BP are restored and the pushes already made stay. */
        uint32_t fsize = in->ops[0].imm, level = in->ops[1].imm & 31;
        int os = in->opsize;
        uint32_t sm = stkmask(c), lim = c->seg[S_SS].limit, base = c->seg[S_SS].base;
        uint32_t sp0 = c->r[R_SP], bp0 = c->r[R_BP];
        int chk = c->model >= X86_MODEL_286;
        int vec = c->model >= X86_MODEL_386 ? X86_EXC_SS : X86_EXC_GP;
#define ENTER_PUSH(v) do { \
            uint32_t p_ = (c->r[R_SP] - os) & sm; \
            if (chk && p_ + os - 1 > lim) { c->r[R_SP] = sp0; c->r[R_BP] = bp0; x86_fault(c, vec, 0); } \
            x86_wr(c, base, p_, wrapmask(c, sm), os, (v)); \
            c->r[R_SP] = sm == 0xFFFF ? ((c->r[R_SP] & 0xFFFF0000u) | p_) : p_; \
        } while (0)
        ENTER_PUSH(x86_get_reg(c, R_BP, os));
        uint32_t frame = c->r[R_SP] & sm;
        if (level > 0) {
            for (uint32_t i = 1; i < level; i++) {
                uint32_t bp = (x86_get_reg(c, R_BP, os) - os) & sm;
                x86_set_reg(c, R_BP, os, bp);
                if (chk && bp + os - 1 > lim) { c->r[R_SP] = sp0; c->r[R_BP] = bp0; x86_fault(c, vec, 0); }
                ENTER_PUSH(x86_rd(c, base, bp, wrapmask(c, sm), os));
            }
            ENTER_PUSH(frame);
        }
#undef ENTER_PUSH
        x86_set_reg(c, R_BP, os, frame);
        uint32_t nsp = (c->r[R_SP] - fsize) & sm;
        c->r[R_SP] = sm == 0xFFFF ? ((c->r[R_SP] & 0xFFFF0000u) | nsp) : nsp;
        break;
    }
    case OP_LEAVE: {
        int os = in->opsize, ss = c->seg[S_SS].big ? 4 : 2;   /* SP <- BP copies at the stack address size (measured) */
        limit_check(c, S_SS, x86_get_reg(c, R_BP, ss) & stkmask(c), os);   /* fault with SP intact */
        x86_set_reg(c, R_SP, ss, x86_get_reg(c, R_BP, ss));
        x86_set_reg(c, R_BP, os, pop(c, os));
        break;
    }

    /* ---- control flow ---------------------------------------------- */
    case OP_JMP:
        a = rd_op(c, in, 0, ea);
        if (in->ops[0].kind == OPK_IMM) a += c->eip;
        check_target(c, a, in->opsize);
        set_ip(c, a, in->opsize);
        break;
    case OP_JCC:
        if (x86_cond(c->eflags, in->cond)) set_ip(c, c->eip + in->ops[0].imm, in->opsize);
        break;
    case OP_JCXZ:
        if ((c->r[R_CX] & admask(in)) == 0) set_ip(c, c->eip + in->ops[0].imm, in->opsize);
        break;
    case OP_LOOP: case OP_LOOPE: case OP_LOOPNE: {
        uint32_t am = admask(in);
        uint32_t cx = (c->r[R_CX] - 1) & am;
        c->r[R_CX] = am == 0xFFFF ? ((c->r[R_CX] & 0xFFFF0000u) | cx) : cx;
        int zf = (c->eflags & X86_ZF) != 0;
        int go = cx != 0 && (in->op == OP_LOOP || (in->op == OP_LOOPE ? zf : !zf));
        if (go) set_ip(c, c->eip + in->ops[0].imm, in->opsize);
        break;
    }
    case OP_CALL:
        a = rd_op(c, in, 0, ea);
        if (in->ops[0].kind == OPK_IMM) a += c->eip;
        check_target(c, a, in->opsize);
        push(c, in->opsize, c->eip);
        set_ip(c, a, in->opsize);
        break;
    case OP_CALLF: case OP_JMPF: {
        uint32_t off, sel;
        if (in->ops[0].kind == OPK_IMM) { off = in->ops[0].imm; sel = in->imm2; }
        else {
            off = mrd(c, in, in->seg, ea, in->opsize);
            sel = mrd(c, in, in->seg, (ea + in->opsize) & admask(in), 2);
        }
        if (c->pmode) { far_transfer_pm(c, (uint16_t)sel, off, in->op == OP_CALLF, in->opsize); break; }
        check_target(c, off, in->opsize);        /* real mode: the new CS has the same 64K limit */
        if (in->op == OP_CALLF) {
            push(c, in->opsize, c->seg[S_CS].sel);
            push(c, in->opsize, c->eip);
        }
        load_seg(c, S_CS, sel);
        set_ip(c, off, in->opsize);
        break;
    }
    case OP_RET:
        check_target(c, peek(c, 0, in->opsize), in->opsize);
        a = pop(c, in->opsize);
        if (in->ops[0].kind == OPK_IMM) x86_set_reg(c, R_SP, stkmask(c) == 0xFFFF ? 2 : 4, c->r[R_SP] + in->ops[0].imm);
        set_ip(c, a, in->opsize);
        break;
    case OP_RETF: {
        check_target(c, peek(c, 0, in->opsize), in->opsize);
        peek(c, in->opsize, in->opsize);                 /* whole frame checked before SP moves */
        a = pop(c, in->opsize);
        b = pop(c, in->opsize);
        if (in->ops[0].kind == OPK_IMM) x86_set_reg(c, R_SP, stkmask(c) == 0xFFFF ? 2 : 4, c->r[R_SP] + in->ops[0].imm);
        load_seg(c, S_CS, b);
        set_ip(c, a, in->opsize);
        break;
    }
    case OP_IRET:
        if (c->pmode) {
            /* Pops EIP, CS, EFLAGS — and, only when the return is to a less
             * privileged level, ESP and SS as well. CS.RPL is what decides,
             * so a frame whose SS does not agree with it is rejected. */
            int cpl = x86_cpl(c);
            int os = in->opsize;
            uint32_t nip = peek(c, 0, os);
            uint16_t ncs = (uint16_t)peek(c, os, os);
            uint32_t nfl = peek(c, 2 * os, os);
            int rpl = ncs & 3;
            if (rpl < cpl) x86_fault(c, X86_EXC_GP, ncs & 0xFFFC);   /* never inward */
            if (rpl > cpl) {
                uint32_t nsp = peek(c, 3 * os, os);
                uint16_t nss = (uint16_t)peek(c, 4 * os, os);
                if ((nss & 3) != rpl) x86_fault(c, X86_EXC_GP, nss & 0xFFFC);
                uint32_t slo, shi;
                if ((nss & 0xFFFC) == 0) x86_fault(c, X86_EXC_GP, 0);
                if (!x86_read_desc(c, nss, &slo, &shi)) x86_fault(c, X86_EXC_GP, nss & 0xFFFC);
                uint16_t sattr = (uint16_t)(((shi >> 8) & 0xFF) | (((shi >> 20) & 0x0F) << 8));
                if (!X86_AR_S(sattr) || (X86_AR_TYPE(sattr) & X86_TYPE_CODE)
                    || !(X86_AR_TYPE(sattr) & X86_TYPE_WRITABLE)
                    || X86_AR_DPL(sattr) != rpl)
                    x86_fault(c, X86_EXC_GP, nss & 0xFFFC);
                if (!X86_AR_P(sattr)) x86_fault(c, X86_EXC_SS, nss & 0xFFFC);
                load_cs_pm(c, ncs, rpl);
                x86_unpack_desc(&c->seg[S_SS], nss, slo, shi);
                c->r[R_SP] = nsp;
                /* Any data segment the new level cannot reach becomes null. */
                for (int k = 0; k < 6; k++) {
                    if (k == S_CS || k == S_SS) continue;
                    x86_seg *g = &c->seg[k];
                    if (!g->usable) continue;
                    int t = X86_AR_TYPE(g->attr);
                    int conf = (t & X86_TYPE_CODE) && (t & X86_TYPE_CONFORM);
                    if (!conf && X86_AR_DPL(g->attr) < rpl) {
                        g->sel = 0; g->base = 0; g->limit = 0; g->attr = 0; g->usable = 0;
                    }
                }
            } else {
                load_cs_pm(c, ncs, cpl);
                uint32_t sm = stkmask(c);
                uint32_t nsp2 = (c->r[R_SP] + 3u * os) & sm;
                c->r[R_SP] = sm == 0xFFFF ? ((c->r[R_SP] & 0xFFFF0000u) | nsp2) : nsp2;
            }
            c->eip = os == 2 ? (nip & 0xFFFF) : nip;
            uint32_t fm = os == 2 ? 0xFFFFu : 0xFFFFFFFFu;
            c->eflags = x86_flags_fixup(c, (c->eflags & ~fm) | (nfl & fm));
            break;
        }
        {
        check_target(c, peek(c, 0, in->opsize), in->opsize);
        peek(c, in->opsize, in->opsize);
        peek(c, 2 * in->opsize, in->opsize);
        a = pop(c, in->opsize);
        b = pop(c, in->opsize);
        uint32_t f = pop(c, in->opsize);
        uint32_t m = in->opsize == 2 ? 0xFFFF : 0xFFFFFFFFu;
        load_seg(c, S_CS, b);
        set_ip(c, a, in->opsize);
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
        int bits = size * 8;
        a = rd_op(c, in, 0, ea) & szmask(size); b = rd_op(c, in, 1, ea) & szmask(size);
        if (cnt == 0) break;                             /* the operand fetch (and its fault) happens regardless */
        uint32_t cf;
        if (cnt > (uint32_t)bits) {
            /* 16-bit operand, count 17-31 (measured on the 386): the
             * result is a 32-bit rotate of the SOURCE replicated in both
             * halves; the destination's bits do not survive at all. */
            uint32_t t = (b << 16) | b;
            if (in->op == OP_SHLD) { t = (t << cnt) | (t >> (32 - cnt)); cf = t & 1; }
            else { t = (t >> cnt) | (t << (32 - cnt)); cf = t >> 31; }
            r = t & 0xFFFF;
        } else if (in->op == OP_SHLD) {
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
        /* No coprocessor: the 8088 still performs the operand bus read;
         * the 286 limit-checks a word (measured). */
        if (in->ops[0].kind == OPK_MEM) (void)mrd(c, in, in->seg, ea, c->model >= X86_MODEL_286 ? 2 : 1);
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
    case OP_CLTS:
        break;                                  /* CR0.TS: no CR0 yet (Phase B); legal at CPL 0 */
    /* ---- descriptor tables and CR0 -------------------------------- */
    case OP_LGDT: case OP_LIDT: {
        /* m16&32: a limit then a base. A 16-bit operand keeps only 24 bits
         * of base — the 286 form, still reachable on a 386. */
        uint32_t lim = mrd(c, in, in->seg, ea, 2);
        uint32_t b = mrd(c, in, in->seg, (ea + 2) & admask(in), 4);
        if (in->opsize == 2) b &= 0x00FFFFFFu;
        if (in->op == OP_LGDT) { c->gdtr.limit = (uint16_t)lim; c->gdtr.base = b; }
        else                   { c->idtr.limit = (uint16_t)lim; c->idtr.base = b; }
        break;
    }
    case OP_SGDT: case OP_SIDT: {
        int is_g = in->op == OP_SGDT;
        mwr(c, in, in->seg, ea, 2, is_g ? c->gdtr.limit : c->idtr.limit);
        mwr(c, in, in->seg, (ea + 2) & admask(in), 4, is_g ? c->gdtr.base : c->idtr.base);
        break;
    }
    case OP_LLDT: case OP_LTR: {
        if (!c->pmode) RAISE(X86_EXC_UD);
        uint16_t lsel = (uint16_t)rd_op(c, in, 0, ea);
        x86_seg *g = in->op == OP_LLDT ? &c->ldtr : &c->tr;
        if ((lsel & 0xFFFC) == 0) {
            if (in->op == OP_LTR) RAISE(X86_EXC_GP);       /* the task register cannot be null */
            memset(g, 0, sizeof *g);
            break;
        }
        if (lsel & 4) x86_fault(c, X86_EXC_GP, lsel & 0xFFFC);   /* both live in the GDT only */
        uint32_t dlo, dhi;
        if (!x86_read_desc(c, lsel, &dlo, &dhi)) x86_fault(c, X86_EXC_GP, lsel & 0xFFFC);
        uint16_t dattr = (uint16_t)(((dhi >> 8) & 0xFF) | (((dhi >> 20) & 0x0F) << 8));
        int dtype = X86_AR_TYPE(dattr);
        int ok_type = in->op == OP_LLDT ? (dtype == 0x2)            /* LDT */
                                        : (dtype == 0x1 || dtype == 0x9);   /* available TSS */
        if (X86_AR_S(dattr) || !ok_type) x86_fault(c, X86_EXC_GP, lsel & 0xFFFC);
        if (!X86_AR_P(dattr)) x86_fault(c, X86_EXC_NP, lsel & 0xFFFC);
        x86_unpack_desc(g, lsel, dlo, dhi);
        if (in->op == OP_LTR)                              /* mark the TSS busy */
            x86_wr(c, c->gdtr.base, (lsel & 0xFFF8) + 4, 0xFFFFFFFFu, 4, dhi | 0x0200u);
        break;
    }
    case OP_SLDT: case OP_STR:
        wr_op(c, in, 0, ea, in->op == OP_SLDT ? c->ldtr.sel : c->tr.sel);
        break;
    case OP_SMSW:
        wr_op(c, in, 0, ea, c->cr0 & 0xFFFF);
        break;
    case OP_LMSW:
        c->cr0 = (c->cr0 & ~0xEu) | (rd_op(c, in, 0, ea) & 0xFu) | (c->cr0 & 1u);
        c->pmode = (c->cr0 & 1) != 0;              /* LMSW can set PE but never clear it */
        break;
    case OP_MOVCR: {
        int cr = in->ops[0].kind == OPK_CR ? in->ops[0].reg : in->ops[1].reg;
        if (in->ops[0].kind == OPK_CR) {
            if (cr == 0) { c->cr0 = rd_op(c, in, 1, ea); c->pmode = (c->cr0 & 1) != 0; }
        } else {
            wr_op(c, in, 0, ea, cr == 0 ? c->cr0 : 0);
        }
        break;
    }

    /* System instructions still to come. */
    case OP_LAR: case OP_LSL: case OP_VERR: case OP_VERW:
    case OP_MOVDR: case OP_MOVTR:
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
    uint32_t start_ip = c->eip - in->len;
    c->fault_armed = 1;
    if (setjmp(c->fault_jb) == 0) execute(c, in, start_ip);
    c->fault_armed = 0;
    /* A fault (286+) or a trap-style exception restarts at the instruction
     * or continues after it, as x86_step would; the caller delivers it. */
    if (c->exc >= 0 && c->model >= X86_MODEL_286) c->eip = start_ip;
}

/* Deliver an exception left pending by translated code. */
void x86_deliver_exception(x86_cpu *c) {
    int vec = c->exc;
    c->exc = -1;
    x86_interrupt(c, vec, 0);
}

int x86_step(x86_cpu *c) {
    if (c->halted) return 1;
    if (c->hle && c->seg[S_CS].sel == c->hle_seg) {
        c->hle(c, (int)(c->eip & 0xFF));
        return c->halted ? 1 : 0;
    }

    uint8_t buf[16];
    x86_insn in;
    uint32_t start_ip = c->eip;
    int is386 = c->model >= X86_MODEL_386;

    if (is386 && start_ip > c->seg[S_CS].limit) {
        /* 386: EIP past the code limit (e.g. after HLT at FFFF) faults on
         * the fetch; the 286 wrapped IP instead. */
        c->exc = X86_EXC_GP;
    } else {
        fetch_bytes(c, buf);
        x86_dec_ctx ctx = { buf, c->model, c->seg[S_CS].big };
        if (!x86_decode(&ctx, &in)) {
            /* > 15 bytes of prefixes: the 8086 just keeps going; 386 #GP.
             * Treat as #UD-ish for now. */
            c->exc = X86_EXC_UD;
        } else if (is386 && start_ip + in.len - 1 > c->seg[S_CS].limit) {
            c->exc = X86_EXC_GP;                      /* instruction straddles the code limit */
        } else {
            c->eip = is386 ? start_ip + in.len : ((start_ip + in.len) & 0xFFFF);
            c->int_inhibit = 0;
            c->fault_armed = 1;
            if (setjmp(c->fault_jb) == 0) {
                /* 286: instructions longer than 10 bytes (prefix padding) are #GP */
                if (c->model == X86_MODEL_286 && in.len > 10) x86_fault(c, X86_EXC_GP, 0);
                execute(c, &in, start_ip);
            }
            c->fault_armed = 0;
        }
    }
    c->insn_count++;

    if (c->exc >= 0) {
        int vec = c->exc;
        c->exc = -1;
        /* Faults on 286+ push the faulting IP. The 8086/186 divide
         * error (and everything else it can raise) pushes the next IP. */
        int trap_semantics = c->model < X86_MODEL_286;
        if (c->trace_exc) c->trace_exc(c, vec, c->exc_err);
        if (!trap_semantics) c->eip = start_ip;
        if (vec == X86_EXC_UD && c->model == X86_MODEL_8086) return -1;   /* cannot happen; be loud */
        x86_interrupt(c, vec, 0);
    }
    return 0;
}
