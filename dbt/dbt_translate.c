/* dbt_translate.c — the translator's front end, shared by every backend.
 *
 * A block is planned here and emitted by the backend (dbt_arch_emit_block
 * in dbt_a64.c, dbt_x64.c). The plan (dbt_block, dbt.h) holds what the
 * block is — its key's mode bits, the shape the cpu gives it (real mode,
 * V86, flat, segmented 16-bit, paged) — and what was decided about each
 * instruction: its class (inline, helper, or the refusal that ends the
 * block), its role (plain, a conditional with a side exit, an
 * unconditional ender, a helper that ends the block), the flags live
 * after it, and whether its immediate is read from memory at run time
 * because the guest keeps patching it. None of that depends on the host:
 * the classes name x86 semantics — what must stay with the interpreter
 * (a CS load, an IOPL-sensitive instruction in V86 mode, a limit fault)
 * and what a backend may inline — and the backward flag-liveness pass is
 * the same for any emitter. What differs between backends is what they
 * make of the plan.
 *
 * Two kinds of plan: real-mode-shaped blocks, flat blocks and segmented
 * 16-bit blocks go through plan_block (classes, roles, liveness); other
 * protected-mode code — 16-bit PM with the shapes the emitters do not
 * cover, and any PM code with A20 off — goes through plan_pm as a run of
 * helpers (all_helper), what the old translate_pm did.
 *
 * Rules paid for in blood (see CLAUDE.md): fmask is the LIVE-OUT mask,
 * not live∩write; block exits mark all flags live so -V stays exact.
 */
#include <unistd.h>
#include "dbt.h"
#include <stdlib.h>
#include <string.h>

#define ARITH  X86_ARITH_FLAGS

/* cpu model of the block being classified (real-mode DIV is inline from
 * the 286 on). A static rather than a plan field so the exported
 * dbt_classify_op* (tools/jittest's fuzzer) see the last model planned. */
static int s_cls_model = X86_MODEL_286;

/* ----------------------------------------------------------------------
 * Classification
 * ---------------------------------------------------------------------- */
/* What the interpreter demands of an instruction, before any backend
 * is asked: C_REFUSE ends the block (the run loop steps it: a CS load in
 * disguise, an undecodable form), C_HELPER keeps it in the block but
 * exact by construction (an interrupt shadow, a model's microcode
 * quirk), and C_INLINE is permission — granted only if the backend says
 * it can (dbt_arch_can_inline, for the block's shape). Every op the
 * interpreter has and a backend might emit is listed as C_INLINE. */
static int classify_sem(const x86_insn *in) {
    switch (in->op) {
    case OP_ADD: case OP_OR: case OP_ADC: case OP_SBB: case OP_AND: case OP_SUB: case OP_XOR: case OP_CMP:
    case OP_TEST: case OP_INC: case OP_DEC: case OP_NOT: case OP_NEG:
    case OP_MOV: case OP_XCHG: case OP_LEA: case OP_NOP: case OP_CBW: case OP_CWD:
    case OP_CLC: case OP_STC: case OP_CMC: case OP_CLD: case OP_STD: case OP_CLI: case OP_STI:
    case OP_CALL: case OP_JMP: case OP_JCC: case OP_JCXZ: case OP_LOOP: case OP_LOOPE: case OP_LOOPNE:
    case OP_RET: case OP_CALLF: case OP_JMPF: case OP_RETF:
    case OP_INT: case OP_INT3:
    case OP_OUT:
    case OP_MOVZX: case OP_MOVSX: case OP_SETCC:
    case OP_PUSH: case OP_LES: case OP_LDS:
    case OP_MOVS: case OP_STOS: case OP_LODS: case OP_CMPS: case OP_SCAS:
    case OP_SHL: case OP_SAL: case OP_SHR: case OP_SAR:
    case OP_ROL: case OP_ROR: case OP_RCL: case OP_RCR: case OP_SETMO:
    case OP_MUL: case OP_IMUL: case OP_IMUL3:
    case OP_PUSHF: case OP_POPF:
    case OP_AAD: case OP_AAA: case OP_AAS: case OP_DAA: case OP_DAS: case OP_SALC:
    case OP_XLAT: case OP_SAHF: case OP_LAHF:
    case OP_PUSHA: case OP_POPA: case OP_ENTER: case OP_LEAVE:
    case OP_BT: case OP_BTS: case OP_BTR: case OP_BTC: case OP_BSF: case OP_BSR:
    case OP_SHLD: case OP_SHRD: case OP_CMPXCHG: case OP_XADD: case OP_BSWAP:
        return C_INLINE;
    case OP_POP:
        if (in->ops[0].kind != OPK_SREG) return C_INLINE;
        if (in->ops[0].reg == S_CS) return C_REFUSE;           /* POP CS: 8086 control transfer */
        return in->ops[0].reg == S_DS || in->ops[0].reg == S_ES ? C_INLINE : C_HELPER;   /* SS: interrupt shadow */
    case OP_DIV: case OP_IDIV:
        return s_cls_model >= X86_MODEL_286 ? C_INLINE : C_HELPER;  /* 8086 microcode quirks; 186 #DE is a trap */
    case OP_AAM:
        return in->ops[0].imm ? C_HELPER : C_REFUSE;
    case OP_MOVSEG:
        /* Loading CS (8086 only) is a control transfer in disguise; SS
         * opens an interrupt shadow, FS/GS are the 386's own. */
        if (in->ops[0].kind != OPK_SREG) return C_INLINE;
        if (in->ops[0].reg == S_CS) return C_REFUSE;
        return in->ops[0].reg == S_DS || in->ops[0].reg == S_ES ? C_INLINE : C_HELPER;
    default:
        return C_REFUSE;
    }
}

