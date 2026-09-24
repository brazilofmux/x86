/* dbt_x64.c — x86-64 backend for the x86 DBT: host code for a block plan.
 *
 * The plan comes from dbt_translate.c (decode, classes, roles, flag
 * liveness); dbt_arch_emit_block at the bottom turns it into x86-64.
 * Same-ISA translation: a guest instruction is mostly its own host
 * encoding with the operands re-registered, memory operands and all,
 * and the guest's arithmetic flags are the host's RFLAGS — no flag
 * tables, no parity lookups, AF and PF for free.
 *
 * Host register convention (System V ABI underneath), pinned across
 * blocks and chains:
 *   RAX RCX RDX RBX = guest EAX ECX EDX EBX — this mapping and no other,
 *                     so that AH CH DH BH are the host's own high bytes
 *   RSI RDI RBP     = guest ESI EDI EBP
 *   R12             = guest ESP (the host stack is RSP, as it must be)
 *   R13             = x86_cpu *cpu
 *   R14             = cpu->mem: flat blocks address [R14 + offset]
 *   R15             = cpu->mem + DS.base, the hottest segment
 *   R8..R11         = scratch; R10 = the next block's key at a tail
 *   RFLAGS          = guest DF always; the guest arithmetic flags while
 *                     a block has produced them (see below)
 * Below the 386 the pinned registers are canonical zero-extended 16-bit
 * values, as in dbt_a64.c, so any of them is a 64-bit index as it stands.
 * SS and ES host pointers are built from cpu->seg[].base per use: with
 * fifteen usable registers, seven guest registers, the cpu, the memory
 * base and four scratch, one segment pointer is what fits.
 *
 * Flags. At every block boundary the guest's arithmetic flags are in
 * cpu->jit_flags (an RFLAGS image; the low FLAGS byte is the 8080's, so
 * PUSHFQ gives the guest's layout directly) and RFLAGS holds nothing.
 * Inside a block, after an instruction that produces flags, they live
 * in RFLAGS and a following consumer — Jcc, SETcc, ADC — uses them
 * natively. Before the block leaves them (a tail, a side exit) they are
 * saved: PUSHFQ; POP [jit_flags]. The point of the convention is that
 * every piece of bookkeeping the translator emits — the budget check at
 * entry, a bitmap test before a store, a wrap or limit check — is free
 * to clobber RFLAGS whenever the guest's flags are not live in it, which
 * the liveness pass knows; when they are, the save is done early and
 * later consumers read the slot (BT for one flag, a byte TEST for
 * CF|ZF). Nothing here pays a POPF on a hot path. s_rf tracks which
 * bits are in RFLAGS at each point of the emission.
 *
 * Checks. An inline instruction that touches memory is preceded by its
 * checks — a word access at offset FFFFh (the 8086 wraps in-segment,
 * the 286+ faults), a store's code-bitmap byte — and any check that
 * fails branches to an out-of-line chunk that runs the whole
 * instruction through the exec thunk (the interpreter: exact, faults
 * and all) and rejoins after the inline code. Every check precedes the
 * instruction's first state change, so the helper starts clean. The
 * bitmap byte of a store is [host address + X86_BM_DELTA]: the bitmap
 * sits at that fixed distance above guest memory (x86_mem.c).
 *
 * Budget: cpu->jit_cnt_save is the remaining instruction count, tested at
 * block entry (nothing executed, exit with our own address) and charged
 * on every exit path. The exit stub adds budget - remaining to
 * insn_count, as dbt_a64.c's does.
 *
 * Thunks: a helper call sequence loads R8 = insn pool index, R9 = the
 * block's linear address, R10 = ip after the instruction, R11 =
 * instructions executed so far in the block including it, and CALLs
 * the exec thunk, which spills the pinned state, calls dbt_h_exec, and
 * reloads; a fault or an SMC hit on the running block leaves for the run
 * loop instead of returning.
 *
 * What is inline so far: real-mode-shaped blocks (real mode proper; V86,
 * flat and segmented protected mode are still all-helper) — MOV, the
 * ALU, INC/DEC/NOT/NEG, shifts by an immediate, XCHG, LEA, PUSH/POP,
 * CBW/CWD, the flag instructions, DS/ES loads, near JMP/CALL/RET/Jcc/
 * LOOP/JCXZ. Everything else is a helper; INT and far transfers end the
 * block through one.
 */
#include "dbt.h"
#include "emit_x64.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__)
#include <cpuid.h>
#endif

#define R_CPU  X64_R13
#define R_MEM  X64_R14
#define R_DSP  X64_R15
#define W_T0   X64_R8
#define W_T1   X64_R9
#define W_T2   X64_R10
#define W_T3   X64_R11
#define R_KEY  W_T2      /* the next block's key at a tail (RAX is guest EAX) */
static inline int R_GPR(int i) { return i == R_SP ? X64_R12 : i; }

#define OFF_R(i)        ((int32_t)(offsetof(x86_cpu, r) + 4 * (i)))
#define OFF_EIP         ((int32_t)offsetof(x86_cpu, eip))
#define OFF_EFLAGS      ((int32_t)offsetof(x86_cpu, eflags))
#define OFF_SEG_SEL(i)  ((int32_t)(offsetof(x86_cpu, seg) + sizeof(x86_seg) * (i) + offsetof(x86_seg, sel)))
#define OFF_SEG_USABLE(i) ((int32_t)(offsetof(x86_cpu, seg) + sizeof(x86_seg) * (i) + offsetof(x86_seg, usable)))
#define OFF_SEG_BASE(i) ((int32_t)(offsetof(x86_cpu, seg) + sizeof(x86_seg) * (i) + offsetof(x86_seg, base)))
#define OFF_SEG_LIMIT(i) ((int32_t)(offsetof(x86_cpu, seg) + sizeof(x86_seg) * (i) + offsetof(x86_seg, limit)))
#define OFF_SEG_BIG(i)  ((int32_t)(offsetof(x86_cpu, seg) + sizeof(x86_seg) * (i) + offsetof(x86_seg, big)))
_Static_assert(offsetof(x86_seg, attr) == offsetof(x86_seg, sel) + 2, "sel and attr share one dword store");
#define OFF_EXC         ((int32_t)offsetof(x86_cpu, exc))
#define OFF_INSN_COUNT  ((int32_t)offsetof(x86_cpu, insn_count))
#define OFF_JIT_BUDGET  ((int32_t)offsetof(x86_cpu, jit_budget))
#define OFF_JIT_CNT     ((int32_t)offsetof(x86_cpu, jit_cnt_save))
#define OFF_JIT_CUR_LIN ((int32_t)offsetof(x86_cpu, jit_cur_lin))
#define OFF_JIT_CUR_HIT ((int32_t)offsetof(x86_cpu, jit_cur_hit))
#define OFF_JIT_FLAGS   ((int32_t)offsetof(x86_cpu, jit_flags))

#define ARITH  X86_ARITH_FLAGS

#define M(base, disp) x64_m((base), (disp))

/* Thunk/stub offsets in the code buffer (emitted with the trampoline). */
static uint32_t s_exec_thunk_off, s_exit_eip_off;

/* -V strict mode: every block returns to dbt_run (no links, no probe). */
static int s_strict_exit = -1;
static uint64_t s_mode_bits;
static int s_regs32;          /* 386: the pinned registers hold 32 bits, 16-bit values need masking as addresses */
static int s_model;
static int s_bmi2 = -1;       /* host has RORX (flag-free rotates for the high bytes) */
static const x86_cpu *s_cpu;
static const dbt_block *s_blk;

/* Per-instruction cursor: what a thunk call or a slow path needs. */
static uint32_t s_cur_lin, s_cur_ip_after, s_cur_ip_start, s_cur_n_done, s_cur_i;

int dbt_jit_available(const x86_cpu *cpu) {
#if defined(__x86_64__)
    return cpu->mem_mirrored;
#else
    (void)cpu;
    return 0;
#endif
}

static void host_features(void) {
    if (s_bmi2 >= 0) return;
    s_bmi2 = 0;
#if defined(__x86_64__)
    unsigned a, b, c, d;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) s_bmi2 = (b >> 8) & 1;
#endif
    if (getenv("X86_NO_BMI2")) s_bmi2 = 0;
}

/* ----------------------------------------------------------------------
 * Pinned-state load/spill sequences
 * ---------------------------------------------------------------------- */

/* Guest registers, the DS pointer, the flag slot, DF into RFLAGS.
 * Clobbers RFLAGS (the arithmetic bits are the slot's now) and W_T0. */
