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
#define OFF_SEG_LIMIT(i) (offsetof(x86_cpu, seg) + sizeof(x86_seg) * (i) + offsetof(x86_seg, limit))
#define OFF_SEG_BIG(i)  (offsetof(x86_cpu, seg) + sizeof(x86_seg) * (i) + offsetof(x86_seg, big))
_Static_assert(offsetof(x86_seg, attr) == offsetof(x86_seg, sel) + 2, "sel and attr share one word store");
#define OFF_SEG_USABLE(i) (offsetof(x86_cpu, seg) + sizeof(x86_seg) * (i) + offsetof(x86_seg, usable))
#define OFF_EXC_ERR     offsetof(x86_cpu, exc_err)
#define OFF_INSN_COUNT  offsetof(x86_cpu, insn_count)
#define OFF_CODE_BITMAP offsetof(x86_cpu, code_bitmap)
#define OFF_JIT_AUX     offsetof(x86_cpu, jit_aux)
#define OFF_JIT_BUDGET  offsetof(x86_cpu, jit_budget)
#define OFF_JIT_CNTSAVE offsetof(x86_cpu, jit_cnt_save)
#define OFF_JIT_CUR_LIN offsetof(x86_cpu, jit_cur_lin)
#define OFF_JIT_CUR_HIT offsetof(x86_cpu, jit_cur_hit)
#define OFF_EXC         offsetof(x86_cpu, exc)
#define OFF_INT_INHIBIT offsetof(x86_cpu, int_inhibit)
#define OFF_DEV_WPLANE  offsetof(x86_cpu, dev_wplane)
#define OFF_DEV_RPLANE  offsetof(x86_cpu, dev_rplane)

#define ARITH  X86_ARITH_FLAGS   /* 0x8D5 — not a logical immediate, load it */

/* Thunk/stub offsets in the code buffer (emitted with the trampoline). */
static uint32_t s_exec_thunk_off, s_smc_thunk_off, s_port_thunk_off, s_fault_stub_off, s_fault_exit_off, s_exit_eip_off;

/* model >= 386: the pinned registers hold the whole 32-bit guest register,
 * so a 16-bit write merges into bits 15:0 and anything that consumes a
 * 16-bit register as an address, a count or a branch target has to mask
 * it first. Below 386 they are canonical 16-bit and none of that applies. */
static int s_regs32;
/* cpu model of the block being classified (real-mode DIV is inline from the 286 on) */
static int s_cls_model = X86_MODEL_286;

static a64_reg_t emit_reg16(emit_t *e, int reg, a64_reg_t tmp);
static void emit_set16(emit_t *e, int reg, a64_reg_t src);
static void emit_push16(emit_t *e, a64_reg_t val);
static void emit_pop16(emit_t *e, a64_reg_t dst);

static inline uint32_t szmask(int size) { return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFu; }
static inline uint32_t topshift(int size) { return size == 1 ? 24 : size == 2 ? 16 : 0; }
/* NOTE: a 32-bit "mask" is emit_and_w32_imm(.., 0xFFFFFFFF), which is not
 * an encodable logical immediate: it emits nothing, which is exactly
 * right. Shifts by 0 are plain moves. So the emitters below take size 4
 * as they stand. */

/* Flat protected mode (a KEY_FLAT block, see dbt_seg_flat): 32-bit
 * effective addresses straight off R_MEM, keys built with s_mode_bits. */
static int s_flat;
static uint64_t s_mode_bits;
/* Virtual-8086 blocks (KEY_V86) are real-mode blocks at CPL 3: what V86
 * does differently goes to the interpreter (classify_v86), and under
 * paging (KEY_PAGED) every memory access goes through cpu->pgd_r/pgd_w
 * (emit_paged). s_iopl3: CLI/STI/PUSHF behave as in real mode. */
static int s_v86, s_paged, s_iopl3;
static int s_pg_user;       /* a paged flat block at CPL 3: user permissions in the TLB check */
static int s_esnull;        /* KEY_ESNULL: ES is null, its accesses are the interpreter's */
static int s_dsnull;        /* KEY_DSNULL: the same for DS */
/* Segmented 16-bit protected mode (a KEY_SEG16 block, see dbt_seg16_ok):
 * the real-mode shape with a limit check on every access. */
static int s_seg16;

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

    /* ---- Exit stub: X0 = next key. EIP = linear - CS.base, which in
     * real mode wraps to 16 bits (A20-folded HMA code) and in protected
     * mode is exact (keys are only made there with A20 on). ---- */
    dbt->exit_stub_off = e.offset;
    emit_ldr_w32_imm(&e, W_T0, R_CPU, OFF_SEG_BASE(S_CS));
    emit_sub_w32(&e, W_T0, A64_W0, W_T0);
    uint32_t pm_eip = emit_pos(&e);
    emit_tbnz_x64(&e, A64_W0, 48, 0);                 /* KEY_PMODE */
    (void)emit_and_w32_imm(&e, W_T0, W_T0, 0xFFFF);
    emit_patch_tb14(&e, pm_eip, emit_pos(&e));
    emit_str_w32_imm(&e, W_T0, R_CPU, OFF_EIP);
    s_exit_eip_off = e.offset;                        /* enter here with cpu->eip already stored */
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
    emit_str_w32_imm(&e, A64_WZR, R_CPU, OFF_EXC_ERR);   /* #GP/#SS(0) in protected mode */
    s_fault_exit_off = e.offset;
    emit_sub_x64(&e, R_CNT, R_CNT, A64_W4);
    emit_str_w32_imm(&e, A64_W3, R_CPU, OFF_EIP);
    emit_b(&e, (int32_t)s_exit_eip_off - (int32_t)emit_pos(&e));

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

        emit_stp_pre_sp(&e, A64_W29, A64_W30, -144);
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
            /* The scratch too (W_T1, W_T0, W_SRC, W_VAL, W_OFF, X_SEGP):
             * a checked store is not always a block's last use of them.
             * INT n and CALL FAR [mem] hold the target CS:IP in W_SRC/W_VAL
             * across the frame's pushes, and a push onto memory that once
             * held translated code — a DOS stack reusing a freed program's
             * space — fired this thunk and sent the far jump into garbage
             * (FreeDOS's installer, XCOPY after SLICEREX). */
            emit_stp_x64_off(&e, A64_W5, A64_W6, A64_SP, 96);
            emit_stp_x64_off(&e, A64_W7, A64_W8, A64_SP, 112);
            emit_stp_x64_off(&e, A64_W9, A64_W10, A64_SP, 128);
            emit_ldr_x64_imm(&e, A64_W9, R_AUX, AUX_HELPERS + 8 * H_POST_STORE);
            emit_blr(&e, A64_W9);
            emit_ldp_x64_off(&e, A64_W9, A64_W10, A64_SP, 128);
            emit_ldp_x64_off(&e, A64_W7, A64_W8, A64_SP, 112);
            emit_ldp_x64_off(&e, A64_W5, A64_W6, A64_SP, 96);
            emit_ldr_x64_imm(&e, A64_W17, A64_SP, 80);
            emit_ldp_x64_off(&e, A64_W15, A64_W16, A64_SP, 64);
            emit_ldp_x64_off(&e, A64_W13, A64_W14, A64_SP, 48);
            emit_ldp_x64_off(&e, A64_W11, A64_W12, A64_SP, 32);
        }
        emit_ldp_x64_off(&e, A64_W3, A64_W4, A64_SP, 16);
        emit_ldp_post_sp(&e, A64_W29, A64_W30, 144);
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
        /* Running block invalidated: charge W4 instructions and exit at
         * ip_after — or, after a helper, wherever the interpreter left
         * EIP (a protected-mode helper can be a near jump). */
        emit_patch_cond19(&e, cont, emit_pos(&e));
        emit_sub_x64(&e, R_CNT, R_CNT, A64_W4);
        if (!is_exec) emit_str_w32_imm(&e, A64_W3, R_CPU, OFF_EIP);
        emit_b(&e, (int32_t)s_exit_eip_off - (int32_t)emit_pos(&e));
    }

    /* ---- Port thunk: BL with X0 = cpu, W1 = port | size << 16, W2 =
     * value, W3/W4 as above. Ports touch no guest register, so only the
     * caller-saved pinned registers are parked; W0 comes back with the
     * helper's verdict and the block acts on it (emit_out). ---- */
    s_port_thunk_off = e.offset;
    emit_stp_pre_sp(&e, A64_W29, A64_W30, -96);
    emit_stp_x64_off(&e, A64_W3, A64_W4, A64_SP, 16);
    emit_stp_x64_off(&e, A64_W11, A64_W12, A64_SP, 32);
    emit_stp_x64_off(&e, A64_W13, A64_W14, A64_SP, 48);
    emit_stp_x64_off(&e, A64_W15, A64_W16, A64_SP, 64);
    emit_str_x64_imm(&e, A64_W17, A64_SP, 80);
    emit_ldr_x64_imm(&e, A64_W9, R_AUX, AUX_HELPERS + 8 * H_OUT);
    emit_blr(&e, A64_W9);
    emit_ldr_x64_imm(&e, A64_W17, A64_SP, 80);
    emit_ldp_x64_off(&e, A64_W15, A64_W16, A64_SP, 64);
    emit_ldp_x64_off(&e, A64_W13, A64_W14, A64_SP, 48);
    emit_ldp_x64_off(&e, A64_W11, A64_W12, A64_SP, 32);
    emit_ldp_x64_off(&e, A64_W3, A64_W4, A64_SP, 16);
    emit_ldp_post_sp(&e, A64_W29, A64_W30, 96);
    emit_ret(&e);

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
 *   X3 = aux + AUX_CACHE + slot(lin)*16 ; LDP key,code ; CMP ; BR or exit. */