/* Real mode (and V86, below): 16-bit forms only — the 386's 32-bit
 * operand and address sizes in real mode are the interpreter's. */
static int classify(const dbt_block *b, const x86_insn *in) {
    if (in->opsize != 2 || in->adsize != 2) return C_REFUSE;   /* 386 forms: Phase B */
    int c = classify_sem(in);
    if (c == C_INLINE && !dbt_arch_can_inline(b, in)) c = C_HELPER;
    return c;
}

/* Virtual-8086 mode on top of the real-mode classes. What V86 does
 * differently is the interpreter's: INT n (through the IDT, IOPL-
 * sensitive), IRET and POPF (IOPL-sensitive, and they keep IOPL and VM),
 * HLT (#GP); I/O is a helper (the interpreter asks the TSS bitmap). CLI, STI and PUSHF are real mode's at
 * IOPL 3 only. Under paging an inline instruction gets one checked access
 * (emit_paged), so those that touch memory twice — a string op, a far
 * CALL or RET, PUSH/POP/CALL through memory — become helpers. */
static int classify_v86(const dbt_block *b, const x86_insn *in, int c) {
    switch (in->op) {
    case OP_INT: case OP_INT3: case OP_INTO: case OP_INT1: case OP_IRET: case OP_POPF: case OP_HLT:
        return C_REFUSE;
    case OP_IN: case OP_OUT: case OP_INS: case OP_OUTS:
        return C_HELPER;           /* the interpreter asks the I/O bitmap and faults to the monitor */
    case OP_CLI: case OP_STI: case OP_PUSHF:
        if (!b->iopl3) return C_REFUSE;
        break;
    default: break;
    }
    if (!b->paged || c != C_INLINE) return c;
    switch (in->op) {
    case OP_MOVS: case OP_STOS: case OP_LODS: case OP_CMPS: case OP_SCAS:
    case OP_CALLF: case OP_RETF:
        return C_HELPER;
    case OP_JMPF:
        return in->ops[0].kind == OPK_IMM ? C_INLINE : C_HELPER;
    case OP_PUSH: case OP_POP: case OP_CALL:
        return in->ops[0].kind == OPK_MEM ? C_HELPER : C_INLINE;
    default:
        return c;
    }
}

/* Protected mode, for now: everything the real-mode backend handles
 * becomes a helper call — the interpreter's own execute() on the
 * pre-decoded instruction, so exact by construction, faults included —
 * except what moves CS or goes through a gate, which the interpreter
 * steps. Near control transfers end the block (see translate). */
static int classify_pm(const x86_insn *in) {
    switch (in->op) {
    case OP_INT: case OP_INT3: case OP_CALLF: case OP_JMPF: case OP_RETF:
        return C_REFUSE;
    case OP_DIV: case OP_IDIV:
        return C_HELPER;          /* #DE is a fault: the thunk's fault exit delivers it */
    case OP_OUT:
        return C_INLINE;          /* the port thunk (emit_out), in every kind of block */
    default:
        return classify_sem(in) == C_REFUSE ? C_REFUSE : C_HELPER;
    }
}


/* Flat protected mode: what a backend inlines here is 32-bit code
 * addressing through DS/ES/SS; everything else is a helper, as in any
 * protected-mode block. */