static void emit_load_pinned(emit_t *e) {
    for (int i = 0; i < 8; i++) { x64_mem_t m = M(R_CPU, OFF_R(i)); emit_mov_rm(e, 4, R_GPR(i), &m); }
    { x64_mem_t m = M(R_CPU, OFF_SEG_BASE(S_DS)); emit_mov_rm(e, 4, W_T0, &m); }
    { x64_mem_t m = x64_mi(R_MEM, W_T0, 0, 0); emit_lea(e, 8, R_DSP, &m); }
    { x64_mem_t m = M(R_CPU, OFF_EFLAGS); emit_mov_rm(e, 4, W_T0, &m); }
    { x64_mem_t m = M(R_CPU, OFF_JIT_FLAGS); emit_mov_mr(e, 4, &m, W_T0); }   /* slot = eflags (extra bits are harmless) */
    emit_alu_ri(e, 4, X64_ALU_AND, W_T0, X86_DF);
    emit_push_r(e, W_T0);
    emit_popfq(e);
}

/* The reverse: registers back, the slot's arithmetic bits and RFLAGS's
 * DF merged into cpu->eflags. Clobbers RFLAGS, W_T0, W_T1. */
static void emit_spill_pinned(emit_t *e) {
    for (int i = 0; i < 8; i++) { x64_mem_t m = M(R_CPU, OFF_R(i)); emit_mov_mr(e, 4, &m, R_GPR(i)); }
    emit_pushfq(e);
    emit_pop_r(e, W_T1);
    emit_alu_ri(e, 4, X64_ALU_AND, W_T1, X86_DF);
    { x64_mem_t m = M(R_CPU, OFF_JIT_FLAGS); emit_mov_rm(e, 4, W_T0, &m); }
    emit_alu_ri(e, 4, X64_ALU_AND, W_T0, ARITH);
    emit_alu_rr(e, 4, X64_ALU_OR, W_T0, W_T1);
    { x64_mem_t m = M(R_CPU, OFF_EFLAGS); emit_alu_mi(e, 4, X64_ALU_AND, &m, ~(uint32_t)(ARITH | X86_DF)); emit_alu_mr(e, 4, X64_ALU_OR, &m, W_T0); }
}

/* ----------------------------------------------------------------------
 * Trampoline, exit stub, thunks.
 *
 * Called from C as
 *   void trampoline(x86_cpu *cpu, uint8_t *mem, void *block, void *aux,
 *                   uint64_t budget);
 * i.e. RDI, RSI, RDX, RCX, R8. RSP is 8 mod 16 on entry; six pushes and
 * an 8-byte adjustment make it 0 mod 16 inside blocks, so a thunk (one
 * CALL deep) is at 8 and aligns with one more push before calling C.
 * ---------------------------------------------------------------------- */
void dbt_emit_trampoline(x86_dbt *dbt) {
    emit_t e = { .buf = dbt->code_buf, .offset = 0, .capacity = CODE_BUF_SIZE };
    x64_mem_t m;
    host_features();

    emit_push_r(&e, X64_RBX); emit_push_r(&e, X64_RBP);
    emit_push_r(&e, X64_R12); emit_push_r(&e, X64_R13);
    emit_push_r(&e, X64_R14); emit_push_r(&e, X64_R15);
    emit_alu_ri(&e, 8, X64_ALU_SUB, X64_RSP, 8);
    emit_mov_rr(&e, 8, R_CPU, X64_RDI);
    emit_mov_rr(&e, 8, R_MEM, X64_RSI);
    m = M(R_CPU, OFF_JIT_BUDGET); emit_mov_mr(&e, 8, &m, X64_R8);
    m = M(R_CPU, OFF_JIT_CNT);    emit_mov_mr(&e, 8, &m, X64_R8);
    emit_mov_rr(&e, 8, W_T2, X64_RDX);
    emit_load_pinned(&e);
    emit_jmp_r(&e, W_T2);

    /* ---- Exit stub: R10 = next key. EIP = linear - CS.base, which in
     * real mode wraps to 16 bits (A20-folded HMA code) and in protected
     * mode is exact (keys are only made there with A20 on). ---- */
    dbt->exit_stub_off = e.offset;
    emit_mov_rr(&e, 4, W_T0, R_KEY);
    m = M(R_CPU, OFF_SEG_BASE(S_CS)); emit_alu_rm(&e, 4, X64_ALU_SUB, W_T0, &m);
    emit_bt_ri(&e, 8, X64_BT_BT, R_KEY, 48);                      /* KEY_PMODE */
    uint32_t pm_eip = emit_jcc_rel8(&e, X64_CC_C);
    emit_movzx_rr(&e, 4, W_T0, 2, W_T0);
    emit_patch_rel8(&e, pm_eip, emit_pos(&e));
    m = M(R_CPU, OFF_EIP); emit_mov_mr(&e, 4, &m, W_T0);
    /* ---- Exit: cpu->eip already stored by whoever jumps here. ---- */
    s_exit_eip_off = e.offset;
    emit_spill_pinned(&e);
    emit_cld(&e);                                            /* the guest's DF stays in cpu->eflags; C wants it clear */
    /* insn_count += budget - remaining */
    m = M(R_CPU, OFF_JIT_BUDGET);  emit_mov_rm(&e, 8, W_T0, &m);
    m = M(R_CPU, OFF_JIT_CNT);     emit_alu_rm(&e, 8, X64_ALU_SUB, W_T0, &m);
    m = M(R_CPU, OFF_INSN_COUNT);  emit_alu_mr(&e, 8, X64_ALU_ADD, &m, W_T0);
    emit_alu_ri(&e, 8, X64_ALU_ADD, X64_RSP, 8);
    emit_pop_r(&e, X64_R15); emit_pop_r(&e, X64_R14);
    emit_pop_r(&e, X64_R13); emit_pop_r(&e, X64_R12);
    emit_pop_r(&e, X64_RBP); emit_pop_r(&e, X64_RBX);
    emit_ret(&e);

    /* ---- Exec thunk: CALLed with R8 = insn pool index, R9 = the block's
     * linear address, R10 = ip after the instruction, R11 = instructions
     * executed in the block so far, including this one. ---- */
    s_exec_thunk_off = e.offset;
    emit_push_r(&e, W_T3);                                   /* n_done, and the alignment */
    m = M(R_CPU, OFF_JIT_CUR_LIN); emit_mov_mr(&e, 4, &m, W_T1);
    m = M(R_CPU, OFF_JIT_CUR_HIT); emit_mov_mi(&e, 4, &m, 0);
    m = M(R_CPU, OFF_EIP);         emit_mov_mr(&e, 4, &m, W_T2);
    emit_mov_rr(&e, 8, W_T2, W_T0);                          /* the index survives the spill in W_T2 */
    emit_spill_pinned(&e);
    emit_cld(&e);
    emit_mov_rr(&e, 8, X64_RDI, R_CPU);
    emit_mov_rr(&e, 4, X64_RSI, W_T2);
    emit_mov_ri(&e, 8, X64_RAX, (uint64_t)(uintptr_t)dbt_h_exec);
    emit_call_r(&e, X64_RAX);
    emit_load_pinned(&e);
    emit_pop_r(&e, W_T3);
    /* The interpreter faulted inside the helper op: cpu->exc is set and
     * eip points at the instruction. Charge it, leave. Or the SMC sweep
     * invalidated the running block: leave wherever the helper put EIP. */
    m = M(R_CPU, OFF_EXC);         emit_alu_mi(&e, 4, X64_ALU_CMP, &m, 0xFFFFFFFFu);
    uint32_t leave1 = emit_jcc_rel32(&e, X64_CC_NE);
    m = M(R_CPU, OFF_JIT_CUR_HIT); emit_alu_mi(&e, 4, X64_ALU_CMP, &m, 0);
    uint32_t leave2 = emit_jcc_rel32(&e, X64_CC_NE);
    emit_ret(&e);
    emit_patch_rel32(&e, leave1, emit_pos(&e));
    emit_patch_rel32(&e, leave2, emit_pos(&e));
    emit_alu_ri(&e, 8, X64_ALU_ADD, X64_RSP, 8);             /* drop the return address */
    m = M(R_CPU, OFF_JIT_CNT);     emit_alu_mr(&e, 8, X64_ALU_SUB, &m, W_T3);
    emit_jmp_rel32_to(&e, s_exit_eip_off);

    dbt->code_used = e.offset;
}

void dbt_arch_patch_link(x86_dbt *dbt, uint32_t site_off, uint8_t *target) {
    /* a link site is a JMP rel32 whose field is at site_off + 1; unlinked
     * it jumps to the instruction after itself */
    uint8_t *site = dbt->code_buf + site_off;
    uint8_t *dst  = target ? target : site + 5;
    int32_t disp = (int32_t)(dst - (site + 5));
    memcpy(site + 1, &disp, 4);
}

/* ----------------------------------------------------------------------
 * Flags: where the guest's arithmetic flags are at this point
 * ---------------------------------------------------------------------- */
