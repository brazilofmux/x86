/* dbt_a64.c — AArch64 backend for the x86 DBT.
 *
 * Register convention (see dbt.h for the full ABI):
 *   X19 cpu  X20 mem  X21..X28 AX CX DX BX SP BP SI DI (canonical 16-bit)
 *   X11 aux  X12 F (arith bits)  X13 budget  X14/15/16 DS/SS/ES host ptrs
 *   X17 bitmap-mem delta
 *   Scratch: X0..X10. X0/X1 are helper arguments at call sites; the
 *   per-op emitters use W9 for the EA offset, X10 for a non-pinned
 *   segment pointer, W8/W7 for operand values, W4..W6 for temporaries.
 *
 * Flags: N-bit arithmetic is done in the top N bits of a W register
 * (operands LSL #16 or #24) so NZCV is exactly the guest's SF/ZF/CF/OF;
 * MRS + one table load turns it into the x86 bit layout. AF and PF are
 * computed separately and only when the liveness pass says they are
 * observed — which, thanks to the block-exit "all live" rule, means
 * before conditional side exits. That is the first thing to optimize
 * once -V is clean (defer the materialization into the side chunk).
 *
 * Three instruction classes (classify()):
 *   INLINE  — emitted directly.
 *   HELPER  — synced call into the interpreter's execute() via the
 *             exec thunk. Exact by construction; slow. The stats print
 *             which ops take this path so they can be promoted.
 *   REFUSE  — ends the block; the run loop steps the interpreter.
 *
 * Blocks never RET: tails B to the shared exit stub with X0 = next
 * key, or BR into the next block through the inline cache probe, or
 * are direct-linked. Helper calls are BL to thunks that save X30.
 */
#include "dbt.h"
#include "emit_a64.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Superblocks keep translating through conditionals (side exits) only
 * while the block is shorter than this many guest bytes. */
#define SUPERBLOCK_BYTE_CAP 48

#define R_CPU  A64_W19
#define R_MEM  A64_W20
#define R_AUX  A64_W11
#define R_F    A64_W12
#define R_CNT  A64_W13
#define R_DSP  A64_W14
#define R_SSP  A64_W15
#define R_ESP  A64_W16
#define R_BMD  A64_W17
#define R_GPR(i) ((a64_reg_t)(A64_W21 + (i)))

/* Scratch roles */
#define W_OFF  A64_W9     /* EA offset (16-bit canonical) */
#define X_SEGP A64_W10    /* non-pinned segment host pointer */
#define W_VAL  A64_W8     /* destination operand value / result */
#define W_SRC  A64_W7     /* source operand value */
#define W_T0   A64_W6
#define W_T1   A64_W5
#define W_T2   A64_W4
#define W_T3   A64_W3

#define OFF_R(i)        (offsetof(x86_cpu, r) + 4 * (i))
#define OFF_EIP         offsetof(x86_cpu, eip)
#define OFF_EFLAGS      offsetof(x86_cpu, eflags)
#define OFF_SEG_SEL(i)  (offsetof(x86_cpu, seg) + sizeof(x86_seg) * (i) + offsetof(x86_seg, sel))
#define OFF_SEG_BASE(i) (offsetof(x86_cpu, seg) + sizeof(x86_seg) * (i) + offsetof(x86_seg, base))
#define OFF_INSN_COUNT  offsetof(x86_cpu, insn_count)
#define OFF_CODE_BITMAP offsetof(x86_cpu, code_bitmap)
#define OFF_JIT_AUX     offsetof(x86_cpu, jit_aux)
#define OFF_JIT_BUDGET  offsetof(x86_cpu, jit_budget)
#define OFF_JIT_CNTSAVE offsetof(x86_cpu, jit_cnt_save)
#define OFF_JIT_CUR_LIN offsetof(x86_cpu, jit_cur_lin)
#define OFF_JIT_CUR_HIT offsetof(x86_cpu, jit_cur_hit)
#define OFF_EXC         offsetof(x86_cpu, exc)
#define OFF_INT_INHIBIT offsetof(x86_cpu, int_inhibit)

#define ARITH  X86_ARITH_FLAGS   /* 0x8D5 — not a logical immediate, load it */

/* Thunk/stub offsets in the code buffer (emitted with the trampoline). */
static uint32_t s_exec_thunk_off, s_smc_thunk_off, s_fault_stub_off, s_fault_exit_off;

/* -V strict mode: every block returns to dbt_run (no links, no probe). */
static int s_strict_exit = -1;

/* ----------------------------------------------------------------------
 * Pinned-state load/spill sequences
 * ---------------------------------------------------------------------- */
static void emit_load_seg_ptr(emit_t *e, a64_reg_t xd, int s) {
    emit_ldr_w32_imm(e, W_T1, R_CPU, OFF_SEG_BASE(s));
    emit_add_x64_w32_uxtw(e, xd, R_MEM, W_T1);
}

static void emit_load_pinned(emit_t *e) {
    for (int i = 0; i < 8; i += 2) emit_ldp_w32_off(e, R_GPR(i), R_GPR(i + 1), R_CPU, (int)OFF_R(i));
    emit_ldr_w32_imm(e, R_F, R_CPU, OFF_EFLAGS);
    emit_movz_w32(e, W_T1, ARITH, 0);
    emit_and_w32(e, R_F, R_F, W_T1);
    emit_load_seg_ptr(e, R_DSP, S_DS);
    emit_load_seg_ptr(e, R_SSP, S_SS);
    emit_load_seg_ptr(e, R_ESP, S_ES);
    emit_ldr_x64_imm(e, A64_W5, R_CPU, OFF_CODE_BITMAP);
    emit_sub_x64(e, R_BMD, A64_W5, R_MEM);
    emit_ldr_x64_imm(e, R_AUX, R_CPU, OFF_JIT_AUX);
}

static void emit_spill_pinned(emit_t *e) {
    for (int i = 0; i < 8; i += 2) emit_stp_w32_off(e, R_GPR(i), R_GPR(i + 1), R_CPU, (int)OFF_R(i));
    emit_ldr_w32_imm(e, W_T0, R_CPU, OFF_EFLAGS);
    emit_movz_w32(e, W_T1, ARITH, 0);
    emit_bic_w32(e, W_T0, W_T0, W_T1);
    emit_orr_w32(e, W_T0, W_T0, R_F);
    emit_str_w32_imm(e, W_T0, R_CPU, OFF_EFLAGS);
}

/* ----------------------------------------------------------------------
 * Trampoline, exit stub, thunks.
 *
 * Called from C as
 *   void trampoline(x86_cpu *cpu, uint8_t *mem, void *block, void *aux,
 *                   uint64_t budget);
 * ---------------------------------------------------------------------- */