static int classify_flat(const dbt_block *b, const x86_insn *in) {
    if (classify_pm(in) == C_REFUSE) return C_REFUSE;
    if (classify_sem(in) == C_HELPER) return C_HELPER;
    return dbt_arch_can_inline(b, in) ? C_INLINE : C_HELPER;
}

/* Segmented 16-bit protected mode: the real-mode shape with limit checks
 * at run time. What touches IOPL (CLI/STI, POPF), a segment register, a
 * far target or an interrupt frame stays with the interpreter — a
 * helper, ending the block when it loads a segment (dbt_loads_segment),
 * or refused. OUT goes through the port thunk in every kind of block. */
static int classify_seg16(const dbt_block *b, const x86_insn *in) {
    if (classify_pm(in) == C_REFUSE) return C_REFUSE;
    if (dbt_loads_segment(in)) return C_HELPER;                  /* descriptor loads: the interpreter's */
    if (in->op == OP_POPF) return C_HELPER;                      /* IOPL and IF under privilege rules */
    if (in->op == OP_CLI || in->op == OP_STI) return C_HELPER;   /* #GP above IOPL */
    if (in->op == OP_OUT) return C_INLINE;
    if (classify_sem(in) == C_HELPER) return C_HELPER;
    return dbt_arch_can_inline(b, in) ? C_INLINE : C_HELPER;
}

/* Exposed for tools/jittest's fuzzer: 0 refuse, 1 inline, 2 helper,
 * for a block of the plain shape of each kind. */
static dbt_block *shape_block(int flat, int seg16) {
    static dbt_block b;
    memset(&b, 0, offsetof(dbt_block, decs));
    b.model = s_cls_model;
    b.regs32 = s_cls_model >= X86_MODEL_386;
    b.wrap_exact = s_cls_model < X86_MODEL_286;
    b.flat = (uint8_t)flat; b.seg16 = (uint8_t)seg16;
    return &b;
}
int dbt_classify_op(const x86_insn *in) { return classify(shape_block(0, 0), in); }
int dbt_classify_op_pm(const x86_insn *in) { return classify_pm(in); }
int dbt_classify_op_seg16(const x86_insn *in) { return classify_seg16(shape_block(0, 1), in); }

/* Inline ops with a word-sized memory or stack access: the 286+ limit
 * check in front of it can raise #GP. Byte accesses cannot straddle. */
static int op_may_fault(const dbt_block *b, const x86_insn *in) {
    switch (in->op) {
    case OP_PUSH: case OP_POP: case OP_CALL: case OP_RET: case OP_CALLF: case OP_RETF: case OP_JMPF:
        return 1;
    case OP_INT: case OP_INT3:
        return 1;                     /* the frame carries FLAGS: all of them must be materialized */
    case OP_MOVS: case OP_STOS: case OP_LODS: case OP_CMPS: case OP_SCAS:
        return in->ops[0].size >= 2 || b->seg16;    /* the slow path's helper can fault, frame and all */
    case OP_XLAT:
        return b->seg16;                            /* an implicit byte read: only a limit can fault it */
    default: break;
    }
    for (int i = 0; i < 2; i++)
        if (in->ops[i].kind == OPK_MEM && (in->ops[i].size >= 2 || b->seg16)) return 1;   /* seg16: a limit is any size's problem */
    return 0;
}

/* Inline ops that store to guest memory. The store's SMC check can find
 * that it patched the running block and leave it right there — an
 * unplanned exit after the op, whose flags the next block (or the run
 * loop) sees in full. */
static int op_stores(const x86_insn *in) {
    switch (in->op) {
    case OP_PUSH: case OP_PUSHA: case OP_CALL: case OP_CALLF: case OP_INT: case OP_INT3: case OP_OUT: return 1;
    case OP_STOS: case OP_MOVS: return 1;
    case OP_LODS: case OP_CMPS: case OP_SCAS: return 0;
    case OP_CMP: case OP_TEST: case OP_LEA: return 0;
    case OP_XCHG: return in->ea_valid;
    default: return in->ops[0].kind == OPK_MEM;
    }
}

static int is_uncond_ender(int op) {
    return op == OP_JMP || op == OP_CALL || op == OP_RET || op == OP_JMPF || op == OP_CALLF || op == OP_RETF
        || op == OP_INT || op == OP_INT3;
}
static int is_cond_ender(int op) {
    return op == OP_JCC || op == OP_JCXZ || op == OP_LOOP || op == OP_LOOPE || op == OP_LOOPNE;
}