static uint32_t s_rf;         /* bits whose truth is in RFLAGS; the rest are in cpu->jit_flags */

/* Save the RFLAGS bits that are live and about to be clobbered into the
 * slot. Whole-image when RFLAGS holds every arithmetic bit; a merge
 * otherwise (the slot's other bits are the truth and must survive). */
static void fl_save(emit_t *e, uint32_t live) {
    if (!(s_rf & live)) { s_rf = 0; return; }
    x64_mem_t m = M(R_CPU, OFF_JIT_FLAGS);
    if ((s_rf & ARITH) == ARITH) {
        emit_pushfq(e);
        emit_pop_m(e, &m);
    } else {
        emit_pushfq(e);
        emit_pop_r(e, W_T3);
        emit_alu_ri(e, 4, X64_ALU_AND, W_T3, s_rf & ARITH);
        emit_alu_mi(e, 4, X64_ALU_AND, &m, ~(s_rf & ARITH));
        emit_alu_mr(e, 4, X64_ALU_OR, &m, W_T3);
    }
    s_rf = 0;
}
/* An instruction just wrote the bits in `wr` into RFLAGS (hardware-exact). */
static void fl_produce(uint32_t wr) { s_rf |= wr; }

/* CF into RFLAGS for a consumer (ADC, SBB, RCL, RCR, CMC): BT writes only CF. */
static void fl_need_cf(emit_t *e) {
    if (s_rf & X86_CF) return;
    x64_mem_t m = M(R_CPU, OFF_JIT_FLAGS);
    emit_bt_mi(e, 4, X64_BT_BT, &m, 0);
    s_rf |= X86_CF;
}

/* The flag bits a guest condition reads. */
static uint32_t cond_bits(int cc) {
    static const uint16_t need[8] = { X86_OF, X86_CF, X86_ZF, X86_CF | X86_ZF, X86_SF, X86_PF, X86_SF | X86_OF, X86_SF | X86_OF | X86_ZF };
    return need[cc >> 1];
}

/* Set up RFLAGS so that Jcc/SETcc `*hostcc` decides guest condition cc,
 * from RFLAGS when its bits are there, else from the slot. `live` is the
 * set of guest bits live after this point (what a clobber must respect). */
static void emit_cond_setup(emit_t *e, int cc, uint32_t live, int *hostcc) {
    uint32_t need = cond_bits(cc);
    if ((s_rf & need) == need) { *hostcc = cc; return; }
    /* From the slot. Whatever live bits RFLAGS holds go there first: the
     * BT below writes CF, the two-bit tests write everything. */
    fl_save(e, live | need);
    x64_mem_t m = M(R_CPU, OFF_JIT_FLAGS);
    int inv = cc & 1;
    switch (cc >> 1) {
    case 0: emit_bt_mi(e, 4, X64_BT_BT, &m, 11); *hostcc = inv ? X64_CC_NC : X64_CC_C; return;   /* O */
    case 1: emit_bt_mi(e, 4, X64_BT_BT, &m, 0);  *hostcc = inv ? X64_CC_NC : X64_CC_C; return;   /* B */
    case 2: emit_bt_mi(e, 4, X64_BT_BT, &m, 6);  *hostcc = inv ? X64_CC_NC : X64_CC_C; return;   /* E */
    case 4: emit_bt_mi(e, 4, X64_BT_BT, &m, 7);  *hostcc = inv ? X64_CC_NC : X64_CC_C; return;   /* S */
    case 5: emit_bt_mi(e, 4, X64_BT_BT, &m, 2);  *hostcc = inv ? X64_CC_NC : X64_CC_C; return;   /* P */
    default: break;
    }
    switch (cc >> 1) {
    case 3:   /* BE: CF | ZF */
        emit_test_mi(e, 1, &m, X86_CF | X86_ZF);
        *hostcc = inv ? X64_CC_Z : X64_CC_NZ;
        return;
    case 6:   /* L: SF != OF */
        emit_mov_rm(e, 4, W_T2, &m);
        emit_mov_rr(e, 4, W_T3, W_T2);
        emit_shift_ri(e, 4, X64_SH_SHR, W_T3, 4);
        emit_alu_rr(e, 4, X64_ALU_XOR, W_T3, W_T2);
        emit_test_ri(e, 4, W_T3, X86_SF);
        *hostcc = inv ? X64_CC_Z : X64_CC_NZ;
        return;
    default:  /* LE: (SF != OF) | ZF */
        emit_mov_rm(e, 4, W_T2, &m);
        emit_mov_rr(e, 4, W_T3, W_T2);
        emit_shift_ri(e, 4, X64_SH_SHR, W_T3, 4);
        emit_alu_rr(e, 4, X64_ALU_XOR, W_T3, W_T2);
        emit_alu_ri(e, 4, X64_ALU_AND, W_T3, X86_SF);
        emit_alu_ri(e, 4, X64_ALU_AND, W_T2, X86_ZF);
        emit_alu_rr(e, 4, X64_ALU_OR, W_T3, W_T2);
        *hostcc = inv ? X64_CC_Z : X64_CC_NZ;
        return;
    }
}

/* ----------------------------------------------------------------------
 * Block tails: cache probe, static edges, dynamic keys.
 * ---------------------------------------------------------------------- */

/* Dynamic tail. R_KEY = next key. Probe the cache:
 *   slot = (lin ^ mode << 16) & mask (dbt_slot); entry = cache + slot * 16;
 *   CMP key; JMP [entry + 8] or exit. RFLAGS is free at every tail. */
static void emit_dynamic_tail(x86_dbt *dbt, emit_t *e) {
    if (s_strict_exit > 0) { emit_jmp_rel32_to(e, dbt->exit_stub_off); return; }
    emit_mov_rr(e, 8, W_T0, R_KEY);
    emit_shift_ri(e, 8, X64_SH_SHR, W_T0, KEY_MODE_SHIFT);
    emit_alu_ri(e, 4, X64_ALU_AND, W_T0, KEY_MODE_MASK);
    emit_shift_ri(e, 4, X64_SH_SHL, W_T0, 16);
    emit_alu_rr(e, 4, X64_ALU_XOR, W_T0, R_KEY);
    emit_alu_ri(e, 4, X64_ALU_AND, W_T0, BLOCK_CACHE_MASK);
    emit_shift_ri(e, 8, X64_SH_SHL, W_T0, 4);
    emit_mov_ri(e, 8, W_T1, (uint64_t)(uintptr_t)dbt->aux->cache);
    x64_mem_t m = x64_mi(W_T1, W_T0, 0, 0);
    emit_alu_rm(e, 8, X64_ALU_CMP, R_KEY, &m);
    uint32_t miss = emit_jcc_rel8(e, X64_CC_NE);
    m = x64_mi(W_T1, W_T0, 0, 8);
    emit_jmp_m(e, &m);
    emit_patch_rel8(e, miss, emit_pos(e));
    emit_jmp_rel32_to(e, dbt->exit_stub_off);
}

/* Static edge to `key`:
 *   JMP <target | .+5>      patchable link site (dbt_arch_patch_link)
 *   MOV R10, key            only reached while unlinked
 *   <probe + exit>
 * The linked path executes nothing but the JMP. */
static void emit_edge(x86_dbt *dbt, emit_t *e, uint64_t key) {
    if (s_strict_exit > 0) {
        emit_mov_ri(e, 8, R_KEY, key);
        emit_dynamic_tail(dbt, e);
        return;
    }
    uint32_t site = e->offset;
    int linked = 0;
    if (dbt_link_record(dbt, key, site) && !dbt->golden) {
        x86_block_entry *be = &dbt->aux->cache[dbt_slot(key)];
        if (be->key == key && be->code) {
            emit_jmp_rel32_to(e, (uint32_t)(be->code - e->buf));
            linked = 1;
        }
    }
    if (!linked) { uint32_t at = emit_jmp_rel32(e); emit_patch_rel32(e, at, emit_pos(e)); }
    emit_mov_ri(e, 8, R_KEY, key);
    emit_dynamic_tail(dbt, e);
}

/* Key of the static target ip within the block's CS. */
static uint64_t target_key(uint32_t ip) {
    const x86_cpu *cpu = s_cpu;
    const dbt_block *b = s_blk;
    if (b->flat || b->all_helper) return dbt_key(cpu->seg[S_CS].sel, cpu->seg[S_CS].base + ip) | s_mode_bits;
    if (b->v86 || b->seg16) return dbt_key(cpu->seg[S_CS].sel, cpu->seg[S_CS].base + (ip & 0xFFFF)) | s_mode_bits;
    return dbt_key(cpu->seg[S_CS].sel, (cpu->seg[S_CS].base + (ip & 0xFFFF)) & cpu->a20_mask) | s_mode_bits;
}