void dbt_emit_trampoline(x86_dbt *dbt) {
    emit_t e = { .buf = dbt->code_buf, .offset = 0, .capacity = CODE_BUF_SIZE };

    emit_stp_pre_sp (&e, A64_W29, A64_W30, -96);
    emit_stp_x64_off(&e, A64_W19, A64_W20, A64_SP, 16);
    emit_stp_x64_off(&e, A64_W21, A64_W22, A64_SP, 32);
    emit_stp_x64_off(&e, A64_W23, A64_W24, A64_SP, 48);
    emit_stp_x64_off(&e, A64_W25, A64_W26, A64_SP, 64);
    emit_stp_x64_off(&e, A64_W27, A64_W28, A64_SP, 80);

    emit_mov_x64_x64(&e, R_CPU, A64_W0);
    emit_mov_x64_x64(&e, R_MEM, A64_W1);
    emit_mov_x64_x64(&e, R_CNT, A64_W4);
    emit_str_x64_imm(&e, A64_W4, R_CPU, OFF_JIT_BUDGET);
    emit_load_pinned(&e);
    emit_br(&e, A64_W2);

    /* ---- Exit stub: X0 = next key. ---- */
    dbt->exit_stub_off = e.offset;
    emit_ldr_w32_imm(&e, W_T0, R_CPU, OFF_SEG_BASE(S_CS));
    emit_sub_w32(&e, W_T0, A64_W0, W_T0);
    (void)emit_and_w32_imm(&e, W_T0, W_T0, 0xFFFF);
    emit_str_w32_imm(&e, W_T0, R_CPU, OFF_EIP);
    emit_spill_pinned(&e);
    /* insn_count += budget - remaining */
    emit_ldr_x64_imm(&e, A64_W5, R_CPU, OFF_JIT_BUDGET);
    emit_sub_x64(&e, A64_W5, A64_W5, R_CNT);
    emit_ldr_x64_imm(&e, A64_W6, R_CPU, OFF_INSN_COUNT);
    emit_add_x64(&e, A64_W6, A64_W6, A64_W5);
    emit_str_x64_imm(&e, A64_W6, R_CPU, OFF_INSN_COUNT);

    emit_ldp_x64_off(&e, A64_W27, A64_W28, A64_SP, 80);
    emit_ldp_x64_off(&e, A64_W25, A64_W26, A64_SP, 64);
    emit_ldp_x64_off(&e, A64_W23, A64_W24, A64_SP, 48);
    emit_ldp_x64_off(&e, A64_W21, A64_W22, A64_SP, 32);
    emit_ldp_x64_off(&e, A64_W19, A64_W20, A64_SP, 16);
    emit_ldp_post_sp(&e, A64_W29, A64_W30, 96);
    emit_ret(&e);

    /* ---- Fault stub: B here with W3 = ip of the faulting instruction,
     * W4 = instructions to charge (including it), W5 = vector. Records
     * the exception for the run loop and exits at that instruction. ---- */
    s_fault_stub_off = e.offset;
    emit_str_w32_imm(&e, A64_W5, R_CPU, OFF_EXC);
    s_fault_exit_off = e.offset;
    emit_sub_x64(&e, R_CNT, R_CNT, A64_W4);
    emit_ldr_w32_imm(&e, W_T1, R_CPU, OFF_SEG_BASE(S_CS));
    emit_add_w32(&e, A64_W0, W_T1, A64_W3);
    emit_ldrh_imm(&e, W_T1, R_CPU, OFF_SEG_SEL(S_CS));
    emit_orr_x64_lsl(&e, A64_W0, A64_W0, W_T1, 32);
    emit_b(&e, (int32_t)dbt->exit_stub_off - (int32_t)emit_pos(&e));

    /* ---- Thunks. Both are entered by BL from a block with
     *   X0 = cpu, W2 = the block's linear address, W3 = ip after the
     *   current instruction, W4 = instructions executed so far in the
     *   block including it,
     * plus the helper's own argument in W1 (insn pool index / store
     * address). They call the C helper with the pinned state synced as
     * needed, then check whether the SMC sweep invalidated the block
     * they were called from; if so the block must not continue into its
     * stale remainder, so the thunk charges the budget and leaves for
     * the run loop at ip-after instead of returning. ---- */
    for (int which = 0; which < 2; which++) {
        int is_exec = which == 0;
        if (is_exec) s_exec_thunk_off = e.offset; else s_smc_thunk_off = e.offset;

        emit_stp_pre_sp(&e, A64_W29, A64_W30, -96);
        emit_stp_x64_off(&e, A64_W3, A64_W4, A64_SP, 16);
        emit_str_w32_imm(&e, A64_W2, R_CPU, OFF_JIT_CUR_LIN);
        emit_str_w32_imm(&e, A64_WZR, R_CPU, OFF_JIT_CUR_HIT);
        if (is_exec) {
            emit_str_w32_imm(&e, A64_W3, R_CPU, OFF_EIP);
            emit_spill_pinned(&e);
            emit_str_x64_imm(&e, R_CNT, R_CPU, OFF_JIT_CNTSAVE);
            emit_ldr_x64_imm(&e, A64_W9, R_AUX, AUX_HELPERS + 8 * H_EXEC);
            emit_blr(&e, A64_W9);
            emit_ldr_x64_imm(&e, R_CNT, R_CPU, OFF_JIT_CNTSAVE);
            emit_load_pinned(&e);
        } else {
            emit_stp_x64_off(&e, A64_W11, A64_W12, A64_SP, 32);
            emit_stp_x64_off(&e, A64_W13, A64_W14, A64_SP, 48);
            emit_stp_x64_off(&e, A64_W15, A64_W16, A64_SP, 64);
            emit_str_x64_imm(&e, A64_W17, A64_SP, 80);
            emit_ldr_x64_imm(&e, A64_W9, R_AUX, AUX_HELPERS + 8 * H_POST_STORE);
            emit_blr(&e, A64_W9);
            emit_ldr_x64_imm(&e, A64_W17, A64_SP, 80);
            emit_ldp_x64_off(&e, A64_W15, A64_W16, A64_SP, 64);
            emit_ldp_x64_off(&e, A64_W13, A64_W14, A64_SP, 48);
            emit_ldp_x64_off(&e, A64_W11, A64_W12, A64_SP, 32);
        }
        emit_ldp_x64_off(&e, A64_W3, A64_W4, A64_SP, 16);
        emit_ldp_post_sp(&e, A64_W29, A64_W30, 96);
        if (is_exec) {
            /* The interpreter faulted inside the helper op: cpu->exc is
             * set and eip points at the instruction. Charge it, leave. */
            emit_ldr_w32_imm(&e, W_T1, R_CPU, OFF_EXC);
            emit_add_w32_imm(&e, W_T1, W_T1, 1);
            uint32_t nofault = emit_pos(&e);
            emit_cbz_w32(&e, W_T1, 0);
            emit_ldr_w32_imm(&e, A64_W3, R_CPU, OFF_EIP);
            emit_b(&e, (int32_t)s_fault_exit_off - (int32_t)emit_pos(&e));
            emit_patch_cond19(&e, nofault, emit_pos(&e));
        }
        emit_ldr_w32_imm(&e, W_T1, R_CPU, OFF_JIT_CUR_HIT);
        uint32_t cont = emit_pos(&e);
        emit_cbnz_w32(&e, W_T1, 0);
        emit_ret(&e);
        /* Running block invalidated: X0 = key(CS, cs.base + ip_after),
         * charge W4 instructions, exit. (HMA-wrapped code is not a case.) */
        emit_patch_cond19(&e, cont, emit_pos(&e));
        emit_sub_x64(&e, R_CNT, R_CNT, A64_W4);
        emit_ldr_w32_imm(&e, W_T1, R_CPU, OFF_SEG_BASE(S_CS));
        emit_add_w32(&e, A64_W0, W_T1, A64_W3);
        emit_ldrh_imm(&e, W_T1, R_CPU, OFF_SEG_SEL(S_CS));
        emit_orr_x64_lsl(&e, A64_W0, A64_W0, W_T1, 32);
        emit_b(&e, (int32_t)dbt->exit_stub_off - (int32_t)emit_pos(&e));
    }

    dbt->code_used = e.offset;
    __builtin___clear_cache((char *)dbt->code_buf, (char *)dbt->code_buf + e.offset);
}

void dbt_arch_patch_link(x86_dbt *dbt, uint32_t site_off, uint8_t *target) {
    uint8_t *site = dbt->code_buf + site_off;
    uint8_t *dst  = target ? target : site + 4;
    int64_t  disp = dst - site;
    uint32_t inst = 0x14000000u | (((uint32_t)((int32_t)disp >> 2)) & 0x03FFFFFFu);
    uint32_t cur;
    memcpy(&cur, site, 4);
    if (cur == inst) return;
    memcpy(site, &inst, 4);
    __builtin___clear_cache((char *)site, (char *)site + 4);
}

/* ----------------------------------------------------------------------
 * Block tails: budget accounting, cache probe, static edges.
 * ---------------------------------------------------------------------- */

/* Dynamic tail. X0 = next key. Probe the cache through the aux base:
 *   X3 = aux + AUX_CACHE + lin*16 ; LDP key,code ; CMP ; BR or exit. */
static void emit_dynamic_tail(emit_t *e, uint32_t exit_stub_off) {
    if (s_strict_exit > 0) {
        emit_b(e, (int32_t)exit_stub_off - (int32_t)emit_pos(e));
        return;
    }
    emit_add_x64_imm_lsl12(e, W_T3, R_AUX, AUX_CACHE >> 12);
    emit_add_x64_w32_uxtw_lsl(e, W_T3, W_T3, A64_W0, 4);
    emit_ldp_x64_off(e, W_T1, W_T2, W_T3, 0);
    emit_cmp_x64_x64(e, W_T1, A64_W0);
    uint32_t miss = emit_pos(e);
    emit_b_cond(e, A64_COND_NE, 0);
    emit_br(e, W_T2);
    emit_patch_cond19(e, miss, emit_pos(e));
    emit_b(e, (int32_t)exit_stub_off - (int32_t)emit_pos(e));
}

/* Static edge to `key`:
 *   B <target | .+4>       patchable link site
 *   MOV X0, #key           only reached while unlinked
 *   <probe + exit>
 * The linked path executes nothing but the B. */
static void emit_edge(x86_dbt *dbt, emit_t *e, uint64_t key) {
    if (s_strict_exit > 0) {
        emit_mov_x64_imm64(e, A64_W0, key);
        emit_dynamic_tail(e, dbt->exit_stub_off);
        return;
    }
    uint32_t lin = dbt_key_lin(key);
    uint32_t site = e->offset;
    int linked = 0;
    if (dbt_link_record(dbt, lin, site)) {
        x86_block_entry *be = &dbt->aux->cache[lin];
        if (be->key == key && be->code) {
            emit_b(e, (int32_t)(be->code - (e->buf + site)));
            linked = 1;
        }
    }
    if (!linked) emit_b(e, 4);
    emit_mov_x64_imm64(e, A64_W0, key);
    emit_dynamic_tail(e, dbt->exit_stub_off);
}

/* Charge n instructions to the budget. Runs once per exit path, before
 * the branch guts (which set NZCV themselves). */
static void emit_tail_prologue(emit_t *e, uint32_t n) {
    emit_sub_x64_imm(e, R_CNT, R_CNT, n);
}

/* ----------------------------------------------------------------------
 * Operand access
 * ---------------------------------------------------------------------- */
typedef struct {
    a64_reg_t segp;   /* host pointer register for the segment */
    a64_reg_t off;    /* W register with the 16-bit offset */
} ea_t;