/* Flag data-flow of one op over the arithmetic bits: rd = bits read,
 * wr = bits written. Helper-class ops sync the whole F both ways. */
static void op_flag_effects(const x86_insn *in, int cls, uint32_t *rd, uint32_t *wr) {
    *rd = 0; *wr = 0;
    if (cls == C_HELPER) { *rd = ARITH; *wr = ARITH; return; }
    switch (in->op) {
    case OP_ADD: case OP_SUB: case OP_CMP: case OP_AND: case OP_OR: case OP_XOR: case OP_TEST: case OP_NEG:
        *wr = ARITH; break;
    case OP_ADC: case OP_SBB:
        *wr = ARITH; *rd = X86_CF; break;
    case OP_INC: case OP_DEC:
        *wr = ARITH & ~X86_CF; break;
    case OP_SHL: case OP_SAL: case OP_SHR: case OP_SAR:
        /* A CL count writes every flag or, when zero, none: the union
         * over both is a pass-through (rd = wr = 0) that computes the
         * live-out set when it does shift. */
        if (in->ops[1].kind == OPK_IMM && (in->ops[1].imm & 0xFF)) *wr = ARITH;
        break;
    case OP_ROL: case OP_ROR: case OP_RCL: case OP_RCR:
        /* CF (and OF) written by a nonzero count; through-carry forms read CF */
        if (in->op == OP_RCL || in->op == OP_RCR) *rd = X86_CF;
        if (in->ops[1].kind == OPK_IMM && (in->ops[1].imm & 0xFF)) *wr = X86_CF | X86_OF;
        break;
    case OP_LAHF: *rd = X86_SF | X86_ZF | X86_AF | X86_PF | X86_CF; break;
    case OP_SAHF: *wr = X86_SF | X86_ZF | X86_AF | X86_PF | X86_CF; break;
    case OP_DIV: case OP_IDIV: case OP_OUT:
        *rd = ARITH; break;
    case OP_CMPS: case OP_SCAS:
        if (!in->rep) *wr = ARITH;   /* repeated: none when CX = 0, so a pass-through */
        break;
    case OP_PUSHF: *rd = ARITH; break;
    case OP_POPF:  *wr = ARITH; break;          /* #DE / #GP: an unplanned exit whose frame holds the flags */
    case OP_SETCC: {
        static const uint16_t need[8] = { X86_OF, X86_CF, X86_ZF, X86_CF | X86_ZF, X86_SF, X86_PF, X86_SF | X86_OF, X86_SF | X86_OF | X86_ZF };
        *rd = need[in->cond >> 1]; break;
    }
    case OP_SHLD: case OP_SHRD:
        *wr = ARITH; break;          /* inline only with a nonzero immediate count */
    case OP_IMUL: case OP_IMUL3: case OP_MUL:
        *wr = ARITH; break;
    case OP_CLC: case OP_STC: *wr = X86_CF; break;
    case OP_CMC: *wr = X86_CF; *rd = X86_CF; break;
    case OP_JCC: case OP_JCXZ: case OP_LOOP: case OP_LOOPE: case OP_LOOPNE:
        *rd = ARITH; break;   /* side exit: every bit observable */
    default: break;
    }
}


/* Real-mode code bytes at CS:ip, through the A20 mirror. */
static void fetch_at(const x86_cpu *c, uint32_t ip, uint8_t *buf) {
    uint32_t base = c->seg[S_CS].base;
    for (int i = 0; i < 16; i++) buf[i] = x86_phys_rd8((x86_cpu *)c, base + ((ip + i) & 0xFFFF));
}

/* An inline op whose immediate keeps being patched (every byte of it at
 * SMC_VOLATILE heat) reads it from memory at run time: returns the
 * immediate's physical address (INSN_AT is the instruction's; a flat
 * block's code is where its page maps), or 0 to bake it in as usual. Only ops that
 * read the immediate as a value through emit_read_operand, and only with
 * no memory operand — a flat memory operand has a slow path that replays
 * the pooled decode, immediate and all. */
static uint32_t dyn_imm_at(const x86_dbt *dbt, const x86_insn *in, uint32_t insn_at) {
    switch (in->op) {
    case OP_ADD: case OP_OR: case OP_ADC: case OP_SBB: case OP_AND: case OP_SUB: case OP_XOR: case OP_CMP:
    case OP_TEST: case OP_MOV:
        break;
    default:
        return 0;
    }
    if (in->ea_valid || in->ops[1].kind != OPK_IMM || !in->ops[1].imm_enc) return 0;
    uint32_t at = insn_at + X86_IMM_AT(in->ops[1].imm_enc), n = X86_IMM_LEN(in->ops[1].imm_enc);
    for (uint32_t k = 0; k < n; k++)
        if (!dbt_smc_hot(dbt->smc_heat, dbt->smc_win, at + k, dbt_smc_window(dbt->cpu))) return 0;
    return at;
}