static void emit_dynamic_tail(emit_t *e, uint32_t exit_stub_off) {
    if (s_strict_exit > 0) {
        emit_b(e, (int32_t)exit_stub_off - (int32_t)emit_pos(e));
        return;
    }
    emit_lsr_x64_imm(e, W_T2, A64_W0, KEY_MODE_SHIFT);          /* slot = (lin ^ mode << 16) & mask, as dbt_slot */
    emit_eor_w32_lsl(e, W_T2, A64_W0, W_T2, 16);
    (void)emit_and_w32_imm(e, W_T2, W_T2, BLOCK_CACHE_MASK);
    emit_add_x64_imm_lsl12(e, W_T3, R_AUX, AUX_CACHE >> 12);
    emit_add_x64_w32_uxtw_lsl(e, W_T3, W_T3, W_T2, 4);
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
    uint32_t site = e->offset;
    int linked = 0;
    if (dbt_link_record(dbt, key, site)) {
        x86_block_entry *be = &dbt->aux->cache[dbt_slot(key)];
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
    int       seg;    /* segment index, for the limit check of a KEY_SEG16 block */
} ea_t;

static a64_reg_t seg_ptr_reg(int s) {
    switch (s) {
    case S_DS: return R_DSP;
    case S_SS: return R_SSP;
    case S_ES: return R_ESP;
    default:   return X_SEGP;
    }
}

static void emit_flat_check(emit_t *e, a64_reg_t off);
static void emit_flat_check_as(emit_t *e, a64_reg_t off, int store_only);
static void emit_ea_paged(emit_t *e, const x86_insn *in, ea_t *ea);
static void emit_pgflat(emit_t *e, a64_reg_t off, int size, int write);
static int writes_mem_operand(const x86_insn *in);
static int flat_access_size(const x86_insn *in);

/* Flat EA: base + index << scale + disp, wrapping at 4 GB like the
 * CPU's 32-bit address arithmetic, then the range check (not for LEA,
 * which touches no memory). */
static void emit_ea_flat(emit_t *e, const x86_insn *in, ea_t *ea) {
    ea->segp = R_MEM;
    int32_t disp = in->disp;
    if (in->base < 0 && in->index < 0) {
        emit_mov_w32_imm32(e, W_OFF, (uint32_t)disp);
        ea->off = W_OFF;
    } else {
        a64_reg_t acc;
        if (in->base >= 0 && in->index >= 0) {
            emit_add_w32_lsl(e, W_OFF, R_GPR(in->base), R_GPR(in->index), in->scale);
            acc = W_OFF;
        } else if (in->index >= 0) {
            acc = R_GPR(in->index);
            if (in->scale) { emit_lsl_w32_imm(e, W_OFF, acc, in->scale); acc = W_OFF; }
        } else {
            acc = R_GPR(in->base);
        }
        if (disp) { emit_add_w32_imm_any(e, W_OFF, acc, disp, W_T1); acc = W_OFF; }
        ea->off = acc;
    }
    if (in->op == OP_LEA) return;
    if (s_paged) {
        emit_pgflat(e, ea->off, flat_access_size(in), writes_mem_operand(in));
        ea->segp = X_SEGP;
        return;
    }
    emit_flat_check_as(e, ea->off, in->op == OP_MOV && in->ops[0].kind == OPK_MEM);
}

/* Compute the effective address. Uses W_OFF (and X_SEGP for CS/FS/GS);
 * a bare [reg] form returns the pinned register itself. */
static void emit_ea_real(emit_t *e, const x86_insn *in, ea_t *ea);
static void emit_ea(emit_t *e, const x86_insn *in, ea_t *ea) {
    ea->seg = in->seg;
    if (s_flat) { emit_ea_flat(e, in, ea); return; }
    emit_ea_real(e, in, ea);
    if (s_paged && in->op != OP_LEA) emit_ea_paged(e, in, ea);
}
static void emit_ea_real(emit_t *e, const x86_insn *in, ea_t *ea) {
    ea->segp = seg_ptr_reg(in->seg);
    if (ea->segp == X_SEGP) emit_load_seg_ptr(e, X_SEGP, in->seg);

    int32_t disp = (int16_t)in->disp;
    if (in->base < 0 && in->index < 0) {
        emit_movz_w32(e, W_OFF, (uint16_t)disp, 0);
        ea->off = W_OFF;
        return;
    }
    if (in->base >= 0 && in->index < 0 && disp == 0) {
        ea->off = emit_reg16(e, in->base, W_OFF);
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
    if (size == 4) return R_GPR(reg);
    /* NOTE: on 386 this returns the full 32-bit register for a 16-bit
     * operand. Every consumer either shifts it to the top of the word
     * (the ALU and shift paths), stores only its low half (STRH, BFI),
     * or masks explicitly (emit_ea, emit_dynamic_key). A new consumer
     * that reads it as a bare 16-bit value must mask. */
    if (size == 2) return R_GPR(reg);
    if (reg < 4) (void)emit_and_w32_imm(e, tmp, R_GPR(reg), 0xFF);
    else emit_ubfx_w32(e, tmp, R_GPR(reg & 3), 8, 8);
    return tmp;
}

/* Write `src` into guest register (low bits of src are the value). */
static void emit_write_reg(emit_t *e, int reg, int size, a64_reg_t src) {
    if (size == 4) {
        if (src != R_GPR(reg)) emit_mov_w32_w32(e, R_GPR(reg), src);
        return;
    }
    if (size == 2) {
        if (s_regs32) emit_bfi_w32(e, R_GPR(reg), src, 0, 16);
        else if (src != R_GPR(reg)) emit_mov_w32_w32(e, R_GPR(reg), src);
        return;
    }
    emit_bfi_w32(e, R_GPR(reg & 3), src, reg < 4 ? 0 : 8, 8);
}

/* A pinned register used as a 16-bit address/count/target. */
static a64_reg_t emit_reg16(emit_t *e, int reg, a64_reg_t tmp) {
    if (!s_regs32) return R_GPR(reg);
    emit_uxth_w32(e, tmp, R_GPR(reg));
    return tmp;
}
/* Merge a 16-bit value back into a pinned register. */
static void emit_set16(emit_t *e, int reg, a64_reg_t src) {
    if (s_regs32) emit_bfi_w32(e, R_GPR(reg), src, 0, 16);
    else if (src != R_GPR(reg)) emit_mov_w32_w32(e, R_GPR(reg), src);
}

/* Where in the block we are, for the thunks' "block invalidated under
 * us" exit: set per instruction by the block emitter. */
static uint32_t s_cur_lin, s_cur_ip_after, s_cur_ip_start, s_cur_n_done;
static int s_wrap_exact;   /* model < 286: word accesses at offset FFFF wrap in-segment; 286+: #GP */

static void emit_thunk_args(emit_t *e) {
    emit_mov_w32_imm32(e, A64_W2, s_cur_lin);
    emit_mov_w32_imm32(e, A64_W3, s_cur_ip_after);
    emit_movz_w32(e, A64_W4, (uint16_t)s_cur_n_done, 0);
}

/* ---- Flat-mode memory ----
 * A flat block addresses guest memory as R_MEM + offset. The fast path is
 * everything below 16 MB — low memory, the HMA, all but the top megabyte
 * of extended memory — less, for reads, the VGA window A0000-AFFFF, whose
 * reads the device answers (device_read, the interpreter's). A store there
 * is fine: it lands and the bitmap check hands the byte to the device, as
 * the interpreter's store does. Anything else branches to an out-of-line
 * chunk that runs the whole instruction through the exec thunk and rejoins
 * after it. Every check precedes the instruction's first state change, so
 * the helper starts clean; and no access through a flat segment can
 * fault, so neither path raises. */
#define FLAT_TOP 0x1000000u
_Static_assert(FLAT_TOP + 4 <= X86_MEM_SIZE, "flat fast path must stay inside guest memory");
typedef struct {
    uint32_t patch_off, back_off;
    uint32_t ip_after, n_done;
    int      ender;      /* the instruction ends the block: after the helper, leave at cpu->eip */
    x86_insn in;
} flat_slow_t;
#define FLAT_SLOW_MAX 256
static flat_slow_t s_fslow[FLAT_SLOW_MAX];
static uint32_t s_nfslow;
static const x86_insn *s_cur_insn;   /* the instruction being emitted, for its slow path */
static uint32_t s_dyn_imm_lin;       /* nonzero: read the current instruction's immediate from this linear address */
static int s_cur_ender;
static int s_in_slow_chunk;          /* emitting a flat slow-path chunk (stats tag) */

/* Record a slow-path entry for the current instruction; the caller emits
 * the branch to it (a B.cond or CBZ/CBNZ, all imm19) right after. */
static void flat_slow_site(emit_t *e) {
    if (s_nfslow >= FLAT_SLOW_MAX) { fprintf(stderr, "dbt: flat slow-path table overflow\n"); abort(); }
    flat_slow_t *f = &s_fslow[s_nfslow++];
    f->patch_off = emit_pos(e);
    f->back_off = 0;
    f->ip_after = s_cur_ip_after;
    f->n_done = s_cur_n_done;
    f->ender = s_cur_ender;
    f->in = *s_cur_insn;
}

/* store_only: the instruction writes this address and never reads it,
 * so the VGA window need not be avoided. */
static void emit_flat_check_as(emit_t *e, a64_reg_t off, int store_only) {
    a64_cond_t slow = A64_COND_NE;
    if (store_only) {
        (void)emit_tst_w32_imm(e, off, ~(FLAT_TOP - 1));              /* off >= 16 MB */
    } else {
        /* slow if off >= 16 MB, or bits 23:16 are 0xA (the VGA window):
         * below 16 MB the CCMP compares those bits with 0xA, above it
         * forces Z — either hazard reads as EQ. */
        emit_ubfx_w32(e, W_T3, off, 16, 8);
        (void)emit_tst_w32_imm(e, off, ~(FLAT_TOP - 1));
        emit_ccmp_w32_imm(e, W_T3, 0xA, 0x4, A64_COND_EQ);
        slow = A64_COND_EQ;
    }
    flat_slow_site(e);
    emit_b_cond(e, slow, 0);
}
static void emit_flat_check(emit_t *e, a64_reg_t off) { emit_flat_check_as(e, off, 0); }

/* ---- Inline faults in flat blocks ----
 * A flat access never faults, but an inline op can (#DE). The site
 * branches to a chunk after the block that enters the fault stub with
 * the instruction's start EIP: nothing of the instruction has been
 * committed, and its flags are all live (op_flag_effects). */
typedef struct { uint32_t patch_off, ip_start, n_done; uint8_t vector; } fault_site_t;
static fault_site_t s_fault[64];
static uint32_t s_nfault;

static void fault_site(emit_t *e, uint8_t vector) {
    if (s_nfault >= 64) { fprintf(stderr, "dbt: fault site table overflow\n"); abort(); }
    s_fault[s_nfault++] = (fault_site_t){ emit_pos(e), s_cur_ip_start, s_cur_n_done, vector };
}
/* KEY_SEG16 access check: off + size - 1 <= limit, and the segment
 * usable (a null DS or ES has limit 0, which alone would let offset 0
 * through). Both come from the cpu at run time — the same block runs
 * under whatever DS and ES the code has loaded since. A 16-bit offset
 * plus 3 cannot overflow the compare. Out of range or unusable: #GP,
 * or #SS through SS on the 386 (CONTRACT, limit_check in the interp),
 * error code 0, at the instruction, nothing committed. Clobbers W_T1, W_T3. */
static void emit_limit_check(emit_t *e, int seg, a64_reg_t off, int size) {
    a64_reg_t hi = off;
    if (size > 1) { emit_add_w32_imm(e, W_T3, off, (uint32_t)size - 1); hi = W_T3; }
    emit_ldr_w32_imm(e, W_T1, R_CPU, OFF_SEG_LIMIT(seg));
    emit_cmp_w32_w32(e, hi, W_T1);
    emit_ldrb_imm(e, W_T1, R_CPU, OFF_SEG_USABLE(seg));
    emit_ccmp_w32_imm(e, W_T1, 0, 0x4, A64_COND_LS);   /* in range: Z = !usable; past it: Z = 1 */
    fault_site(e, (uint8_t)(seg == S_SS && s_regs32 ? X86_EXC_SS : X86_EXC_GP));
    emit_b_cond(e, A64_COND_EQ, 0);
}

static void emit_fault_chunks(emit_t *e) {
    for (uint32_t k = 0; k < s_nfault; k++) {
        fault_site_t *f = &s_fault[k];
        emit_patch_cond19(e, f->patch_off, emit_pos(e));
        emit_mov_w32_imm32(e, A64_W3, f->ip_start);
        emit_movz_w32(e, A64_W4, (uint16_t)f->n_done, 0);
        emit_movz_w32(e, A64_W5, f->vector, 0);
        emit_b(e, (int32_t)s_fault_stub_off - (int32_t)emit_pos(e));
    }
    s_nfault = 0;
}

/* Post-store SMC check for the `size` bytes at host address X3: one
 * load of their bitmap entries (LDRB/LDRH/LDR — a word store that starts
 * just before a block still reaches into it), CBZ over the thunk call,
 * which hooks each byte. Clobbers W_T2, X0..X4 on the slow path. */
static void emit_smc_check_x3(emit_t *e, int size) {
    emit_ldst_reg(e, size == 4 ? 2 : size == 2 ? 1 : 0, 1, W_T2, W_T3, R_BMD, 3, 0);
    uint32_t skip = emit_pos(e);
    emit_cbz_w32(e, W_T2, 0);
    uint32_t dev_done = 0, dev_miss1 = 0, dev_miss2 = 0;
    if (size == 1) {
        /* A byte into device memory and nothing else (no code, no
         * descriptor): the device's fast path when it offers one — the
         * plane write and the window refresh vga_store would do. */
        (void)emit_subs_w32_imm(e, A64_WZR, W_T2, X86_BM_DEVICE);
        dev_miss1 = emit_pos(e);
        emit_b_cond(e, A64_COND_NE, 0);
        emit_ldr_x64_imm(e, A64_W0, R_CPU, OFF_DEV_WPLANE);
        dev_miss2 = emit_pos(e);
        emit_cbz_x64(e, A64_W0, 0);
        emit_sub_x64(e, A64_W1, W_T3, R_MEM);
        emit_sub_x64_imm_lsl12(e, A64_W1, A64_W1, 0xA0);           /* offset in the A0000 window */
        emit_ldrb_imm(e, A64_W2, W_T3, 0);
        emit_strb_reg_uxtw(e, A64_W2, A64_W0, A64_W1);
        emit_ldr_x64_imm(e, A64_W0, R_CPU, OFF_DEV_RPLANE);
        emit_ldrb_reg_uxtw(e, A64_W2, A64_W0, A64_W1);
        emit_strb_imm(e, A64_W2, W_T3, 0);
        dev_done = emit_pos(e);
        emit_b(e, 0);
        emit_patch_cond19(e, dev_miss1, emit_pos(e));
        emit_patch_cond19(e, dev_miss2, emit_pos(e));
    }
    emit_mov_x64_x64(e, A64_W0, R_CPU);
    emit_sub_x64(e, A64_W1, W_T3, R_MEM);
    if (size > 1) (void)emit_orr_w32_imm(e, A64_W1, A64_W1, (uint32_t)size << 28);
    emit_thunk_args(e);
    /* A block ender's stores (CALL's return address, a far CALL's or an
     * inlined INT's frame) may land on the running block itself — IO.SYS
     * points SS at its own code while relocating — and the thunk would
     * then leave the block with the instruction half done (CS pushed, IP
     * not, no transfer). The ender leaves the block anyway: invalidate,
     * but name no running block, and let the ender finish. */
    if (s_cur_ender) emit_mov_w32_imm32(e, A64_W2, 0xFFFFFFFFu);
    emit_bl(e, (int32_t)s_smc_thunk_off - (int32_t)emit_pos(e));
    emit_patch_cond19(e, skip, emit_pos(e));
    if (dev_done) emit_patch_b26(e, dev_done, emit_pos(e));
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
            if (w->smc) { emit_add_x64_w32_uxtw(e, W_T3, w->segp, W_T3); emit_smc_check_x3(e, 1); }
            emit_lsr_w32_imm(e, W_T1, w->reg, 8);
            emit_strb_imm(e, W_T1, w->segp, 0);
            if (w->smc) { emit_mov_x64_x64(e, W_T3, w->segp); emit_smc_check_x3(e, 1); }
        }
        emit_b(e, (int32_t)w->back_off - (int32_t)emit_pos(e));
    }
    s_nwrap = 0;
}

/* Set while a word read has already wrap-checked this exact EA earlier in
 * the same instruction. On 286+ that check faults, so a read-modify-write
 * can never reach its store with a straddling offset and the store's own
 * check is unreachable code. The 8086/186 wrap instead: there the store
 * really does have to split, so both checks stay. */
static int s_ea_checked;

/* rd = rn + imm, any imm below 16M (two ADDs past 4K) */
static void emit_add_x64_big(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm) {
    if (imm >> 12) { emit_add_x64_imm_lsl12(e, rd, rn, imm >> 12); rn = rd; }
    if ((imm & 0xFFF) || rd != rn) emit_add_x64_imm(e, rd, rn, imm & 0xFFF);
}

/* A flat block under paging: the access at linear OFF (size bytes)
 * through the interpreter's own TLB. The entry for its page must match
 * and allow it — valid, plain RAM (X86_TLB_MEM), dirty for a store, and
 * the user bits at CPL 3 — and the access must stay on the page; then
 * X_SEGP = R_MEM + physical page - linear page, so the flat emitters'
 * [base + off] addressing (off+2, off+4 included) lands in the right
 * page. Anything else is the flat slow path: the interpreter walks,
 * faults, sets accessed/dirty and fills the entry for next time.
 * Clobbers X0-X2, W_T1; leaves OFF and W_T2/W_T3 alone. */
static void emit_pgflat(emit_t *e, a64_reg_t off, int size, int write) {
    emit_ubfx_w32(e, A64_W0, off, 12, 8);                       /* TLB index */
    emit_add_x64_big(e, A64_W2, R_CPU, (uint32_t)offsetof(x86_cpu, tlb));
    emit_lsl_x64_imm(e, A64_W0, A64_W0, 3);
    emit_add_x64(e, A64_W2, A64_W2, A64_W0);
    emit_ldp_w32_off(e, A64_W0, A64_W1, A64_W2, 0);             /* w0 = tag, w1 = physical page */
    emit_eor_w32(e, W_T1, A64_W0, off);
    (void)emit_tst_w32_imm(e, W_T1, 0xFFFFF000u);               /* the entry is for this page */
    flat_slow_site(e);
    emit_b_cond(e, A64_COND_NE, 0);
    uint32_t need = X86_TLB_V | X86_TLB_MEM | (write ? X86_TLB_D : 0)
                  | (s_pg_user ? (write ? X86_TLB_UW : X86_TLB_U) : 0);
    emit_movz_w32(e, W_T1, (uint16_t)need, 0);
    emit_bic_w32(e, W_T1, W_T1, A64_W0);                         /* a needed bit missing? */
    flat_slow_site(e);
    emit_cbnz_w32(e, W_T1, 0);
    if (size > 1) {
        (void)emit_and_w32_imm(e, W_T1, off, 0xFFF);
        emit_cmp_w32_imm(e, W_T1, (uint32_t)(0x1000 - size));
        flat_slow_site(e);
        emit_b_cond(e, A64_COND_HI, 0);
    }
    (void)emit_and_w32_imm(e, W_T1, off, 0xFFFFF000u);
    emit_sub_x64(e, A64_W1, A64_W1, W_T1);                       /* physical - linear page, signed */
    emit_add_x64(e, X_SEGP, R_MEM, A64_W1);
}

/* V86 under paging: the identity host address segp + off becomes the
 * physical one through the page's delta in cpu->pgd_r (pgd_w for a
 * store, which also needs the page dirty already), left in X_SEGP.
 * Anything else — no translation, an access crossing into the next page,
 * or past the 64K offset (a #GP on the 386) — takes the slow path: the
 * whole instruction through the interpreter, which walks, faults and sets
 * accessed/dirty exactly. Every check precedes the instruction's first
 * state change. Clobbers W_T1-W_T3; the result is in X_SEGP. */
static void emit_paged(emit_t *e, a64_reg_t segp, a64_reg_t off, int size, int write) {
    if (size > 1) {
        emit_movz_w32(e, W_T1, (uint16_t)(0x10000 - size), 0);
        emit_subs_w32(e, A64_WZR, off, W_T1);
        flat_slow_site(e);
        emit_b_cond(e, A64_COND_HI, 0);
    }
    emit_add_x64_w32_uxtw(e, W_T3, segp, off);                    /* identity host address */
    emit_sub_x64(e, W_T2, W_T3, R_MEM);
    emit_lsr_x64_imm(e, W_T2, W_T2, 12);                          /* linear page, < 0x110 */
    emit_add_x64_big(e, X_SEGP, R_CPU, (uint32_t)(write ? offsetof(x86_cpu, pgd_w) : offsetof(x86_cpu, pgd_r)));
    emit_ldr_x64_reg_lsl3(e, W_T2, X_SEGP, W_T2);                 /* delta, or X86_PGD_NONE */
    (void)emit_tst_w32_imm(e, W_T2, 1);
    flat_slow_site(e);
    emit_b_cond(e, A64_COND_NE, 0);
    if (size > 1) {
        (void)emit_and_w32_imm(e, W_T1, W_T3, 0xFFF);
        emit_cmp_w32_imm(e, W_T1, (uint32_t)(0x1000 - size));
        flat_slow_site(e);
        emit_b_cond(e, A64_COND_HI, 0);
    }
    emit_add_x64(e, X_SEGP, W_T3, W_T2);
}

/* Does the instruction write its memory operand (so the page must be
 * writable and dirty)? When unsure, yes: a read of such a page only
 * takes the slow path. */
static int writes_mem_operand(const x86_insn *in) {
    switch (in->op) {
    case OP_CMP: case OP_TEST: case OP_PUSH: case OP_CALL: case OP_JMP: case OP_CALLF: case OP_JMPF:
    case OP_LES: case OP_LDS: case OP_LSS: case OP_LFS: case OP_LGS:
    case OP_MOVZX: case OP_MOVSX: case OP_MUL: case OP_IMUL: case OP_DIV: case OP_IDIV:
    case OP_BT:
        return 0;
    case OP_MOV: case OP_MOVSEG:
        return in->ops[0].kind == OPK_MEM;
    default:
        return 1;
    }
}

/* Bytes an instruction's memory operand covers (a far pointer's four or
 * six for LES/LDS and indirect far transfers). */
static int flat_access_size(const x86_insn *in) {
    int size = in->opsize;
    for (int i = 0; i < 3; i++) if (in->ops[i].kind == OPK_MEM && in->ops[i].size) size = in->ops[i].size;
    switch (in->op) {
    case OP_LES: case OP_LDS: case OP_LSS: case OP_LFS: case OP_LGS: case OP_JMPF: case OP_CALLF:
        size = in->opsize + 2; break;
    default: break;
    }
    return size;
}

/* The EA of a paged V86 block, translated for the instruction's one
 * memory access: its size is the memory operand's (a far pointer's four
 * bytes for LES/LDS and indirect far transfers). */
static void emit_ea_paged(emit_t *e, const x86_insn *in, ea_t *ea) {
    int size = in->opsize;
    for (int i = 0; i < 3; i++) if (in->ops[i].kind == OPK_MEM && in->ops[i].size) size = in->ops[i].size;
    switch (in->op) {
    case OP_LES: case OP_LDS: case OP_LSS: case OP_LFS: case OP_LGS: case OP_JMPF: case OP_CALLF:
        size = in->opsize + 2; break;
    default: break;
    }
    emit_paged(e, ea->segp, ea->off, size, writes_mem_operand(in));
    ea->segp = X_SEGP;
    /* a real zero, not WZR: an ADD immediate (LES/LDS's selector at +2)
     * reads register 31 as SP */
    emit_movz_w32(e, W_OFF, 0, 0);
    ea->off = W_OFF;
}