static a64_reg_t seg_ptr_reg(int s) {
    switch (s) {
    case S_DS: return R_DSP;
    case S_SS: return R_SSP;
    case S_ES: return R_ESP;
    default:   return X_SEGP;
    }
}

/* Compute the effective address. Uses W_OFF (and X_SEGP for CS/FS/GS);
 * a bare [reg] form returns the pinned register itself. */
static void emit_ea(emit_t *e, const x86_insn *in, ea_t *ea) {
    ea->segp = seg_ptr_reg(in->seg);
    if (ea->segp == X_SEGP) emit_load_seg_ptr(e, X_SEGP, in->seg);

    int32_t disp = (int16_t)in->disp;
    if (in->base < 0 && in->index < 0) {
        emit_movz_w32(e, W_OFF, (uint16_t)disp, 0);
        ea->off = W_OFF;
        return;
    }
    if (in->base >= 0 && in->index < 0 && disp == 0) {
        ea->off = R_GPR(in->base);
        return;
    }
    a64_reg_t acc = R_GPR(in->base >= 0 ? in->base : in->index);
    if (in->base >= 0 && in->index >= 0) {
        emit_add_w32(e, W_OFF, R_GPR(in->base), R_GPR(in->index));
        acc = W_OFF;
    }
    if (disp) emit_add_w32_imm_any(e, W_OFF, acc, disp, W_T1);
    (void)emit_and_w32_imm(e, W_OFF, W_OFF, 0xFFFF);
    ea->off = W_OFF;
}

/* Read guest register `reg` of `size` into a canonical (zero-extended)
 * value; returns the register holding it, which is the pinned register
 * itself for 16-bit reads. */
static a64_reg_t emit_read_reg(emit_t *e, int reg, int size, a64_reg_t tmp) {
    if (size == 2) return R_GPR(reg);
    if (reg < 4) (void)emit_and_w32_imm(e, tmp, R_GPR(reg), 0xFF);
    else emit_ubfx_w32(e, tmp, R_GPR(reg & 3), 8, 8);
    return tmp;
}

/* Write `src` into guest register (low bits of src are the value). */
static void emit_write_reg(emit_t *e, int reg, int size, a64_reg_t src) {
    if (size == 2) {
        if (src != R_GPR(reg)) emit_mov_w32_w32(e, R_GPR(reg), src);
        return;
    }
    emit_bfi_w32(e, R_GPR(reg & 3), src, reg < 4 ? 0 : 8, 8);
}

/* Where in the block we are, for the thunks' "block invalidated under
 * us" exit: set per instruction by the block emitter. */
static uint32_t s_cur_lin, s_cur_ip_after, s_cur_ip_start, s_cur_n_done;
static int s_wrap_exact;   /* model < 286: word accesses at offset FFFF wrap in-segment; 286+: #GP */

static void emit_thunk_args(emit_t *e) {
    emit_mov_w32_imm32(e, A64_W2, s_cur_lin);
    emit_movz_w32(e, A64_W3, (uint16_t)s_cur_ip_after, 0);
    emit_movz_w32(e, A64_W4, (uint16_t)s_cur_n_done, 0);
}

/* Post-store SMC check for the byte at host address X3: LDRB bitmap
 * byte, CBZ over the thunk call. Clobbers W_T2, X0..X4 on the slow path. */
static void emit_smc_check_x3(emit_t *e) {
    emit_ldrb_reg_x(e, W_T2, W_T3, R_BMD);
    uint32_t skip = emit_pos(e);
    emit_cbz_w32(e, W_T2, 0);
    emit_mov_x64_x64(e, A64_W0, R_CPU);
    emit_sub_x64(e, A64_W1, W_T3, R_MEM);
    emit_thunk_args(e);
    emit_bl(e, (int32_t)s_smc_thunk_off - (int32_t)emit_pos(e));
    emit_patch_cond19(e, skip, emit_pos(e));
}

/* ---- Segment-limit slow paths ----
 * A 16-bit access whose offset is FFFF wraps in-segment on the 8086/186
 * and is #GP on the 286+ (CONTRACT, measured). The fast path tests the
 * offset (EOR #FFFF; CBZ) and the rare case runs an out-of-line chunk
 * emitted after the block body: the split access, or a jump to the
 * fault stub with the instruction's start IP (nothing of it committed:
 * every check precedes its instruction's first state change). */
typedef struct {
    uint32_t patch_off, back_off;
    uint8_t  is_store, smc;      /* smc: run the code-bitmap check on the split store (not for stack pushes) */
    a64_reg_t segp, reg;         /* reg: destination (load) or value (store) */
    uint32_t ip_after, ip_start, n_done;   /* thunk context at the access */
} wrap_slow_t;
static wrap_slow_t s_wrap[256];
static uint32_t s_nwrap;

static void emit_wrap_check(emit_t *e, a64_reg_t segp, a64_reg_t off, a64_reg_t reg, int is_store, int smc) {
    (void)emit_eor_w32_imm(e, W_T3, off, 0xFFFF);
    if (s_nwrap < 256) {
        s_wrap[s_nwrap] = (wrap_slow_t){ emit_pos(e), 0, (uint8_t)is_store, (uint8_t)smc, segp, reg, s_cur_ip_after, s_cur_ip_start, s_cur_n_done };
        emit_cbz_w32(e, W_T3, 0);
        s_wrap[s_nwrap].back_off = 0;   /* filled by the caller after the fast access */
        s_nwrap++;
    }
}
static void emit_wrap_back(emit_t *e) {
    if (s_nwrap && s_nwrap <= 256) s_wrap[s_nwrap - 1].back_off = emit_pos(e);
}

static void emit_wrap_slow_chunks(emit_t *e) {
    for (uint32_t k = 0; k < s_nwrap; k++) {
        wrap_slow_t *w = &s_wrap[k];
        emit_patch_cond19(e, w->patch_off, emit_pos(e));
        if (!s_wrap_exact) {
            /* 286+: #GP at this instruction; it counts as executed like x86_step's */
            emit_movz_w32(e, A64_W3, (uint16_t)w->ip_start, 0);
            emit_movz_w32(e, A64_W4, (uint16_t)w->n_done, 0);
            emit_movz_w32(e, A64_W5, X86_EXC_GP, 0);
            emit_b(e, (int32_t)s_fault_stub_off - (int32_t)emit_pos(e));
            continue;
        }
        emit_movz_w32(e, W_T3, 0xFFFF, 0);
        if (!w->is_store) {
            emit_ldrb_reg_uxtw(e, w->reg, w->segp, W_T3);
            emit_ldrb_imm(e, W_T3, w->segp, 0);
            emit_orr_w32_lsl(e, w->reg, w->reg, W_T3, 8);
        } else {
            s_cur_ip_after = w->ip_after; s_cur_n_done = w->n_done;
            emit_strb_reg_uxtw(e, w->reg, w->segp, W_T3);
            if (w->smc) { emit_add_x64_w32_uxtw(e, W_T3, w->segp, W_T3); emit_smc_check_x3(e); }
            emit_lsr_w32_imm(e, W_T1, w->reg, 8);
            emit_strb_imm(e, W_T1, w->segp, 0);
            if (w->smc) { emit_mov_x64_x64(e, W_T3, w->segp); emit_smc_check_x3(e); }
        }
        emit_b(e, (int32_t)w->back_off - (int32_t)emit_pos(e));
    }
    s_nwrap = 0;
}

static void emit_read_mem(emit_t *e, const ea_t *ea, int size, a64_reg_t dst) {
    if (size == 1) { emit_ldrb_reg_uxtw(e, dst, ea->segp, ea->off); return; }
    emit_wrap_check(e, ea->segp, ea->off, dst, 0, 0);
    emit_ldrh_reg_uxtw(e, dst, ea->segp, ea->off);
    emit_wrap_back(e);
}

/* Store + inline SMC check. Clobbers W_T2, W_T3, X0..X4 on the slow path. */
static void emit_write_mem(emit_t *e, const ea_t *ea, int size, a64_reg_t src) {
    if (size == 1) {
        emit_strb_reg_uxtw(e, src, ea->segp, ea->off);
    } else {
        emit_wrap_check(e, ea->segp, ea->off, src, 1, 1);
        emit_strh_reg_uxtw(e, src, ea->segp, ea->off);
    }
    emit_add_x64_w32_uxtw(e, W_T3, ea->segp, ea->off);
    emit_smc_check_x3(e);
    if (size == 2) emit_wrap_back(e);
}

/* Read operand i (canonical). Memory operands need the EA computed. */
static a64_reg_t emit_read_operand(emit_t *e, const x86_insn *in, int i, const ea_t *ea, a64_reg_t tmp) {
    const x86_operand *o = &in->ops[i];
    switch (o->kind) {
    case OPK_REG:  return emit_read_reg(e, o->reg, o->size, tmp);
    case OPK_IMM:  emit_mov_w32_imm32(e, tmp, o->imm & (o->size == 1 ? 0xFF : 0xFFFF)); return tmp;
    case OPK_SREG: emit_ldrh_imm(e, tmp, R_CPU, OFF_SEG_SEL(o->reg)); return tmp;
    case OPK_MEM:  emit_read_mem(e, ea, o->size, tmp); return tmp;
    }
    return tmp;
}