/* R_KEY = key for a run-time ip in register `ip` (16-bit, may have
 * garbage above on a 386): lin = (cs.base + ip) & a20, CS static. */
static void emit_dynamic_key(emit_t *e, int ip) {
    const x86_cpu *cpu = s_cpu;
    emit_movzx_rr(e, 4, R_KEY, 2, ip);
    if (cpu->seg[S_CS].base) emit_alu_ri(e, 4, X64_ALU_ADD, R_KEY, cpu->seg[S_CS].base);
    if (!s_blk->v86 && cpu->seg[S_CS].base + 0xFFFF > 0xFFFFF && cpu->a20_mask == 0xFFFFF)
        emit_alu_ri(e, 4, X64_ALU_AND, R_KEY, 0xFFFFF);
    emit_mov_ri(e, 8, W_T0, ((uint64_t)cpu->seg[S_CS].sel << 32) | s_mode_bits);
    emit_alu_rr(e, 8, X64_ALU_OR, R_KEY, W_T0);
}

/* R_KEY = the key of wherever the interpreter left CS:EIP after a helper:
 * real mode and V86 read the live CS (a far transfer moves it) and a
 * 16-bit IP, masking for A20 as the block's key does; protected-mode
 * blocks end before anything that loads CS, so theirs is baked. */
static void emit_dynamic_key_cpu(emit_t *e, const dbt_block *b, const x86_cpu *cpu) {
    x64_mem_t m;
    if (b->flat || b->seg16 || b->all_helper) {
        m = M(R_CPU, OFF_EIP); emit_mov_rm(e, 4, R_KEY, &m);
        if (!b->flat && !(b->all_helper && cpu->seg[S_CS].big)) emit_movzx_rr(e, 4, R_KEY, 2, R_KEY);
        emit_alu_ri(e, 4, X64_ALU_ADD, R_KEY, cpu->seg[S_CS].base);   /* wraps at 4 GB, as the CPU's does */
        emit_mov_ri(e, 8, W_T0, ((uint64_t)cpu->seg[S_CS].sel << 32) | s_mode_bits);
        emit_alu_rr(e, 8, X64_ALU_OR, R_KEY, W_T0);
        return;
    }
    m = M(R_CPU, OFF_EIP); emit_movzx_rm(e, 4, R_KEY, 2, &m);
    m = M(R_CPU, OFF_SEG_BASE(S_CS)); emit_alu_rm(e, 4, X64_ALU_ADD, R_KEY, &m);
    if (!b->v86 && cpu->a20_mask == 0xFFFFF) emit_alu_ri(e, 4, X64_ALU_AND, R_KEY, 0xFFFFF);
    m = M(R_CPU, OFF_SEG_SEL(S_CS)); emit_movzx_rm(e, 4, W_T0, 2, &m);
    emit_shift_ri(e, 8, X64_SH_SHL, W_T0, 32);
    emit_alu_rr(e, 8, X64_ALU_OR, R_KEY, W_T0);
    if (s_mode_bits) { emit_mov_ri(e, 8, W_T0, s_mode_bits); emit_alu_rr(e, 8, X64_ALU_OR, R_KEY, W_T0); }
}

/* Charge n instructions to the budget; RFLAGS is free at a tail. */
static void emit_tail_prologue(emit_t *e, uint32_t n) {
    x64_mem_t m = M(R_CPU, OFF_JIT_CNT);
    emit_alu_mi(e, 8, X64_ALU_SUB, &m, n);
}

/* ----------------------------------------------------------------------
 * Helper calls and slow paths
 * ---------------------------------------------------------------------- */
static void emit_helper_call(x86_dbt *dbt, emit_t *e, const x86_insn *in, int tag) {
    uint32_t idx = dbt->golden ? 0 : dbt->insn_used++;    /* (X86_GOLDEN: the index is translation order, not content) */
    dbt->insn_pool[idx] = *in;
    dbt->insn_tag[idx] = (uint8_t)tag;
    dbt->insn_lin[idx] = dbt->cpu->seg[S_CS].base + s_cur_ip_start;
    dbt->helper_insns++;
    emit_mov_ri(e, 4, W_T0, idx);
    emit_mov_ri(e, 4, W_T1, s_cur_lin);
    emit_mov_ri(e, 4, W_T2, s_cur_ip_after);
    emit_mov_ri(e, 4, W_T3, s_cur_n_done);
    emit_call_rel32_to(e, s_exec_thunk_off);
}

/* A helper op in the block body: the flags go to the slot first (the
 * thunk hands them to the interpreter), and come back there. */
static void emit_helper_op(x86_dbt *dbt, emit_t *e, const x86_insn *in, uint32_t live_in) {
    fl_save(e, live_in);
    emit_helper_call(dbt, e, in, s_blk->all_helper ? 0 : 1);
    s_rf = 0;
}

/* Slow paths: one chunk per checked inline instruction, emitted after the
 * block. Entered with the flags already in the slot (every check is
 * preceded by fl_save), it runs the whole instruction through the exec
 * thunk, restores RFLAGS if the inline code left flags there for what
 * follows, and rejoins. An instruction with several checks has several
 * sites and one chunk. */
typedef struct {
    uint32_t patch_off, back_off;
    uint32_t ip_after, ip_start, n_done, op_i;
    uint32_t rf_after;        /* bits the continuation expects in RFLAGS */
} slow_site_t;
#define SLOW_MAX 256
static slow_site_t s_slow[SLOW_MAX];
static uint32_t s_nslow;
static uint32_t s_slow_first;    /* this instruction's first site */

/* Record a slow-path branch site for the current instruction and emit
 * the Jcc rel32 toward it. */
static void slow_site(emit_t *e, int cc) {
    if (s_nslow >= SLOW_MAX) { fprintf(stderr, "dbt: slow-path table overflow\n"); abort(); }
    slow_site_t *s = &s_slow[s_nslow++];
    s->patch_off = emit_jcc_rel32(e, cc);
    s->back_off = 0;
    s->ip_after = s_cur_ip_after;
    s->ip_start = s_cur_ip_start;
    s->n_done = s_cur_n_done;
    s->op_i = s_cur_i;
    s->rf_after = 0;
}
/* After the inline code: where this instruction's chunk rejoins, and
 * what it must put back into RFLAGS. */
static void slow_back(emit_t *e, uint32_t rf_after) {
    for (uint32_t k = s_slow_first; k < s_nslow; k++) { s_slow[k].back_off = emit_pos(e); s_slow[k].rf_after = rf_after; }
}
static void emit_slow_chunks(x86_dbt *dbt, emit_t *e) {
    for (uint32_t k = 0; k < s_nslow; ) {
        slow_site_t *s = &s_slow[k];
        uint32_t chunk = emit_pos(e), j = k;
        for (; j < s_nslow && s_slow[j].op_i == s->op_i; j++) emit_patch_rel32(e, s_slow[j].patch_off, chunk);
        s_cur_ip_after = s->ip_after;
        s_cur_ip_start = s->ip_start;
        s_cur_n_done = s->n_done;
        emit_helper_call(dbt, e, &s_blk->decs[s->op_i], 2);
        if (s->rf_after) {
            x64_mem_t m = M(R_CPU, OFF_JIT_FLAGS);
            emit_mov_rm(e, 4, W_T0, &m);
            emit_alu_ri(e, 4, X64_ALU_AND, W_T0, ARITH | X86_DF);
            emit_push_r(e, W_T0);
            emit_popfq(e);
        }
        emit_jmp_rel32_to(e, s->back_off);
        k = j;
    }
    s_nslow = 0;
}

/* ----------------------------------------------------------------------
 * Operands
 * ---------------------------------------------------------------------- */
typedef struct {
    int segp;      /* host register: mem + segment base */
    int off;       /* host register: the 16-bit offset (zero-extended) */
} ea_t;

/* Host pointer for segment s: R_DSP for DS, else built into `into`. */
static int emit_seg_ptr(emit_t *e, int s, int into) {
    if (s == S_DS) return R_DSP;
    x64_mem_t m = M(R_CPU, OFF_SEG_BASE(s));
    emit_mov_rm(e, 4, into, &m);
    m = x64_mi(R_MEM, into, 0, 0);
    emit_lea(e, 8, into, &m);
    return into;
}

static int is_high8(const x86_operand *o);
/* 16-bit effective address into W_T0 (or a pinned register when it is
 * exactly that, below the 386); the segment pointer in W_T1 unless DS.
 * Flag-free. */