/* ----------------------------------------------------------------------
 * Protected-mode blocks that are nothing but helpers
 *
 * Everything the real-mode emitters inline for control transfers — the
 * interrupt frame, the IVT read, a far target's base from its selector —
 * is real-mode reasoning, so none of it applies here. A block is a run of
 * helper calls on pre-decoded instructions; what the translation saves is
 * the interpreter's fetch and decode. It ends after a near control
 * transfer (the helper has set EIP; the tail probes for the block there),
 * before anything classify_pm refuses (the run loop steps it), or at the
 * end of what the code segment's limit allows.
 * ---------------------------------------------------------------------- */
static int plan_pm(x86_dbt *dbt, dbt_block *b) {
    x86_cpu *cpu = dbt->cpu;
    const x86_seg *cs = &cpu->seg[S_CS];
    /* A20 off in protected mode would fold linear addresses under the
     * block's feet; nothing we run does it, so do not translate it. */
    if (cpu->a20_mask != 0xFFFFFFFFu) return 0;
    b->all_helper = 1;
    b->mode_bits = dbt_cpu_mode_bits(cpu);
    uint32_t ipmask = cs->big ? 0xFFFFFFFFu : 0xFFFFu;

    uint32_t ip = cpu->eip, n_ops = 0;
    b->start_ip = ip;
    uint8_t buf[16];
    x86_dec_ctx ctx = { buf, cpu->model, cs->big };
    while (n_ops < MAX_BLOCK_INSNS) {
        x86_insn *in = &b->decs[n_ops];
        if (ip > cs->limit) break;
        uint32_t lin = cs->base + ip;
        if (lin >= cpu->mem_size || cpu->mem_size - lin < 16) break;   /* the interpreter's open bus */
        for (int i = 0; i < 16; i++) buf[i] = cpu->mem[lin + (uint32_t)i];
        if (!x86_decode(&ctx, in)) break;
        if (ip + in->len - 1 > cs->limit || ip + in->len - 1 < ip) break;   /* straddles the limit: #GP is the interpreter's */
        if (((ip + in->len) & ipmask) != ip + in->len) break;              /* 16-bit IP wrap */
        if (classify_pm(in) == C_REFUSE) { dbt->refused_by_op[in->op]++; break; }
        ip += in->len;
        b->ip_afters[n_ops] = ip;
        b->cls[n_ops] = C_HELPER;
        b->role[n_ops] = ROLE_PLAIN;
        n_ops++;
        if (dbt_near_transfer(in->op)) { b->ends_dynamic = 1; break; }
    }
    b->n_ops = n_ops;
    b->end_ip = ip;
    return n_ops != 0;
}

/* ----------------------------------------------------------------------
 * Block planning: decode, classify, roles, flag liveness
 * ---------------------------------------------------------------------- */