static void emit_write_operand(emit_t *e, const x86_insn *in, int i, const ea_t *ea, a64_reg_t src) {
    const x86_operand *o = &in->ops[i];
    if (o->kind == OPK_REG) emit_write_reg(e, o->reg, o->size, src);
    else emit_write_mem(e, ea, o->size, src);
}

/* ----------------------------------------------------------------------
 * Flags
 * ---------------------------------------------------------------------- */
enum { T_ADD = AUX_NZCV_ADD, T_SUB = AUX_NZCV_SUB, T_INC = AUX_NZCV_INC, T_DEC = AUX_NZCV_DEC };

/* R_F = table[NZCV]. Three instructions; the table row selects the
 * carry sense and whether CF is written. Clobbers W_T1. */
static void emit_flags_from_nzcv(emit_t *e, int table) {
    emit_mrs_nzcv(e, W_T1);
    emit_addsub_shifted(e, 1, 0, 0, W_T1, R_AUX, W_T1, 1, 27);   /* X5 = aux + nzcv*2 */
    emit_ldrh_imm(e, R_F, W_T1, (uint32_t)table);
}

/* R_F |= PF(res low byte). Clobbers W_T1. */
static void emit_flag_pf(emit_t *e, a64_reg_t res) {
    (void)emit_and_w32_imm(e, W_T1, res, 0xFF);
    emit_ldrb_reg_uxtw(e, W_T1, R_AUX, W_T1);
    emit_orr_w32(e, R_F, R_F, W_T1);
}

/* R_F |= AF from the carry-recovery identity (a ^ b ^ res) & 0x10. */
static void emit_flag_af(emit_t *e, a64_reg_t a, a64_reg_t b, a64_reg_t res) {
    emit_eor_w32(e, W_T1, a, b);
    emit_eor_w32(e, W_T1, W_T1, res);
    (void)emit_and_w32_imm(e, W_T1, W_T1, X86_AF);
    emit_orr_w32(e, R_F, R_F, W_T1);
}

/* Set host NZCV so that B.<returned cond> is taken iff x86 condition cc
 * holds on R_F. Clobbers W_T0. */
static a64_cond_t emit_test_cond(emit_t *e, int cc) {
    switch (cc >> 1) {
    case 0: (void)emit_tst_w32_imm(e, R_F, X86_OF); break;
    case 1: (void)emit_tst_w32_imm(e, R_F, X86_CF); break;
    case 2: (void)emit_tst_w32_imm(e, R_F, X86_ZF); break;
    case 3: emit_tst_w32_imm_any(e, R_F, X86_CF | X86_ZF, W_T0); break;
    case 4: (void)emit_tst_w32_imm(e, R_F, X86_SF); break;
    case 5: (void)emit_tst_w32_imm(e, R_F, X86_PF); break;
    case 6:   /* L: SF != OF — OF sits 4 bits above SF */
        emit_eor_w32_lsr(e, W_T0, R_F, R_F, 4);
        (void)emit_tst_w32_imm(e, W_T0, X86_SF);
        break;
    default:  /* LE: (SF != OF) | ZF — ZF is the bit below SF */
        emit_eor_w32_lsr(e, W_T0, R_F, R_F, 4);
        emit_logical_shifted(e, 1, 0, W_T0, W_T0, W_T0, 0, 1);
        (void)emit_tst_w32_imm(e, W_T0, X86_SF);
        break;
    }
    return (cc & 1) ? A64_COND_EQ : A64_COND_NE;
}

/* ----------------------------------------------------------------------
 * ALU
 * ---------------------------------------------------------------------- */
static inline uint32_t szmask(int size) { return size == 1 ? 0xFF : 0xFFFF; }
static inline uint32_t topshift(int size) { return size == 1 ? 24 : 16; }

/* res = a op b with flags per fmask. a and b are canonical; a must not
 * be W_T0/W_T1 (they are clobbered before a is dead). res may alias a. */
static void emit_alu(emit_t *e, int op, int size, a64_reg_t a, a64_reg_t b, a64_reg_t res, uint32_t fmask) {
    uint32_t sh = topshift(size), m = szmask(size);
    int wr_res = (op != OP_CMP && op != OP_TEST);

    if (!fmask) {
        switch (op) {
        case OP_ADD: emit_add_w32(e, res, a, b); (void)emit_and_w32_imm(e, res, res, m); break;
        case OP_SUB: emit_sub_w32(e, res, a, b); (void)emit_and_w32_imm(e, res, res, m); break;
        case OP_ADC: case OP_SBB:
            (void)emit_and_w32_imm(e, W_T1, R_F, X86_CF);
            if (op == OP_ADC) { emit_add_w32(e, res, a, b); emit_add_w32(e, res, res, W_T1); }
            else              { emit_sub_w32(e, res, a, b); emit_sub_w32(e, res, res, W_T1); }
            (void)emit_and_w32_imm(e, res, res, m);
            break;
        case OP_AND: emit_and_w32(e, res, a, b); break;
        case OP_OR:  emit_orr_w32(e, res, a, b); break;
        case OP_XOR: emit_eor_w32(e, res, a, b); break;
        default: break;   /* CMP / TEST: pure flag ops, nothing left */
        }
        return;
    }

    int table = T_ADD;
    int need_af = (fmask & X86_AF) != 0;
    /* AF needs the original operands after res is written; keep copies
     * of whichever ones res aliases. */
    a64_reg_t a_keep = a, b_keep = b;
    if (need_af && res == a) { emit_mov_w32_w32(e, W_T2, a); a_keep = W_T2; }
    if (need_af && res == b) { emit_mov_w32_w32(e, W_T3, b); b_keep = W_T3; }

    switch (op) {
    case OP_ADD:
        emit_lsl_w32_imm(e, W_T0, a, sh);
        emit_adds_w32_lsl(e, W_T0, W_T0, b, sh);
        break;
    case OP_SUB: case OP_CMP:
        emit_lsl_w32_imm(e, W_T0, a, sh);
        emit_subs_w32_lsl(e, W_T0, W_T0, b, sh);
        table = T_SUB;
        break;
    case OP_ADC: case OP_SBB:
        /* ADCS would add the carry at bit 0, below the shifted field.
         * Fold it into the operand instead: b' = b + CF, then a ± b'
         * with plain ADDS/SUBS. The one corner case, b = all-ones with
         * CF set, makes b' << sh vanish and loses the guest carry; the
         * result and OF are still right there (a ± 0), so just OR the
         * lost bit back into CF after the table. */
        (void)emit_and_w32_imm(e, A64_W2, R_F, X86_CF);    /* W2 = CF in */
        emit_add_w32(e, A64_W1, b, A64_W2);              /* W1 = b' (W_T1 is the NZCV temp) */
        emit_lsl_w32_imm(e, W_T0, a, sh);
        if (op == OP_ADC) emit_adds_w32_lsl(e, W_T0, W_T0, A64_W1, sh);
        else { emit_subs_w32_lsl(e, W_T0, W_T0, A64_W1, sh); table = T_SUB; }
        break;
    case OP_AND: case OP_TEST:
        emit_lsl_w32_imm(e, W_T0, a, sh);
        emit_ands_w32_lsl(e, W_T0, W_T0, b, sh);
        need_af = 0;
        break;
    case OP_OR:
        emit_lsl_w32_imm(e, W_T0, a, sh);
        emit_logical_shifted(e, 1, 0, W_T0, W_T0, b, 0, sh);
        emit_tst_w32(e, W_T0, W_T0);
        need_af = 0;
        break;
    case OP_XOR:
        emit_lsl_w32_imm(e, W_T0, a, sh);
        emit_eor_w32_lsl(e, W_T0, W_T0, b, sh);
        emit_tst_w32(e, W_T0, W_T0);
        need_af = 0;
        break;
    }
    emit_flags_from_nzcv(e, table);
    if (op == OP_ADC || op == OP_SBB) {
        emit_orr_w32_lsr(e, R_F, R_F, A64_W1, 8 * size);
        /* Second corner: b' = b + CF landing exactly on the sign bit
         * (b = 7F.., CF = 1) reads as -2^(n-1) in the field where the
         * guest added +2^(n-1); V comes out inverted, and only then. */
        (void)emit_eor_w32_imm(e, A64_W0, A64_W1, size == 1 ? 0x80 : 0x8000);
        (void)emit_eor_w32_imm(e, A64_W2, A64_W2, 1);
        emit_orr_w32(e, A64_W0, A64_W0, A64_W2);
        emit_cbnz_w32(e, A64_W0, 8);
        (void)emit_eor_w32_imm(e, R_F, R_F, X86_OF);
    }
    /* Result in low form: into res, or a temp for CMP/TEST when PF/AF want it. */
    a64_reg_t low = res;
    if (!wr_res) low = W_T0;
    if (wr_res || (fmask & (X86_PF | X86_AF)))
        emit_lsr_w32_imm(e, low, W_T0, sh);
    if (fmask & X86_PF) emit_flag_pf(e, low);
    if (need_af) emit_flag_af(e, a_keep, b_keep, low);
}