static void emit_read_mem(emit_t *e, const ea_t *ea, int size, a64_reg_t dst) {
    if (s_seg16) {
        emit_limit_check(e, ea->seg, ea->off, size);
        s_ea_checked = 1;                       /* 286+: a straddling store after this is unreachable */
    }
    if (size == 1) { emit_ldrb_reg_uxtw(e, dst, ea->segp, ea->off); return; }
    if (s_flat || s_seg16) {
        if (size == 2) emit_ldrh_reg_uxtw(e, dst, ea->segp, ea->off);
        else emit_ldr_w32_reg_uxtw(e, dst, ea->segp, ea->off);
        return;
    }
    emit_wrap_check(e, ea->segp, ea->off, dst, 0, 0);
    emit_ldrh_reg_uxtw(e, dst, ea->segp, ea->off);
    emit_wrap_back(e);
    if (!s_wrap_exact) s_ea_checked = 1;
}

/* Store + inline SMC check. Clobbers W_T2, W_T3, X0..X4 on the slow path. */
static void emit_write_mem(emit_t *e, const ea_t *ea, int size, a64_reg_t src) {
    int check = size == 2 && !s_ea_checked && !s_flat && !s_seg16;
    if (s_seg16 && !s_ea_checked) emit_limit_check(e, ea->seg, ea->off, size);
    if (size == 4) emit_str_w32_reg_uxtw(e, src, ea->segp, ea->off);
    else if (size == 1) emit_strb_reg_uxtw(e, src, ea->segp, ea->off);
    else {
        if (check) emit_wrap_check(e, ea->segp, ea->off, src, 1, 1);
        emit_strh_reg_uxtw(e, src, ea->segp, ea->off);
    }
    emit_add_x64_w32_uxtw(e, W_T3, ea->segp, ea->off);
    emit_smc_check_x3(e, size);
    if (check) emit_wrap_back(e);
}