static int plan_block(x86_dbt *dbt, dbt_block *b) {
    x86_cpu *cpu = dbt->cpu;
    uint64_t key = b->key;
    b->flat = cpu->pmode && !b->v86 && (key & KEY_FLAT) != 0;    /* from here on: real mode or V86, a flat block, or a segmented 16-bit one */
    b->seg16 = cpu->pmode && !b->v86 && !b->flat;
    b->ss32 = b->seg16 && (key & KEY_SS32) != 0;
    b->mode_bits = key & 0x7FFF000000000000ull;
    b->esnull = (key & KEY_ESNULL) != 0;
    b->devread = cpu->device_read != NULL;
    b->dsnull = (key & KEY_DSNULL) != 0;
    b->pg_user = (b->flat || b->seg16) && b->paged && (cpu->seg[S_CS].sel & 3) == 3;
    uint32_t code_page = 0, code_delta = 0;     /* physical - linear, mod 2^32: add it before indexing mem */
    if (b->paged) {
        /* A paged block (V86, or flat protected mode) stays on one code
         * page. Its key is linear; its bytes, and their code-bitmap
         * marks, are wherever the page maps — one-to-one, or remapped
         * (UMB code, a memory manager mapped high), which phys_alias lets
         * the SMC sweep find. dbt_tlb_flushed re-peeks it. */
        int user = b->v86 || b->pg_user;
        code_page = (cpu->seg[S_CS].base + (b->flat ? cpu->eip : (cpu->eip & 0xFFFF))) & 0xFFFFF000u;
        uint32_t phys = x86_page_peek(cpu, code_page, user);
        if (phys == X86_PG_BAD || phys + 0xFFFu >= cpu->mem_size) return 0;
        if (!dbt_note_code_page(dbt, code_page >> 12, phys >> 12, user)) return 0;
        if (phys != (code_page & cpu->a20_mask)) {
            uint32_t *alias = &dbt->phys_alias[phys >> 12];
            if (*alias && *alias != (code_page >> 12) + 1) return 0;   /* one alias per physical page */
            *alias = (code_page >> 12) + 1;
            code_delta = phys - code_page;
        }
        (void)x86_phys_rd8(cpu, code_page | ((cpu->seg[S_CS].base + cpu->eip) & 0xFFF));   /* the fetch's walk: accessed bits */
    }
    b->code_delta = code_delta;

    /* ---- Phase 1: decode ---- */
    x86_insn *decs = b->decs;
    uint32_t ip = b->flat ? cpu->eip : cpu->eip & 0xFFFF;
    uint32_t start_ip = ip;
    uint32_t n_ops = 0;
    uint8_t buf[16];
    x86_dec_ctx ctx = { buf, cpu->model, (uint8_t)b->flat };
    s_cls_model = cpu->model;

    while (n_ops < MAX_BLOCK_INSNS) {
        x86_insn *in = &decs[n_ops];
        if (b->flat && b->paged) {
            uint32_t lin = cpu->seg[S_CS].base + ip;
            if ((lin & 0xFFFFF000u) != code_page) break;
            memcpy(buf, cpu->mem + (uint32_t)(lin + code_delta), 16);
        } else if (b->flat) {
            /* flat CS: no limit to hit short of 4 GB; stay inside memory */
            uint32_t lin = cpu->seg[S_CS].base + ip;
            if (lin >= cpu->mem_size || cpu->mem_size - lin < 16) break;
            memcpy(buf, cpu->mem + lin, 16);
        } else if (b->paged) {
            uint32_t lin = cpu->seg[S_CS].base + ip;
            if ((lin & 0xFFFFF000u) != code_page) break;
            memcpy(buf, cpu->mem + (uint32_t)(lin + code_delta), 16);          /* identity: the mirror applies A20 */
        } else {
            fetch_at(cpu, ip, buf);
        }
        if (!x86_decode(&ctx, in)) break;
        if (!b->flat && ip + in->len > 0x10000) break;  /* IP wrap: leave it to the interp */
        if (b->paged && ((cpu->seg[S_CS].base + ip + in->len - 1) & 0xFFFFF000u) != code_page) break;   /* runs off the page */
        if (!b->flat) {
            /* An instruction whose bytes keep being patched (FreeCOM
             * rewrites its INT n's number before every call) stays out of
             * blocks: the interpreter runs it from memory as it is, and
             * the patches no longer invalidate the code around it. */
            uint32_t at = cpu->seg[S_CS].base + ip + code_delta;
            if (!b->v86 && !b->seg16) at &= cpu->a20_mask;
            uint8_t now = dbt_smc_window(cpu);
            int hot = 0;
            for (uint32_t k = 0; k < in->len && at + k < cpu->mem_size; k++)
                if (dbt_smc_hot(dbt->smc_heat, dbt->smc_win, at + k, now)) { hot = 1; break; }
            if (hot) { dbt->smc_hot_refusals++; break; }
        }
        if (b->seg16 && ip + in->len - 1 > cpu->seg[S_CS].limit) break;   /* past the code limit: #GP is the interpreter's */
        int c = b->flat ? classify_flat(b, in) : b->seg16 ? classify_seg16(b, in) : classify(b, in);
        if (b->v86) c = classify_v86(b, in, c);
        if (b->seg16 && b->paged && c == C_INLINE) {
            /* one checked access an instruction (emit_ea_seg16_paged); a
             * string op, or a stack op through memory, has two */
            switch (in->op) {
            case OP_MOVS: case OP_STOS: case OP_LODS: case OP_CMPS: case OP_SCAS:
                c = C_HELPER; break;
            case OP_PUSH: case OP_POP: case OP_CALL: case OP_JMP:
                if (in->ops[0].kind == OPK_MEM) c = C_HELPER;
                break;
            default: break;
            }
        }
        if (b->devread && !b->flat && c == C_INLINE
            && (in->op == OP_MOVS || in->op == OP_LODS || in->op == OP_CMPS || in->op == OP_SCAS))
            c = C_HELPER;                              /* a read through SI/DI may be the window's */
        if (b->flat && c == C_INLINE) {
            if (b->esnull && in->ea_valid && in->seg == S_ES) c = C_HELPER;          /* #GP: the interpreter's */
            if (b->dsnull && in->ea_valid && in->seg == S_DS) c = C_HELPER;
            if (b->paged && (in->op == OP_PUSHA || in->op == OP_POPA)) c = C_HELPER;  /* two accesses */
        }
        if (cpu->model == X86_MODEL_286 && in->len > 10) c = C_REFUSE;   /* #GP: the interpreter's */
        if (c == C_REFUSE) {
            dbt->refused_by_op[in->op]++;
            break;
        }
        int r = ROLE_PLAIN;
        if (c == C_INLINE && is_uncond_ender(in->op)) r = ROLE_UNCOND;
        else if (c == C_INLINE && is_cond_ender(in->op)) r = ROLE_COND;
        else if ((b->flat || b->seg16) && c == C_HELPER && (dbt_near_transfer(in->op) || dbt_loads_segment(in))) r = ROLE_HELPER_END;
        else if (b->v86 && c == C_HELPER && (dbt_near_transfer(in->op) || in->op == OP_CALLF || in->op == OP_RETF || in->op == OP_JMPF))
            r = ROLE_HELPER_END;                       /* paged V86: a transfer run by the interpreter */
        b->cls[n_ops] = (uint8_t)c;
        b->role[n_ops] = (uint8_t)r;
        /* physical, so under paging too (a paged block's page, and with it
         * code_delta, holds for the block's life): DOOM's renderer patches
         * its own immediates, and under EMM386 each patch was an SMC
         * invalidation — ten million of them, 24x slower than bare DOS */
        b->dyn_lin[n_ops] = b->flat && c == C_INLINE ? dyn_imm_at(dbt, in, cpu->seg[S_CS].base + ip + code_delta) : 0;
        b->ip_afters[n_ops] = ip + in->len;
        n_ops++;
        ip += in->len;
        if (r == ROLE_UNCOND || r == ROLE_HELPER_END) break;
        if (r == ROLE_COND && ip - start_ip >= SUPERBLOCK_BYTE_CAP) break;
    }
    b->n_ops = n_ops;
    b->start_ip = start_ip;
    b->end_ip = ip;
    if (n_ops == 0) return 0;

    /* ---- Phase 2: backward flag liveness. fmask[i] = bits live after
     * op i (LIVE-OUT, not live∩write). Block exits observe everything. */
    {
        uint32_t live = ARITH;
        for (int i = (int)n_ops - 1; i >= 0; i--) {
            uint32_t rd, wr;
            op_flag_effects(&decs[i], b->cls[i], &rd, &wr);
            /* 286+: a limit fault is an unplanned exit whose frame holds
             * the flags, so an op that can fault observes all of them. */
            if (cpu->model >= X86_MODEL_286 && !b->flat && op_may_fault(b, &decs[i])) rd |= ARITH;
            /* A store can leave the block after the op (SMC): all live out. */
            b->fmask[i] = live | (b->cls[i] == C_INLINE && op_stores(&decs[i]) ? ARITH : 0);
            live = (b->fmask[i] & ~wr) | rd;
            b->live_in[i] = live;
        }
    }
    return 1;
}