/* INC/DEC: like ADD/SUB 1 with CF preserved. */
static void emit_incdec(emit_t *e, int is_inc, int size, a64_reg_t a, a64_reg_t res, uint32_t fmask) {
    uint32_t sh = topshift(size), m = szmask(size);
    if (!fmask) {
        if (is_inc) emit_add_w32_imm(e, res, a, 1); else emit_sub_w32_imm(e, res, a, 1);
        (void)emit_and_w32_imm(e, res, res, m);
        return;
    }
    a64_reg_t a_keep = a;
    if ((fmask & X86_AF) && res == a) { emit_mov_w32_w32(e, W_T2, a); a_keep = W_T2; }
    (void)emit_and_w32_imm(e, W_T3, R_F, X86_CF);         /* old CF */
    emit_lsl_w32_imm(e, W_T0, a, sh);
    emit_movz_w32(e, W_T1, (uint16_t)(1u << (sh - 16)), 16);
    if (is_inc) emit_adds_w32(e, W_T0, W_T0, W_T1); else emit_subs_w32(e, W_T0, W_T0, W_T1);
    emit_flags_from_nzcv(e, is_inc ? T_INC : T_DEC);
    emit_orr_w32(e, R_F, R_F, W_T3);
    emit_lsr_w32_imm(e, res, W_T0, sh);
    if (fmask & X86_PF) emit_flag_pf(e, res);
    if (fmask & X86_AF) {
        emit_movz_w32(e, W_T1, 1, 0);
        emit_eor_w32(e, W_T1, W_T1, a_keep);
        emit_eor_w32(e, W_T1, W_T1, res);
        (void)emit_and_w32_imm(e, W_T1, W_T1, X86_AF);
        emit_orr_w32(e, R_F, R_F, W_T1);
    }
}

/* SHL/SHR/SAR by a static count 1..bits-1. CONTRACT (interp): AF
 * cleared; SHL OF = MSB(res)^CF; SHR OF = MSB(a) for count 1 else 0;
 * SAR OF = 0. */
static void emit_shift_imm(emit_t *e, int op, int size, uint32_t cnt, a64_reg_t a, a64_reg_t res, uint32_t fmask) {
    uint32_t sh = topshift(size), bits = size * 8, m = szmask(size);
    if (!fmask) {
        switch (op) {
        case OP_SHL: case OP_SAL: emit_lsl_w32_imm(e, res, a, cnt); (void)emit_and_w32_imm(e, res, res, m); break;
        case OP_SHR: emit_lsr_w32_imm(e, res, a, cnt); break;
        default:     emit_sbfx_w32(e, res, a, cnt, bits - cnt); (void)emit_and_w32_imm(e, res, res, m); break;
        }
        return;
    }
    if (op == OP_SHL || op == OP_SAL) {
        /* a << cnt is a << (cnt-1) doubled: the ADDS carry is the last
         * bit out and V is MSB(res) ^ CF — exactly x86's CF/OF. */
        emit_lsl_w32_imm(e, W_T0, a, sh + cnt - 1);
        emit_adds_w32(e, W_T0, W_T0, W_T0);
        emit_flags_from_nzcv(e, T_ADD);
        emit_lsr_w32_imm(e, res, W_T0, sh);
        if (fmask & X86_PF) emit_flag_pf(e, res);
        return;
    }
    a64_reg_t a_keep = a;
    if (res == a) { emit_mov_w32_w32(e, W_T2, a); a_keep = W_T2; }
    if (op == OP_SHR) emit_ubfx_w32(e, res, a, cnt, bits - cnt);
    else { emit_sbfx_w32(e, res, a, cnt, bits - cnt); (void)emit_and_w32_imm(e, res, res, m); }
    emit_addsub_shifted(e, 0, 0, 1, A64_WZR, A64_WZR, res, 0, sh);   /* NZ from res, C=V=0 */
    emit_flags_from_nzcv(e, T_ADD);
    emit_ubfx_w32(e, W_T1, a_keep, cnt - 1, 1);                       /* CF = bit cnt-1 of a */
    emit_orr_w32(e, R_F, R_F, W_T1);
    if (op == OP_SHR && cnt == 1) {                                   /* OF = MSB(a) */
        emit_lsr_w32_imm(e, W_T1, a_keep, bits - 1);
        emit_orr_w32_lsl(e, R_F, R_F, W_T1, 11);
    }
    if (fmask & X86_PF) emit_flag_pf(e, res);
}

/* ----------------------------------------------------------------------
 * Stack
 * ---------------------------------------------------------------------- */
/* Stack pushes skip the SMC check (the stack essentially never
 * overlaps code) but keep the 8086 wrap check: PUSH at SP=1 exists. */
/* A push is a store like any other: it can land on translated code (a
 * .COM has SS = CS, and an interrupt frame goes wherever SP points), so
 * it carries the same code-bitmap check. SP is re-derived from the
 * pinned register afterwards rather than carried in a temp — the SMC
 * helper call clobbers every scratch, and the wrap chunk rejoins here. */
static void emit_push16(emit_t *e, a64_reg_t val) {
    /* new SP in a temp until the store is known to succeed (fault: SP intact) */
    emit_sub_w32_imm(e, W_T2, R_GPR(R_SP), 2);
    (void)emit_and_w32_imm(e, W_T2, W_T2, 0xFFFF);
    emit_wrap_check(e, R_SSP, W_T2, val, 1, 1);
    emit_strh_reg_uxtw(e, val, R_SSP, W_T2);
    emit_add_x64_w32_uxtw(e, W_T3, R_SSP, W_T2);
    emit_smc_check_x3(e);
    emit_wrap_back(e);
    emit_sub_w32_imm(e, R_GPR(R_SP), R_GPR(R_SP), 2);
    (void)emit_and_w32_imm(e, R_GPR(R_SP), R_GPR(R_SP), 0xFFFF);
}
static void emit_pop16(emit_t *e, a64_reg_t dst) {
    emit_wrap_check(e, R_SSP, R_GPR(R_SP), dst, 0, 0);
    emit_ldrh_reg_uxtw(e, dst, R_SSP, R_GPR(R_SP));
    emit_wrap_back(e);
    emit_add_w32_imm(e, R_GPR(R_SP), R_GPR(R_SP), 2);
    (void)emit_and_w32_imm(e, R_GPR(R_SP), R_GPR(R_SP), 0xFFFF);
}

/* ----------------------------------------------------------------------
 * Classification
 * ---------------------------------------------------------------------- */
enum { C_REFUSE = 0, C_INLINE, C_HELPER };

static int is_shift_inline(const x86_insn *in) {
    if (in->op != OP_SHL && in->op != OP_SAL && in->op != OP_SHR && in->op != OP_SAR) return 0;
    if (in->ops[1].kind != OPK_IMM) return 0;
    uint32_t cnt = in->ops[1].imm & 0xFF;
    return cnt < (uint32_t)in->ops[0].size * 8;   /* 0 included: static no-op */
}

static int classify(const x86_insn *in) {
    if (in->opsize != 2 || in->adsize != 2) return C_REFUSE;   /* 386 forms: Phase B */
    switch (in->op) {
    case OP_ADD: case OP_OR: case OP_ADC: case OP_SBB: case OP_AND: case OP_SUB: case OP_XOR: case OP_CMP:
    case OP_TEST: case OP_INC: case OP_DEC: case OP_NOT: case OP_NEG:
    case OP_MOV: case OP_XCHG: case OP_LEA: case OP_NOP: case OP_CBW: case OP_CWD:
    case OP_CLC: case OP_STC: case OP_CMC: case OP_CLD: case OP_STD: case OP_CLI: case OP_STI:
    case OP_CALL: case OP_JMP: case OP_JCC: case OP_JCXZ: case OP_LOOP: case OP_LOOPE: case OP_LOOPNE:
    case OP_RET: case OP_CALLF: case OP_JMPF: case OP_RETF:
    case OP_INT: case OP_INT3:
        return C_INLINE;
    case OP_PUSH:
        return C_INLINE;
    case OP_POP:
        if (in->ops[0].kind != OPK_SREG) return C_INLINE;
        return in->ops[0].reg == S_CS ? C_REFUSE : C_HELPER;   /* POP CS: 8086 control transfer */
    case OP_SHL: case OP_SAL: case OP_SHR: case OP_SAR:
        return is_shift_inline(in) ? C_INLINE : C_HELPER;
    case OP_ROL: case OP_ROR: case OP_RCL: case OP_RCR: case OP_SETMO:
    case OP_MUL: case OP_IMUL: case OP_IMUL3:
    case OP_AAD: case OP_AAA: case OP_AAS: case OP_DAA: case OP_DAS: case OP_SALC:
    case OP_XLAT: case OP_SAHF: case OP_LAHF:
    case OP_PUSHA: case OP_POPA: case OP_PUSHF: case OP_POPF: case OP_ENTER: case OP_LEAVE:
    case OP_MOVS: case OP_CMPS: case OP_STOS: case OP_LODS: case OP_SCAS:
    case OP_LES: case OP_LDS:
    case OP_MOVZX: case OP_MOVSX: case OP_SETCC:
    case OP_BT: case OP_BTS: case OP_BTR: case OP_BTC: case OP_BSF: case OP_BSR:
    case OP_SHLD: case OP_SHRD: case OP_CMPXCHG: case OP_XADD: case OP_BSWAP:
        return C_HELPER;
    case OP_AAM:
        return in->ops[0].imm ? C_HELPER : C_REFUSE;
    case OP_MOVSEG:
        /* Loading CS (8086 only) is a control transfer in disguise. */
        if (in->ops[0].kind == OPK_SREG && in->ops[0].reg == S_CS) return C_REFUSE;
        return C_HELPER;
    default:
        return C_REFUSE;
    }
}