static void emit_ea(emit_t *e, const x86_insn *in, ea_t *ea) {
    ea->segp = emit_seg_ptr(e, in->seg, W_T1);
    if (in->base < 0 && in->index < 0) {
        emit_mov_ri(e, 4, W_T0, (uint32_t)in->disp & 0xFFFF);
        ea->off = W_T0;
        return;
    }
    /* a bare [reg] is its own offset — unless a high-byte operand is about
     * to rotate that very register (test [bx], bh): then a copy */
    int hi = is_high8(&in->ops[0]) || is_high8(&in->ops[1]);
    if (in->index < 0 && in->disp == 0 && !s_regs32 && !hi) { ea->off = R_GPR(in->base); return; }
    x64_mem_t m;
    if (in->base >= 0 && in->index >= 0) m = x64_mi(R_GPR(in->base), R_GPR(in->index), 0, in->disp);
    else m = x64_m(R_GPR(in->base >= 0 ? in->base : in->index), in->disp);
    emit_lea(e, 4, W_T0, &m);
    emit_movzx_rr(e, 4, W_T0, 2, W_T0);
    ea->off = W_T0;
}
static x64_mem_t ea_mem(const ea_t *ea) { return x64_mi(ea->segp, ea->off, 0, 0); }

/* Checks before an access at ea of `size` bytes: a word at FFFFh (the
 * 8086 wraps in-segment, the 286+ raises #GP: the helper does either),
 * and for a store the code bitmap. RFLAGS must be free here. */
static void emit_check_wrap(emit_t *e, const ea_t *ea, int size) {
    if (size < 2) return;
    emit_alu_ri(e, 2, X64_ALU_CMP, ea->off, 0xFFFF);
    slow_site(e, X64_CC_E);
}
static void emit_check_smc(emit_t *e, const ea_t *ea, int size) {
    x64_mem_t m = x64_mi(ea->segp, ea->off, 0, (int32_t)X86_BM_DELTA);
    emit_alu_mi(e, size, X64_ALU_CMP, &m, 0);
    slow_site(e, X64_CC_NE);
}

/* The high bytes: an instruction naming AH..BH cannot also name R8..R15,
 * so a memory access through R15/R8 with a high-byte register operand
 * rotates the byte down into AL..BL for the duration (RORX: no flags),
 * or swaps it there and back without BMI2 (XCHG: no flags either). */
static int is_high8(const x86_operand *o) { return o->kind == OPK_REG && o->size == 1 && o->reg >= 4; }
static void emit_high8_open(emit_t *e, int reg) {
    int r = reg & 3;
    if (s_bmi2) emit_rorx_rri(e, 4, r, r, 8);
    else emit_xchg_rr(e, 1, r, r + 4);
}
static void emit_high8_close(emit_t *e, int reg) {
    int r = reg & 3;
    if (s_bmi2) emit_rorx_rri(e, 4, r, r, 24);
    else emit_xchg_rr(e, 1, r, r + 4);
}

/* Register operand → host register number for its size (the byte regs
 * 4..7 are the high bytes). */
static int host_reg(const x86_operand *o) { return o->size == 1 ? o->reg : R_GPR(o->reg); }

/* ----------------------------------------------------------------------
 * Stack
 * ---------------------------------------------------------------------- */

/* PUSH the 16-bit value in register `val` (a guest register is fine,
 * except that a guest register in W_T0..W_T3 is not: the SS pointer and
 * the new SP go there). The new SP sits in W_T0 until the store is known
 * to succeed (a fault leaves SP intact). */
static void emit_push16(emit_t *e, int val) {
    x64_mem_t m = M(X64_R12, -2);
    emit_lea(e, 4, W_T0, &m);
    emit_movzx_rr(e, 4, W_T0, 2, W_T0);
    ea_t ea = { emit_seg_ptr(e, S_SS, W_T1), W_T0 };
    emit_check_wrap(e, &ea, 2);
    emit_check_smc(e, &ea, 2);
    m = ea_mem(&ea);
    emit_mov_mr(e, 2, &m, val);
    emit_mov_rr(e, 2, X64_R12, W_T0);
}
/* POP into register dst (16 bits; dst may be W_T2 or W_T3). */
static void emit_pop16(emit_t *e, int dst) {
    ea_t ea = { emit_seg_ptr(e, S_SS, W_T1), X64_R12 };
    if (s_regs32) { emit_movzx_rr(e, 4, W_T0, 2, X64_R12); ea.off = W_T0; }
    emit_check_wrap(e, &ea, 2);
    x64_mem_t m = ea_mem(&ea);
    emit_mov_rm(e, 2, dst, &m);
    m = M(X64_R12, 2);
    emit_lea(e, 4, W_T0, &m);
    emit_mov_rr(e, 2, X64_R12, W_T0);
}

/* ----------------------------------------------------------------------
 * Segment loads (real mode): DS and ES
 * ---------------------------------------------------------------------- */
static void emit_load_seg_real(emit_t *e, int s, int sel /* register, 16 bits */) {
    x64_mem_t m;
    /* limit = max(limit, FFFF): the unreal-mode cache survives a real-mode load */
    m = M(R_CPU, OFF_SEG_LIMIT(s));
    emit_alu_mi(e, 4, X64_ALU_CMP, &m, 0xFFFF);
    uint32_t keep = emit_jcc_rel8(e, X64_CC_AE);
    emit_mov_mi(e, 4, &m, 0xFFFF);
    emit_patch_rel8(e, keep, emit_pos(e));
    emit_movzx_rr(e, 4, W_T0, 2, sel);
    m = M(R_CPU, OFF_SEG_SEL(s));    emit_mov_mr(e, 4, &m, W_T0);          /* sel, attr = 0 */
    m = M(R_CPU, OFF_SEG_USABLE(s)); emit_mov_mi(e, 1, &m, 1);
    m = M(R_CPU, OFF_SEG_BIG(s));    emit_mov_mi(e, 1, &m, 0);
    emit_shift_ri(e, 4, X64_SH_SHL, W_T0, 4);
    m = M(R_CPU, OFF_SEG_BASE(s));   emit_mov_mr(e, 4, &m, W_T0);
    if (s == S_DS) { m = x64_mi(R_MEM, W_T0, 0, 0); emit_lea(e, 8, R_DSP, &m); }
}

/* ----------------------------------------------------------------------
 * Straight-line ops
 * ---------------------------------------------------------------------- */
static const int alu_of_op[OP__COUNT] = {
    [OP_ADD] = X64_ALU_ADD, [OP_OR] = X64_ALU_OR, [OP_ADC] = X64_ALU_ADC, [OP_SBB] = X64_ALU_SBB,
    [OP_AND] = X64_ALU_AND, [OP_SUB] = X64_ALU_SUB, [OP_XOR] = X64_ALU_XOR, [OP_CMP] = X64_ALU_CMP,
};
static const int sh_of_op[OP__COUNT] = {
    [OP_ROL] = X64_SH_ROL, [OP_ROR] = X64_SH_ROR, [OP_RCL] = X64_SH_RCL, [OP_RCR] = X64_SH_RCR,
    [OP_SHL] = X64_SH_SHL, [OP_SHR] = X64_SH_SHR, [OP_SAL] = X64_SH_SHL, [OP_SAR] = X64_SH_SAR,
};

/* Can this instruction be emitted inline by the code below? (The plan's
 * class says the interpreter is not required; this says the emitter is
 * ready for it.) */
static int inline_ok(const x86_insn *in) {
    if (in->opsize != 2 || in->adsize != 2) return 0;
    for (int i = 0; i < 2; i++) if (in->ops[i].kind == OPK_MEM && in->ops[i].size > 2) return 0;
    switch (in->op) {
    case OP_ADD: case OP_OR: case OP_ADC: case OP_SBB: case OP_AND: case OP_SUB: case OP_XOR: case OP_CMP:
    case OP_TEST: case OP_INC: case OP_DEC: case OP_NOT: case OP_NEG:
    case OP_MOV: case OP_XCHG: case OP_LEA: case OP_NOP: case OP_CBW: case OP_CWD:
    case OP_CLC: case OP_STC: case OP_CMC: case OP_CLD: case OP_STD: case OP_CLI: case OP_STI:
    case OP_SETCC:
    case OP_CALL: case OP_JMP: case OP_JCC: case OP_JCXZ: case OP_LOOP: case OP_LOOPE: case OP_LOOPNE: case OP_RET:
        return 1;
    case OP_PUSH:   /* (the 8086's FE /6, a byte push, is the interpreter's) */
        return in->ops[0].kind == OPK_SREG || in->ops[0].kind == OPK_IMM || in->ops[0].size == 2;
    case OP_SHL: case OP_SAL: case OP_SHR: case OP_SAR: case OP_ROL: case OP_ROR: case OP_RCL: case OP_RCR:
        return in->ops[1].kind == OPK_IMM && (in->ops[1].imm & 0xFF) < (uint32_t)in->ops[0].size * 8;
    case OP_POP:
        if (in->ops[0].kind == OPK_SREG) return in->ops[0].reg == S_DS || in->ops[0].reg == S_ES;
        return in->ops[0].size == 2;
    case OP_MOVSEG:
        if (in->ops[0].kind == OPK_SREG) return in->ops[0].reg == S_DS || in->ops[0].reg == S_ES;
        return 1;
    default:
        return 0;
    }
}