/* ----------------------------------------------------------------------
 * Entry: a key to host code (or NULL: the run loop steps the interpreter)
 * ---------------------------------------------------------------------- */
/* X86_PERF_MAP=1: every block as it is emitted goes into /tmp/perf-<pid>.map
 * ("start size name", perf's JIT symbol format), so `perf report` names
 * translated code by guest key even after flushes reuse the buffer
 * (a later block at the same host address is simply a later line). */
static void perf_map_note(x86_dbt *dbt, const dbt_block *b, uint64_t key, const uint8_t *entry) {
    static FILE *f, *g, *h;
    static long hoff;
    static int on = -1;
    if (on < 0) {
        on = getenv("X86_PERF_MAP") != NULL;
        if (on) {
            char path[64];
            snprintf(path, sizeof path, "/tmp/perf-%d.map", (int)getpid());
            f = fopen(path, "w");
            snprintf(path, sizeof path, "/tmp/perf-%d.blocks", (int)getpid());
            g = fopen(path, "w");                    /* each block's guest code, by the same name */
            snprintf(path, sizeof path, "/tmp/perf-%d.code", (int)getpid());
            h = fopen(path, "wb");                   /* ...and its host bytes, at the offset the .blocks line gives */
            if (!f) on = 0;
        }
    }
    if (!on) return;
    size_t size = (size_t)(dbt->code_buf + dbt->code_used - entry);
    fprintf(f, "%lx %zx x86_%04X_%04X:%08X\n", (unsigned long)(uintptr_t)entry, size,
            (unsigned)(key >> 48), (unsigned)(key >> 32) & 0xFFFF, (unsigned)key);
    fflush(f);
    if (g) {
        fprintf(g, "x86_%04X_%04X:%08X (%zu host bytes) code@%ld\n", (unsigned)(key >> 48), (unsigned)(key >> 32) & 0xFFFF, (unsigned)key, size, hoff);
        if (h) { fwrite(entry, 1, size, h); fflush(h); hoff += (long)size; }
        uint32_t ip = b->start_ip;
        for (uint32_t i = 0; i < b->n_ops; i++) {
            char buf[80];
            x86_disasm(&b->decs[i], buf, sizeof buf);
            static const char cls[] = "RIH";
            fprintf(g, "    %04X  %c  %s\n", ip, b->cls[i] < 3 ? cls[b->cls[i]] : '?', buf);
            ip = b->ip_afters[i];
        }
        fflush(g);
    }
}