/* Exposed for tools/jittest's fuzzer: 0 refuse, 1 inline, 2 helper. */
int dbt_classify_op(const x86_insn *in) { return classify(in); }

/* Inline ops with a word-sized memory or stack access: the 286+ limit
 * check in front of it can raise #GP. Byte accesses cannot straddle. */
static int op_may_fault(const x86_insn *in) {
    switch (in->op) {
    case OP_PUSH: case OP_POP: case OP_CALL: case OP_RET: case OP_CALLF: case OP_RETF: case OP_JMPF:
        return 1;
    case OP_INT: case OP_INT3:
        return 1;                     /* the frame carries FLAGS: all of them must be materialized */
    default: break;
    }
    for (int i = 0; i < 2; i++)
        if (in->ops[i].kind == OPK_MEM && in->ops[i].size >= 2) return 1;
    return 0;
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
        if (in->ops[1].imm & 0xFF) *wr = ARITH;
        break;
    case OP_CLC: case OP_STC: *wr = X86_CF; break;
    case OP_CMC: *wr = X86_CF; *rd = X86_CF; break;
    case OP_JCC: case OP_JCXZ: case OP_LOOP: case OP_LOOPE: case OP_LOOPNE:
        *rd = ARITH; break;   /* side exit: every bit observable */
    default: break;
    }
}

/* ----------------------------------------------------------------------
 * Per-op emission (straight-line ops)
 * ---------------------------------------------------------------------- */
static void emit_helper_op(x86_dbt *dbt, emit_t *e, const x86_insn *in) {
    uint32_t idx = dbt->insn_used++;
    dbt->insn_pool[idx] = *in;
    dbt->helper_insns++;
    emit_mov_x64_x64(e, A64_W0, R_CPU);
    emit_mov_w32_imm32(e, A64_W1, idx);
    emit_thunk_args(e);
    emit_bl(e, (int32_t)s_exec_thunk_off - (int32_t)emit_pos(e));
}

static void emit_op(x86_dbt *dbt, emit_t *e, const x86_insn *in, int cls, uint32_t fmask) {
    if (cls == C_HELPER) { emit_helper_op(dbt, e, in); return; }

    ea_t ea = { 0, 0 };
    if (in->ea_valid) emit_ea(e, in, &ea);
    int size = in->ops[0].size;
    const x86_operand *d = &in->ops[0];

    switch (in->op) {
    case OP_ADD: case OP_OR: case OP_ADC: case OP_SBB: case OP_AND: case OP_SUB: case OP_XOR: case OP_CMP:
    case OP_TEST: {
        a64_reg_t a = emit_read_operand(e, in, 0, &ea, W_VAL);
        a64_reg_t b = emit_read_operand(e, in, 1, &ea, W_SRC);
        int wr = (in->op != OP_CMP && in->op != OP_TEST);
        /* Pinned 16-bit register destination: compute in place. */
        a64_reg_t res = (wr && d->kind == OPK_REG && d->size == 2) ? a : W_VAL;
        emit_alu(e, in->op, size, a, b, res, fmask);
        if (wr && res == W_VAL) emit_write_operand(e, in, 0, &ea, W_VAL);
        break;
    }
    case OP_INC: case OP_DEC: {
        a64_reg_t a = emit_read_operand(e, in, 0, &ea, W_VAL);
        a64_reg_t res = (d->kind == OPK_REG && d->size == 2) ? a : W_VAL;
        emit_incdec(e, in->op == OP_INC, size, a, res, fmask);
        if (res == W_VAL) emit_write_operand(e, in, 0, &ea, W_VAL);
        break;
    }
    case OP_NOT: {
        a64_reg_t a = emit_read_operand(e, in, 0, &ea, W_VAL);
        emit_mvn_w32(e, W_VAL, a);
        if (d->kind == OPK_REG && d->size == 2) (void)emit_and_w32_imm(e, W_VAL, W_VAL, 0xFFFF);
        emit_write_operand(e, in, 0, &ea, W_VAL);
        break;
    }
    case OP_NEG: {
        a64_reg_t a = emit_read_operand(e, in, 0, &ea, W_VAL);
        emit_mov_w32_w32(e, W_SRC, a);
        emit_movz_w32(e, W_VAL, 0, 0);
        emit_alu(e, OP_SUB, size, W_VAL, W_SRC, W_VAL, fmask);
        emit_write_operand(e, in, 0, &ea, W_VAL);
        break;
    }
    case OP_SHL: case OP_SAL: case OP_SHR: case OP_SAR: {
        uint32_t cnt = in->ops[1].imm & 0xFF;
        if (cnt == 0) break;
        a64_reg_t a = emit_read_operand(e, in, 0, &ea, W_VAL);
        a64_reg_t res = (d->kind == OPK_REG && d->size == 2) ? a : W_VAL;
        emit_shift_imm(e, in->op, size, cnt, a, res, fmask);
        if (res == W_VAL) emit_write_operand(e, in, 0, &ea, W_VAL);
        break;
    }
    case OP_MOV: {
        a64_reg_t v = emit_read_operand(e, in, 1, &ea, W_VAL);
        emit_write_operand(e, in, 0, &ea, v);
        break;
    }
    case OP_XCHG: {
        a64_reg_t a = emit_read_operand(e, in, 0, &ea, W_VAL);
        a64_reg_t b = emit_read_operand(e, in, 1, &ea, W_SRC);
        if (a != W_VAL) emit_mov_w32_w32(e, W_VAL, a);
        if (b != W_SRC) emit_mov_w32_w32(e, W_SRC, b);
        emit_write_operand(e, in, 0, &ea, W_SRC);
        emit_write_operand(e, in, 1, &ea, W_VAL);
        break;
    }
    case OP_LEA:
        emit_write_reg(e, d->reg, 2, ea.off);
        break;
    case OP_NOP:
        break;
    case OP_CBW:
        emit_sbfx_w32(e, W_T0, R_GPR(R_AX), 0, 8);
        emit_uxth_w32(e, R_GPR(R_AX), W_T0);
        break;
    case OP_CWD:
        emit_sbfx_w32(e, W_T0, R_GPR(R_AX), 15, 1);
        emit_uxth_w32(e, R_GPR(R_DX), W_T0);
        break;
    case OP_CLC: (void)emit_and_w32_imm(e, R_F, R_F, ~(uint32_t)X86_CF); break;
    case OP_STC: (void)emit_orr_w32_imm(e, R_F, R_F, X86_CF); break;
    case OP_CMC: (void)emit_eor_w32_imm(e, R_F, R_F, X86_CF); break;
    case OP_CLD: case OP_STD: case OP_CLI: case OP_STI: {
        uint32_t bit = (in->op == OP_CLD || in->op == OP_STD) ? X86_DF : X86_IF;
        emit_ldr_w32_imm(e, W_T0, R_CPU, OFF_EFLAGS);
        if (in->op == OP_CLD || in->op == OP_CLI) (void)emit_and_w32_imm(e, W_T0, W_T0, ~bit);
        else (void)emit_orr_w32_imm(e, W_T0, W_T0, bit);
        emit_str_w32_imm(e, W_T0, R_CPU, OFF_EFLAGS);
        break;
    }
    case OP_PUSH: {
        a64_reg_t v = emit_read_operand(e, in, 0, &ea, W_VAL);
        /* 8086 PUSH SP stores the decremented value; 286+ the old one. */
        if (d->kind == OPK_REG && d->reg == R_SP && dbt->cpu->model == X86_MODEL_8086) {
            emit_sub_w32_imm(e, W_VAL, R_GPR(R_SP), 2);
            (void)emit_and_w32_imm(e, W_VAL, W_VAL, 0xFFFF);
            v = W_VAL;
        }
        emit_push16(e, v);
        break;
    }
    case OP_POP:
        emit_pop16(e, W_VAL);
        emit_write_operand(e, in, 0, &ea, W_VAL);
        break;
    default:
        break;   /* classify() keeps everything else out */
    }
}

/* ----------------------------------------------------------------------
 * Control flow
 * ---------------------------------------------------------------------- */

/* Key of the static target ip within the block's CS. */
static uint64_t target_key(const x86_cpu *cpu, uint32_t ip) {
    uint32_t lin = (cpu->seg[S_CS].base + (ip & 0xFFFF)) & cpu->a20_mask;
    return dbt_key(cpu->seg[S_CS].sel, lin);
}

/* X0 = key for a run-time ip in W register `ip` (16-bit canonical):
 * lin = (cs.base + ip) & a20, CS selector in the top half. The A20 mask
 * only matters when cs.base + 0xFFFF can pass 1 MB with A20 off. */