/* Read operand i (canonical). Memory operands need the EA computed. */
static a64_reg_t emit_read_operand(emit_t *e, const x86_insn *in, int i, const ea_t *ea, a64_reg_t tmp) {
    const x86_operand *o = &in->ops[i];
    switch (o->kind) {
    case OPK_REG:  return emit_read_reg(e, o->reg, o->size, tmp);
    case OPK_IMM:
        if (s_dyn_imm_lin && i == 1) {
            /* Patched immediate (see dbt->smc_heat): load it as the code
             * is now, sign-extending an imm8 the way the decoder did. */
            uint8_t enc = o->imm_enc;
            emit_mov_w32_imm32(e, tmp, s_dyn_imm_lin);
            switch (X86_IMM_LEN(enc)) {
            case 1:
                if (X86_IMM_SX(enc)) {
                    emit_ldrsb_w32_reg_uxtw(e, tmp, R_MEM, tmp);
                    (void)emit_and_w32_imm(e, tmp, tmp, szmask(o->size));
                } else {
                    emit_ldrb_reg_uxtw(e, tmp, R_MEM, tmp);
                }
                break;
            case 2: emit_ldrh_reg_uxtw(e, tmp, R_MEM, tmp); break;
            default: emit_ldr_w32_reg_uxtw(e, tmp, R_MEM, tmp); break;
            }
            return tmp;
        }
        emit_mov_w32_imm32(e, tmp, o->imm & szmask(o->size));
        return tmp;
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
/* NZCV left intact by the immediately preceding arithmetic. The flag
 * tail after an ADDS/SUBS (mrs, table load, the OR-ins for CF/PF/AF)
 * writes no condition flags, and neither does the tail prologue, so a
 * conditional branch that follows such an op can test NZCV directly
 * instead of waiting on the table load that builds R_F. R_F is still
 * built — the exit needs it — but nothing on the branch's dependency
 * chain does. Only set for ops with a register destination: a memory
 * destination emits the SMC check, whose helper call would clobber NZCV. */
typedef struct { int valid, table, logical; } nzcv_state;
static nzcv_state s_nzcv;

/* Map a guest condition onto the host NZCV, or -1 if it does not map.
 * N is SF, Z is ZF and V is OF for every table we emit. The carry and
 * the signed pairs only line up for SUB/CMP, where ARM's "no borrow"
 * carry is exactly what its LO/LS/LT/LE are defined against. */
static int fuse_cond(int cc, int table, int logical) {
    switch (cc) {
    case 4:  return A64_COND_EQ;
    case 5:  return A64_COND_NE;
    case 8:  return A64_COND_MI;
    case 9:  return A64_COND_PL;
    case 0:  return logical ? -1 : A64_COND_VS;   /* logicals force OF = 0 */
    case 1:  return logical ? -1 : A64_COND_VC;
    default: break;
    }
    if (logical || table != T_SUB) return -1;
    switch (cc) {
    case 2:  return A64_COND_CC;   /* B  (x86 CF = borrow = !C) */
    case 3:  return A64_COND_CS;   /* AE */
    case 6:  return A64_COND_LS;   /* BE */
    case 7:  return A64_COND_HI;   /* A  */
    case 12: return A64_COND_LT;
    case 13: return A64_COND_GE;
    case 14: return A64_COND_LE;
    case 15: return A64_COND_GT;
    default: return -1;
    }
}

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
        (void)emit_and_w32_imm(e, A64_W2, R_F, X86_CF);    /* W2 = CF in */
        if (size == 4) {
            /* Full width: the host's own carry-in. ADCS computes a+b+C
             * with the flags of that sum, which is x86's ADC; SBCS
             * computes a-b-!C, so C = !CF going in and the T_SUB row
             * (CF = !C) reads the borrow back out. */
            if (op == OP_ADC) { emit_cmp_w32_imm(e, A64_W2, 1); emit_adcs_w32(e, W_T0, a, b); }
            else { emit_subs_w32(e, A64_WZR, A64_WZR, A64_W2); emit_sbcs_w32(e, W_T0, a, b); table = T_SUB; }
            break;
        }
        /* ADCS would add the carry at bit 0, below the shifted field.
         * Fold it into the operand instead: b' = b + CF, then a ± b'
         * with plain ADDS/SUBS. The one corner case, b = all-ones with
         * CF set, makes b' << sh vanish and loses the guest carry; the
         * result and OF are still right there (a ± 0), so just OR the
         * lost bit back into CF after the table. */
        /* W1 = b' (W_T1 is the NZCV temp). On 386 a word operand can be a
         * whole 32-bit register: mask it, or its upper half lands in the
         * carry-out bit OR'd into F below. */
        if (size == 2 && s_regs32) {
            emit_uxth_w32(e, A64_W1, b);
            emit_add_w32(e, A64_W1, A64_W1, A64_W2);
        } else {
            emit_add_w32(e, A64_W1, b, A64_W2);
        }
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
    if ((op == OP_ADC || op == OP_SBB) && size < 4) {
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
    if (!emit_addsubs_w32_imm_any(e, !is_inc, W_T0, W_T0, 1u << sh)) {
        emit_movz_w32(e, W_T1, (uint16_t)(1u << (sh - 16)), 16);
        if (is_inc) emit_adds_w32(e, W_T0, W_T0, W_T1); else emit_subs_w32(e, W_T0, W_T0, W_T1);
    }
    emit_flags_from_nzcv(e, is_inc ? T_INC : T_DEC);
    emit_orr_w32(e, R_F, R_F, W_T3);
    emit_lsr_w32_imm(e, res, W_T0, sh);
    if (fmask & X86_PF) emit_flag_pf(e, res);
    if (fmask & X86_AF) {
        /* AF = (a ^ 1 ^ res) & 10h, and bit 4 of the 1 is zero, so the
         * operand xor drops out: (a ^ res) & 10h. (Recovering a from res
         * instead would save the copy but put two more ops on the
         * dependency chain after res, which measured slower.) */
        emit_eor_w32(e, W_T1, a_keep, res);
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
        case OP_SHR: emit_ubfx_w32(e, res, a, cnt, bits - cnt); break;   /* not LSR: a may be a whole 32-bit register */
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
        emit_ubfx_w32(e, W_T1, a_keep, bits - 1, 1);                  /* not LSR: a may be a whole 32-bit register */
        emit_orr_w32_lsl(e, R_F, R_F, W_T1, 11);
    }
    if (fmask & X86_PF) emit_flag_pf(e, res);
}

/* SHL/SHR/SAR r/m32 by CL. CONTRACT (interp, 186+): count & 31; a zero
 * count changes nothing, flags included. Otherwise as the immediate
 * forms: AF cleared, SHL OF = MSB(res)^CF, SHR OF = MSB(a) for count 1
 * else 0, SAR OF = 0. res may alias a. */
static void emit_shift_cl(emit_t *e, int op, a64_reg_t a, a64_reg_t res, uint32_t fmask) {
    int shl = op == OP_SHL || op == OP_SAL;
    if (!fmask) {
        /* the variable shifts take the count modulo 32 themselves */
        if (shl) emit_lslv_w32(e, res, a, R_GPR(R_CX));
        else if (op == OP_SHR) emit_lsrv_w32(e, res, a, R_GPR(R_CX));
        else emit_asrv_w32(e, res, a, R_GPR(R_CX));
        return;
    }
    (void)emit_and_w32_imm(e, W_T2, R_GPR(R_CX), 0x1F);
    uint32_t zero = emit_pos(e);
    emit_cbz_w32(e, W_T2, 0);
    emit_sub_w32_imm(e, W_T3, W_T2, 1);
    if (shl) {
        /* a << (cnt-1), doubled: C is the last bit out, V = MSB(res)^CF */
        emit_lslv_w32(e, W_T0, a, W_T3);
        emit_adds_w32(e, W_T0, W_T0, W_T0);
        emit_flags_from_nzcv(e, T_ADD);
        emit_mov_w32_w32(e, res, W_T0);
    } else {
        /* W_T0 = a shifted by cnt-1: its bit 0 is CF, and for SHR its
         * MSB is OF — MSB(a) when cnt is 1, and 0 for any larger count. */
        if (op == OP_SHR) { emit_lsrv_w32(e, W_T0, a, W_T3); emit_lsr_w32_imm(e, res, W_T0, 1); }
        else              { emit_asrv_w32(e, W_T0, a, W_T3); emit_asr_w32_imm(e, res, W_T0, 1); }
        emit_tst_w32(e, res, res);                                    /* N, Z; C = V = 0 */
        emit_flags_from_nzcv(e, T_ADD);
        (void)emit_and_w32_imm(e, W_T3, W_T0, 1);
        emit_orr_w32(e, R_F, R_F, W_T3);
        if (op == OP_SHR) {
            emit_lsr_w32_imm(e, W_T3, W_T0, 31);
            emit_orr_w32_lsl(e, R_F, R_F, W_T3, 11);
        }
    }
    if (fmask & X86_PF) emit_flag_pf(e, res);
    emit_patch_cond19(e, zero, emit_pos(e));
}

/* DIV/IDIV r/m32: EDX:EAX by src. CONTRACT (interp, 186+): flags
 * unchanged; #DE for a zero divisor or a quotient outside 32 bits (which
 * for IDIV includes INT64_MIN / -1: SDIV returns INT64_MIN, out of
 * range). Every check precedes the register writes, so the fault sees
 * the instruction uncommitted. src must not be W_T0..W_T3. */
static void emit_div32(emit_t *e, int is_signed, a64_reg_t src) {
    fault_site(e, X86_EXC_DE);
    emit_cbz_w32(e, src, 0);
    emit_orr_x64_lsl(e, W_T0, R_GPR(R_AX), R_GPR(R_DX), 32);        /* X6 = EDX:EAX (pinned regs are zero-extended) */
    if (is_signed) {
        emit_sxtw_x64_w32(e, W_T3, src);
        emit_sdiv_x64(e, W_T2, W_T0, W_T3);
        emit_cmp_x64_w32_sxtw(e, W_T2, W_T2);                        /* fits in int32? */
        fault_site(e, X86_EXC_DE);
        emit_b_cond(e, A64_COND_NE, 0);
        emit_msub_x64(e, W_T3, W_T2, W_T3, W_T0);                    /* r = dividend - q * divisor */
    } else {
        emit_udiv_x64(e, W_T2, W_T0, src);
        emit_lsr_x64_imm(e, W_T3, W_T2, 32);
        fault_site(e, X86_EXC_DE);
        emit_cbnz_x64(e, W_T3, 0);
        emit_msub_x64(e, W_T3, W_T2, src, W_T0);
    }
    emit_mov_w32_w32(e, R_GPR(R_AX), W_T2);
    emit_mov_w32_w32(e, R_GPR(R_DX), W_T3);
}

/* DIV/IDIV r/m8 and r/m16 in real mode, 286 and later. CONTRACT
 * (interp, 186+): flags unchanged; #DE (a fault at the instruction on the
 * 286+) for a zero divisor or a quotient that does not fit. AX / r/m8 ->
 * AL quotient, AH remainder; DX:AX / r/m16 -> AX, DX. The remainder takes
 * the dividend's sign, as SDIV/MSUB give it. src must not be W_T0..W_T3. */
static void emit_div16(emit_t *e, int size, int is_signed, a64_reg_t src) {
    if (size == 1) {
        if (is_signed) { emit_sxtb_w32(e, W_T3, src); emit_sxth_w32(e, W_T0, R_GPR(R_AX)); }
        else           { emit_uxtb_w32(e, W_T3, src); emit_uxth_w32(e, W_T0, R_GPR(R_AX)); }
    } else {
        emit_uxth_w32(e, W_T0, R_GPR(R_AX));
        emit_orr_w32_lsl(e, W_T0, W_T0, R_GPR(R_DX), 16);            /* DX:AX */
        if (is_signed) emit_sxth_w32(e, W_T3, src); else emit_uxth_w32(e, W_T3, src);
    }
    fault_site(e, X86_EXC_DE);
    emit_cbz_w32(e, W_T3, 0);
    if (is_signed) emit_sdiv_w32(e, W_T2, W_T0, W_T3); else emit_udiv_w32(e, W_T2, W_T0, W_T3);
    if (is_signed) {                                                 /* INT32_MIN / -1 lands here too */
        if (size == 1) emit_sxtb_w32(e, W_T1, W_T2); else emit_sxth_w32(e, W_T1, W_T2);
        emit_cmp_w32_w32(e, W_T1, W_T2);
        fault_site(e, X86_EXC_DE);
        emit_b_cond(e, A64_COND_NE, 0);
    } else {
        emit_lsr_w32_imm(e, W_T1, W_T2, size * 8);
        fault_site(e, X86_EXC_DE);
        emit_cbnz_w32(e, W_T1, 0);
    }
    emit_msub_w32(e, W_T1, W_T2, W_T3, W_T0);                        /* remainder */
    if (size == 1) {
        (void)emit_and_w32_imm(e, W_T2, W_T2, 0xFF);
        emit_bfi_w32(e, W_T2, W_T1, 8, 8);
        emit_set16(e, R_AX, W_T2);
    } else {
        emit_uxth_w32(e, W_T2, W_T2);
        emit_uxth_w32(e, W_T1, W_T1);
        emit_set16(e, R_AX, W_T2);
        emit_set16(e, R_DX, W_T1);
    }
}

/* ---- String instructions (16-bit addressing, byte and word) ----
 * The fast path does every check an iteration needs before it commits
 * anything; a miss branches to a slow path that runs the instruction
 * through the helper from the registers as they stand. The interpreter
 * then raises the fault with its own pointer-commit rules (the 286
 * advances a pointer before its access) or wraps (8086). REP is
 * restartable, so the same holds for a loop that has already done some
 * iterations. Checks: a word at offset FFFF in real mode; in a KEY_SEG16
 * block, the segment's limit and usable bit. Byte accesses in real mode
 * cannot miss, and emit nothing. Clobbers W_T1, W_T3. Returns whether a
 * branch was emitted. `site` records it: the generic slow path
 * (flat_slow_site) when NULL, else a list the caller patches itself. */
typedef struct { uint32_t pos[8]; int n; } site_list_t;
static int emit_str_check(emit_t *e, int seg, a64_reg_t off, int size, site_list_t *site) {
    a64_cond_t miss;
    if (s_seg16) {
        a64_reg_t hi = off;
        if (size > 1) { emit_add_w32_imm(e, W_T3, off, (uint32_t)size - 1); hi = W_T3; }
        emit_ldr_w32_imm(e, W_T1, R_CPU, OFF_SEG_LIMIT(seg));
        emit_cmp_w32_w32(e, hi, W_T1);
        emit_ldrb_imm(e, W_T1, R_CPU, OFF_SEG_USABLE(seg));
        emit_ccmp_w32_imm(e, W_T1, 0, 0x4, A64_COND_LS);
        miss = A64_COND_EQ;
    } else if (size == 2) {
        (void)emit_eor_w32_imm(e, W_T3, off, 0xFFFF);
        if (site) site->pos[site->n++] = emit_pos(e); else flat_slow_site(e);
        emit_cbz_w32(e, W_T3, 0);
        return 1;
    } else {
        return 0;
    }
    if (site) site->pos[site->n++] = emit_pos(e); else flat_slow_site(e);
    emit_b_cond(e, miss, 0);
    return 1;
}

/* W2 = the pointer step: +size, or -size with DF set (DF lives in
 * cpu->eflags, not in the pinned arithmetic flags). Clobbers W_T0. */
static void emit_str_delta(emit_t *e, int size) {
    emit_ldr_w32_imm(e, A64_W2, R_CPU, OFF_EFLAGS);
    emit_ubfx_w32(e, A64_W2, A64_W2, 10, 1);                         /* DF */
    emit_movz_w32(e, W_T0, (uint16_t)size, 0);
    emit_sub_w32_lsl(e, A64_W2, W_T0, A64_W2, size == 1 ? 1 : 2);   /* size - 2*size*DF */
}
/* reg = (reg + W2) & FFFF, the upper half of a 386 register kept */
static void emit_str_adv(emit_t *e, int reg) {
    emit_add_w32(e, W_T0, R_GPR(reg), A64_W2);
    (void)emit_and_w32_imm(e, W_T0, W_T0, 0xFFFF);
    emit_set16(e, reg, W_T0);
}
static void emit_str_load(emit_t *e, a64_reg_t dst, a64_reg_t segp, a64_reg_t off, int size) {
    if (size == 1) emit_ldrb_reg_uxtw(e, dst, segp, off); else emit_ldrh_reg_uxtw(e, dst, segp, off);
}

/* LODS, STOS, MOVS, CMPS, SCAS without a repeat prefix. */
static void emit_string1(emit_t *e, const x86_insn *in, uint32_t fmask) {
    int size = in->ops[0].size, seg = in->seg, bails = 0;
    a64_reg_t segp = seg_ptr_reg(seg);
    if (segp == X_SEGP) emit_load_seg_ptr(e, X_SEGP, seg);
    switch (in->op) {
    case OP_LODS:
        emit_uxth_w32(e, W_OFF, R_GPR(R_SI));
        emit_str_check(e, seg, W_OFF, size, NULL);
        emit_str_load(e, W_VAL, segp, W_OFF, size);
        emit_write_reg(e, R_AX, size, W_VAL);
        emit_str_delta(e, size);
        emit_str_adv(e, R_SI);
        break;
    case OP_STOS: {
        emit_uxth_w32(e, W_OFF, R_GPR(R_DI));
        emit_str_check(e, S_ES, W_OFF, size, NULL);
        emit_str_delta(e, size);
        emit_str_adv(e, R_DI);               /* before the store: its SMC exit leaves after the instruction */
        a64_reg_t v = emit_read_reg(e, R_AX, size, W_VAL);
        if (size == 1) emit_strb_reg_uxtw(e, v, R_ESP, W_OFF); else emit_strh_reg_uxtw(e, v, R_ESP, W_OFF);
        emit_add_x64_w32_uxtw(e, W_T3, R_ESP, W_OFF);
        emit_smc_check_x3(e, size);
        break;
    }
    case OP_MOVS:
        emit_uxth_w32(e, W_OFF, R_GPR(R_SI));
        emit_uxth_w32(e, A64_W1, R_GPR(R_DI));
        emit_str_check(e, seg, W_OFF, size, NULL);
        emit_str_check(e, S_ES, A64_W1, size, NULL);
        emit_str_load(e, W_VAL, segp, W_OFF, size);
        emit_str_delta(e, size);
        emit_str_adv(e, R_SI);
        emit_str_adv(e, R_DI);
        if (size == 1) emit_strb_reg_uxtw(e, W_VAL, R_ESP, A64_W1); else emit_strh_reg_uxtw(e, W_VAL, R_ESP, A64_W1);
        emit_add_x64_w32_uxtw(e, W_T3, R_ESP, A64_W1);
        emit_smc_check_x3(e, size);
        break;
    case OP_CMPS: case OP_SCAS:
        if (in->op == OP_CMPS) {
            emit_uxth_w32(e, W_OFF, R_GPR(R_SI));
            bails |= emit_str_check(e, seg, W_OFF, size, NULL);
        } else if (size == 1) {
            (void)emit_and_w32_imm(e, W_VAL, R_GPR(R_AX), 0xFF);
        } else {
            emit_uxth_w32(e, W_VAL, R_GPR(R_AX));
        }
        emit_uxth_w32(e, A64_W1, R_GPR(R_DI));
        bails |= emit_str_check(e, S_ES, A64_W1, size, NULL);
        if (in->op == OP_CMPS) emit_str_load(e, W_VAL, segp, W_OFF, size);
        emit_str_load(e, W_SRC, R_ESP, A64_W1, size);
        emit_str_delta(e, size);
        if (in->op == OP_CMPS) emit_str_adv(e, R_SI);
        emit_str_adv(e, R_DI);
        emit_alu(e, OP_CMP, size, W_VAL, W_SRC, W_VAL, fmask);
        if (!bails) { s_nzcv.valid = 1; s_nzcv.table = T_SUB; s_nzcv.logical = 0; }
        break;
    default: break;
    }
}

/* REPE/REPNE CMPS and SCAS. CONTRACT (interp): CX = 0 on entry does
 * nothing, flags included; otherwise each iteration compares, advances,
 * decrements CX, and the loop stops at CX = 0 or on the ZF condition,
 * with the flags of the last comparison. The loop itself only tests for
 * equality (ZF is exactly a == b); the full flags are built once, from
 * the last pair, on the way out — and on the way to the slow path when
 * an iteration has already run, since the helper then takes over from a
 * state whose flags are the last comparison's. */
static void emit_rep_cmp(emit_t *e, const x86_insn *in, uint32_t fmask) {
    int size = in->ops[0].size, seg = in->seg, is_cmps = in->op == OP_CMPS;
    int repe = in->rep == 0xF3;
    a64_reg_t segp = seg_ptr_reg(seg);
    site_list_t miss = { .n = 0 };
    if (segp == X_SEGP) emit_load_seg_ptr(e, X_SEGP, seg);
    emit_str_delta(e, size);
    if (!is_cmps) {
        if (size == 1) (void)emit_and_w32_imm(e, W_VAL, R_GPR(R_AX), 0xFF);
        else emit_uxth_w32(e, W_VAL, R_GPR(R_AX));
    }
    emit_movz_w32(e, A64_W1, 0, 0);                                  /* no iteration yet */
    a64_reg_t cx = emit_reg16(e, R_CX, W_T0);
    uint32_t none = emit_pos(e);
    emit_cbz_w32(e, cx, 0);
    uint32_t top = emit_pos(e);
    if (is_cmps) { emit_uxth_w32(e, W_OFF, R_GPR(R_SI)); emit_str_check(e, seg, W_OFF, size, &miss); }
    emit_uxth_w32(e, A64_W0, R_GPR(R_DI));
    emit_str_check(e, S_ES, A64_W0, size, &miss);
    if (is_cmps) emit_str_load(e, W_VAL, segp, W_OFF, size);
    emit_str_load(e, W_SRC, R_ESP, A64_W0, size);
    if (is_cmps) emit_str_adv(e, R_SI);
    emit_str_adv(e, R_DI);
    emit_sub_w32_imm(e, W_T0, R_GPR(R_CX), 1);
    (void)emit_and_w32_imm(e, W_T0, W_T0, 0xFFFF);
    emit_set16(e, R_CX, W_T0);
    emit_movz_w32(e, A64_W1, 1, 0);
    emit_cmp_w32_w32(e, W_VAL, W_SRC);
    uint32_t stop = emit_pos(e);
    emit_b_cond(e, repe ? A64_COND_NE : A64_COND_EQ, 0);
    emit_cbnz_w32(e, W_T0, (int32_t)top - (int32_t)emit_pos(e));   /* W_T0: the new CX */
    emit_patch_cond19(e, stop, emit_pos(e));
    emit_alu(e, OP_CMP, size, W_VAL, W_SRC, W_VAL, fmask);
    uint32_t done_b = 0;
    if (miss.n) {
        done_b = emit_pos(e);
        emit_b(e, 0);
        for (int k = 0; k < miss.n; k++) emit_patch_cond19(e, miss.pos[k], emit_pos(e));
        uint32_t skip = emit_pos(e);
        emit_cbz_w32(e, A64_W1, 0);
        emit_alu(e, OP_CMP, size, W_VAL, W_SRC, W_VAL, ARITH);
        emit_patch_cond19(e, skip, emit_pos(e));
        flat_slow_site(e);
        emit_cbz_w32(e, A64_WZR, 0);                                 /* always: the helper, then after the instruction */
    }
    emit_patch_cond19(e, none, emit_pos(e));
    if (done_b) emit_patch_b26(e, done_b, emit_pos(e));
}

/* REP MOVS and REP STOS. Each iteration checks its accesses (as above)
 * and the destination's code-bitmap bytes before it stores anything: a
 * set bit — translated code, a device, a watched descriptor — hands the
 * rest of the loop to the helper, whose stores go through the hooks the
 * interpreter's do. Every iteration before it has fully committed (both
 * pointers, CX), and REP is restartable, so the helper simply carries
 * on. The loop never stores into code, so it never needs the SMC exit
 * that has no exact place to land in the middle of a repeat. */
static void emit_rep_store(emit_t *e, const x86_insn *in) {
    int size = in->ops[0].size, seg = in->seg, is_movs = in->op == OP_MOVS;
    a64_reg_t segp = seg_ptr_reg(seg);
    if (segp == X_SEGP) emit_load_seg_ptr(e, X_SEGP, seg);
    emit_str_delta(e, size);
    a64_reg_t cx = emit_reg16(e, R_CX, W_T0);
    uint32_t none = emit_pos(e);
    emit_cbz_w32(e, cx, 0);
    uint32_t top = emit_pos(e);
    if (is_movs) { emit_uxth_w32(e, W_OFF, R_GPR(R_SI)); emit_str_check(e, seg, W_OFF, size, NULL); }
    emit_uxth_w32(e, A64_W1, R_GPR(R_DI));
    emit_str_check(e, S_ES, A64_W1, size, NULL);
    emit_add_x64_w32_uxtw(e, W_T3, R_ESP, A64_W1);                  /* X3 = host address of ES:DI */
    emit_ldst_reg(e, size == 2 ? 1 : 0, 1, W_T2, W_T3, R_BMD, 3, 0); /* its bitmap byte(s) */
    flat_slow_site(e);
    emit_cbnz_w32(e, W_T2, 0);
    a64_reg_t v;
    if (is_movs) { emit_str_load(e, W_VAL, segp, W_OFF, size); v = W_VAL; }
    else v = emit_read_reg(e, R_AX, size, W_VAL);
    if (size == 1) emit_strb_imm(e, v, W_T3, 0); else emit_strh_imm(e, v, W_T3, 0);
    if (is_movs) emit_str_adv(e, R_SI);
    emit_str_adv(e, R_DI);
    emit_sub_w32_imm(e, W_T0, R_GPR(R_CX), 1);
    (void)emit_and_w32_imm(e, W_T0, W_T0, 0xFFFF);
    emit_set16(e, R_CX, W_T0);
    emit_cbnz_w32(e, W_T0, (int32_t)top - (int32_t)emit_pos(e));
    emit_patch_cond19(e, none, emit_pos(e));
}

/* PUSHF with a 16-bit operand: FLAGS as the guest sees them, low word. */
static void emit_pushf16(emit_t *e) {
    emit_ldr_w32_imm(e, W_T0, R_CPU, OFF_EFLAGS);
    emit_movz_w32(e, W_T1, ARITH, 0);
    emit_bic_w32(e, W_T0, W_T0, W_T1);
    emit_orr_w32(e, W_VAL, W_T0, R_F);
    (void)emit_and_w32_imm(e, W_VAL, W_VAL, 0xFFFF);
    emit_push16(e, W_VAL);
}
/* POPF with a 16-bit operand, real mode: the low word of EFLAGS
 * replaced whole (no privilege rules outside protected mode), then
 * x86_flags_fixup's per-model masks: bit 1 set, bits 3 and 5 clear; the
 * 8086/186 read 12-15 as ones, the 286 in real mode as zeros, the 386
 * clears bit 15 and VM. The interpreter has no single-step trap, so TF
 * is only a bit here too; an IF that comes on is seen by the run loop
 * at the block's end, where every interrupt is delivered. */
static void emit_popf16(emit_t *e, int model) {
    emit_pop16(e, W_VAL);
    emit_uxth_w32(e, W_VAL, W_VAL);
    if (model >= X86_MODEL_386) {
        emit_ldr_w32_imm(e, W_T0, R_CPU, OFF_EFLAGS);
        emit_and_w32_imm_any(e, W_T0, W_T0, 0xFFFF0000u & ~(uint32_t)X86_VM, W_T1);
        emit_orr_w32(e, W_VAL, W_VAL, W_T0);
        emit_and_w32_imm_any(e, W_VAL, W_VAL, ~0x8028u, W_T1);
        (void)emit_orr_w32_imm(e, W_VAL, W_VAL, 0x2);
    } else if (model == X86_MODEL_286) {
        emit_and_w32_imm_any(e, W_VAL, W_VAL, 0x0FD7, W_T1);
        (void)emit_orr_w32_imm(e, W_VAL, W_VAL, 0x2);
    } else {
        emit_and_w32_imm_any(e, W_VAL, W_VAL, 0xFFD7, W_T1);
        emit_orr_w32_imm_any(e, W_VAL, W_VAL, 0xF002, W_T1);
    }
    emit_str_w32_imm(e, W_VAL, R_CPU, OFF_EFLAGS);
    emit_movz_w32(e, W_T1, ARITH, 0);
    emit_and_w32(e, R_F, W_VAL, W_T1);
}

/* MUL and one-operand IMUL, r/m8 and r/m16. CONTRACT (interp): AX (or
 * DX:AX) = the product; all arithmetic flags cleared, then SF/ZF/PF from
 * the low half and CF = OF = the high half matters (unsigned: nonzero;
 * signed: not the low half's sign extension). src must not be W_T0..W_T3. */
static void emit_mul16(emit_t *e, int size, int is_signed, a64_reg_t src, uint32_t fmask) {
    if (size == 1) {
        if (is_signed) { emit_sxtb_w32(e, W_T2, R_GPR(R_AX)); emit_sxtb_w32(e, W_T3, src); }
        else { (void)emit_and_w32_imm(e, W_T2, R_GPR(R_AX), 0xFF); emit_uxtb_w32(e, W_T3, src); }
    } else {
        if (is_signed) { emit_sxth_w32(e, W_T2, R_GPR(R_AX)); emit_sxth_w32(e, W_T3, src); }
        else { emit_uxth_w32(e, W_T2, R_GPR(R_AX)); emit_uxth_w32(e, W_T3, src); }
    }
    emit_mul_w32(e, W_T0, W_T2, W_T3);                               /* fits in 32 bits either way */
    emit_uxth_w32(e, W_T2, W_T0);
    emit_set16(e, R_AX, W_T2);
    if (size == 2) {
        emit_lsr_w32_imm(e, W_T1, W_T0, 16);
        emit_set16(e, R_DX, W_T1);
    }
    if (!fmask) return;
    if (is_signed) {
        if (size == 1) emit_sxtb_w32(e, W_T1, W_T0); else emit_sxth_w32(e, W_T1, W_T0);
        emit_cmp_w32_w32(e, W_T1, W_T0);
    } else {
        emit_lsr_w32_imm(e, W_T1, W_T0, size * 8);
        (void)emit_subs_w32_imm(e, A64_WZR, W_T1, 0);
    }
    emit_cset_w32(e, W_T3, A64_COND_NE);
    emit_lsl_w32_imm(e, W_T1, W_T0, 32 - 8 * (uint32_t)size);
    emit_tst_w32(e, W_T1, W_T1);                                     /* N, Z of the low half; C = V = 0 */
    emit_flags_from_nzcv(e, T_ADD);
    if (fmask & X86_PF) emit_flag_pf(e, W_T0);
    emit_orr_w32(e, R_F, R_F, W_T3);
    emit_orr_w32_lsl(e, R_F, R_F, W_T3, 11);
}

/* Real-mode segment load (DS or ES): what x86_load_seg does outside
 * protected mode — selector, base = selector << 4, the limit raised to
 * FFFF but an unreal one kept, usable, attributes and D bit clear — and
 * the pinned host pointer. sel holds the selector in its low 16 bits.
 * Clobbers W_T0..W_T2. */
static void emit_load_seg_real(emit_t *e, int s, a64_reg_t sel) {
    emit_ldr_w32_imm(e, W_T2, R_CPU, OFF_SEG_LIMIT(s));
    emit_movz_w32(e, W_T1, 0xFFFF, 0);
    emit_subs_w32(e, A64_WZR, W_T2, W_T1);
    emit_csel_w32(e, W_T2, W_T2, W_T1, A64_COND_HI);                 /* max(limit, FFFF) */
    emit_str_w32_imm(e, W_T2, R_CPU, OFF_SEG_LIMIT(s));
    emit_uxth_w32(e, W_T0, sel);
    emit_str_w32_imm(e, W_T0, R_CPU, OFF_SEG_SEL(s));                /* sel, attr = 0 */
    emit_movz_w32(e, W_T2, 1, 0);
    emit_strb_imm(e, W_T2, R_CPU, OFF_SEG_USABLE(s));
    emit_lsl_w32_imm(e, W_T1, W_T0, 4);
    emit_str_w32_imm(e, W_T1, R_CPU, OFF_SEG_BASE(s));
    emit_strb_imm(e, A64_WZR, R_CPU, OFF_SEG_BIG(s));
    emit_add_x64_w32_uxtw(e, s == S_DS ? R_DSP : R_ESP, R_MEM, W_T1);
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
/* Flat 32-bit stack: the address is range-checked like any other flat
 * access (slow path: the whole instruction through the helper) before
 * anything moves; ESP is re-derived after the SMC check, which clobbers
 * the scratch registers when it fires. */
static void emit_push32_flat(emit_t *e, a64_reg_t val) {
    emit_sub_w32_imm(e, W_T2, R_GPR(R_SP), 4);
    a64_reg_t base = R_MEM;
    if (s_paged) { emit_pgflat(e, W_T2, 4, 1); base = X_SEGP; }
    else emit_flat_check_as(e, W_T2, 1);
    emit_str_w32_reg_uxtw(e, val, base, W_T2);
    emit_add_x64_w32_uxtw(e, W_T3, base, W_T2);
    emit_smc_check_x3(e, 4);
    emit_sub_w32_imm(e, R_GPR(R_SP), R_GPR(R_SP), 4);
}
static void emit_pop32_flat(emit_t *e, a64_reg_t dst) {
    a64_reg_t base = R_MEM;
    if (s_paged) { emit_pgflat(e, R_GPR(R_SP), 4, 0); base = X_SEGP; }
    else emit_flat_check(e, R_GPR(R_SP));
    emit_ldr_w32_reg_uxtw(e, dst, base, R_GPR(R_SP));
    emit_add_w32_imm(e, R_GPR(R_SP), R_GPR(R_SP), 4);
}

/* PUSHAD/POPAD on the flat stack. CONTRACT (interp, 386): PUSHAD stores
 * EAX ECX EDX EBX ESP EBP ESI EDI downward, ESP as it was before the
 * instruction; POPAD skips the ESP slot. Both ends of the 32-byte frame
 * are range-checked, then PUSHAD checks the frame's code-bitmap bytes
 * before storing anything: any set bit (code, device, descriptor) takes
 * the slow path, which replays the whole instruction through the
 * interpreter, per-byte store hooks and all. Four LDP/STP do the rest. */
static void emit_pusha_flat(emit_t *e) {
    emit_sub_w32_imm(e, W_T2, R_GPR(R_SP), 32);
    emit_flat_check_as(e, W_T2, 1);
    emit_sub_w32_imm(e, W_T3, R_GPR(R_SP), 4);
    emit_flat_check_as(e, W_T3, 1);
    emit_add_x64_w32_uxtw(e, W_T3, R_MEM, W_T2);                    /* X3 = host address of the frame */
    emit_add_x64(e, W_T0, R_BMD, W_T3);
    emit_ldp_x64_off(e, W_T2, W_T1, W_T0, 0);
    emit_orr_x64(e, W_T2, W_T2, W_T1);
    emit_ldp_x64_off(e, W_T1, W_T0, W_T0, 16);
    emit_orr_x64(e, W_T2, W_T2, W_T1);
    emit_orr_x64(e, W_T2, W_T2, W_T0);
    flat_slow_site(e);
    emit_cbnz_x64(e, W_T2, 0);
    emit_stp_w32_off(e, R_GPR(R_DI), R_GPR(R_SI), W_T3, 0);
    emit_stp_w32_off(e, R_GPR(R_BP), R_GPR(R_SP), W_T3, 8);
    emit_stp_w32_off(e, R_GPR(R_BX), R_GPR(R_DX), W_T3, 16);
    emit_stp_w32_off(e, R_GPR(R_CX), R_GPR(R_AX), W_T3, 24);
    emit_sub_w32_imm(e, R_GPR(R_SP), R_GPR(R_SP), 32);
}
static void emit_popa_flat(emit_t *e) {
    emit_flat_check(e, R_GPR(R_SP));
    emit_add_w32_imm(e, W_T2, R_GPR(R_SP), 28);
    emit_flat_check(e, W_T2);
    emit_add_x64_w32_uxtw(e, W_T3, R_MEM, R_GPR(R_SP));
    emit_ldp_w32_off(e, R_GPR(R_DI), R_GPR(R_SI), W_T3, 0);
    emit_ldp_w32_off(e, R_GPR(R_BP), W_T0, W_T3, 8);                 /* the ESP image is discarded */
    emit_ldp_w32_off(e, R_GPR(R_BX), R_GPR(R_DX), W_T3, 16);
    emit_ldp_w32_off(e, R_GPR(R_CX), R_GPR(R_AX), W_T3, 24);
    emit_add_w32_imm(e, R_GPR(R_SP), R_GPR(R_SP), 32);
}

/* emit_push16/emit_pop16 are the stack operations of whatever block is
 * being emitted: 16-bit real mode, or (s_flat) the 32-bit flat stack. */
/* A 16-bit stack (real mode, or a KEY_SEG16 block) takes a word or, on
 * the 386 in a segmented block, a dword. */
static void emit_push_stk(emit_t *e, a64_reg_t val, int size) {
    if (s_flat) { emit_push32_flat(e, val); return; }
    if (s_paged) {
        /* one checked slot, then the store through its translation */
        emit_sub_w32_imm(e, W_OFF, R_GPR(R_SP), (uint32_t)size);
        (void)emit_and_w32_imm(e, W_OFF, W_OFF, 0xFFFF);
        emit_paged(e, R_SSP, W_OFF, size, 1);
        if (size == 4) emit_str_w32_imm(e, val, X_SEGP, 0); else emit_strh_imm(e, val, X_SEGP, 0);
        emit_mov_x64_x64(e, W_T3, X_SEGP);
        emit_smc_check_x3(e, size);
        emit_sub_w32_imm(e, W_T2, R_GPR(R_SP), (uint32_t)size);
        (void)emit_and_w32_imm(e, W_T2, W_T2, 0xFFFF);
        emit_set16(e, R_SP, W_T2);
        return;
    }
    /* new SP in a temp until the store is known to succeed (fault: SP intact) */
    emit_sub_w32_imm(e, W_T2, R_GPR(R_SP), (uint32_t)size);
    (void)emit_and_w32_imm(e, W_T2, W_T2, 0xFFFF);   /* also strips ESP[31:16] for the address */
    if (s_seg16) emit_limit_check(e, S_SS, W_T2, size);
    else emit_wrap_check(e, R_SSP, W_T2, val, 1, 1);
    if (size == 4) emit_str_w32_reg_uxtw(e, val, R_SSP, W_T2);
    else emit_strh_reg_uxtw(e, val, R_SSP, W_T2);
    emit_add_x64_w32_uxtw(e, W_T3, R_SSP, W_T2);
    emit_smc_check_x3(e, size);
    if (!s_seg16) emit_wrap_back(e);
    emit_sub_w32_imm(e, W_T2, R_GPR(R_SP), (uint32_t)size);
    (void)emit_and_w32_imm(e, W_T2, W_T2, 0xFFFF);
    emit_set16(e, R_SP, W_T2);
}
static void emit_pop_stk(emit_t *e, a64_reg_t dst, int size) {
    if (s_flat) { emit_pop32_flat(e, dst); return; }
    if (s_paged) {
        a64_reg_t sp0 = emit_reg16(e, R_SP, W_OFF);
        if (sp0 != W_OFF) emit_mov_w32_w32(e, W_OFF, sp0);
        emit_paged(e, R_SSP, W_OFF, size, 0);
        if (size == 4) emit_ldr_w32_imm(e, dst, X_SEGP, 0); else emit_ldrh_imm(e, dst, X_SEGP, 0);
        emit_add_w32_imm(e, W_T2, W_OFF, (uint32_t)size);
        (void)emit_and_w32_imm(e, W_T2, W_T2, 0xFFFF);
        emit_set16(e, R_SP, W_T2);
        return;
    }
    a64_reg_t sp = emit_reg16(e, R_SP, W_T2);
    if (s_seg16) emit_limit_check(e, S_SS, sp, size);
    else emit_wrap_check(e, R_SSP, sp, dst, 0, 0);
    if (size == 4) emit_ldr_w32_reg_uxtw(e, dst, R_SSP, sp);
    else emit_ldrh_reg_uxtw(e, dst, R_SSP, sp);
    if (!s_seg16) emit_wrap_back(e);
    emit_add_w32_imm(e, W_T2, sp, (uint32_t)size);
    (void)emit_and_w32_imm(e, W_T2, W_T2, 0xFFFF);
    emit_set16(e, R_SP, W_T2);
}
static void emit_push16(emit_t *e, a64_reg_t val) { emit_push_stk(e, val, 2); }
static void emit_pop16(emit_t *e, a64_reg_t dst) { emit_pop_stk(e, dst, 2); }

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

static int classify_op(const x86_insn *in) {
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
        return C_INLINE;
    case OP_PUSH:
        return C_INLINE;
    case OP_POP:
        if (in->ops[0].kind != OPK_SREG) return C_INLINE;
        if (in->ops[0].reg == S_CS) return C_REFUSE;           /* POP CS: 8086 control transfer */
        return in->ops[0].reg == S_DS || in->ops[0].reg == S_ES ? C_INLINE : C_HELPER;   /* SS: interrupt shadow */
    case OP_LES: case OP_LDS:
        return C_INLINE;
    case OP_MOVS: case OP_STOS: case OP_LODS: case OP_CMPS: case OP_SCAS:
        /* through DS/ES/SS only; REP MOVS/STOS/LODS stay helpers: an
         * SMC exit inside a storing loop has no exact place to land */
        if (in->seg != S_DS && in->seg != S_ES && in->seg != S_SS) return C_HELPER;
        if (in->rep && in->op == OP_LODS) return C_HELPER;
        return C_INLINE;
    case OP_DIV: case OP_IDIV:
        return s_cls_model >= X86_MODEL_286 ? C_INLINE : C_HELPER;  /* 8086 microcode quirks; 186 #DE is a trap */
    case OP_SHL: case OP_SAL: case OP_SHR: case OP_SAR:
        return is_shift_inline(in) ? C_INLINE : C_HELPER;
    case OP_MUL:
        return C_INLINE;
    case OP_IMUL:
        return in->opcode2 == 0xAF ? C_HELPER : C_INLINE;   /* one-operand form inline */
    case OP_PUSHF: case OP_POPF:
        return C_INLINE;                                     /* 16-bit (classify); POPF real mode only (classify_seg16) */
    case OP_ROL: case OP_ROR: case OP_RCL: case OP_RCR: case OP_SETMO:
    case OP_IMUL3:
    case OP_AAD: case OP_AAA: case OP_AAS: case OP_DAA: case OP_DAS: case OP_SALC:
    case OP_XLAT: case OP_SAHF: case OP_LAHF:
    case OP_PUSHA: case OP_POPA: case OP_ENTER: case OP_LEAVE:
    case OP_BT: case OP_BTS: case OP_BTR: case OP_BTC: case OP_BSF: case OP_BSR:
    case OP_SHLD: case OP_SHRD: case OP_CMPXCHG: case OP_XADD: case OP_BSWAP:
        return C_HELPER;
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

static int classify(const x86_insn *in) {
    if (in->opsize != 2 || in->adsize != 2) return C_REFUSE;   /* 386 forms: Phase B */
    return classify_op(in);
}

/* Virtual-8086 mode on top of the real-mode classes. What V86 does
 * differently is the interpreter's: INT n (through the IDT, IOPL-
 * sensitive), IRET and POPF (IOPL-sensitive, and they keep IOPL and VM),
 * HLT (#GP); I/O is a helper (the interpreter asks the TSS bitmap). CLI, STI and PUSHF are real mode's at
 * IOPL 3 only. Under paging an inline instruction gets one checked access
 * (emit_paged), so those that touch memory twice — a string op, a far
 * CALL or RET, PUSH/POP/CALL through memory — become helpers. */
static int classify_v86(const x86_insn *in, int c) {
    switch (in->op) {
    case OP_INT: case OP_INT3: case OP_INTO: case OP_INT1: case OP_IRET: case OP_POPF: case OP_HLT:
        return C_REFUSE;
    case OP_IN: case OP_OUT: case OP_INS: case OP_OUTS:
        return C_HELPER;           /* the interpreter asks the I/O bitmap and faults to the monitor */
    case OP_CLI: case OP_STI: case OP_PUSHF:
        if (!s_iopl3) return C_REFUSE;
        break;
    default: break;
    }
    if (!s_paged || c != C_INLINE) return c;
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
        return classify_op(in) == C_REFUSE ? C_REFUSE : C_HELPER;
    }
}

static int is_near_transfer(int op) {
    return op == OP_JMP || op == OP_CALL || op == OP_RET || op == OP_JCC || op == OP_JCXZ
        || op == OP_LOOP || op == OP_LOOPE || op == OP_LOOPNE;
}

static int loads_segment(const x86_insn *in) {
    switch (in->op) {
    case OP_MOVSEG: case OP_POP: return in->ops[0].kind == OPK_SREG;
    case OP_LES: case OP_LDS: case OP_LSS: case OP_LFS: case OP_LGS: return 1;
    default: return 0;
    }
}

/* Flat protected mode: the real-mode emitters, widened to 32 bits, take
 * what they cover with 32-bit addressing through DS/ES/SS; everything
 * else is a helper, as in any protected-mode block. Near transfers need
 * a 32-bit operand size (a 16-bit one truncates EIP) and the LOOP family
 * a 32-bit address size (ECX). */
static int classify_flat(const x86_insn *in) {
    if (classify_pm(in) == C_REFUSE) return C_REFUSE;
    if (in->ea_valid && (in->adsize != 4 || (in->seg != S_DS && in->seg != S_ES && in->seg != S_SS)))
        return C_HELPER;
    switch (in->op) {
    case OP_ADD: case OP_OR: case OP_AND: case OP_SUB: case OP_XOR: case OP_CMP: case OP_TEST:
    case OP_INC: case OP_DEC: case OP_NOT: case OP_NEG: case OP_MOV: case OP_XCHG: case OP_LEA:
    case OP_NOP: case OP_CLC: case OP_STC: case OP_CMC: case OP_CLD: case OP_STD:
    case OP_MOVZX: case OP_MOVSX: case OP_CBW: case OP_CWD:
        return C_INLINE;
    case OP_ADC: case OP_SBB:
        return C_INLINE;
    case OP_SHL: case OP_SAL: case OP_SHR: case OP_SAR:
        if (in->ops[1].kind == OPK_REG) return in->ops[0].size == 4 ? C_INLINE : C_HELPER;   /* by CL */
        return is_shift_inline(in) ? C_INLINE : C_HELPER;
    case OP_DIV: case OP_IDIV:
        return in->ops[0].size == 4 ? C_INLINE : C_HELPER;
    case OP_SETCC:
        return C_INLINE;
    case OP_PUSHA: case OP_POPA:
        return in->opsize == 4 ? C_INLINE : C_HELPER;
    case OP_SHLD: case OP_SHRD:
        /* 32-bit, immediate count 1..31: one EXTR. CL counts, zero counts
         * and 16-bit forms (the 386's count > 16 quirk) stay helpers. */
        return in->ops[0].size == 4 && in->imm2 != 0xFFFFFFFFu && (in->imm2 & 31) ? C_INLINE : C_HELPER;
    case OP_IMUL: case OP_MUL:
        return in->ops[0].size == 4 ? C_INLINE : C_HELPER;   /* IMUL r32, r/m32; EDX:EAX = EAX * r/m32 */
    case OP_IMUL3:
        return in->ops[0].size == 4 ? C_INLINE : C_HELPER;
    case OP_PUSH:
        return in->opsize == 4 && in->ops[0].kind != OPK_SREG ? C_INLINE : C_HELPER;
    case OP_POP:
        return in->opsize == 4 && in->ops[0].kind == OPK_REG ? C_INLINE : C_HELPER;
    case OP_JMP: case OP_CALL: case OP_RET: case OP_JCC:
        return in->opsize == 4 ? C_INLINE : C_HELPER;
    case OP_JCXZ: case OP_LOOP: case OP_LOOPE: case OP_LOOPNE:
        return in->opsize == 4 && in->adsize == 4 ? C_INLINE : C_HELPER;
    case OP_OUT:
        return C_INLINE;
    default:
        return C_HELPER;
    }
}

/* Segmented 16-bit protected mode: the real-mode set with 16-bit
 * addressing through DS, ES or SS (limit-checked at run time), plus the
 * 32-bit-operand forms of the straight-line ops the emitters handle at
 * any width (DOS/4GW's dispatcher moves dwords through 16-bit
 * segments). What touches IOPL (CLI/STI), a segment register, a far
 * target or an interrupt frame stays with the interpreter — a helper,
 * ending the block when it loads a segment (loads_segment), or refused. */
static int classify_seg16(const x86_insn *in) {
    if (classify_pm(in) == C_REFUSE) return C_REFUSE;
    if (loads_segment(in)) return C_HELPER;                  /* descriptor loads: the interpreter's */
    if (in->op == OP_POPF) return C_HELPER;                  /* IOPL and IF under privilege rules */
    if (in->adsize != 2) return C_HELPER;
    if (in->ea_valid && in->seg != S_DS && in->seg != S_ES && in->seg != S_SS) return C_HELPER;
    switch (in->op) {
    case OP_CLI: case OP_STI:
        return C_HELPER;              /* #GP above IOPL */
    case OP_OUT:
        return C_INLINE;
    default: break;
    }
    if (in->opsize == 4) {
        switch (in->op) {
        case OP_ADD: case OP_OR: case OP_ADC: case OP_SBB: case OP_AND: case OP_SUB: case OP_XOR: case OP_CMP:
        case OP_TEST: case OP_INC: case OP_DEC: case OP_NOT: case OP_NEG: case OP_MOV: case OP_XCHG: case OP_LEA:
        case OP_MOVZX: case OP_MOVSX: case OP_CBW: case OP_CWD:
            return C_INLINE;
        case OP_SHL: case OP_SAL: case OP_SHR: case OP_SAR:
            /* by CL only at 32 bits (emit_shift_cl); a byte op under a 66 prefix is not */
            if (in->ops[1].kind == OPK_REG) return in->ops[0].size == 4 ? C_INLINE : C_HELPER;
            return is_shift_inline(in) ? C_INLINE : C_HELPER;
        case OP_PUSH:
            return in->ops[0].kind != OPK_SREG ? C_INLINE : C_HELPER;
        case OP_POP:
            return in->ops[0].kind != OPK_SREG ? C_INLINE : C_HELPER;
        default:
            return C_HELPER;          /* 32-bit near transfers included: EIP would need to stay whole */
        }
    }
    return classify_op(in);
}

/* Exposed for tools/jittest's fuzzer: 0 refuse, 1 inline, 2 helper. */
int dbt_classify_op(const x86_insn *in) { return classify(in); }
int dbt_classify_op_pm(const x86_insn *in) { return classify_pm(in); }
int dbt_classify_op_seg16(const x86_insn *in) { return classify_seg16(in); }

/* Inline ops with a word-sized memory or stack access: the 286+ limit
 * check in front of it can raise #GP. Byte accesses cannot straddle. */
static int op_may_fault(const x86_insn *in) {
    switch (in->op) {
    case OP_PUSH: case OP_POP: case OP_CALL: case OP_RET: case OP_CALLF: case OP_RETF: case OP_JMPF:
        return 1;
    case OP_INT: case OP_INT3:
        return 1;                     /* the frame carries FLAGS: all of them must be materialized */
    case OP_MOVS: case OP_STOS: case OP_LODS: case OP_CMPS: case OP_SCAS:
        return in->ops[0].size >= 2 || s_seg16;    /* the slow path's helper can fault, frame and all */
    default: break;
    }
    for (int i = 0; i < 2; i++)
        if (in->ops[i].kind == OPK_MEM && (in->ops[i].size >= 2 || s_seg16)) return 1;   /* seg16: a limit is any size's problem */
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

/* ----------------------------------------------------------------------
 * Per-op emission (straight-line ops)
 * ---------------------------------------------------------------------- */
static void emit_helper_op(x86_dbt *dbt, emit_t *e, const x86_insn *in) {
    uint32_t idx = dbt->insn_used++;
    dbt->insn_pool[idx] = *in;
    dbt->insn_tag[idx] = (uint8_t)(!s_flat ? 0 : s_in_slow_chunk ? 2 : 1);
    dbt->insn_lin[idx] = dbt->cpu->seg[S_CS].base + s_cur_ip_start;
    dbt->helper_insns++;
    emit_mov_x64_x64(e, A64_W0, R_CPU);
    emit_mov_w32_imm32(e, A64_W1, idx);
    emit_thunk_args(e);
    emit_bl(e, (int32_t)s_exec_thunk_off - (int32_t)emit_pos(e));
}

/* OUT imm8/DX, AL/AX/EAX through the port thunk; exact (the interpreter's
 * own io_write and permission check) and the block goes on — unless the
 * helper says to leave: after the instruction (charged, EIP = ip_after)
 * or at it with the #GP it recorded. Both exits spill R_F, so an OUT is
 * an op_stores-style op with all flags live around it. */
static void emit_out(emit_t *e, const x86_insn *in) {
    int size = in->ops[1].size;
    emit_mov_x64_x64(e, A64_W0, R_CPU);
    if (in->ops[0].kind == OPK_IMM) emit_mov_w32_imm32(e, A64_W1, (in->ops[0].imm & 0xFF) | ((uint32_t)size << 16));
    else {
        emit_uxth_w32(e, A64_W1, R_GPR(R_DX));
        (void)emit_orr_w32_imm(e, A64_W1, A64_W1, (uint32_t)size << 16);
    }
    if (size == 4) emit_mov_w32_w32(e, A64_W2, R_GPR(R_AX));
    else if (size == 2) emit_uxth_w32(e, A64_W2, R_GPR(R_AX));
    else (void)emit_and_w32_imm(e, A64_W2, R_GPR(R_AX), 0xFF);
    emit_mov_w32_imm32(e, A64_W3, s_cur_ip_after);
    emit_movz_w32(e, A64_W4, (uint16_t)s_cur_n_done, 0);
    emit_bl(e, (int32_t)s_port_thunk_off - (int32_t)emit_pos(e));
    uint32_t go_on = emit_pos(e);
    emit_cbz_w32(e, A64_W0, 0);
    emit_cmp_w32_imm(e, A64_W0, 2);
    uint32_t leave = emit_pos(e);
    emit_b_cond(e, A64_COND_NE, 0);
    emit_mov_w32_imm32(e, A64_W3, s_cur_ip_start);              /* #GP: at the instruction, cpu->exc set */
    emit_b(e, (int32_t)s_fault_exit_off - (int32_t)emit_pos(e));
    emit_patch_cond19(e, leave, emit_pos(e));
    emit_sub_x64(e, R_CNT, R_CNT, A64_W4);                       /* leave after it */
    emit_str_w32_imm(e, A64_W3, R_CPU, OFF_EIP);
    emit_b(e, (int32_t)s_exit_eip_off - (int32_t)emit_pos(e));
    emit_patch_cond19(e, go_on, emit_pos(e));
}

static void emit_op(x86_dbt *dbt, emit_t *e, const x86_insn *in, int cls, uint32_t fmask) {
    nzcv_state nz_in = s_nzcv;          /* what the previous op left, for SETcc */
    s_nzcv.valid = 0;                   /* only the op just emitted can leave NZCV usable */
    if (cls == C_HELPER) { emit_helper_op(dbt, e, in); return; }

    ea_t ea = { 0, 0, 0 };
    if (in->ea_valid) emit_ea(e, in, &ea);
    int size = in->ops[0].size;
    const x86_operand *d = &in->ops[0];

    switch (in->op) {
    case OP_ADD: case OP_OR: case OP_ADC: case OP_SBB: case OP_AND: case OP_SUB: case OP_XOR: case OP_CMP:
    case OP_TEST: {
        a64_reg_t a = emit_read_operand(e, in, 0, &ea, W_VAL);
        a64_reg_t b = emit_read_operand(e, in, 1, &ea, W_SRC);
        int wr = (in->op != OP_CMP && in->op != OP_TEST);
        /* Pinned register destination of the full width: compute in place. */
        a64_reg_t res = (wr && d->kind == OPK_REG && (d->size == 4 || (d->size == 2 && !s_regs32))) ? a : W_VAL;
        emit_alu(e, in->op, size, a, b, res, fmask);
        if (wr && res == W_VAL) emit_write_operand(e, in, 0, &ea, W_VAL);
        /* Narrow ADC/SBB fix OF up after the table, so their NZCV is not
         * the guest's; a memory destination would clobber NZCV in the SMC
         * helper. Everything else leaves it usable for a following Jcc. */
        if ((size == 4 || (in->op != OP_ADC && in->op != OP_SBB)) && d->kind != OPK_MEM) {
            s_nzcv.valid = 1;
            s_nzcv.table = (in->op == OP_SUB || in->op == OP_CMP) ? T_SUB : T_ADD;
            s_nzcv.logical = in->op == OP_AND || in->op == OP_TEST || in->op == OP_OR || in->op == OP_XOR;
        }
        break;
    }
    case OP_INC: case OP_DEC: {
        a64_reg_t a = emit_read_operand(e, in, 0, &ea, W_VAL);
        a64_reg_t res = (d->kind == OPK_REG && (d->size == 4 || (d->size == 2 && !s_regs32))) ? a : W_VAL;
        emit_incdec(e, in->op == OP_INC, size, a, res, fmask);
        if (res == W_VAL) emit_write_operand(e, in, 0, &ea, W_VAL);
        if (d->kind != OPK_MEM) {
            s_nzcv.valid = 1;
            s_nzcv.table = in->op == OP_INC ? T_INC : T_DEC;   /* CF untouched: no carry conditions */
            s_nzcv.logical = 0;
        }
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
        int by_cl = in->ops[1].kind == OPK_REG;
        uint32_t cnt = in->ops[1].imm & 0xFF;
        if (!by_cl && cnt == 0) break;
        a64_reg_t a = emit_read_operand(e, in, 0, &ea, W_VAL);
        a64_reg_t res = (d->kind == OPK_REG && (d->size == 4 || (d->size == 2 && !s_regs32))) ? a : W_VAL;
        if (by_cl) emit_shift_cl(e, in->op, a, res, fmask);   /* 32-bit only (classify_flat) */
        else emit_shift_imm(e, in->op, size, cnt, a, res, fmask);
        if (res == W_VAL) emit_write_operand(e, in, 0, &ea, W_VAL);
        break;
    }
    case OP_DIV: case OP_IDIV: {
        a64_reg_t src = emit_read_operand(e, in, 0, &ea, W_SRC);
        if (size == 4) emit_div32(e, in->op == OP_IDIV, src);
        else emit_div16(e, size, in->op == OP_IDIV, src);
        break;
    }
    case OP_SETCC: {
        /* The previous op's NZCV is still the guest's only with no
         * memory operand: the flat range check in emit_ea writes it. */
        int fused = nz_in.valid && !in->ea_valid ? fuse_cond(in->cond, nz_in.table, nz_in.logical) : -1;
        a64_cond_t c = fused >= 0 ? (a64_cond_t)fused : emit_test_cond(e, in->cond);
        emit_cset_w32(e, W_VAL, c);
        emit_write_operand(e, in, 0, &ea, W_VAL);
        break;
    }
    case OP_MOVSEG:
        if (d->kind == OPK_SREG) {                                   /* MOV DS/ES, r/m16 (classify) */
            a64_reg_t v = emit_read_operand(e, in, 1, &ea, W_VAL);
            emit_load_seg_real(e, d->reg, v);
        } else {                                                     /* MOV r/m16, sreg */
            a64_reg_t v = emit_read_operand(e, in, 1, &ea, W_VAL);
            emit_write_operand(e, in, 0, &ea, v);
        }
        break;
    case OP_LDS: case OP_LES: {
        /* offset then selector, each read (and on the 286 limit-checked)
         * before anything is written, as the interpreter does */
        emit_read_mem(e, &ea, 2, W_VAL);
        emit_add_w32_imm(e, W_T0, ea.off, 2);
        (void)emit_and_w32_imm(e, W_T0, W_T0, 0xFFFF);
        ea_t ea2 = { ea.segp, W_T0, ea.seg };
        emit_read_mem(e, &ea2, 2, W_SRC);
        emit_write_reg(e, d->reg, 2, W_VAL);
        emit_load_seg_real(e, in->op == OP_LDS ? S_DS : S_ES, W_SRC);
        break;
    }
    case OP_LODS: case OP_STOS: case OP_MOVS: case OP_CMPS: case OP_SCAS:
        if (!in->rep) emit_string1(e, in, fmask);
        else if (in->op == OP_CMPS || in->op == OP_SCAS) emit_rep_cmp(e, in, fmask);
        else emit_rep_store(e, in);                                  /* MOVS, STOS (classify) */
        break;
    case OP_PUSHF: emit_pushf16(e); break;
    case OP_POPF:  emit_popf16(e, dbt->cpu->model); break;
    case OP_PUSHA: emit_pusha_flat(e); break;
    case OP_POPA:  emit_popa_flat(e); break;
    case OP_OUT:   emit_out(e, in); break;
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
        emit_write_reg(e, d->reg, d->size, ea.off);
        break;
    case OP_SHLD: case OP_SHRD: {
        /* CONTRACT (interp): CF = last bit out of the destination, OF =
         * sign change of the destination, AF cleared, SZP from the result. */
        uint32_t cnt = in->imm2 & 31;
        int shld = in->op == OP_SHLD;
        a64_reg_t a = emit_read_operand(e, in, 0, &ea, W_VAL);
        a64_reg_t b = emit_read_operand(e, in, 1, &ea, W_SRC);
        if (shld) emit_extr_w32(e, W_T0, a, b, 32 - cnt);    /* (a:b) >> (32-cnt) */
        else      emit_extr_w32(e, W_T0, b, a, cnt);         /* (b:a) >> cnt */
        if (fmask) {
            emit_tst_w32(e, W_T0, W_T0);                        /* N, Z; C = V = 0 */
            emit_flags_from_nzcv(e, T_ADD);
            emit_ubfx_w32(e, W_T1, a, shld ? 32 - cnt : cnt - 1, 1);
            emit_orr_w32(e, R_F, R_F, W_T1);
            emit_eor_w32(e, W_T1, a, W_T0);
            emit_lsr_w32_imm(e, W_T1, W_T1, 31);
            emit_orr_w32_lsl(e, R_F, R_F, W_T1, 11);
            if (fmask & X86_PF) emit_flag_pf(e, W_T0);
        }
        emit_write_operand(e, in, 0, &ea, W_T0);
        break;
    }
    case OP_MUL: case OP_IMUL:
        if (size < 4 && (in->op == OP_MUL || in->opcode2 != 0xAF)) {
            a64_reg_t src = emit_read_operand(e, in, 0, &ea, W_SRC);
            emit_mul16(e, size, in->op == OP_IMUL, src, fmask);
            break;
        }
        if (in->op == OP_MUL || in->opcode2 != 0xAF) {
            /* One-operand 32-bit MUL/IMUL: EDX:EAX = EAX * r/m32 (DOOM's
             * FixedMul). CONTRACT (interp): SZP from the LOW half, AF
             * clear, CF = OF = the high half is significant. */
            int is_signed = in->op == OP_IMUL;
            a64_reg_t src = emit_read_operand(e, in, 0, &ea, W_SRC);
            if (is_signed) emit_smull(e, W_T0, R_GPR(R_AX), src);
            else emit_umull(e, W_T0, R_GPR(R_AX), src);
            emit_lsr_x64_imm(e, R_GPR(R_DX), W_T0, 32);
            if (fmask) {
                emit_tst_w32(e, W_T0, W_T0);
                emit_flags_from_nzcv(e, T_ADD);
                if (fmask & X86_PF) emit_flag_pf(e, W_T0);
                if (is_signed) emit_cmp_x64_w32_sxtw(e, W_T0, W_T0);
                else (void)emit_subs_w32_imm(e, A64_WZR, R_GPR(R_DX), 0);
                emit_cset_w32(e, W_T2, A64_COND_NE);
                emit_orr_w32(e, R_F, R_F, W_T2);
                emit_orr_w32_lsl(e, R_F, R_F, W_T2, 11);
            }
            emit_mov_w32_w32(e, R_GPR(R_AX), W_T0);
            break;
        }
        /* fall through: IMUL r32, r/m32 */
    case OP_IMUL3: {
        /* 32-bit two- and three-operand IMUL. CONTRACT (interp, 386):
         * SZP from the HIGH half of the product, AF clear, CF = OF = the
         * product does not fit in 32 bits. */
        a64_reg_t a, b;
        if (in->op == OP_IMUL) {
            a = emit_read_operand(e, in, 0, &ea, W_VAL);
            b = emit_read_operand(e, in, 1, &ea, W_SRC);
        } else {
            a = emit_read_operand(e, in, 1, &ea, W_VAL);
            emit_mov_w32_imm32(e, W_SRC, in->imm2);
            b = W_SRC;
        }
        emit_smull(e, W_T0, a, b);                               /* X6 = full product */
        if (fmask) {
            emit_asr_x64_imm(e, W_T2, W_T0, 32);                 /* W4 = high half */
            emit_tst_w32(e, W_T2, W_T2);
            emit_flags_from_nzcv(e, T_ADD);                      /* SF, ZF */
            if (fmask & X86_PF) emit_flag_pf(e, W_T2);
            emit_cmp_x64_w32_sxtw(e, W_T0, W_T0);
            emit_cset_w32(e, W_T2, A64_COND_NE);
            emit_orr_w32(e, R_F, R_F, W_T2);                     /* CF */
            emit_orr_w32_lsl(e, R_F, R_F, W_T2, 11);             /* OF */
        }
        emit_write_reg(e, d->reg, 4, W_T0);
        break;
    }
    case OP_MOVZX: case OP_MOVSX: {
        const x86_operand *s = &in->ops[1];
        a64_reg_t v;
        if (s->kind == OPK_MEM) { emit_read_mem(e, &ea, s->size, W_VAL); v = W_VAL; }   /* loads zero-extend */
        else v = emit_read_reg(e, s->reg, s->size, W_VAL);   /* a word register comes back whole */
        if (in->op == OP_MOVSX) {
            if (s->size == 1) emit_sxtb_w32(e, W_VAL, v); else emit_sxth_w32(e, W_VAL, v);
            v = W_VAL;
        } else if (s->size == 2 && s->kind == OPK_REG) {
            emit_uxth_w32(e, W_VAL, v);
            v = W_VAL;
        }
        emit_write_reg(e, d->reg, d->size, v);
        break;
    }
    case OP_NOP:
        break;
    case OP_CBW:
        if (in->opsize == 4) { emit_sxth_w32(e, R_GPR(R_AX), R_GPR(R_AX)); break; }   /* CWDE */
        emit_sbfx_w32(e, W_T0, R_GPR(R_AX), 0, 8);
        emit_uxth_w32(e, W_T0, W_T0);
        emit_set16(e, R_AX, W_T0);
        break;
    case OP_CWD:
        if (in->opsize == 4) { emit_asr_w32_imm(e, R_GPR(R_DX), R_GPR(R_AX), 31); break; }   /* CDQ */
        emit_sbfx_w32(e, W_T0, R_GPR(R_AX), 15, 1);
        emit_uxth_w32(e, W_T0, W_T0);
        emit_set16(e, R_DX, W_T0);
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
        emit_push_stk(e, v, in->opsize);
        break;
    }
    case OP_POP:
        if (d->kind == OPK_SREG) {                                   /* POP DS/ES (classify) */
            emit_pop16(e, W_VAL);
            emit_load_seg_real(e, d->reg, W_VAL);
            break;
        }
        /* POP [mem] on the 386 leaves SP where it was if the destination
         * faults (CONTRACT, measured): check it before the pop. The EA
         * never involves SP with 16-bit addressing. */
        if (d->kind == OPK_MEM && s_regs32 && !s_flat) {
            if (s_seg16) emit_limit_check(e, ea.seg, ea.off, d->size);
            else if (d->size == 2) emit_wrap_check(e, ea.segp, ea.off, W_VAL, 1, 1);   /* 286+: the chunk is a #GP, never returns */
            s_ea_checked = 1;
        }
        emit_pop_stk(e, W_VAL, in->opsize);
        emit_write_operand(e, in, 0, &ea, W_VAL);
        break;
    default:
        break;   /* classify() keeps everything else out */
    }
    /* A flat or paged memory operand has a slow path — the helper — which
     * leaves no NZCV behind for a following Jcc to fuse on. */
    if ((s_flat || s_paged) && in->ea_valid && in->op != OP_LEA) s_nzcv.valid = 0;
}

/* ----------------------------------------------------------------------
 * Control flow
 * ---------------------------------------------------------------------- */

/* Key of the static target ip within the block's CS. */
static uint64_t target_key(const x86_cpu *cpu, uint32_t ip) {
    if (s_flat) return dbt_key(cpu->seg[S_CS].sel, cpu->seg[S_CS].base + ip) | s_mode_bits;
    if (s_v86) return dbt_key(cpu->seg[S_CS].sel, cpu->seg[S_CS].base + (ip & 0xFFFF)) | s_mode_bits;
    uint32_t lin = (cpu->seg[S_CS].base + (ip & 0xFFFF)) & cpu->a20_mask;
    return dbt_key(cpu->seg[S_CS].sel, lin) | (s_seg16 ? s_mode_bits : 0);
}

/* X0 = key for a run-time ip in W register `ip` (16-bit canonical):
 * lin = (cs.base + ip) & a20, CS selector in the top half. The A20 mask
 * only matters when cs.base + 0xFFFF can pass 1 MB with A20 off. */
static void emit_dynamic_key(emit_t *e, const x86_cpu *cpu, a64_reg_t ip) {
    uint32_t base = cpu->seg[S_CS].base;
    if (s_flat) {   /* full EIP; linear wraps at 4 GB */
        if (base) { emit_mov_w32_imm32(e, A64_W0, base); emit_add_w32(e, A64_W0, A64_W0, ip); }
        else emit_mov_w32_w32(e, A64_W0, ip);
        emit_movk_x64(e, A64_W0, cpu->seg[S_CS].sel, 32);
        emit_movk_x64(e, A64_W0, (uint16_t)(s_mode_bits >> 48), 48);
        return;
    }
    if (s_regs32) { emit_uxth_w32(e, W_T0, ip); ip = W_T0; }   /* may be a raw 32-bit register */
    emit_mov_w32_imm32(e, A64_W0, base);
    emit_add_w32(e, A64_W0, A64_W0, ip);
    if (!s_v86 && base + 0xFFFF > 0xFFFFF && cpu->a20_mask == 0xFFFFF)
        (void)emit_and_w32_imm(e, A64_W0, A64_W0, 0xFFFFF);
    emit_movk_x64(e, A64_W0, cpu->seg[S_CS].sel, 32);
    if (s_seg16 || s_v86) emit_movk_x64(e, A64_W0, (uint16_t)(s_mode_bits >> 48), 48);
}

/* Inline part of a conditional: guts + test + B.cond toward the taken
 * arm. Returns the B.cond offset to patch. */
static uint32_t emit_cond_side_branch(emit_t *e, const x86_insn *in) {
    uint32_t patch;
    /* This emitter writes NZCV itself (the TST in the unfused JCC path and
     * in LOOPE/LOOPNE), and the LOOP family does not go through emit_op,
     * which is what normally clears the flag. Snapshot and invalidate. */
    nzcv_state nz = s_nzcv;
    s_nzcv.valid = 0;
    switch (in->op) {
    case OP_JCC: {
        int fused = nz.valid ? fuse_cond(in->cond, nz.table, nz.logical) : -1;
        a64_cond_t c = fused >= 0 ? (a64_cond_t)fused : emit_test_cond(e, in->cond);
        patch = emit_pos(e);
        emit_b_cond(e, c, 0);
        break;
    }
    case OP_JCXZ: {
        /* must be emitted before the patch site is taken; JECXZ tests all of ECX */
        a64_reg_t cx = in->adsize == 4 ? R_GPR(R_CX) : emit_reg16(e, R_CX, W_T0);
        patch = emit_pos(e);
        emit_cbz_w32(e, cx, 0);
        break;
    }
    default: {   /* LOOP family: CX = (CX-1) & FFFF (ECX - 1 with a 32-bit address size), taken when != 0 [&& ZF cond] */
        a64_reg_t cnt = W_T2;
        if (in->adsize == 4) {
            emit_sub_w32_imm(e, R_GPR(R_CX), R_GPR(R_CX), 1);
            cnt = R_GPR(R_CX);
        } else {
            emit_sub_w32_imm(e, W_T2, R_GPR(R_CX), 1);
            (void)emit_and_w32_imm(e, W_T2, W_T2, 0xFFFF);
            emit_set16(e, R_CX, W_T2);
        }
        if (in->op == OP_LOOP) {
            patch = emit_pos(e);
            emit_cbnz_w32(e, cnt, 0);
        } else {
            emit_cbz_w32(e, cnt, 12);                         /* skip the flag test + branch */
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
    if (!s_v86 && cpu->a20_mask == 0xFFFFF) (void)emit_and_w32_imm(e, A64_W0, A64_W0, 0xFFFFF);
    emit_orr_x64_lsl(e, A64_W0, A64_W0, W_SRC, 32);
    if (s_v86) emit_movk_x64(e, A64_W0, (uint16_t)(s_mode_bits >> 48), 48);
}
static void emit_far_ender(x86_dbt *dbt, emit_t *e, const x86_insn *in, uint32_t ip_after) {
    x86_cpu *cpu = dbt->cpu;
    if (in->op == OP_RETF) {
        /* Both slots are fetched before SP moves so a 286 limit fault on
         * either leaves SP intact, as the interpreter's frame precheck does. */
        a64_reg_t sp = emit_reg16(e, R_SP, W_T2);
        emit_add_w32_imm(e, W_T0, sp, 2);
        (void)emit_and_w32_imm(e, W_T0, W_T0, 0xFFFF);
        emit_wrap_check(e, R_SSP, W_T0, W_SRC, 0, 0);
        emit_ldrh_reg_uxtw(e, W_SRC, R_SSP, W_T0);
        emit_wrap_back(e);
        emit_wrap_check(e, R_SSP, sp, W_VAL, 0, 0);
        emit_ldrh_reg_uxtw(e, W_VAL, R_SSP, sp);
        emit_wrap_back(e);
        int32_t adj = 4 + (in->ops[0].kind == OPK_IMM ? (int32_t)(in->ops[0].imm & 0xFFFF) : 0);
        emit_add_w32_imm_any(e, W_T0, sp, adj, W_T1);
        (void)emit_and_w32_imm(e, W_T0, W_T0, 0xFFFF);
        emit_set16(e, R_SP, W_T0);
        emit_load_cs_dynamic(e, cpu);
        emit_dynamic_tail(e, dbt->exit_stub_off);
        return;
    }
    int is_imm = in->ops[0].kind == OPK_IMM;
    if (!is_imm) {
        /* ptr16:16 in memory: offset then selector, each wrap-checked on its own */
        ea_t ea = { 0, 0, 0 };
        emit_ea(e, in, &ea);
        emit_read_mem(e, &ea, 2, W_VAL);
        emit_add_w32_imm(e, W_T0, ea.off, 2);
        (void)emit_and_w32_imm(e, W_T0, W_T0, 0xFFFF);
        ea_t ea2 = { ea.segp, W_T0, ea.seg };
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
        uint32_t lin = ((uint32_t)sel << 4) + off;
        if (!s_v86) lin &= cpu->a20_mask;
        emit_edge(dbt, e, dbt_key(sel, lin) | (s_v86 ? s_mode_bits : 0));
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
    emit_smc_check_x3(e, 2);
    if (s_wrap_exact) emit_wrap_back(e);
    emit_sub_w32_imm(e, W_T2, R_GPR(R_SP), 2);
    (void)emit_and_w32_imm(e, W_T2, W_T2, 0xFFFF);
    emit_set16(e, R_SP, W_T2);
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
            ea_t ea = { 0, 0, 0 };
            if (in->ea_valid) emit_ea(e, in, &ea);
            a64_reg_t t = emit_read_operand(e, in, 0, &ea, W_VAL);
            emit_dynamic_key(e, cpu, t);
            emit_dynamic_tail(e, dbt->exit_stub_off);
        }
        return;
    case OP_CALL: {
        ea_t ea = { 0, 0, 0 };
        a64_reg_t t = 0;
        int dyn = in->ops[0].kind != OPK_IMM;
        if (dyn) {
            if (in->ea_valid) emit_ea(e, in, &ea);
            t = emit_read_operand(e, in, 0, &ea, W_VAL);
            if (t != W_VAL) { emit_mov_w32_w32(e, W_VAL, t); t = W_VAL; }
        }
        emit_mov_w32_imm32(e, W_SRC, ip_after);
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
        if (in->ops[0].kind == OPK_IMM && s_flat) {
            emit_add_w32_imm_any(e, R_GPR(R_SP), R_GPR(R_SP), (int32_t)(in->ops[0].imm & 0xFFFF), W_T1);
        } else if (in->ops[0].kind == OPK_IMM) {
            emit_add_w32_imm_any(e, W_T2, R_GPR(R_SP), (int32_t)(in->ops[0].imm & 0xFFFF), W_T1);
            (void)emit_and_w32_imm(e, W_T2, W_T2, 0xFFFF);
            emit_set16(e, R_SP, W_T2);
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

/* ----------------------------------------------------------------------
 * Protected-mode blocks (milestone 1: every instruction a helper).
 *
 * Everything the real-mode backend inlines for control transfers — the
 * interrupt frame, the IVT read, a far target's base from its selector —
 * is real-mode reasoning, so none of it is used here. A block is a run of
 * helper calls on pre-decoded instructions; what the translation saves is
 * the interpreter's fetch and decode. It ends after a near control
 * transfer (the helper has set EIP; the tail probes for the block there),
 * before anything classify_pm refuses (the run loop steps it), or at the
 * end of what the code segment's limit allows.
 * ---------------------------------------------------------------------- */

/* X0 = key for cpu->eip after a helper moved it. */
static void emit_pm_dynamic_key(emit_t *e, const x86_cpu *cpu, uint64_t mode_bits) {
    emit_ldr_w32_imm(e, A64_W0, R_CPU, OFF_EIP);
    emit_mov_w32_imm32(e, W_T0, cpu->seg[S_CS].base);
    emit_add_w32(e, A64_W0, A64_W0, W_T0);           /* linear wraps at 4 GB, as the CPU's does */
    emit_movk_x64(e, A64_W0, cpu->seg[S_CS].sel, 32);
    emit_movk_x64(e, A64_W0, (uint16_t)(mode_bits >> 48), 48);
}

/* X0 = the key of wherever a V86 helper left CS:IP (a far transfer run
 * by the interpreter moves CS too): live selector and base, 16-bit IP. */
static void emit_v86_dynamic_key(emit_t *e, uint64_t mode_bits) {
    emit_ldr_w32_imm(e, A64_W0, R_CPU, OFF_EIP);
    emit_uxth_w32(e, A64_W0, A64_W0);
    emit_ldr_w32_imm(e, W_T0, R_CPU, OFF_SEG_BASE(S_CS));
    emit_add_w32(e, A64_W0, A64_W0, W_T0);
    emit_ldrh_imm(e, W_T1, R_CPU, OFF_SEG_SEL(S_CS));
    emit_orr_x64_lsl(e, A64_W0, A64_W0, W_T1, 32);
    emit_movk_x64(e, A64_W0, (uint16_t)(mode_bits >> 48), 48);
}

static uint8_t *translate_pm(x86_dbt *dbt, uint64_t key) {
    x86_cpu *cpu = dbt->cpu;
    s_flat = 0;
    s_seg16 = 0;
    const x86_seg *cs = &cpu->seg[S_CS];
    /* A20 off in protected mode would fold linear addresses under the
     * block's feet; nothing we run does it, so do not translate it. */
    if (cpu->a20_mask != 0xFFFFFFFFu) return NULL;
    uint64_t mode_bits = dbt_cpu_mode_bits(cpu);
    uint32_t ipmask = cs->big ? 0xFFFFFFFFu : 0xFFFFu;

    x86_insn decs[MAX_BLOCK_INSNS];
    uint32_t ip_afters[MAX_BLOCK_INSNS];
    uint32_t ip = cpu->eip, start_ip = ip, n_ops = 0;
    int ends_dynamic = 0;
    uint8_t buf[16];
    x86_dec_ctx ctx = { buf, cpu->model, cs->big };
    while (n_ops < MAX_BLOCK_INSNS) {
        x86_insn *in = &decs[n_ops];
        if (ip > cs->limit) break;
        uint32_t lin = cs->base + ip;
        if (lin >= cpu->mem_size || cpu->mem_size - lin < 16) break;   /* the interpreter's open bus */
        for (int i = 0; i < 16; i++) buf[i] = cpu->mem[lin + (uint32_t)i];
        if (!x86_decode(&ctx, in)) break;
        if (ip + in->len - 1 > cs->limit || ip + in->len - 1 < ip) break;   /* straddles the limit: #GP is the interpreter's */
        if (((ip + in->len) & ipmask) != ip + in->len) break;              /* 16-bit IP wrap */
        if (classify_pm(in) == C_REFUSE) { dbt->refused_by_op[in->op]++; break; }
        ip += in->len;
        ip_afters[n_ops++] = ip;
        if (is_near_transfer(in->op)) { ends_dynamic = 1; break; }
    }
    if (n_ops == 0) return NULL;

    emit_t e = { .buf = dbt->code_buf, .offset = dbt->code_used, .capacity = CODE_BUF_SIZE };
    uint8_t *entry = dbt->code_buf + e.offset;
    s_cur_lin = dbt_key_lin(key);
    uint32_t budget_patch = emit_pos(&e);
    emit_tbnz_x64(&e, R_CNT, 63, 0);
    for (uint32_t i = 0; i < n_ops; i++) {
        s_cur_ip_after = ip_afters[i];
        s_cur_ip_start = ip_afters[i] - decs[i].len;
        s_cur_n_done = i + 1;
        if (decs[i].op == OP_OUT) emit_out(&e, &decs[i]);
        else emit_helper_op(dbt, &e, &decs[i]);
    }
    emit_tail_prologue(&e, n_ops);
    if (ends_dynamic) {
        emit_pm_dynamic_key(&e, cpu, mode_bits);
        emit_dynamic_tail(&e, dbt->exit_stub_off);
    } else {
        emit_edge(dbt, &e, dbt_key(cs->sel, cs->base + ip) | mode_bits);
    }
    emit_patch_tb14(&e, budget_patch, emit_pos(&e));
    emit_mov_x64_imm64(&e, A64_W0, key);
    emit_b(&e, (int32_t)dbt->exit_stub_off - (int32_t)emit_pos(&e));

    dbt->code_used = e.offset;
    __builtin___clear_cache((char *)entry, (char *)(dbt->code_buf + e.offset));
    uint32_t lin = dbt_key_lin(key);
    dbt_mark_block_bytes(dbt, lin, lin + (ip - start_ip));
    dbt_watch_cs_desc(dbt);
    return entry;
}

/* An inline op whose immediate keeps being patched (every byte of it at
 * SMC_VOLATILE heat) reads it from memory at run time: returns the
 * immediate's linear address, or 0 to bake it in as usual. Only ops that
 * read the immediate as a value through emit_read_operand, and only with
 * no memory operand — a flat memory operand has a slow path that replays
 * the pooled decode, immediate and all. */
static uint32_t dyn_imm_at(const x86_dbt *dbt, const x86_insn *in, uint32_t insn_lin) {
    switch (in->op) {
    case OP_ADD: case OP_OR: case OP_ADC: case OP_SBB: case OP_AND: case OP_SUB: case OP_XOR: case OP_CMP:
    case OP_TEST: case OP_MOV:
        break;
    default:
        return 0;
    }
    if (in->ea_valid || in->ops[1].kind != OPK_IMM || !in->ops[1].imm_enc) return 0;
    uint32_t at = insn_lin + X86_IMM_AT(in->ops[1].imm_enc), n = X86_IMM_LEN(in->ops[1].imm_enc);
    for (uint32_t k = 0; k < n; k++)
        if (!dbt_smc_hot(dbt->smc_heat, dbt->smc_win, at + k, dbt_smc_window(dbt->cpu))) return 0;
    return at;
}

/* Out-of-range paths of a flat block: the whole instruction through the
 * exec thunk, then back after its inline code — or, for a block ender
 * (already charged), on to wherever the helper left EIP. */
static void emit_flat_slow_chunks(x86_dbt *dbt, emit_t *e) {
    for (uint32_t k = 0; k < s_nfslow; k++) {
        flat_slow_t *f = &s_fslow[k];
        emit_patch_cond19(e, f->patch_off, emit_pos(e));
        s_cur_ip_after = f->ip_after;
        s_cur_n_done = f->n_done;
        s_in_slow_chunk = 1;
        emit_helper_op(dbt, e, &f->in);
        s_in_slow_chunk = 0;
        if (f->ender) {
            if (s_v86) emit_v86_dynamic_key(e, s_mode_bits);
            else emit_pm_dynamic_key(e, dbt->cpu, s_mode_bits);
            emit_dynamic_tail(e, dbt->exit_stub_off);
        } else {
            emit_b(e, (int32_t)f->back_off - (int32_t)emit_pos(e));
        }
    }
    s_nfslow = 0;
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
    if (cpu->hle && cpu->seg[S_CS].base == ((uint32_t)cpu->hle_seg << 4)) return NULL;

    s_v86 = (key & KEY_V86) != 0;
    s_paged = (key & KEY_PAGED) != 0;
    s_iopl3 = (key & KEY_IOPL3) != 0;
    if (cpu->pmode && !s_v86 && (!(key & (KEY_FLAT | KEY_SEG16)) || cpu->a20_mask != 0xFFFFFFFFu)) return translate_pm(dbt, key);
    s_flat = cpu->pmode && !s_v86 && (key & KEY_FLAT) != 0;    /* from here on: real mode or V86, a flat block, or a segmented 16-bit one */
    s_seg16 = cpu->pmode && !s_v86 && !s_flat;
    s_mode_bits = key & 0x7FFF000000000000ull;
    s_esnull = (key & KEY_ESNULL) != 0;
    s_dsnull = (key & KEY_DSNULL) != 0;
    s_pg_user = s_flat && s_paged && (cpu->seg[S_CS].sel & 3) == 3;
    uint32_t code_page = 0, code_delta = 0;
    if (s_paged) {
        /* A paged block (V86, or flat protected mode) stays on one code
         * page. Its key is linear; its bytes, and their code-bitmap
         * marks, are wherever the page maps — one-to-one, or remapped
         * (UMB code, a memory manager mapped high), which phys_alias lets
         * the SMC sweep find. dbt_tlb_flushed re-peeks it. */
        int user = s_v86 || s_pg_user;
        code_page = (cpu->seg[S_CS].base + (s_flat ? cpu->eip : (cpu->eip & 0xFFFF))) & 0xFFFFF000u;
        uint32_t phys = x86_page_peek(cpu, code_page, user);
        if (phys == X86_PG_BAD || phys + 0xFFFu >= cpu->mem_size) return NULL;
        if (!dbt_note_code_page(dbt, code_page >> 12, phys >> 12, user)) return NULL;
        if (phys != (code_page & cpu->a20_mask)) {
            uint32_t *alias = &dbt->phys_alias[phys >> 12];
            if (*alias && *alias != (code_page >> 12) + 1) return NULL;   /* one alias per physical page */
            *alias = (code_page >> 12) + 1;
            code_delta = phys - code_page;
        }
        (void)x86_phys_rd8(cpu, code_page | ((cpu->seg[S_CS].base + cpu->eip) & 0xFFF));   /* the fetch's walk: accessed bits */
    }

    emit_t e = { .buf = dbt->code_buf, .offset = dbt->code_used, .capacity = CODE_BUF_SIZE };
    uint8_t *entry = dbt->code_buf + e.offset;

    /* ---- Phase 1: decode ---- */
    x86_insn decs[MAX_BLOCK_INSNS];
    uint32_t ip_afters[MAX_BLOCK_INSNS];
    uint8_t  cls[MAX_BLOCK_INSNS];
    uint8_t  role[MAX_BLOCK_INSNS];
    uint32_t dyn_lin[MAX_BLOCK_INSNS];
    enum { R_PLAIN, R_UNCOND, R_COND, R_HELPER_END };
    uint32_t ip = s_flat ? cpu->eip : cpu->eip & 0xFFFF;
    uint32_t start_ip = ip;
    uint32_t n_ops = 0;
    uint8_t buf[16];
    x86_dec_ctx ctx = { buf, cpu->model, (uint8_t)s_flat };
    s_cls_model = cpu->model;

    while (n_ops < MAX_BLOCK_INSNS) {
        x86_insn *in = &decs[n_ops];
        if (s_flat && s_paged) {
            uint32_t lin = cpu->seg[S_CS].base + ip;
            if ((lin & 0xFFFFF000u) != code_page) break;
            memcpy(buf, cpu->mem + lin + code_delta, 16);
        } else if (s_flat) {
            /* flat CS: no limit to hit short of 4 GB; stay inside memory */
            uint32_t lin = cpu->seg[S_CS].base + ip;
            if (lin >= cpu->mem_size || cpu->mem_size - lin < 16) break;
            memcpy(buf, cpu->mem + lin, 16);
        } else if (s_paged) {
            uint32_t lin = cpu->seg[S_CS].base + ip;
            if ((lin & 0xFFFFF000u) != code_page) break;
            memcpy(buf, cpu->mem + lin + code_delta, 16);          /* identity: the mirror applies A20 */
        } else {
            fetch_at(cpu, ip, buf);
        }
        if (!x86_decode(&ctx, in)) break;
        if (!s_flat && ip + in->len > 0x10000) break;  /* IP wrap: leave it to the interp */
        if (s_paged && ((cpu->seg[S_CS].base + ip + in->len - 1) & 0xFFFFF000u) != code_page) break;   /* runs off the page */
        if (!s_flat) {
            /* An instruction whose bytes keep being patched (FreeCOM
             * rewrites its INT n's number before every call) stays out of
             * blocks: the interpreter runs it from memory as it is, and
             * the patches no longer invalidate the code around it. */
            uint32_t at = cpu->seg[S_CS].base + ip + code_delta;
            if (!s_v86 && !s_seg16) at &= cpu->a20_mask;
            uint8_t now = dbt_smc_window(cpu);
            int hot = 0;
            for (uint32_t k = 0; k < in->len && at + k < cpu->mem_size; k++)
                if (dbt_smc_hot(dbt->smc_heat, dbt->smc_win, at + k, now)) { hot = 1; break; }
            if (hot) { dbt->smc_hot_refusals++; break; }
        }
        if (s_seg16 && ip + in->len - 1 > cpu->seg[S_CS].limit) break;   /* past the code limit: #GP is the interpreter's */
        int c = s_flat ? classify_flat(in) : s_seg16 ? classify_seg16(in) : classify(in);
        if (s_v86) c = classify_v86(in, c);
        if (s_flat && c == C_INLINE) {
            if (s_esnull && in->ea_valid && in->seg == S_ES) c = C_HELPER;          /* #GP: the interpreter's */
            if (s_dsnull && in->ea_valid && in->seg == S_DS) c = C_HELPER;
            if (s_paged && (in->op == OP_PUSHA || in->op == OP_POPA)) c = C_HELPER;  /* two accesses */
        }
        if (cpu->model == X86_MODEL_286 && in->len > 10) c = C_REFUSE;   /* #GP: the interpreter's */
        if (c == C_REFUSE) {
            dbt->refused_by_op[in->op]++;
            break;
        }
        int r = R_PLAIN;
        if (c == C_INLINE && is_uncond_ender(in->op)) r = R_UNCOND;
        else if (c == C_INLINE && is_cond_ender(in->op)) r = R_COND;
        else if ((s_flat || s_seg16) && c == C_HELPER && (is_near_transfer(in->op) || loads_segment(in))) r = R_HELPER_END;
        else if (s_v86 && c == C_HELPER && (is_near_transfer(in->op) || in->op == OP_CALLF || in->op == OP_RETF || in->op == OP_JMPF))
            r = R_HELPER_END;                          /* paged V86: a transfer run by the interpreter */
        cls[n_ops] = (uint8_t)c;
        role[n_ops] = (uint8_t)r;
        dyn_lin[n_ops] = s_flat && !s_paged && c == C_INLINE ? dyn_imm_at(dbt, in, cpu->seg[S_CS].base + ip) : 0;
        ip_afters[n_ops] = ip + in->len;
        n_ops++;
        ip += in->len;
        if (r == R_UNCOND || r == R_HELPER_END) break;
        if (r == R_COND && ip - start_ip >= SUPERBLOCK_BYTE_CAP) break;
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
            if (cpu->model >= X86_MODEL_286 && !s_flat && op_may_fault(&decs[i])) rd |= ARITH;
            /* A store can leave the block after the op (SMC): all live out. */
            fmask[i] = live | (cls[i] == C_INLINE && op_stores(&decs[i]) ? ARITH : 0);
            live = (fmask[i] & ~wr) | rd;
        }
    }

    /* ---- Phase 3: emit ----
     * Entry: budget check. Exhausted → out-of-line exit with our own key. */
    s_cur_lin = dbt_key_lin(key);
    s_wrap_exact = cpu->model < X86_MODEL_286;
    s_regs32 = cpu->model >= X86_MODEL_386;
    s_nwrap = 0;
    s_nfault = 0;
    s_nzcv.valid = 0;                 /* nothing carries into a block: its first op may be a Jcc */
    s_ea_checked = 0;
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
        s_cur_insn = in;
        s_cur_ender = 0;
        s_ea_checked = 0;                 /* per instruction: never leaks to the next */
        if (role[i] == R_UNCOND || (role[i] == R_COND && i == n_ops - 1)) {
            emit_tail_prologue(&e, n_ops);
            /* The whole block is charged now: a thunk, fault or slow-path
             * exit inside the ender must not charge it again. */
            s_cur_n_done = 0;
            s_cur_ender = 1;
            emit_branch_ender(dbt, &e, in, ip_afters[i]);
            final_by_branch = 1;
            break;
        }
        if (role[i] == R_COND) {
            sides[n_sides].patch_off = emit_cond_side_branch(&e, in);
            sides[n_sides].insns = i + 1;
            sides[n_sides].op = i;
            n_sides++;
            continue;
        }
        if (role[i] == R_HELPER_END) {
            /* A helper that moved EIP (a 16-bit near transfer) or loaded a
             * segment: leave from where it left EIP. After a segment load
             * the flat assumption is off, so go back to the run loop,
             * which rebuilds the key, rather than probing with ours. */
            emit_helper_op(dbt, &e, in);
            emit_tail_prologue(&e, n_ops);
            if (s_v86) emit_v86_dynamic_key(&e, s_mode_bits);
            else emit_pm_dynamic_key(&e, cpu, s_mode_bits);
            if (loads_segment(in)) emit_b(&e, (int32_t)dbt->exit_stub_off - (int32_t)emit_pos(&e));
            else emit_dynamic_tail(&e, dbt->exit_stub_off);
            final_by_branch = 1;
            break;
        }
        uint32_t first_slow = s_nfslow;
        s_dyn_imm_lin = dyn_lin[i];
        emit_op(dbt, &e, in, cls[i], fmask[i]);
        s_dyn_imm_lin = 0;
        for (uint32_t k = first_slow; k < s_nfslow; k++) s_fslow[k].back_off = emit_pos(&e);
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

    /* Segment-wrap slow paths for the word accesses above, and the flat
     * out-of-range paths. */
    emit_wrap_slow_chunks(&e);
    emit_flat_slow_chunks(dbt, &e);
    emit_fault_chunks(&e);

    /* Budget-exhausted exit: nothing executed, next = this block. */
    emit_patch_tb14(&e, budget_patch, emit_pos(&e));
    emit_mov_x64_imm64(&e, A64_W0, key);
    emit_b(&e, (int32_t)dbt->exit_stub_off - (int32_t)emit_pos(&e));

    dbt->code_used = e.offset;
    __builtin___clear_cache((char *)entry, (char *)(dbt->code_buf + e.offset));

    uint32_t lin = dbt_key_lin(key);
    uint32_t skip[2 * MAX_BLOCK_INSNS], nskip = 0;
    for (uint32_t i = 0; i < n_ops; i++)
        if (dyn_lin[i]) {
            skip[2 * nskip] = dyn_lin[i];
            skip[2 * nskip + 1] = dyn_lin[i] + X86_IMM_LEN(decs[i].ops[1].imm_enc);
            nskip++;
        }
    lin += code_delta;                               /* the bitmap is physical */
    dbt_mark_block_bytes_except(dbt, lin, lin + (ip - start_ip), skip, nskip);
    if (s_flat || s_seg16) dbt_watch_cs_desc(dbt);
    return entry;
}