/* The checks of an access at ea: RFLAGS is freed first (the live guest
 * bits go to the slot), then the wrap check for a word, the bitmap for
 * a store. */
static void emit_checks(emit_t *e, const ea_t *ea, int size, int store, uint32_t live_in) {
    fl_save(e, live_in);
    emit_check_wrap(e, ea, size);
    if (store) emit_check_smc(e, ea, size);
}

/* An ALU op (add .. cmp, test) in any operand shape. High-byte
 * operands next to a memory operand go through the rotate. */
static void emit_op_alu(emit_t *e, const x86_insn *in, const ea_t *ea, uint32_t live_in) {
    int alu = in->op == OP_TEST ? -1 : alu_of_op[in->op];
    int size = in->ops[0].size;
    const x86_operand *d = &in->ops[0], *s = &in->ops[1];
    x64_mem_t m;
    int cin = in->op == OP_ADC || in->op == OP_SBB;
    if (d->kind == OPK_MEM) {
        emit_checks(e, ea, size, in->op != OP_CMP && in->op != OP_TEST, live_in);
        if (cin) fl_need_cf(e);                                   /* after the checks: they clobber CF */
        m = ea_mem(ea);
        if (s->kind == OPK_IMM) {
            if (alu < 0) emit_test_mi(e, size, &m, s->imm);
            else emit_alu_mi(e, size, alu, &m, s->imm);
        } else {
            int hi = is_high8(s);
            if (hi) emit_high8_open(e, s->reg);
            int r = hi ? (s->reg & 3) : host_reg(s);
            if (alu < 0) emit_test_mr(e, size, &m, r);
            else emit_alu_mr(e, size, alu, &m, r);
            if (hi) emit_high8_close(e, s->reg);
        }
        fl_produce(ARITH);
        slow_back(e, s_rf);
        return;
    }
    if (s->kind == OPK_MEM) {
        emit_checks(e, ea, size, 0, live_in);
        if (cin) fl_need_cf(e);
        m = ea_mem(ea);
        int dhi = is_high8(d);
        if (dhi) emit_high8_open(e, d->reg);
        int r = dhi ? (d->reg & 3) : host_reg(d);
        if (alu < 0) { emit_movzx_rm(e, 4, W_T2, size, &m); emit_test_rr(e, size, r, W_T2); }
        else emit_alu_rm(e, size, alu, r, &m);
        if (dhi) emit_high8_close(e, d->reg);
        fl_produce(ARITH);
        slow_back(e, s_rf);
        return;
    }
    int r = host_reg(d);
    if (cin) fl_need_cf(e);
    if (s->kind == OPK_IMM) {
        if (alu < 0) emit_test_ri(e, size, r, s->imm);
        else emit_alu_ri(e, size, alu, r, s->imm);
    } else {
        if (alu < 0) emit_test_rr(e, size, r, host_reg(s));
        else emit_alu_rr(e, size, alu, r, host_reg(s));
    }
    fl_produce(ARITH);
}