static void emit_dynamic_key(emit_t *e, const x86_cpu *cpu, a64_reg_t ip) {
    uint32_t base = cpu->seg[S_CS].base;
    emit_mov_w32_imm32(e, A64_W0, base);
    emit_add_w32(e, A64_W0, A64_W0, ip);
    if (base + 0xFFFF > 0xFFFFF && cpu->a20_mask == 0xFFFFF)
        (void)emit_and_w32_imm(e, A64_W0, A64_W0, 0xFFFFF);
    emit_movk_x64(e, A64_W0, cpu->seg[S_CS].sel, 32);
}

/* Inline part of a conditional: guts + test + B.cond toward the taken
 * arm. Returns the B.cond offset to patch. */
static uint32_t emit_cond_side_branch(emit_t *e, const x86_insn *in) {
    uint32_t patch;
    switch (in->op) {
    case OP_JCC: {
        a64_cond_t c = emit_test_cond(e, in->cond);
        patch = emit_pos(e);
        emit_b_cond(e, c, 0);
        break;
    }
    case OP_JCXZ:
        patch = emit_pos(e);
        emit_cbz_w32(e, R_GPR(R_CX), 0);
        break;
    default: {   /* LOOP family: CX = (CX-1) & FFFF, taken when CX != 0 [&& ZF cond] */
        emit_sub_w32_imm(e, R_GPR(R_CX), R_GPR(R_CX), 1);
        (void)emit_and_w32_imm(e, R_GPR(R_CX), R_GPR(R_CX), 0xFFFF);
        if (in->op == OP_LOOP) {
            patch = emit_pos(e);
            emit_cbnz_w32(e, R_GPR(R_CX), 0);
        } else {
            emit_cbz_w32(e, R_GPR(R_CX), 12);                 /* skip the flag test + branch */
            (void)emit_tst_w32_imm(e, R_F, X86_ZF);
            patch = emit_pos(e);
            emit_b_cond(e, in->op == OP_LOOPE ? A64_COND_NE : A64_COND_EQ, 0);
        }
        break;
    }
    }
    return patch;
}

static void emit_cond_taken_tail(x86_dbt *dbt, emit_t *e, const x86_insn *in, uint32_t ip_after) {
    emit_edge(dbt, e, target_key(dbt->cpu, ip_after + in->ops[0].imm));
}

/* Far transfers (real mode: base = sel << 4). CS is stored to the cpu
 * before the tail — the exit stub derives EIP from it and the key
 * carries the selector. A static target (ptr16:16 immediate) is an
 * ordinary linkable edge; the others build the key from W_SRC (sel)
 * and W_VAL (offset) at run time, masking for A20 when it is off. */
static void emit_load_cs_dynamic(emit_t *e, const x86_cpu *cpu) {
    emit_strh_imm(e, W_SRC, R_CPU, OFF_SEG_SEL(S_CS));
    emit_lsl_w32_imm(e, W_T0, W_SRC, 4);
    emit_str_w32_imm(e, W_T0, R_CPU, OFF_SEG_BASE(S_CS));
    emit_add_w32(e, A64_W0, W_T0, W_VAL);
    if (cpu->a20_mask == 0xFFFFF) (void)emit_and_w32_imm(e, A64_W0, A64_W0, 0xFFFFF);
    emit_orr_x64_lsl(e, A64_W0, A64_W0, W_SRC, 32);
}
static void emit_far_ender(x86_dbt *dbt, emit_t *e, const x86_insn *in, uint32_t ip_after) {
    x86_cpu *cpu = dbt->cpu;
    if (in->op == OP_RETF) {
        /* Both slots are fetched before SP moves so a 286 limit fault on
         * either leaves SP intact, as the interpreter's frame precheck does. */
        emit_add_w32_imm(e, W_T0, R_GPR(R_SP), 2);
        (void)emit_and_w32_imm(e, W_T0, W_T0, 0xFFFF);
        emit_wrap_check(e, R_SSP, W_T0, W_SRC, 0, 0);
        emit_ldrh_reg_uxtw(e, W_SRC, R_SSP, W_T0);
        emit_wrap_back(e);
        emit_wrap_check(e, R_SSP, R_GPR(R_SP), W_VAL, 0, 0);
        emit_ldrh_reg_uxtw(e, W_VAL, R_SSP, R_GPR(R_SP));
        emit_wrap_back(e);
        int32_t adj = 4 + (in->ops[0].kind == OPK_IMM ? (int32_t)(in->ops[0].imm & 0xFFFF) : 0);
        emit_add_w32_imm_any(e, R_GPR(R_SP), R_GPR(R_SP), adj, W_T1);
        (void)emit_and_w32_imm(e, R_GPR(R_SP), R_GPR(R_SP), 0xFFFF);
        emit_load_cs_dynamic(e, cpu);
        emit_dynamic_tail(e, dbt->exit_stub_off);
        return;
    }
    int is_imm = in->ops[0].kind == OPK_IMM;
    if (!is_imm) {
        /* ptr16:16 in memory: offset then selector, each wrap-checked on its own */
        ea_t ea = { 0, 0 };
        emit_ea(e, in, &ea);
        emit_read_mem(e, &ea, 2, W_VAL);
        emit_add_w32_imm(e, W_T0, ea.off, 2);
        (void)emit_and_w32_imm(e, W_T0, W_T0, 0xFFFF);
        ea_t ea2 = { ea.segp, W_T0 };
        emit_read_mem(e, &ea2, 2, W_SRC);
    }
    if (in->op == OP_CALLF) {
        emit_movz_w32(e, W_T0, cpu->seg[S_CS].sel, 0);
        emit_push16(e, W_T0);
        emit_movz_w32(e, W_T0, (uint16_t)ip_after, 0);
        emit_push16(e, W_T0);
    }
    if (is_imm) {
        uint16_t sel = (uint16_t)in->imm2, off = (uint16_t)in->ops[0].imm;
        emit_movz_w32(e, W_T0, sel, 0);
        emit_strh_imm(e, W_T0, R_CPU, OFF_SEG_SEL(S_CS));
        emit_mov_w32_imm32(e, W_T0, (uint32_t)sel << 4);
        emit_str_w32_imm(e, W_T0, R_CPU, OFF_SEG_BASE(S_CS));
        uint32_t lin = (((uint32_t)sel << 4) + off) & cpu->a20_mask;
        emit_edge(dbt, e, dbt_key(sel, lin));
    } else {
        emit_load_cs_dynamic(e, cpu);
        emit_dynamic_tail(e, dbt->exit_stub_off);
    }
}

/* Interrupt-frame push: unchecked, like the microcode's (x86_interrupt
 * uses push_raw, so a 286 takes no #GP here and writes linearly). The
 * 8086/186 still wrap in-segment, which the wrap chunk handles. */
static void emit_push16_raw(emit_t *e, a64_reg_t val) {
    emit_sub_w32_imm(e, W_T2, R_GPR(R_SP), 2);
    (void)emit_and_w32_imm(e, W_T2, W_T2, 0xFFFF);
    if (s_wrap_exact) emit_wrap_check(e, R_SSP, W_T2, val, 1, 1);
    emit_strh_reg_uxtw(e, val, R_SSP, W_T2);
    emit_add_x64_w32_uxtw(e, W_T3, R_SSP, W_T2);
    emit_smc_check_x3(e);
    if (s_wrap_exact) emit_wrap_back(e);
    emit_sub_w32_imm(e, R_GPR(R_SP), R_GPR(R_SP), 2);
    (void)emit_and_w32_imm(e, R_GPR(R_SP), R_GPR(R_SP), 0xFFFF);
}

/* INT n / INT3. The IVT entry is read before the frame is pushed
 * (measured), the pushed FLAGS are the pre-clear value, and IF/TF are
 * cleared in cpu->eflags — R_F holds only the arithmetic bits, so the
 * exit stub's spill leaves our store intact. Landing on the HLE stub
 * segment is a refused key, so the run loop steps the service. */
static void emit_int_ender(x86_dbt *dbt, emit_t *e, const x86_insn *in, uint32_t ip_after) {
    x86_cpu *cpu = dbt->cpu;
    uint32_t vec = in->op == OP_INT3 ? 3 : (in->ops[0].imm & 0xFF);
    emit_ldrh_imm(e, W_VAL, R_MEM, vec * 4);          /* handler offset */
    emit_ldrh_imm(e, W_SRC, R_MEM, vec * 4 + 2);      /* handler selector */

    emit_ldr_w32_imm(e, W_T0, R_CPU, OFF_EFLAGS);
    emit_movz_w32(e, W_T1, ARITH, 0);
    emit_bic_w32(e, W_T0, W_T0, W_T1);
    emit_orr_w32(e, W_T0, W_T0, R_F);                 /* FLAGS as the guest sees it */
    emit_push16_raw(e, W_T0);
    emit_ldr_w32_imm(e, W_T0, R_CPU, OFF_EFLAGS);     /* re-read: a firing SMC check clobbers the scratch */
    emit_movz_w32(e, W_T1, X86_IF | X86_TF, 0);
    emit_bic_w32(e, W_T0, W_T0, W_T1);
    emit_str_w32_imm(e, W_T0, R_CPU, OFF_EFLAGS);
    emit_strb_imm(e, A64_WZR, R_CPU, OFF_INT_INHIBIT);   /* uint8_t: a word store would reach into cpu->exc */

    emit_movz_w32(e, W_T0, cpu->seg[S_CS].sel, 0);
    emit_push16_raw(e, W_T0);
    emit_movz_w32(e, W_T0, (uint16_t)ip_after, 0);
    emit_push16_raw(e, W_T0);

    emit_load_cs_dynamic(e, cpu);
    emit_dynamic_tail(e, dbt->exit_stub_off);
}