uint8_t *dbt_translate_block(x86_dbt *dbt, uint64_t key) {
    x86_cpu *cpu = dbt->cpu;

    if (dbt->flush_pending || dbt->code_used + 65536 > CODE_BUF_SIZE || dbt->insn_used + MAX_BLOCK_INSNS > INSN_POOL_SIZE) {
        /* Out of JIT space (or A20 flipped): wipe and restart. Already
         * inside the W^X bracket (the run loop wraps us) — do not nest
         * another. */
        dbt_cache_invalidate_all(dbt);
        dbt->flush_pending = 0;
        dbt->code_used = 0;
        dbt_emit_trampoline(dbt);
    }

    /* HLE stub segment: the interpreter step dispatches the host service. */
    if (cpu->hle && cpu->seg[S_CS].base == ((uint32_t)cpu->hle_seg << 4)) return NULL;

    static dbt_block blk;           /* the decoded instructions make it too big for the stack */
    dbt_block *b = &blk;
    memset(b, 0, offsetof(dbt_block, decs));
    b->key = key;
    b->model = cpu->model;
    b->wrap_exact = cpu->model < X86_MODEL_286;
    b->regs32 = cpu->model >= X86_MODEL_386;
    b->v86 = (key & KEY_V86) != 0;
    b->paged = (key & KEY_PAGED) != 0;
    b->iopl3 = (key & KEY_IOPL3) != 0;
    if (cpu->pmode && !b->v86 && (!(key & (KEY_FLAT | KEY_SEG16)) || cpu->a20_mask != 0xFFFFFFFFu)) {
        if (!plan_pm(dbt, b)) return NULL;
        uint8_t *entry = dbt_arch_emit_block(dbt, b);
        if (!entry) return NULL;
        perf_map_note(dbt, b, key, entry);
        uint32_t lin = dbt_key_lin(key);
        dbt_mark_block_bytes(dbt, lin, lin + (b->end_ip - b->start_ip));
        dbt_watch_cs_desc(dbt);
        return entry;
    }
    if (!plan_block(dbt, b)) return NULL;
    uint8_t *entry = dbt_arch_emit_block(dbt, b);
    if (!entry) return NULL;
    perf_map_note(dbt, b, key, entry);

    uint32_t lin = dbt_key_lin(key);
    uint32_t skip[2 * MAX_BLOCK_INSNS], nskip = 0;
    for (uint32_t i = 0; i < b->n_ops; i++)
        if (b->dyn_lin[i]) {
            skip[2 * nskip] = b->dyn_lin[i];
            skip[2 * nskip + 1] = b->dyn_lin[i] + X86_IMM_LEN(b->decs[i].ops[1].imm_enc);
            nskip++;
        }
    lin += b->code_delta;                            /* the bitmap is physical */
    dbt_mark_block_bytes_except(dbt, lin, lin + (b->end_ip - b->start_ip), skip, nskip);
    if (b->flat || b->seg16) dbt_watch_cs_desc(dbt);
    return entry;
}