static void emit_op(x86_dbt *dbt, emit_t *e, const x86_insn *in, uint32_t live_in, uint32_t live_out) {
    ea_t ea = { 0, 0 };
    x64_mem_t m;
    int size = in->ops[0].size;
    const x86_operand *d = &in->ops[0], *s = &in->ops[1];
    s_slow_first = s_nslow;
    if (in->ea_valid && in->op != OP_LEA && in->op != OP_SETCC) emit_ea(e, in, &ea);

    switch (in->op) {
    case OP_ADD: case OP_OR: case OP_ADC: case OP_SBB: case OP_AND: case OP_SUB: case OP_XOR: case OP_CMP: case OP_TEST:
        emit_op_alu(e, in, &ea, live_in);
        return;

    case OP_INC: case OP_DEC: case OP_NOT: case OP_NEG: {
        uint32_t wr = in->op == OP_NOT ? 0 : in->op == OP_NEG ? ARITH : (ARITH & ~X86_CF);
        if (d->kind == OPK_MEM) {
            emit_checks(e, &ea, size, 1, live_in);
            m = ea_mem(&ea);
            if (in->op == OP_INC) emit_inc_m(e, size, &m);
            else if (in->op == OP_DEC) emit_dec_m(e, size, &m);
            else emit_g3_m(e, size, in->op == OP_NOT ? X64_G3_NOT : X64_G3_NEG, &m);
            fl_produce(wr);
            slow_back(e, s_rf);
            return;
        }
        int r = host_reg(d);
        if (in->op == OP_INC) emit_inc_r(e, size, r);
        else if (in->op == OP_DEC) emit_dec_r(e, size, r);
        else emit_g3_r(e, size, in->op == OP_NOT ? X64_G3_NOT : X64_G3_NEG, r);
        fl_produce(wr);
        return;
    }

    case OP_SHL: case OP_SAL: case OP_SHR: case OP_SAR: case OP_ROL: case OP_ROR: case OP_RCL: case OP_RCR: {
        uint32_t cnt = s->imm & 0xFF;
        int sh = sh_of_op[in->op];
        int rot = in->op == OP_ROL || in->op == OP_ROR || in->op == OP_RCL || in->op == OP_RCR;
        if (cnt == 0) return;                                    /* no flags, nothing (CONTRACT: count 0 leaves them) */
        if (in->op == OP_RCL || in->op == OP_RCR) fl_need_cf(e);
        /* OF after a shift by more than one is the last step's on the
         * 8086 and undefined (and different) on the host: two steps then */
        int two = cnt > 1 && (live_out & X86_OF) && in->op != OP_SAR;
        if (d->kind == OPK_MEM) {
            emit_checks(e, &ea, size, 1, live_in & (in->op == OP_RCL || in->op == OP_RCR ? ~X86_CF : ~0u));
            if (in->op == OP_RCL || in->op == OP_RCR) fl_need_cf(e);   /* (the checks may have moved it out) */
            m = ea_mem(&ea);
            if (two) { emit_shift_mi(e, size, sh, &m, (uint8_t)(cnt - 1)); emit_shift_mi(e, size, sh, &m, 1); }
            else emit_shift_mi(e, size, sh, &m, (uint8_t)cnt);
            fl_produce(rot ? (X86_CF | X86_OF) : ARITH);
            slow_back(e, s_rf);
            return;
        }
        int r = host_reg(d);
        if (two) { emit_shift_ri(e, size, sh, r, (uint8_t)(cnt - 1)); emit_shift_ri(e, size, sh, r, 1); }
        else emit_shift_ri(e, size, sh, r, (uint8_t)cnt);
        fl_produce(rot ? (X86_CF | X86_OF) : ARITH);
        return;
    }

    case OP_MOV:
        if (d->kind == OPK_MEM) {
            emit_checks(e, &ea, size, 1, live_in);
            m = ea_mem(&ea);
            if (s->kind == OPK_IMM) emit_mov_mi(e, size, &m, s->imm);
            else if (is_high8(s)) { emit_high8_open(e, s->reg); emit_mov_mr(e, size, &m, s->reg & 3); emit_high8_close(e, s->reg); }
            else emit_mov_mr(e, size, &m, host_reg(s));
            slow_back(e, s_rf);
            return;
        }
        if (s->kind == OPK_MEM) {
            emit_checks(e, &ea, size, 0, live_in);
            m = ea_mem(&ea);
            if (is_high8(d)) { emit_high8_open(e, d->reg); emit_mov_rm(e, size, d->reg & 3, &m); emit_high8_close(e, d->reg); }
            else emit_mov_rm(e, size, host_reg(d), &m);
            slow_back(e, s_rf);
            return;
        }
        if (s->kind == OPK_IMM) emit_mov_ri(e, size, host_reg(d), s->imm);
        else emit_mov_rr(e, size, host_reg(d), host_reg(s));
        return;

    case OP_MOVSEG:
        if (d->kind == OPK_SREG) {                               /* MOV DS/ES, r/m16 */
            int src;
            if (s->kind == OPK_MEM) { emit_checks(e, &ea, 2, 0, live_in); m = ea_mem(&ea); emit_movzx_rm(e, 4, W_T2, 2, &m); src = W_T2; }
            else { fl_save(e, live_in); src = host_reg(s); }     /* (the limit compare) */
            emit_load_seg_real(e, d->reg, src);
            if (s->kind == OPK_MEM) slow_back(e, 0);
            return;
        }
        m = M(R_CPU, OFF_SEG_SEL(s->reg));                       /* MOV r/m16, sreg */
        if (d->kind == OPK_MEM) {
            emit_checks(e, &ea, 2, 1, live_in);
            emit_movzx_rm(e, 4, W_T2, 2, &m);
            m = ea_mem(&ea);
            emit_mov_mr(e, 2, &m, W_T2);
            slow_back(e, s_rf);
        } else {
            emit_mov_rm(e, 2, host_reg(d), &m);
        }
        return;

    case OP_XCHG:
        if (in->ea_valid) {
            const x86_operand *r = d->kind == OPK_REG ? d : s;
            emit_checks(e, &ea, size, 1, live_in);
            m = ea_mem(&ea);
            if (is_high8(r)) { emit_high8_open(e, r->reg); emit_xchg_rm(e, size, r->reg & 3, &m); emit_high8_close(e, r->reg); }
            else emit_xchg_rm(e, size, host_reg(r), &m);
            slow_back(e, s_rf);
            return;
        }
        emit_xchg_rr(e, size, host_reg(d), host_reg(s));
        return;

    case OP_LEA:
        if (in->base < 0 && in->index < 0) { emit_mov_ri(e, 2, host_reg(d), (uint32_t)in->disp & 0xFFFF); return; }
        if (in->base >= 0 && in->index >= 0) m = x64_mi(R_GPR(in->base), R_GPR(in->index), 0, in->disp);
        else m = x64_m(R_GPR(in->base >= 0 ? in->base : in->index), in->disp);
        emit_lea(e, 4, W_T0, &m);
        emit_mov_rr(e, 2, host_reg(d), W_T0);
        return;

    case OP_NOP: return;
    case OP_CBW: emit_cbw(e, 2); return;
    case OP_CWD: emit_cwd(e, 2); return;
    case OP_CLC: emit_clc(e); fl_produce(X86_CF); return;
    case OP_STC: emit_stc(e); fl_produce(X86_CF); return;
    case OP_CMC: fl_need_cf(e); emit_cmc(e); fl_produce(X86_CF); return;
    case OP_CLD: emit_cld(e); return;
    case OP_STD: emit_std(e); return;
    case OP_CLI: case OP_STI:
        fl_save(e, live_in);
        m = M(R_CPU, OFF_EFLAGS + 1);
        if (in->op == OP_CLI) emit_alu_mi(e, 1, X64_ALU_AND, &m, (uint8_t)~(X86_IF >> 8));
        else emit_alu_mi(e, 1, X64_ALU_OR, &m, X86_IF >> 8);
        return;

    case OP_SETCC: {
        int hc;
        emit_cond_setup(e, in->cond, live_in, &hc);
        if (d->kind == OPK_MEM) {
            emit_setcc_r(e, hc, W_T2);
            emit_ea(e, in, &ea);                                 /* after the condition: its slot path uses W_T2/W_T3, this W_T0/W_T1 */
            emit_checks(e, &ea, 1, 1, live_out);
            m = ea_mem(&ea);
            emit_mov_mr(e, 1, &m, W_T2);
            slow_back(e, s_rf);
        } else if (is_high8(d)) {
            emit_setcc_r(e, hc, W_T2);
            emit_high8_open(e, d->reg);
            emit_mov_rr(e, 1, d->reg & 3, W_T2);
            emit_high8_close(e, d->reg);
        } else {
            emit_setcc_r(e, hc, host_reg(d));
        }
        return;
    }

    case OP_PUSH: {
        int val;
        if (d->kind == OPK_MEM) { emit_checks(e, &ea, 2, 0, live_in); m = ea_mem(&ea); emit_movzx_rm(e, 4, W_T2, 2, &m); val = W_T2; }
        else {
            fl_save(e, live_in);
            if (d->kind == OPK_IMM) { emit_mov_ri(e, 4, W_T2, d->imm & 0xFFFF); val = W_T2; }
            else if (d->kind == OPK_SREG) { m = M(R_CPU, OFF_SEG_SEL(d->reg)); emit_movzx_rm(e, 4, W_T2, 2, &m); val = W_T2; }
            else if (d->reg == R_SP && s_model == X86_MODEL_8086) {
                /* 8086 PUSH SP stores the decremented value; 286+ the old one */
                m = M(X64_R12, -2); emit_lea(e, 4, W_T2, &m); val = W_T2;
            } else val = host_reg(d);
        }
        emit_push16(e, val);
        slow_back(e, 0);
        return;
    }

    case OP_POP: {
        if (d->kind == OPK_SREG) {                               /* POP DS/ES */
            fl_save(e, live_in);
            emit_pop16(e, W_T2);
            emit_load_seg_real(e, d->reg, W_T2);
            slow_back(e, 0);
            return;
        }
        if (d->kind == OPK_MEM) {
            /* the destination's checks, then the stack's: nothing changes
             * before every check has passed (a fault leaves SP intact) */
            emit_checks(e, &ea, 2, 1, live_in);
            ea_t sp = { emit_seg_ptr(e, S_SS, W_T3), X64_R12 };
            if (s_regs32) { emit_movzx_rr(e, 4, W_T2, 2, X64_R12); sp.off = W_T2; }
            emit_check_wrap(e, &sp, 2);
            m = ea_mem(&sp);
            emit_mov_rm(e, 2, W_T2, &m);
            m = ea_mem(&ea);
            emit_mov_mr(e, 2, &m, W_T2);
            m = M(X64_R12, 2); emit_lea(e, 4, W_T2, &m);
            emit_mov_rr(e, 2, X64_R12, W_T2);
            slow_back(e, 0);
            return;
        }
        fl_save(e, live_in);
        if (d->reg == R_SP) {                                    /* POP SP: the popped value is the final SP */
            emit_pop16(e, W_T2);
            emit_mov_rr(e, 2, X64_R12, W_T2);
        } else {
            emit_pop16(e, host_reg(d));
        }
        slow_back(e, 0);
        return;
    }

    default:
        break;
    }
    /* not inline after all: the interpreter */
    emit_helper_op(dbt, e, in, live_in);
}

/* ----------------------------------------------------------------------
 * Control flow
 * ---------------------------------------------------------------------- */

/* Inline part of a conditional: guts + Jcc toward the taken arm. Returns
 * the rel32 field to patch. Jcc itself leaves RFLAGS alone; the LOOP
 * family tests CX without flags where it can. */
static uint32_t emit_cond_side_branch(emit_t *e, const x86_insn *in, uint32_t live) {
    x64_mem_t m;
    switch (in->op) {
    case OP_JCC: {
        int hc;
        emit_cond_setup(e, in->cond, live, &hc);
        return emit_jcc_rel32(e, hc);
    }
    case OP_JCXZ:
        if (!s_regs32) {                                         /* canonical: RCX == CX */
            uint32_t zero = emit_jrcxz_rel8(e);
            uint32_t over = emit_jmp_rel8(e);
            emit_patch_rel8(e, zero, emit_pos(e));
            uint32_t taken = emit_jmp_rel32(e);
            emit_patch_rel8(e, over, emit_pos(e));
            return taken;
        }
        fl_save(e, live);
        emit_test_rr(e, 2, X64_RCX, X64_RCX);
        return emit_jcc_rel32(e, X64_CC_E);
    default: {   /* LOOP family: CX = CX - 1, taken when != 0 [&& ZF cond] */
        m = M(X64_RCX, -1);
        emit_lea(e, 4, W_T0, &m);
        emit_mov_rr(e, 2, X64_RCX, W_T0);                        /* 16-bit write: canonical stays canonical */
        if (in->op == OP_LOOP && !s_regs32) {
            uint32_t done = emit_jrcxz_rel8(e);
            uint32_t taken = emit_jmp_rel32(e);
            emit_patch_rel8(e, done, emit_pos(e));
            return taken;
        }
        if (in->op == OP_LOOP) {
            fl_save(e, live);
            emit_test_rr(e, 2, X64_RCX, X64_RCX);
            return emit_jcc_rel32(e, X64_CC_NE);
        }
        /* LOOPE/LOOPNE: the guest's ZF and CX both. The flags go to the
         * slot first: both arms must agree on where they are. */
        int hc;
        fl_save(e, live | X86_ZF);
        emit_cond_setup(e, in->op == OP_LOOPE ? 4 : 5, live, &hc);
        uint32_t no = emit_jcc_rel8(e, hc ^ 1);                  /* ZF condition fails: not taken */
        emit_test_rr(e, 2, X64_RCX, X64_RCX);
        uint32_t taken = emit_jcc_rel32(e, X64_CC_NE);
        emit_patch_rel8(e, no, emit_pos(e));
        return taken;
    }
    }
}

/* A block ender: from here on everything is a tail. The flags are saved
 * and the whole block charged before the ender's own checks, so a slow
 * path (a wrap at FFFFh, a push onto code) that runs the transfer through
 * the interpreter, or faults there, charges nothing more. */