static void emit_branch_ender(x86_dbt *dbt, emit_t *e, const x86_insn *in, uint32_t ip_after) {
    x86_cpu *cpu = dbt->cpu;
    switch (in->op) {
    case OP_INT: case OP_INT3:
        emit_int_ender(dbt, e, in, ip_after);
        return;
    case OP_CALLF: case OP_JMPF: case OP_RETF:
        emit_far_ender(dbt, e, in, ip_after);
        return;
    case OP_JMP:
        if (in->ops[0].kind == OPK_IMM) {
            emit_edge(dbt, e, target_key(cpu, ip_after + in->ops[0].imm));
        } else {
            ea_t ea = { 0, 0 };
            if (in->ea_valid) emit_ea(e, in, &ea);
            a64_reg_t t = emit_read_operand(e, in, 0, &ea, W_VAL);
            emit_dynamic_key(e, cpu, t);
            emit_dynamic_tail(e, dbt->exit_stub_off);
        }
        return;
    case OP_CALL: {
        ea_t ea = { 0, 0 };
        a64_reg_t t = 0;
        int dyn = in->ops[0].kind != OPK_IMM;
        if (dyn) {
            if (in->ea_valid) emit_ea(e, in, &ea);
            t = emit_read_operand(e, in, 0, &ea, W_VAL);
            if (t != W_VAL) { emit_mov_w32_w32(e, W_VAL, t); t = W_VAL; }
        }
        emit_movz_w32(e, W_SRC, (uint16_t)ip_after, 0);
        emit_push16(e, W_SRC);
        if (dyn) {
            emit_dynamic_key(e, cpu, t);
            emit_dynamic_tail(e, dbt->exit_stub_off);
        } else {
            emit_edge(dbt, e, target_key(cpu, ip_after + in->ops[0].imm));
        }
        return;
    }
    case OP_RET:
        emit_pop16(e, W_VAL);
        if (in->ops[0].kind == OPK_IMM) {
            emit_add_w32_imm_any(e, R_GPR(R_SP), R_GPR(R_SP), (int32_t)(in->ops[0].imm & 0xFFFF), W_T1);
            (void)emit_and_w32_imm(e, R_GPR(R_SP), R_GPR(R_SP), 0xFFFF);
        }
        emit_dynamic_key(e, cpu, W_VAL);
        emit_dynamic_tail(e, dbt->exit_stub_off);
        return;
    default: {   /* conditional in final position: two-edge ender */
        uint32_t patch = emit_cond_side_branch(e, in);
        emit_edge(dbt, e, target_key(cpu, ip_after));      /* not taken */
        emit_patch_cond19(e, patch, emit_pos(e));
        emit_cond_taken_tail(dbt, e, in, ip_after);
        return;
    }
    }
}

/* ----------------------------------------------------------------------
 * Block translation
 * ---------------------------------------------------------------------- */
static void fetch_at(const x86_cpu *c, uint32_t ip, uint8_t *buf) {
    uint32_t base = c->seg[S_CS].base;
    for (int i = 0; i < 16; i++) buf[i] = x86_phys_rd8((x86_cpu *)c, base + ((ip + i) & 0xFFFF));
}

uint8_t *dbt_translate_block(x86_dbt *dbt, uint64_t key) {
    x86_cpu *cpu = dbt->cpu;
    if (s_strict_exit < 0)
        s_strict_exit = dbt->verify && getenv("X86_VERIFY_STRICT") != NULL;
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
    if (cpu->hle && cpu->seg[S_CS].sel == cpu->hle_seg) return NULL;

    emit_t e = { .buf = dbt->code_buf, .offset = dbt->code_used, .capacity = CODE_BUF_SIZE };
    uint8_t *entry = dbt->code_buf + e.offset;

    /* ---- Phase 1: decode ---- */
    x86_insn decs[MAX_BLOCK_INSNS];
    uint32_t ip_afters[MAX_BLOCK_INSNS];
    uint8_t  cls[MAX_BLOCK_INSNS];
    uint32_t ip = cpu->eip & 0xFFFF;
    uint32_t start_ip = ip;
    uint32_t n_ops = 0;
    uint8_t buf[16];
    x86_dec_ctx ctx = { buf, cpu->model, 0 };

    while (n_ops < MAX_BLOCK_INSNS) {
        x86_insn *in = &decs[n_ops];
        fetch_at(cpu, ip, buf);
        if (!x86_decode(&ctx, in)) break;
        if (ip + in->len > 0x10000) break;             /* IP wrap: leave it to the interp */
        int c = classify(in);
        if (cpu->model == X86_MODEL_286 && in->len > 10) c = C_REFUSE;   /* #GP: the interpreter's */
        if (c == C_REFUSE) {
            dbt->refused_by_op[in->op]++;
            break;
        }
        cls[n_ops] = (uint8_t)c;
        ip_afters[n_ops] = ip + in->len;
        n_ops++;
        ip += in->len;
        if (is_uncond_ender(in->op)) break;
        if (is_cond_ender(in->op) && ip - start_ip >= SUPERBLOCK_BYTE_CAP) break;
    }
    if (n_ops == 0) return NULL;

    /* ---- Phase 2: backward flag liveness. fmask[i] = bits live after
     * op i (LIVE-OUT, not live∩write). Block exits observe everything. */
    uint32_t fmask[MAX_BLOCK_INSNS];
    {
        uint32_t live = ARITH;
        for (int i = (int)n_ops - 1; i >= 0; i--) {
            uint32_t rd, wr;
            op_flag_effects(&decs[i], cls[i], &rd, &wr);
            /* 286+: a limit fault is an unplanned exit whose frame holds
             * the flags, so an op that can fault observes all of them. */
            if (cpu->model >= X86_MODEL_286 && op_may_fault(&decs[i])) rd |= ARITH;
            fmask[i] = live;
            live = (live & ~wr) | rd;
        }
    }

    /* ---- Phase 3: emit ----
     * Entry: budget check. Exhausted → out-of-line exit with our own key. */
    s_cur_lin = dbt_key_lin(key);
    s_wrap_exact = cpu->model < X86_MODEL_286;
    s_nwrap = 0;
    uint32_t budget_patch = emit_pos(&e);
    emit_tbnz_x64(&e, R_CNT, 63, 0);

    struct { uint32_t patch_off; uint32_t insns; uint32_t op; } sides[MAX_BLOCK_INSNS];
    uint32_t n_sides = 0;
    int final_by_branch = 0;

    for (uint32_t i = 0; i < n_ops; i++) {
        const x86_insn *in = &decs[i];
        s_cur_ip_after = ip_afters[i];
        s_cur_ip_start = ip_afters[i] - in->len;
        s_cur_n_done = i + 1;
        if (is_uncond_ender(in->op) || (is_cond_ender(in->op) && i == n_ops - 1)) {
            emit_tail_prologue(&e, n_ops);
            emit_branch_ender(dbt, &e, in, ip_afters[i]);
            final_by_branch = 1;
            break;
        }
        if (is_cond_ender(in->op)) {
            sides[n_sides].patch_off = emit_cond_side_branch(&e, in);
            sides[n_sides].insns = i + 1;
            sides[n_sides].op = i;
            n_sides++;
            continue;
        }
        emit_op(dbt, &e, in, cls[i], fmask[i]);
    }

    if (!final_by_branch) {
        emit_tail_prologue(&e, n_ops);
        emit_edge(dbt, &e, target_key(cpu, ip));
    }

    /* Side-exit chunks: taken arms of mid-block conditionals, each with
     * its exact instruction count. */
    for (uint32_t k = 0; k < n_sides; k++) {
        emit_patch_cond19(&e, sides[k].patch_off, emit_pos(&e));
        emit_tail_prologue(&e, sides[k].insns);
        emit_cond_taken_tail(dbt, &e, &decs[sides[k].op], ip_afters[sides[k].op]);
    }

    /* Segment-wrap slow paths for the word accesses above. */
    emit_wrap_slow_chunks(&e);

    /* Budget-exhausted exit: nothing executed, next = this block. */
    emit_patch_tb14(&e, budget_patch, emit_pos(&e));
    emit_mov_x64_imm64(&e, A64_W0, key);
    emit_b(&e, (int32_t)dbt->exit_stub_off - (int32_t)emit_pos(&e));

    dbt->code_used = e.offset;
    __builtin___clear_cache((char *)entry, (char *)(dbt->code_buf + e.offset));

    uint32_t lin = dbt_key_lin(key);
    dbt_mark_block_bytes(dbt, lin, lin + (ip - start_ip));
    return entry;
}