static void emit_branch_ender(x86_dbt *dbt, emit_t *e, const x86_insn *in, uint32_t ip_after, uint32_t n_ops) {
    ea_t ea;
    x64_mem_t m;
    s_slow_first = s_nslow;
    if (in->op == OP_JCC || in->op == OP_JCXZ || in->op == OP_LOOP || in->op == OP_LOOPE || in->op == OP_LOOPNE) {
        /* conditional in final position: two-edge ender */
        uint32_t patch = emit_cond_side_branch(e, in, ARITH);
        uint32_t rf = s_rf;
        fl_save(e, ARITH);
        emit_tail_prologue(e, n_ops);
        emit_edge(dbt, e, target_key(ip_after));                 /* not taken */
        emit_patch_rel32(e, patch, emit_pos(e));
        s_rf = rf;
        fl_save(e, ARITH);
        emit_tail_prologue(e, n_ops);
        emit_edge(dbt, e, target_key(ip_after + in->ops[0].imm));
        return;
    }
    fl_save(e, ARITH);
    emit_tail_prologue(e, n_ops);
    switch (in->op) {
    case OP_JMP:
        if (in->ops[0].kind == OPK_IMM) { emit_edge(dbt, e, target_key(ip_after + in->ops[0].imm)); return; }
        if (in->ops[0].kind == OPK_MEM) {
            emit_ea(e, in, &ea);
            emit_check_wrap(e, &ea, 2);
            m = ea_mem(&ea);
            emit_movzx_rm(e, 4, W_T3, 2, &m);
        } else {
            emit_movzx_rr(e, 4, W_T3, 2, host_reg(&in->ops[0]));
        }
        emit_dynamic_key(e, W_T3);
        emit_dynamic_tail(dbt, e);
        break;
    case OP_CALL: {
        int dyn = in->ops[0].kind != OPK_IMM;
        if (dyn && in->ops[0].kind == OPK_MEM) {
            emit_ea(e, in, &ea);
            emit_check_wrap(e, &ea, 2);
            m = ea_mem(&ea);
            emit_movzx_rm(e, 4, W_T3, 2, &m);
        } else if (dyn) {
            emit_movzx_rr(e, 4, W_T3, 2, host_reg(&in->ops[0]));
        }
        emit_mov_ri(e, 4, W_T2, (uint16_t)ip_after);
        emit_push16(e, W_T2);
        if (dyn) { emit_dynamic_key(e, W_T3); emit_dynamic_tail(dbt, e); }
        else emit_edge(dbt, e, target_key(ip_after + in->ops[0].imm));
        break;
    }
    default:   /* RET */
        emit_pop16(e, W_T3);
        if (in->ops[0].kind == OPK_IMM) {
            m = M(X64_R12, (int32_t)(in->ops[0].imm & 0xFFFF));
            emit_lea(e, 4, W_T2, &m);
            emit_mov_rr(e, 2, X64_R12, W_T2);
        }
        emit_dynamic_key(e, W_T3);
        emit_dynamic_tail(dbt, e);
        break;
    }
    /* An ender's slow path ran the transfer through the interpreter:
     * the chunk rejoins here, past the inline tail, and leaves from
     * wherever the interpreter left EIP. */
    if (s_nslow > s_slow_first) {
        slow_back(e, 0);
        emit_dynamic_key_cpu(e, s_blk, s_cpu);
        emit_dynamic_tail(dbt, e);
    }
}

/* ----------------------------------------------------------------------
 * Block emission: a plan (dbt_translate.c) to x86-64
 * ---------------------------------------------------------------------- */
uint8_t *dbt_arch_emit_block(x86_dbt *dbt, const dbt_block *b) {
    x86_cpu *cpu = dbt->cpu;
    x64_mem_t m;
    if (s_strict_exit < 0)
        s_strict_exit = dbt->verify && getenv("X86_VERIFY_STRICT") != NULL;
    s_cur_lin = dbt_key_lin(b->key);
    s_mode_bits = b->mode_bits;
    s_regs32 = b->regs32;
    s_model = b->model;
    s_cpu = cpu;
    s_blk = b;
    s_rf = 0;
    s_nslow = 0;
    /* what this backend emits inline so far: real-mode-shaped blocks
     * without paging; everything else is helpers, and transfers end them */
    int inl = !b->all_helper && !b->flat && !b->seg16 && !b->v86 && !b->paged && !getenv("X86_X64_HELPERS");

    emit_t e = { .buf = dbt->code_buf, .offset = dbt->code_used, .capacity = CODE_BUF_SIZE };
    uint8_t *entry = dbt->code_buf + e.offset;

    /* Entry: budget check. Exhausted → out-of-line exit at our own start. */
    m = M(R_CPU, OFF_JIT_CNT);
    emit_alu_mi(&e, 8, X64_ALU_CMP, &m, 0);
    uint32_t budget_patch = emit_jcc_rel32(&e, X64_CC_S);

    struct { uint32_t patch_off; uint32_t insns; uint32_t op; uint32_t rf; } sides[MAX_BLOCK_INSNS];
    uint32_t n_sides = 0;
    uint32_t n = 0;
    int moved = 0, ended = 0;
    uint32_t ip = b->start_ip;
    for (uint32_t i = 0; i < b->n_ops; i++) {
        const x86_insn *in = &b->decs[i];
        s_cur_i = i;
        s_cur_ip_after = b->ip_afters[i];
        s_cur_ip_start = b->ip_afters[i] - in->len;
        s_cur_n_done = i + 1;
        n = i + 1;
        ip = b->ip_afters[i];
        uint32_t live_out = b->fmask[i], live_in = b->live_in[i];
        int is_inline = inl && b->cls[i] == C_INLINE && inline_ok(in);
        if (is_inline && (b->role[i] == ROLE_UNCOND || (b->role[i] == ROLE_COND && i == b->n_ops - 1))) {
            s_cur_n_done = 0;                                    /* the whole block is charged by the tail */
            emit_branch_ender(dbt, &e, in, b->ip_afters[i], b->n_ops);
            ended = 1;
            break;
        }
        if (is_inline && b->role[i] == ROLE_COND) {
            /* the taken arm is a block exit that observes every flag, so
             * the guts may drop nothing: live_in (ARITH, by op_flag_effects) */
            sides[n_sides].patch_off = emit_cond_side_branch(&e, in, live_in);
            sides[n_sides].insns = i + 1;
            sides[n_sides].op = i;
            sides[n_sides].rf = s_rf;
            n_sides++;
            continue;
        }
        if (is_inline) { emit_op(dbt, &e, in, live_in, live_out); continue; }
        /* a helper; a transfer or a segment load through one ends the
         * block here — whatever the plan thought — so every flag is live
         * into it, as at any block exit */
        int ends = b->role[i] == ROLE_UNCOND || b->role[i] == ROLE_COND || b->role[i] == ROLE_HELPER_END
                || dbt_near_transfer(in->op)
                || in->op == OP_CALLF || in->op == OP_JMPF || in->op == OP_RETF || in->op == OP_INT || in->op == OP_INT3;
        emit_helper_op(dbt, &e, in, ends ? ARITH : live_in);
        if (ends) { moved = 1; break; }
    }
    if (!ended) {
        fl_save(&e, ARITH);
        emit_tail_prologue(&e, n);
        if (moved) {
            emit_dynamic_key_cpu(&e, b, cpu);
            if (dbt_loads_segment(&b->decs[n - 1]) && (b->flat || b->seg16)) emit_jmp_rel32_to(&e, dbt->exit_stub_off);   /* the key must be rebuilt */
            else emit_dynamic_tail(dbt, &e);
        } else {
            emit_edge(dbt, &e, target_key(ip));
        }
    }

    /* Side-exit chunks: taken arms of mid-block conditionals, each with
     * its exact instruction count, and the flags as they were there. */
    for (uint32_t k = 0; k < n_sides; k++) {
        emit_patch_rel32(&e, sides[k].patch_off, emit_pos(&e));
        s_rf = sides[k].rf;
        fl_save(&e, ARITH);
        emit_tail_prologue(&e, sides[k].insns);
        const x86_insn *in = &b->decs[sides[k].op];
        emit_edge(dbt, &e, target_key(b->ip_afters[sides[k].op] + in->ops[0].imm));
    }

    /* Slow paths of the checked instructions. */
    emit_slow_chunks(dbt, &e);

    /* Budget-exhausted exit: nothing executed, next = this block. */
    emit_patch_rel32(&e, budget_patch, emit_pos(&e));
    m = M(R_CPU, OFF_EIP);
    emit_mov_mi(&e, 4, &m, b->start_ip);
    emit_jmp_rel32_to(&e, s_exit_eip_off);

    dbt->code_used = e.offset;
    return entry;
}
