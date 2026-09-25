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
#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#endif

/* The C calling convention the thunks and the trampoline speak: System V,
 * or on Windows the Microsoft x64 one � arguments in RCX RDX R8 R9, 32
 * bytes of the caller's stack for the callee to spill them into, and
 * RSI RDI callee-saved (the trampoline saves them with the rest). */
#if defined(_WIN64)
#define ABI_A0 X64_RCX
#define ABI_A1 X64_RDX
#define ABI_A2 X64_R8
#define ABI_SHADOW 32
#else
#define ABI_A0 X64_RDI
#define ABI_A1 X64_RSI
#define ABI_A2 X64_RDX
#define ABI_SHADOW 0
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
#define OFF_EXC_ERR     ((int32_t)offsetof(x86_cpu, exc_err))
#define OFF_INSN_COUNT  ((int32_t)offsetof(x86_cpu, insn_count))
#define OFF_JIT_BUDGET  ((int32_t)offsetof(x86_cpu, jit_budget))
#define OFF_JIT_CNT     ((int32_t)offsetof(x86_cpu, jit_cnt_save))
#define OFF_JIT_CUR_LIN ((int32_t)offsetof(x86_cpu, jit_cur_lin))
#define OFF_JIT_CUR_HIT ((int32_t)offsetof(x86_cpu, jit_cur_hit))
#define OFF_JIT_FLAGS   ((int32_t)offsetof(x86_cpu, jit_flags))
#define OFF_DEV_WPLANE  ((int32_t)offsetof(x86_cpu, dev_wplane))
#define OFF_DEV_RPLANE  ((int32_t)offsetof(x86_cpu, dev_rplane))
#define OFF_PGD_R       ((int32_t)offsetof(x86_cpu, pgd_r))
#define OFF_PGD_W       ((int32_t)offsetof(x86_cpu, pgd_w))
#define OFF_TLB         ((int32_t)offsetof(x86_cpu, tlb))
#define OFF_CR0         ((int32_t)offsetof(x86_cpu, cr0))
#define OFF_FPU_SW      ((int32_t)(offsetof(x86_cpu, fpu) + offsetof(x86_fpu, sw)))

#define ARITH  X86_ARITH_FLAGS

#define M(base, disp) x64_m((base), (disp))

/* Thunk/stub offsets in the code buffer (emitted with the trampoline). */
static uint32_t s_exec_thunk_off, s_post_thunk_off, s_port_thunk_off, s_exit_eip_off;

/* -V strict mode: every block returns to dbt_run (no links, no probe). */
static int s_strict_exit = -1;
static uint64_t s_mode_bits;
static int s_regs32;          /* 386: the pinned registers hold 32 bits, 16-bit values need masking as addresses */
static int s_flat;            /* a flat block: 32-bit code and addresses off R_MEM, no segment arithmetic */
static int s_seg16;           /* segmented 16-bit protected mode: the real-mode shape with limit checks in place of wrap checks */
static int s_ss32;            /* ...on a 32-bit stack (SS.B): pushes and pops move all of ESP */
static int s_paged;           /* under paging: each access translated — V86 through cpu->pgd_r/pgd_w (emit_paged), flat and seg16 through cpu->tlb */
static int s_pg_user;         /* ...at CPL 3: the entry must allow user access */
static int s_skip_smc;        /* the current store looks at its bitmap bytes after landing (emit_post_store) */
static uint32_t s_dyn_imm_lin; /* nonzero: read the current instruction's immediate from this physical address */
/* Flat blocks address memory as R_MEM + offset; the fast path is everything
 * below 16 MB, less (for reads, while a device answers them) the VGA
 * window A0000-AFFFF. Anything else is the slow path's. */
#define FLAT_TOP 0x1000000u
_Static_assert(FLAT_TOP + 4 <= X86_MEM_SIZE, "flat fast path must stay inside guest memory");
static int s_model;
static int s_bmi2 = -1;       /* host has RORX (flag-free rotates for the high bytes) */
static const x86_cpu *s_cpu;
static const dbt_block *s_blk;

/* Per-instruction cursor: what a thunk call or a slow path needs. */
static uint32_t s_cur_lin, s_cur_ip_after, s_cur_ip_start, s_cur_n_done, s_cur_i;

int dbt_jit_available(const x86_cpu *cpu) {
#if defined(__x86_64__) || defined(_M_X64)
    return cpu->mem_mirrored;
#else
    (void)cpu;
    return 0;
#endif
}

static void host_features(void) {
    if (s_bmi2 >= 0) return;
    s_bmi2 = 0;
#if defined(__x86_64__) || defined(_M_X64)
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
 * i.e. RDI, RSI, RDX, RCX, R8 (on Windows RCX, RDX, R8, R9 and the
 * budget on the stack above the shadow space). RSP is 8 mod 16 on entry;
 * six pushes (eight on Windows) and an 8-byte adjustment make it 0 mod 16
 * inside blocks, so a thunk (one CALL deep) is at 8 and aligns with one
 * more push before calling C.
 * ---------------------------------------------------------------------- */
void dbt_emit_trampoline(x86_dbt *dbt) {
    emit_t e = { .buf = dbt->code_buf, .offset = 0, .capacity = CODE_BUF_SIZE };
    x64_mem_t m;
    host_features();

    emit_push_r(&e, X64_RBX); emit_push_r(&e, X64_RBP);
    emit_push_r(&e, X64_R12); emit_push_r(&e, X64_R13);
    emit_push_r(&e, X64_R14); emit_push_r(&e, X64_R15);
#if defined(_WIN64)
    emit_push_r(&e, X64_RSI); emit_push_r(&e, X64_RDI);
    emit_alu_ri(&e, 8, X64_ALU_SUB, X64_RSP, 8);
    emit_mov_rr(&e, 8, R_CPU, X64_RCX);
    emit_mov_rr(&e, 8, R_MEM, X64_RDX);
    emit_mov_rr(&e, 8, W_T2, X64_R8);
    m = M(X64_RSP, 8 + 8 * 8 + 8 + 32); emit_mov_rm(&e, 8, W_T0, &m);   /* budget: past the pushes, the return address, the shadow space */
    m = M(R_CPU, OFF_JIT_BUDGET); emit_mov_mr(&e, 8, &m, W_T0);
    m = M(R_CPU, OFF_JIT_CNT);    emit_mov_mr(&e, 8, &m, W_T0);
#else
    emit_alu_ri(&e, 8, X64_ALU_SUB, X64_RSP, 8);
    emit_mov_rr(&e, 8, R_CPU, X64_RDI);
    emit_mov_rr(&e, 8, R_MEM, X64_RSI);
    m = M(R_CPU, OFF_JIT_BUDGET); emit_mov_mr(&e, 8, &m, X64_R8);
    m = M(R_CPU, OFF_JIT_CNT);    emit_mov_mr(&e, 8, &m, X64_R8);
    emit_mov_rr(&e, 8, W_T2, X64_RDX);
#endif
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
#if defined(_WIN64)
    emit_pop_r(&e, X64_RDI); emit_pop_r(&e, X64_RSI);
#endif
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
    emit_mov_rr(&e, 8, ABI_A0, R_CPU);
    emit_mov_rr(&e, 4, ABI_A1, W_T2);
    emit_mov_ri(&e, 8, X64_RAX, (uint64_t)(uintptr_t)dbt_h_exec);
    if (ABI_SHADOW) emit_alu_ri(&e, 8, X64_ALU_SUB, X64_RSP, ABI_SHADOW);
    emit_call_r(&e, X64_RAX);
    if (ABI_SHADOW) emit_alu_ri(&e, 8, X64_ALU_ADD, X64_RSP, ABI_SHADOW);
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

    /* ---- Post-store thunk: CALLed after a store landed on bytes the code
     * bitmap marks, with R8 = phys | size << 28, R9 = the block's linear
     * address, R10 = ip after the instruction, R11 = instructions executed
     * so far including it. Runs the store hook (device memory, then SMC)
     * with only the caller-saved pinned registers parked: no spill, no
     * reload; the guest's flags are in the slot already and its DF rides
     * in RFLAGS across the call. If the sweep invalidated the running
     * block, it leaves at ip-after; else it returns. ---- */
    s_post_thunk_off = e.offset;
    emit_pushfq(&e);
    emit_push_r(&e, X64_RAX); emit_push_r(&e, X64_RCX); emit_push_r(&e, X64_RDX);
    emit_push_r(&e, X64_RSI); emit_push_r(&e, X64_RDI);
    emit_push_r(&e, W_T2); emit_push_r(&e, W_T3);
    emit_alu_ri(&e, 8, X64_ALU_SUB, X64_RSP, 8 + ABI_SHADOW);  /* 16-byte alignment for the call */
    m = M(R_CPU, OFF_JIT_CUR_LIN); emit_mov_mr(&e, 4, &m, W_T1);
    m = M(R_CPU, OFF_JIT_CUR_HIT); emit_mov_mi(&e, 4, &m, 0);
    emit_mov_rr(&e, 8, ABI_A0, R_CPU);
    emit_mov_rr(&e, 4, ABI_A1, W_T0);
    emit_cld(&e);
    emit_mov_ri(&e, 8, X64_RAX, (uint64_t)(uintptr_t)dbt_h_post_store);
    emit_call_r(&e, X64_RAX);
    emit_alu_ri(&e, 8, X64_ALU_ADD, X64_RSP, 8 + ABI_SHADOW);
    emit_pop_r(&e, W_T3); emit_pop_r(&e, W_T2);
    emit_pop_r(&e, X64_RDI); emit_pop_r(&e, X64_RSI);
    emit_pop_r(&e, X64_RDX); emit_pop_r(&e, X64_RCX); emit_pop_r(&e, X64_RAX);
    emit_popfq(&e);
    m = M(R_CPU, OFF_JIT_CUR_HIT); emit_alu_mi(&e, 4, X64_ALU_CMP, &m, 0);
    uint32_t hit = emit_jcc_rel8(&e, X64_CC_NE);
    emit_ret(&e);
    emit_patch_rel8(&e, hit, emit_pos(&e));
    emit_alu_ri(&e, 8, X64_ALU_ADD, X64_RSP, 8);             /* drop the return address */
    m = M(R_CPU, OFF_EIP);         emit_mov_mr(&e, 4, &m, W_T2);
    m = M(R_CPU, OFF_JIT_CNT);     emit_alu_mr(&e, 8, X64_ALU_SUB, &m, W_T3);
    emit_jmp_rel32_to(&e, s_exit_eip_off);

    /* ---- Port thunk: CALLed for OUT with R8 = port | size << 16, R9 =
     * the value. Same parking as the post-store thunk; the guest's flags
     * are all in the slot (an OUT sees every flag live). Returns
     * dbt_h_out's verdict in R8: 0 go on, 1 leave after the instruction,
     * 2 #GP at it. ---- */
    s_port_thunk_off = e.offset;
    emit_pushfq(&e);
    emit_push_r(&e, X64_RAX); emit_push_r(&e, X64_RCX); emit_push_r(&e, X64_RDX);
    emit_push_r(&e, X64_RSI); emit_push_r(&e, X64_RDI);
    emit_alu_ri(&e, 8, X64_ALU_SUB, X64_RSP, 8 + ABI_SHADOW);
    emit_mov_rr(&e, 4, ABI_A1, W_T0);                        /* before A2: on Windows A2 is R8, W_T0 */
    emit_mov_rr(&e, 4, ABI_A2, W_T1);
    emit_mov_rr(&e, 8, ABI_A0, R_CPU);
    emit_cld(&e);
    emit_mov_ri(&e, 8, X64_RAX, (uint64_t)(uintptr_t)dbt_h_out);
    emit_call_r(&e, X64_RAX);
    emit_mov_rr(&e, 4, W_T0, X64_RAX);
    emit_alu_ri(&e, 8, X64_ALU_ADD, X64_RSP, 8 + ABI_SHADOW);
    emit_pop_r(&e, X64_RDI); emit_pop_r(&e, X64_RSI);
    emit_pop_r(&e, X64_RDX); emit_pop_r(&e, X64_RCX); emit_pop_r(&e, X64_RAX);
    emit_popfq(&e);
    emit_ret(&e);

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
    /* (X86_GOLDEN never runs a block: a fixed stand-in keeps the hash
     * free of where ASLR put the cache, so two builds compare) */
    emit_mov_ri(e, 8, W_T1, dbt->golden ? 0x60D1DE4ACAC4E000ull : (uint64_t)(uintptr_t)dbt->aux->cache);
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
    if (s_flat) emit_mov_rr(e, 4, R_KEY, ip); else emit_movzx_rr(e, 4, R_KEY, 2, ip);
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
/* X86_KIND_COUNT=1: guest instructions retired by block kind (real, v86,
 * flat, seg16, pm helper-only), counted at block tails, printed at exit. */
static uint64_t s_kind_insns[5];
static int s_kind_count = -1;
static void kind_count_print(void) {
    static const char *nm[5] = { "real", "v86", "flat", "seg16", "pm-helpers" };
    uint64_t t = 0;
    for (int i = 0; i < 5; i++) t += s_kind_insns[i];
    fprintf(stderr, "  JIT insns by block kind (at tails):");
    for (int i = 0; i < 5; i++) fprintf(stderr, " %s %llu (%.1f%%)", nm[i], (unsigned long long)s_kind_insns[i], t ? 100.0 * s_kind_insns[i] / t : 0.0);
    fprintf(stderr, "\n");
}
static void emit_tail_prologue(emit_t *e, uint32_t n) {
    x64_mem_t m = M(R_CPU, OFF_JIT_CNT);
    emit_alu_mi(e, 8, X64_ALU_SUB, &m, n);
    if (s_kind_count < 0) { s_kind_count = getenv("X86_KIND_COUNT") != NULL; if (s_kind_count) atexit(kind_count_print); }
    if (s_kind_count) {
        int k = s_blk->all_helper ? 4 : s_blk->v86 ? 1 : s_blk->flat ? 2 : s_blk->seg16 ? 3 : 0;
        emit_push_r(e, W_T0);
        emit_mov_ri(e, 8, W_T0, (uint64_t)(uintptr_t)&s_kind_insns[k]);
        x64_mem_t c = M(W_T0, 0);
        emit_alu_mi(e, 8, X64_ALU_ADD, &c, n);
        emit_pop_r(e, W_T0);
    }
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
        if (!s->back_off) { fprintf(stderr, "dbt: slow site without a rejoin (op %u)\n", s->op_i); abort(); }
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
    int seg;       /* the segment register (S_*), for a seg16 block's limit check */
} ea_t;

/* The stack's width in the block: ESP whole on a flat or SS32 stack, SP alone otherwise. */
static int sp_width(void) { return s_flat || s_ss32 ? 4 : 2; }

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
/* Flat: a 32-bit offset off R_MEM — a pinned register as it stands, or
 * base + index * 2^scale + disp into W_T0 (a 32-bit LEA wraps at 4 GB like
 * the guest's address arithmetic). Flag-free. */
static void emit_ea_flat(emit_t *e, const x86_insn *in, ea_t *ea) {
    ea->segp = R_MEM;
    ea->seg = in->seg;
    int hi = is_high8(&in->ops[0]) || is_high8(&in->ops[1]);
    if (in->base < 0 && in->index < 0) { emit_mov_ri(e, 4, W_T0, (uint32_t)in->disp); ea->off = W_T0; return; }
    if (in->index < 0 && in->disp == 0 && !hi) { ea->off = R_GPR(in->base); return; }
    x64_mem_t m;
    if (in->base >= 0 && in->index >= 0) m = x64_mi(R_GPR(in->base), R_GPR(in->index), in->scale, in->disp);
    else if (in->index >= 0) m = x64_mi(X64_NOREG, R_GPR(in->index), in->scale, in->disp);
    else m = x64_m(R_GPR(in->base), in->disp);
    emit_lea(e, 4, W_T0, &m);
    ea->off = W_T0;
}

/* 16-bit effective address into W_T0 (or a pinned register when it is
 * exactly that, below the 386); the segment pointer in W_T1 unless DS.
 * Flag-free. */
static void emit_ea(emit_t *e, const x86_insn *in, ea_t *ea) {
    if (s_flat) { emit_ea_flat(e, in, ea); return; }
    ea->segp = emit_seg_ptr(e, in->seg, W_T1);
    ea->seg = in->seg;
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
/* The operand at ea (+ disp): a translated ea (emit_paged) has no offset register. */
static x64_mem_t ea_mem_d(const ea_t *ea, int32_t disp) { return ea->off == X64_NOREG ? x64_m(ea->segp, disp) : x64_mi(ea->segp, ea->off, 0, disp); }
static x64_mem_t ea_mem(const ea_t *ea) { return ea_mem_d(ea, 0); }

/* Checks before an access at ea of `size` bytes: a word at FFFFh (the
 * 8086 wraps in-segment, the 286+ raises #GP: the helper does either),
 * and for a store the code bitmap. RFLAGS must be free here. */
/* ---- Fault sites (seg16) ----
 * A limit check that fails raises #GP — or #SS through SS on the 386 —
 * with error code 0 at the instruction, nothing of it committed: every
 * check precedes its instruction's first state change, and an op that
 * can fault has every flag live-in (op_may_fault), so the slot is whole. */
typedef struct { uint32_t patch_off, ip_start, n_done; uint8_t vector; } fault_site_t;
#define FAULT_MAX 128
static fault_site_t s_fault[FAULT_MAX];
static uint32_t s_nfault;
static uint32_t s_usable_ok;     /* seg16: segments (1 << S_*) whose usable bit this block has already checked */
static uint32_t s_check_busy;    /* scratch registers (1 << reg) holding values across an access check */

static void fault_site(emit_t *e, int cc, uint8_t vector) {
    if (s_nfault >= FAULT_MAX) { fprintf(stderr, "dbt: fault site table overflow\n"); abort(); }
    s_fault[s_nfault++] = (fault_site_t){ emit_jcc_rel32(e, cc), s_cur_ip_start, s_cur_n_done, vector };
}
static void emit_fault_chunks(emit_t *e) {
    for (uint32_t k = 0; k < s_nfault; k++) {
        fault_site_t *f = &s_fault[k];
        emit_patch_rel32(e, f->patch_off, emit_pos(e));
        x64_mem_t m = M(R_CPU, OFF_EXC);     emit_mov_mi(e, 4, &m, f->vector);
        m = M(R_CPU, OFF_EXC_ERR);           emit_mov_mi(e, 4, &m, 0);
        m = M(R_CPU, OFF_EIP);               emit_mov_mi(e, 4, &m, f->ip_start);
        if (f->n_done) { m = M(R_CPU, OFF_JIT_CNT); emit_alu_mi(e, 8, X64_ALU_SUB, &m, f->n_done); }
        emit_jmp_rel32_to(e, s_exit_eip_off);
    }
    s_nfault = 0;
}

/* KEY_SEG16 access check: off + size - 1 <= limit and the segment usable
 * (a null one has limit 0, which alone would let offset 0 through). Both
 * from the cpu at run time: the block runs under whatever the code has
 * loaded since. In 64-bit arithmetic (an SS32 offset near 4G must not
 * wrap the sum). Clobbers RFLAGS and a scratch that is not the ea's. */
/* A scratch for a check's arithmetic: W_T3, else W_T2, else W_T3 parked
 * on the host stack (POP [mem] holds the stack's pointer and offset in
 * both while the destination's are in W_T0/W_T1); the caller pops it
 * before its branch (flags survive a pop). */
static int check_scratch(emit_t *e, const ea_t *ea, int *park) {
    uint32_t busy = s_check_busy | (ea->off == X64_NOREG ? 0 : 1u << ea->off) | (1u << ea->segp);
    int t = W_T3;
    *park = 0;
    if (busy & (1u << W_T3)) t = W_T2;
    if (busy & (1u << t)) { t = W_T3; *park = 1; emit_push_r(e, t); }
    return t;
}

/* A read while a device answers the VGA window (planar modes: the
 * latches load on every read): host address - mem with bits 19:16 == A
 * is the window's, and the instruction runs through the interpreter. */
static void emit_check_window(emit_t *e, const ea_t *ea) {
    int park, t = check_scratch(e, ea, &park);
    x64_mem_t m = x64_mi(ea->segp, ea->off, 0, 0);
    emit_lea(e, 8, t, &m);
    emit_alu_rr(e, 8, X64_ALU_SUB, t, R_MEM);
    emit_shift_ri(e, 8, X64_SH_SHR, t, 16);
    emit_alu_ri(e, 4, X64_ALU_CMP, t, 0xA);
    if (park) emit_pop_r(e, t);
    slow_site(e, X64_CC_E);
}

/* ---- Paged V86 ----
 * One translated access per instruction: the linear page's delta from
 * cpu->pgd_r (reads) or pgd_w (writes: the page must be writable and
 * dirty already) is added to the identity host address. No entry, an
 * access that crosses the page, or a word at the segment's end: the
 * whole instruction through the interpreter (slow_site). Clobbers W_T1
 * and W_T3; the ea becomes { W_T3, no offset }. RFLAGS must be free. */
static void emit_paged(emit_t *e, ea_t *ea, int size, int write) {
    x64_mem_t m;
    if (size > 1) { emit_alu_ri(e, 4, X64_ALU_CMP, ea->off, (uint32_t)(0x10000 - size)); slow_site(e, X64_CC_A); }
    m = ea_mem(ea); emit_lea(e, 8, W_T3, &m);                              /* identity host address */
    if (size > 1) {
        emit_mov_rr(e, 4, W_T1, W_T3);
        emit_alu_ri(e, 4, X64_ALU_AND, W_T1, 0xFFF);
        emit_alu_ri(e, 4, X64_ALU_CMP, W_T1, (uint32_t)(0x1000 - size));
        slow_site(e, X64_CC_A);
    }
    emit_mov_rr(e, 8, W_T1, W_T3);
    emit_alu_rr(e, 8, X64_ALU_SUB, W_T1, R_MEM);
    emit_shift_ri(e, 8, X64_SH_SHR, W_T1, 12);                              /* linear page, < 0x110 */
    m = x64_mi(R_CPU, W_T1, 3, write ? OFF_PGD_W : OFF_PGD_R); emit_mov_rm(e, 8, W_T1, &m);   /* delta, or X86_PGD_NONE */
    emit_test_ri(e, 8, W_T1, 1);
    slow_site(e, X64_CC_NE);
    emit_alu_rr(e, 8, X64_ALU_ADD, W_T3, W_T1);
    ea->segp = W_T3;
    ea->off = X64_NOREG;
}

static void emit_limit_check(emit_t *e, int seg, int segp, int off, int size);

/* ---- Paged flat and segmented blocks: the interpreter's TLB ----
 * The entry for the access's linear page must match and allow it: valid,
 * plain RAM (X86_TLB_MEM; a pure store may also go to the VGA window,
 * X86_TLB_WMEM, where the bitmap hands it to the device), dirty for a
 * store, the user bits at CPL 3; and the access must stay on the page.
 * Anything else is the slow path: the interpreter walks, faults, sets
 * accessed/dirty and fills the entry for next time. `lin` holds the
 * linear address and survives; `idx` is a scratch for the index; W_T3
 * takes the tag and then the physical page. */
static void emit_tlb_lookup(emit_t *e, int lin, int idx, int size, int write, int pure_store) {
    x64_mem_t m;
    uint32_t need = X86_TLB_V | (pure_store ? X86_TLB_WMEM : X86_TLB_MEM) | (write ? X86_TLB_D : 0)
                  | (s_pg_user ? (write ? X86_TLB_UW : X86_TLB_U) : 0);
    emit_mov_rr(e, 4, idx, lin); emit_shift_ri(e, 4, X64_SH_SHR, idx, 12); emit_movzx_rr(e, 4, idx, 1, idx);   /* index */
    m = x64_mi(R_CPU, idx, 3, OFF_TLB); emit_mov_rm(e, 4, W_T3, &m);                      /* tag */
    emit_alu_rr(e, 4, X64_ALU_XOR, W_T3, lin);
    emit_test_ri(e, 4, W_T3, 0xFFFFF000u);                                                 /* the entry is for this page */
    slow_site(e, X64_CC_NE);
    m = x64_mi(R_CPU, idx, 3, OFF_TLB); emit_mov_rm(e, 4, W_T3, &m);
    emit_alu_ri(e, 4, X64_ALU_AND, W_T3, need);
    emit_alu_ri(e, 4, X64_ALU_CMP, W_T3, need);                                            /* every needed bit */
    slow_site(e, X64_CC_NE);
    if (size > 1) {
        emit_mov_rr(e, 4, W_T3, lin);
        emit_alu_ri(e, 4, X64_ALU_AND, W_T3, 0xFFF);
        emit_alu_ri(e, 4, X64_ALU_CMP, W_T3, (uint32_t)(0x1000 - size));
        slow_site(e, X64_CC_A);
    }
    m = x64_mi(R_CPU, idx, 3, OFF_TLB + 4); emit_mov_rm(e, 4, W_T3, &m);                  /* physical page, zero-extended */
}
/* Flat: ea->segp becomes R_MEM + physical page - linear page, the offset
 * (never W_T1 or W_T3) stays. Clobbers W_T1, W_T3. */
static void emit_pgflat(emit_t *e, ea_t *ea, int size, int write, int pure_store) {
    emit_tlb_lookup(e, ea->off, W_T1, size, write, pure_store);
    emit_mov_rr(e, 4, W_T1, ea->off);
    emit_alu_ri(e, 4, X64_ALU_AND, W_T1, 0xFFFFF000u);
    emit_alu_rr(e, 8, X64_ALU_SUB, W_T3, W_T1);                                            /* signed delta */
    emit_alu_rr(e, 8, X64_ALU_ADD, W_T3, R_MEM);
    ea->segp = W_T3;
}
/* Segmented: after the limit check, the linear address segp + off - mem;
 * the ea becomes the translated host address with no offset. Clobbers
 * W_T0, W_T1, W_T3 (the offset and segment pointer are spent). */
static void emit_pgseg16(emit_t *e, ea_t *ea, int size, int write, int pure_store) {
    x64_mem_t m = ea_mem(ea);
    emit_lea(e, 8, W_T1, &m);
    emit_alu_rr(e, 8, X64_ALU_SUB, W_T1, R_MEM);                                           /* linear, fits 32 bits */
    emit_tlb_lookup(e, W_T1, W_T0, size, write, pure_store);
    emit_alu_ri(e, 4, X64_ALU_AND, W_T1, 0xFFF);
    emit_alu_rr(e, 8, X64_ALU_ADD, W_T3, W_T1);
    emit_alu_rr(e, 8, X64_ALU_ADD, W_T3, R_MEM);
    ea->segp = W_T3;
    ea->off = X64_NOREG;
}
/* The one translated access of an instruction in a paged block. */
static void emit_translate(emit_t *e, ea_t *ea, int size, int write, int pure_store) {
    if (s_seg16) { emit_limit_check(e, ea->seg, ea->segp, ea->off, size); emit_pgseg16(e, ea, size, write, pure_store); }
    else emit_paged(e, ea, size, write);
}

static void emit_limit_check(emit_t *e, int seg, int segp, int off, int size) {
    uint8_t vec = (uint8_t)(seg == S_SS && s_regs32 ? X86_EXC_SS : X86_EXC_GP);
    x64_mem_t m = M(R_CPU, OFF_SEG_LIMIT(seg));
    if (seg == S_SS && s_ss32) {
        /* a 32-bit ESP: off + size - 1 can pass 4G, so in 64 bits */
        ea_t ea = { segp, off, seg };
        int park, t = check_scratch(e, &ea, &park);
        emit_mov_rm(e, 4, t, &m);                           /* zero-extended */
        emit_alu_rr(e, 8, X64_ALU_SUB, t, off);             /* limit - off, exact as a signed 64-bit value */
        emit_alu_ri(e, 8, X64_ALU_CMP, t, (uint32_t)(size - 1));
        if (park) emit_pop_r(e, t);                         /* (flags survive a pop) */
        fault_site(e, X64_CC_L, vec);
    } else if (size == 1) {
        emit_alu_rm(e, 4, X64_ALU_CMP, off, &m);            /* a 16-bit offset: off > limit, unsigned */
        fault_site(e, X64_CC_A, vec);
    } else {
        ea_t ea = { segp, off, seg };
        int park, t = check_scratch(e, &ea, &park);
        x64_mem_t hi = M(off, size - 1);
        emit_lea(e, 4, t, &hi);                             /* the last byte: at most FFFF + 3, no wrap */
        emit_alu_rm(e, 4, X64_ALU_CMP, t, &m);
        if (park) emit_pop_r(e, t);
        fault_site(e, X64_CC_A, vec);
    }
    /* Usable: SS always is in a KEY_SEG16 block (dbt_seg16_ok), and a
     * segment cannot change inside a block (every load ends it), so DS or
     * ES is asked once, at its first access in the block; later accesses
     * are only reached past that check. */
    if (seg != S_SS && !(s_usable_ok & (1u << seg))) {
        m = M(R_CPU, OFF_SEG_USABLE(seg));
        emit_alu_mi(e, 1, X64_ALU_CMP, &m, 0);
        fault_site(e, X64_CC_E, vec);
        s_usable_ok |= 1u << seg;
    }
}

static void emit_check_wrap(emit_t *e, const ea_t *ea, int size) {
    if (s_seg16) { emit_limit_check(e, ea->seg, ea->segp, ea->off, size); return; }
    if (size < 2) return;
    emit_alu_ri(e, 2, X64_ALU_CMP, ea->off, 0xFFFF);
    slow_site(e, X64_CC_E);
}
static void emit_check_smc(emit_t *e, const ea_t *ea, int size) {
    x64_mem_t m = ea_mem_d(ea, (int32_t)X86_BM_DELTA);
    emit_alu_mi(e, size, X64_ALU_CMP, &m, 0);
    slow_site(e, X64_CC_NE);
}

/* Post-store paths: a plain store lands first and its bitmap bytes are
 * tested after (one compare of 1, 2 or 4 bytes; a wider store that
 * starts just before a block still reaches into it). Nonzero goes to an
 * out-of-line chunk: bytes that are device memory and nothing else, with
 * the device offering its single-plane fast path (cpu->dev_wplane), are
 * copied into the write plane and the window bytes refreshed from the
 * read plane inline; anything else — code, a descriptor, an empty bus,
 * the device's other modes — runs the store hook through the post-store
 * thunk. Entered with RFLAGS free (the checks before the store saved the
 * live bits) and nothing of the instruction left in the scratch. */
typedef struct {
    uint32_t patch_off, back_off;
    uint32_t ip_after, n_done;
    int segp, off;
    uint8_t size;
} post_site_t;
#define POST_MAX 128
static post_site_t s_post[POST_MAX];
static uint32_t s_npost;

static void emit_post_store(emit_t *e, const ea_t *ea, int size) {
    if (s_npost >= POST_MAX) { fprintf(stderr, "dbt: post-store table overflow\n"); abort(); }
    x64_mem_t m = ea_mem_d(ea, (int32_t)X86_BM_DELTA);
    emit_alu_mi(e, size, X64_ALU_CMP, &m, 0);
    post_site_t *p = &s_post[s_npost++];
    p->patch_off = emit_jcc_rel32(e, X64_CC_NE);
    p->back_off = emit_pos(e);
    p->ip_after = s_cur_ip_after;
    p->n_done = s_cur_n_done;
    p->segp = ea->segp; p->off = ea->off;
    p->size = (uint8_t)size;
}

/* Leave sites: an inline op whose helper asked to leave the block (R8
 * nonzero after the port thunk): 2 is a fault at the instruction, else
 * leave after it; either way charge the instructions done including it. */
typedef struct { uint32_t patch_off, ip_after, ip_start, n_done; } leave_site_t;
#define LEAVE_MAX 64
static leave_site_t s_leave[LEAVE_MAX];
static uint32_t s_nleave;

static void emit_leave_chunks(emit_t *e) {
    for (uint32_t k = 0; k < s_nleave; k++) {
        leave_site_t *l = &s_leave[k];
        emit_patch_rel32(e, l->patch_off, emit_pos(e));
        x64_mem_t m = M(R_CPU, OFF_JIT_CNT);
        emit_alu_mi(e, 8, X64_ALU_SUB, &m, l->n_done);
        emit_mov_ri(e, 4, W_T1, l->ip_after);
        emit_mov_ri(e, 4, W_T2, l->ip_start);
        emit_alu_ri(e, 4, X64_ALU_CMP, W_T0, 2);
        emit_cmov_rr(e, 4, X64_CC_E, W_T1, W_T2);
        m = M(R_CPU, OFF_EIP);
        emit_mov_mr(e, 4, &m, W_T1);
        emit_jmp_rel32_to(e, s_exit_eip_off);
    }
    s_nleave = 0;
}

static void emit_post_chunks(emit_t *e) {
    static const uint32_t dev_rep[5] = { 0, 0x80, 0x8080, 0, 0x80808080u };
    for (uint32_t k = 0; k < s_npost; k++) {
        post_site_t *p = &s_post[k];
        int size = p->size;
        emit_patch_rel32(e, p->patch_off, emit_pos(e));
        ea_t pea = { p->segp, p->off, 0 };
        x64_mem_t m = ea_mem(&pea);
        emit_lea(e, 8, W_T3, &m);
        emit_alu_rr(e, 8, X64_ALU_SUB, W_T3, R_MEM);                    /* W_T3 = phys */
        m = x64_mi(R_MEM, W_T3, 0, (int32_t)X86_BM_DELTA);
        emit_alu_mi(e, size, X64_ALU_CMP, &m, dev_rep[size]);
        uint32_t miss1 = emit_jcc_rel8(e, X64_CC_NE);
        m = M(R_CPU, OFF_DEV_WPLANE); emit_mov_rm(e, 8, W_T2, &m);
        emit_test_rr(e, 8, W_T2, W_T2);
        uint32_t miss2 = emit_jcc_rel8(e, X64_CC_E);
        m = x64_mi(R_MEM, W_T3, 0, 0);          emit_mov_rm(e, size, W_T0, &m);   /* the bytes just stored */
        m = x64_mi(W_T2, W_T3, 0, -0xA0000);    emit_mov_mr(e, size, &m, W_T0);   /* into the write plane */
        m = M(R_CPU, OFF_DEV_RPLANE); emit_mov_rm(e, 8, W_T2, &m);
        m = x64_mi(W_T2, W_T3, 0, -0xA0000);    emit_mov_rm(e, size, W_T0, &m);   /* the window shows the read plane */
        m = x64_mi(R_MEM, W_T3, 0, 0);          emit_mov_mr(e, size, &m, W_T0);
        emit_jmp_rel32_to(e, p->back_off);
        emit_patch_rel8(e, miss1, emit_pos(e));
        emit_patch_rel8(e, miss2, emit_pos(e));
        emit_mov_rr(e, 4, W_T0, W_T3);
        if (size > 1) emit_alu_ri(e, 4, X64_ALU_OR, W_T0, (uint32_t)size << 28);
        emit_mov_ri(e, 4, W_T1, s_cur_lin);
        emit_mov_ri(e, 4, W_T2, p->ip_after);
        emit_mov_ri(e, 4, W_T3, p->n_done);
        emit_call_rel32_to(e, s_post_thunk_off);
        emit_jmp_rel32_to(e, p->back_off);
    }
    s_npost = 0;
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
static void emit_push_seg(emit_t *e, int val, int size) {
    x64_mem_t m = M(X64_R12, -size);
    emit_lea(e, 4, W_T0, &m);
    if (!s_ss32) emit_movzx_rr(e, 4, W_T0, 2, W_T0);
    ea_t ea = { emit_seg_ptr(e, S_SS, W_T1), W_T0, S_SS };
    s_check_busy |= 1u << val;
    if (s_paged) emit_translate(e, &ea, size, 1, 0); else emit_check_wrap(e, &ea, size);
    s_check_busy &= ~(1u << val);
    emit_check_smc(e, &ea, size);
    m = ea_mem(&ea);
    emit_mov_mr(e, size, &m, val);
    if (s_paged && s_seg16) {                                /* (the translation spent W_T0) */
        m = M(X64_R12, -size); emit_lea(e, 4, W_T0, &m);
        if (!s_ss32) emit_movzx_rr(e, 4, W_T0, 2, W_T0);
    }
    emit_mov_rr(e, sp_width(), X64_R12, W_T0);
}
/* POP `size` bytes (2, or 4 for a 386's 32-bit operand on a segmented
 * stack) into register dst (dst may be W_T2 or W_T3). */
static void emit_pop_seg(emit_t *e, int dst, int size) {
    ea_t ea = { emit_seg_ptr(e, S_SS, W_T1), X64_R12, S_SS };
    if (s_regs32 && !s_ss32) { emit_movzx_rr(e, 4, W_T0, 2, X64_R12); ea.off = W_T0; }
    if (s_paged) emit_translate(e, &ea, size, 0, 0); else emit_check_wrap(e, &ea, size);
    x64_mem_t m = ea_mem(&ea);
    emit_mov_rm(e, size, dst, &m);
    m = M(X64_R12, size);
    emit_lea(e, 4, W_T0, &m);
    emit_mov_rr(e, sp_width(), X64_R12, W_T0);
}
static void emit_push16(emit_t *e, int val) { emit_push_seg(e, val, 2); }
static void emit_pop16(emit_t *e, int dst) { emit_pop_seg(e, dst, 2); }

static void emit_check_flat(emit_t *e, ea_t *ea, int size, int store, int reads);

/* The flat stack: PUSH/POP of a dword at ESP off R_MEM, range-checked
 * (a 32-bit wrap lands above FLAT_TOP), the push's bitmap bytes too. */
static void emit_push32(emit_t *e, int val) {
    x64_mem_t m = M(X64_R12, -4);
    emit_lea(e, 4, W_T0, &m);
    ea_t ea = { R_MEM, W_T0, S_SS };
    emit_check_flat(e, &ea, 4, 1, 0);
    m = ea_mem(&ea);
    emit_mov_mr(e, 4, &m, val);
    emit_mov_rr(e, 4, X64_R12, W_T0);
}
static void emit_pop32(emit_t *e, int dst) {
    ea_t ea = { R_MEM, X64_R12, S_SS };
    emit_check_flat(e, &ea, 4, 0, 1);
    x64_mem_t m = ea_mem(&ea);
    emit_mov_rm(e, 4, dst, &m);
    m = M(X64_R12, 4);
    emit_lea(e, 4, X64_R12, &m);
}
/* The block's own stack width: a word in real mode, a dword flat. */
static void emit_push_stk(emit_t *e, int val) { if (s_flat) emit_push32(e, val); else emit_push16(e, val); }
static void emit_pop_stk(emit_t *e, int dst)  { if (s_flat) emit_pop32(e, dst); else emit_pop16(e, dst); }
/* ...with the instruction's own operand size (32 bits on a segmented stack: seg16's 66-prefixed forms). */
static void emit_push_w(emit_t *e, int val, int w) { if (s_flat) emit_push32(e, val); else emit_push_seg(e, val, w); }
static void emit_pop_w(emit_t *e, int dst, int w)  { if (s_flat) emit_pop32(e, dst); else emit_pop_seg(e, dst, w); }

/* PUSHAD/POPAD on the flat stack. CONTRACT (interp, 386): PUSHAD stores
 * EAX ECX EDX EBX ESP EBP ESI EDI downward, ESP as it was before the
 * instruction; POPAD skips the ESP slot. Both ends of the 32-byte frame
 * are range-checked and PUSHAD's bitmap bytes all looked at first. */
static void emit_pusha32(emit_t *e, uint32_t live_in) {
    x64_mem_t m;
    fl_save(e, live_in);
    m = M(X64_R12, -32); emit_lea(e, 4, W_T0, &m);
    emit_alu_ri(e, 4, X64_ALU_CMP, W_T0, FLAT_TOP - 28); slow_site(e, X64_CC_AE);
    for (int k = 0; k < 4; k++) { m = x64_mi(R_MEM, W_T0, 0, (int32_t)X86_BM_DELTA + 8 * k); emit_alu_mi(e, 8, X64_ALU_CMP, &m, 0); slow_site(e, X64_CC_NE); }
    static const int order[8] = { R_DI, R_SI, R_BP, R_SP, R_BX, R_DX, R_CX, R_AX };
    for (int k = 0; k < 8; k++) { m = x64_mi(R_MEM, W_T0, 0, 4 * k); emit_mov_mr(e, 4, &m, R_GPR(order[k])); }
    emit_mov_rr(e, 4, X64_R12, W_T0);
}
static void emit_popa32(emit_t *e, uint32_t live_in) {
    x64_mem_t m;
    fl_save(e, live_in);
    emit_alu_ri(e, 4, X64_ALU_CMP, X64_R12, FLAT_TOP - 28); slow_site(e, X64_CC_AE);
    static const int order[8] = { R_DI, R_SI, R_BP, -1, R_BX, R_DX, R_CX, R_AX };
    for (int k = 0; k < 8; k++) { if (order[k] < 0) continue; m = x64_mi(R_MEM, X64_R12, 0, 4 * k); emit_mov_rm(e, 4, R_GPR(order[k]), &m); }
    m = M(X64_R12, 32); emit_lea(e, 4, X64_R12, &m);
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

static void emit_string1(emit_t *e, const x86_insn *in, uint32_t live_in);
static void emit_rep_string(emit_t *e, const x86_insn *in, uint32_t live_in);
static void emit_mul16(emit_t *e, const x86_insn *in, ea_t *ea, uint32_t live_in, uint32_t live_out);
static void emit_div16(emit_t *e, const x86_insn *in, ea_t *ea, uint32_t live_in);
static void emit_shift_cl(emit_t *e, const x86_insn *in, ea_t *ea, uint32_t live_in, uint32_t live_out);
static void emit_pushf16(emit_t *e, uint32_t live_in);
static void emit_popf16(emit_t *e, uint32_t live_in);

/* POP [mem] addresses its destination with the SP it has already
 * incremented; the inline POP computes the address first, so a [esp+...]
 * destination is the helper's. */
static int pop_via_sp(const x86_insn *in) {
    return in->op == OP_POP && in->ops[0].kind == OPK_MEM && (in->base == R_SP || in->index == R_SP);
}

/* Can this instruction be emitted inline by the code below? (The plan's
 * class says the interpreter is not required; this says the emitter is
 * ready for it.) */
static int inline_ok(const dbt_block *b, const x86_insn *in) {
    if (in->opsize != 2 || in->adsize != 2) return 0;
    int farptr = in->op == OP_CALLF || in->op == OP_JMPF || in->op == OP_LDS || in->op == OP_LES;
    for (int i = 0; i < 2; i++) if (in->ops[i].kind == OPK_MEM && in->ops[i].size > 2 && !farptr) return 0;
    switch (in->op) {
    case OP_ADD: case OP_OR: case OP_ADC: case OP_SBB: case OP_AND: case OP_SUB: case OP_XOR: case OP_CMP:
    case OP_TEST: case OP_INC: case OP_DEC: case OP_NOT: case OP_NEG:
    case OP_MOV: case OP_XCHG: case OP_LEA: case OP_NOP: case OP_CBW: case OP_CWD:
    case OP_CLC: case OP_STC: case OP_CMC: case OP_CLD: case OP_STD: case OP_CLI: case OP_STI:
    case OP_SETCC: case OP_OUT: case OP_WAIT:
    case OP_CALL: case OP_JMP: case OP_JCC: case OP_JCXZ: case OP_LOOP: case OP_LOOPE: case OP_LOOPNE: case OP_RET:
        return 1;
    case OP_PUSH:   /* (the 8086's FE /6, a byte push, is the interpreter's) */
        return in->ops[0].kind == OPK_SREG || in->ops[0].kind == OPK_IMM || in->ops[0].size == 2;
    case OP_POP:
        if (in->ops[0].kind == OPK_SREG) return in->ops[0].reg == S_DS || in->ops[0].reg == S_ES;
        return in->ops[0].size == 2;
    case OP_MOVSEG:
        if (in->ops[0].kind == OPK_SREG) return in->ops[0].reg == S_DS || in->ops[0].reg == S_ES;
        return 1;
    case OP_CALLF: case OP_JMPF: case OP_RETF: case OP_INT: case OP_INT3:
        return 1;
    case OP_MOVS: case OP_STOS: case OP_CMPS: case OP_SCAS: case OP_LODS:
        if (in->seg != S_DS && in->seg != S_ES && in->seg != S_SS) return 0;
        if (!in->rep) return 1;
        return in->op != OP_LODS;                    /* (a 386's upper halves: emit_rep_string keeps them) */
    case OP_MUL: case OP_IMUL:
        return in->opcode2 != 0xAF;                  /* one-operand forms */
    case OP_DIV: case OP_IDIV:
        return b->model >= X86_MODEL_286;
    case OP_SHL: case OP_SAL: case OP_SHR: case OP_SAR: case OP_ROL: case OP_ROR: case OP_RCL: case OP_RCR:
        if (in->ops[1].kind == OPK_IMM) return (in->ops[1].imm & 0xFF) < (uint32_t)in->ops[0].size * 8;
        if (b->model == X86_MODEL_8086) return 0;    /* counts are not masked there */
        if (in->ops[0].size == 1 && b->model >= X86_MODEL_386) return 0;   /* the byte-by-16/24 quirk */
        return 1;
    case OP_PUSHF: case OP_POPF:
        return 1;
    case OP_LDS: case OP_LES: case OP_XLAT: case OP_LAHF: case OP_SAHF:
        return 1;
    case OP_MOVZX: case OP_MOVSX:
        return 1;
    default:
        return 0;
    }
}

/* Segmented 16-bit protected mode: the real-mode list, less what the
 * interpreter keeps in protected mode (far transfers, interrupt frames,
 * segment loads, POPF/CLI/STI: the front end's) and what this backend
 * keeps with the interpreter (32-bit near transfers: EIP stays whole). */
static int inline_ok_seg16(const dbt_block *b, const x86_insn *in) {
    if (in->adsize != 2) return 0;
    if (in->ea_valid && in->seg != S_DS && in->seg != S_ES && in->seg != S_SS) return 0;
    if (in->opsize == 4) {
        /* 66-prefixed forms (DOS/4GW's kernel, Windows' 32-bit helpers in
         * 16-bit segments): the size-generic emitters, 16-bit addressing */
        switch (in->op) {
        case OP_ADD: case OP_OR: case OP_ADC: case OP_SBB: case OP_AND: case OP_SUB: case OP_XOR: case OP_CMP:
        case OP_TEST: case OP_INC: case OP_DEC: case OP_NOT: case OP_NEG: case OP_MOV: case OP_XCHG: case OP_LEA:
        case OP_MOVZX: case OP_MOVSX: case OP_CBW: case OP_CWD:
            return 1;
        case OP_SHL: case OP_SAL: case OP_SHR: case OP_SAR:
            if (in->ops[1].kind == OPK_IMM) return (in->ops[1].imm & 0xFF) < (uint32_t)in->ops[0].size * 8;
            return in->ops[0].size == 4;                           /* by CL: not a byte under a 66 prefix */
        case OP_PUSH:
            if (b->paged && in->ops[0].kind == OPK_MEM) return 0;
            return in->ops[0].kind != OPK_SREG && !(in->ops[0].kind == OPK_REG && in->ops[0].reg == R_SP);
        case OP_POP:
            if (b->paged && in->ops[0].kind == OPK_MEM) return 0;
            return in->ops[0].kind != OPK_SREG && !(in->ops[0].kind == OPK_REG && in->ops[0].reg == R_SP);
        default:
            return 0;                                              /* near transfers included: EIP would need to stay whole */
        }
    }
    if (b->paged) {                                          /* the translation uses W_T1..W_T3 */
        if (in->op == OP_SETCC && in->ops[0].kind == OPK_MEM) return 0;
        if (in->op == OP_CALL && in->ops[0].kind == OPK_REG && in->ops[0].reg == R_SP) return 0;
    }
    switch (in->op) {
    case OP_CALLF: case OP_JMPF: case OP_RETF: case OP_INT: case OP_INT3: case OP_IRET:
    case OP_LDS: case OP_LES: case OP_POPF: case OP_CLI: case OP_STI:
        return 0;
    case OP_MOVS: case OP_STOS: case OP_CMPS: case OP_SCAS: case OP_LODS:
        if (b->paged) return 0;                                    /* two accesses: the front end's rule too */
        break;
    case OP_MOVSEG:
        return in->ops[0].kind != OPK_SREG;
    case OP_POP:
        if (in->ops[0].kind == OPK_SREG || pop_via_sp(in)) return 0;
        break;
    default:
        break;
    }
    if (in->ea_valid && in->seg != S_DS && in->seg != S_ES && in->seg != S_SS) return 0;
    return inline_ok(b, in);
}

/* Virtual-8086 mode: the real-mode list (the front end keeps what is
 * IOPL-sensitive, I/O and — under paging — anything with two accesses
 * with the interpreter); a paged block translates one access with all
 * the scratch, so SETcc into memory and CALL SP stay helpers there. */
static int inline_ok_v86(const dbt_block *b, const x86_insn *in) {
    if (b->paged) {
        if (in->op == OP_SETCC && in->ops[0].kind == OPK_MEM) return 0;
        if (in->op == OP_CALL && in->ops[0].kind == OPK_REG && in->ops[0].reg == R_SP) return 0;
    }
    return inline_ok(b, in);
}

/* Flat protected mode: 32-bit code and addressing through DS/ES/SS. */
static int inline_ok_flat(const dbt_block *b, const x86_insn *in) {
    if (in->ea_valid && (in->adsize != 4 || (in->seg != S_DS && in->seg != S_ES && in->seg != S_SS))) return 0;
    if (b->paged) {                                      /* one translated access an instruction */
        switch (in->op) {
        case OP_MOVS: case OP_STOS: case OP_CMPS: case OP_SCAS: case OP_LODS: case OP_PUSHA: case OP_POPA:
            return 0;
        case OP_PUSH: case OP_POP: case OP_CALL:
            if (in->ops[0].kind == OPK_MEM) return 0;
            break;
        default: break;
        }
        if (in->op == OP_CALL && in->ops[0].kind == OPK_REG && in->ops[0].reg == R_SP) return 0;
    }
    switch (in->op) {
    case OP_ADD: case OP_OR: case OP_ADC: case OP_SBB: case OP_AND: case OP_SUB: case OP_XOR: case OP_CMP:
    case OP_TEST: case OP_INC: case OP_DEC: case OP_NOT: case OP_NEG:
    case OP_MOV: case OP_XCHG: case OP_LEA: case OP_NOP: case OP_CBW: case OP_CWD:
    case OP_CLC: case OP_STC: case OP_CMC: case OP_CLD: case OP_STD:
    case OP_MOVZX: case OP_MOVSX: case OP_SETCC: case OP_LAHF: case OP_SAHF:
    case OP_MUL: case OP_IMUL: case OP_IMUL3: case OP_OUT: case OP_WAIT:
        return 1;
    case OP_SHL: case OP_SAL: case OP_SHR: case OP_SAR: case OP_ROL: case OP_ROR: case OP_RCL: case OP_RCR:
        if (in->ops[1].kind == OPK_IMM) return (in->ops[1].imm & 0xFF) < (uint32_t)in->ops[0].size * 8;
        return in->ops[0].size != 1;                 /* by CL: the byte-by-16/24 quirk */
    case OP_DIV: case OP_IDIV:
        return 1;
    case OP_PUSH:
        return in->opsize == 4 && in->ops[0].kind != OPK_SREG;
    case OP_POP:
        return in->opsize == 4 && in->ops[0].kind != OPK_SREG && !pop_via_sp(in);
    case OP_PUSHA: case OP_POPA: case OP_LEAVE:
        return in->opsize == 4;
    case OP_JMP: case OP_CALL: case OP_RET: case OP_JCC:
        return in->opsize == 4;
    case OP_JCXZ: case OP_LOOP: case OP_LOOPE: case OP_LOOPNE:
        return in->opsize == 4 && in->adsize == 4;
    case OP_XLAT:
        return in->adsize == 4;
    case OP_MOVS: case OP_STOS: case OP_CMPS: case OP_SCAS: case OP_LODS:
        if (in->adsize != 4 || (in->seg != S_DS && in->seg != S_ES && in->seg != S_SS)) return 0;
        if (in->rep && in->op == OP_LODS) return 0;
        return 1;
    case OP_SHLD: case OP_SHRD:
        return in->ops[0].size == 4 && in->imm2 != 0xFFFFFFFFu && (in->imm2 & 31);   /* 32-bit, immediate 1..31 */
    case OP_BT: case OP_BTS: case OP_BTR: case OP_BTC:
        return in->ops[1].kind == OPK_IMM || !in->ea_valid;   /* a register bit offset into memory: a bit string */
    default:
        return 0;
    }
}

/* The front end asks: real-mode-shaped blocks and flat ones, unpaged. */
int dbt_arch_can_inline(const dbt_block *b, const x86_insn *in) {
    if (getenv("X86_X64_HELPERS")) return 0;
    if (b->paged && getenv("X86_X64_NOPAGED")) return 0;
    if (b->v86) return inline_ok_v86(b, in);
    if (b->flat) return inline_ok_flat(b, in);
    if (b->seg16) return inline_ok_seg16(b, in);
    return inline_ok(b, in);
}

/* Flat checks: the offset below FLAT_TOP (every access of up to 4 bytes
 * then stays inside memory); a read outside the VGA window while a device
 * answers reads there; a store's bitmap bytes. RFLAGS must be free. */
static void emit_check_flat(emit_t *e, ea_t *ea, int size, int store, int reads) {
    if (s_paged) {
        emit_pgflat(e, ea, size, store, store && s_skip_smc);      /* (the window: reads have no MEM entry, pure stores go by the bitmap) */
        if (store && !s_skip_smc) emit_check_smc(e, ea, size);
        return;
    }
    emit_alu_ri(e, 4, X64_ALU_CMP, ea->off, FLAT_TOP);
    slow_site(e, X64_CC_AE);
    if (reads && s_blk->devread) {
        x64_mem_t m = M(ea->off, -0xA0000);
        emit_lea(e, 4, W_T3, &m);
        emit_alu_ri(e, 4, X64_ALU_CMP, W_T3, 0x10000);
        slow_site(e, X64_CC_B);
    }
    if (store && !s_skip_smc) emit_check_smc(e, ea, size);
}

/* The checks of an access at ea: RFLAGS is freed first (the live guest
 * bits go to the slot), then the wrap check for a word, the bitmap for
 * a store — or, in a flat block, the range and window checks. `reads`:
 * the instruction reads the operand (a pure store need not avoid the
 * window: the bitmap sends it to the device). */
static void emit_checks_rw(emit_t *e, ea_t *ea, int size, int store, int reads, uint32_t live_in) {
    fl_save(e, live_in);
    if (s_flat) { emit_check_flat(e, ea, size, store, reads); return; }
    if (s_paged) {
        emit_translate(e, ea, size, store, store && s_skip_smc);   /* (V86: the window's pages have no entry, reads there are the interpreter's) */
        if (store && !s_skip_smc) emit_check_smc(e, ea, size);
        return;
    }
    emit_check_wrap(e, ea, size);
    if (reads && s_blk->devread) emit_check_window(e, ea);
    if (store && !s_skip_smc) emit_check_smc(e, ea, size);
}
static void emit_checks(emit_t *e, ea_t *ea, int size, int store, uint32_t live_in) {
    emit_checks_rw(e, ea, size, store, 1, live_in);
}

/* A value of SIZE bytes from memory into a 32-bit scratch, zero-extended. */
static void emit_load_val(emit_t *e, int size, int dst, const x64_mem_t *m) {
    if (size == 4) emit_mov_rm(e, 4, dst, m); else emit_movzx_rm(e, 4, dst, size, m);
}

/* A patched immediate (s_dyn_imm_lin): loaded from where the code is
 * now, sign-extending an imm8 the way the decoder did, into W_T2. */
static int emit_dyn_imm(emit_t *e, const x86_operand *o) {
    uint8_t enc = o->imm_enc;
    x64_mem_t m = M(R_MEM, (int32_t)s_dyn_imm_lin);
    switch (X86_IMM_LEN(enc)) {
    case 1:
        if (X86_IMM_SX(enc)) emit_movsx_rm(e, 4, W_T2, 1, &m); else emit_movzx_rm(e, 4, W_T2, 1, &m);
        break;
    case 2: emit_movzx_rm(e, 4, W_T2, 2, &m); break;
    default: emit_mov_rm(e, 4, W_T2, &m); break;
    }
    return W_T2;
}

/* An ALU op (add .. cmp, test) in any operand shape. High-byte
 * operands next to a memory operand go through the rotate. */
static void emit_op_alu(emit_t *e, const x86_insn *in, ea_t *ea, uint32_t live_in) {
    int alu = in->op == OP_TEST ? -1 : alu_of_op[in->op];
    int size = in->ops[0].size;
    const x86_operand *d = &in->ops[0], *s = &in->ops[1];
    x64_mem_t m;
    int cin = in->op == OP_ADC || in->op == OP_SBB;
    if (d->kind == OPK_MEM) {
        emit_checks(e, ea, size, in->op != OP_CMP && in->op != OP_TEST, live_in);
        if (cin) fl_need_cf(e);                                   /* after the checks: they clobber CF */
        m = ea_mem(ea);
        if (s->kind == OPK_IMM && s_dyn_imm_lin) {
            int r = emit_dyn_imm(e, s);
            if (alu < 0) emit_test_mr(e, size, &m, r);
            else emit_alu_mr(e, size, alu, &m, r);
        } else if (s->kind == OPK_IMM) {
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
        if (alu < 0) { emit_load_val(e, size, W_T2, &m); emit_test_rr(e, size, r, W_T2); }
        else emit_alu_rm(e, size, alu, r, &m);
        if (dhi) emit_high8_close(e, d->reg);
        fl_produce(ARITH);
        slow_back(e, s_rf);
        return;
    }
    int r = host_reg(d);
    if (cin) fl_need_cf(e);
    if (s->kind == OPK_IMM && s_dyn_imm_lin) {
        int v = emit_dyn_imm(e, s);
        if (alu < 0) emit_test_rr(e, size, r, v);
        else emit_alu_rr(e, size, alu, r, v);
    } else if (s->kind == OPK_IMM) {
        if (alu < 0) emit_test_ri(e, size, r, s->imm);
        else emit_alu_ri(e, size, alu, r, s->imm);
    } else {
        if (alu < 0) emit_test_rr(e, size, r, host_reg(s));
        else emit_alu_rr(e, size, alu, r, host_reg(s));
    }
    fl_produce(ARITH);
}

static void emit_op(x86_dbt *dbt, emit_t *e, const x86_insn *in, uint32_t live_in, uint32_t live_out) {
    ea_t ea = { 0, 0, 0 };
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
        if (s->kind == OPK_REG) {                                /* by CL */
            emit_shift_cl(e, in, &ea, live_in, live_out);
            return;
        }
        uint32_t cnt = s->imm & 0xFF;
        int sh = sh_of_op[in->op];
        int rot = in->op == OP_ROL || in->op == OP_ROR || in->op == OP_RCL || in->op == OP_RCR;
        if (cnt == 0) {                                          /* no flags, no change (CONTRACT: count 0 leaves them)... */
            if (d->kind == OPK_MEM) {                            /* ...but the operand is still read and written back:
                                                                  * a limit, a page or a watched byte faults as the
                                                                  * interpreter's would (the value itself is unchanged) */
                emit_checks(e, &ea, size, 1, live_in);
                slow_back(e, s_rf);
            }
            return;
        }
        if (in->op == OP_RCL || in->op == OP_RCR) fl_need_cf(e);
        /* OF after a shift by more than one is the last step's on the
         * 8086 and undefined (and different) on the host: two steps then */
        int two = cnt > 1 && (live_out & X86_OF) && in->op != OP_SAR;
        if (d->kind == OPK_MEM) {
            emit_checks(e, &ea, size, 1, live_in);
            if (in->op == OP_RCL || in->op == OP_RCR) fl_need_cf(e);   /* (the checks moved it to the slot) */
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
            /* the store lands, then its bitmap bytes are looked at (emit_post_store) */
            s_skip_smc = 1;
            emit_checks_rw(e, &ea, size, 1, 0, live_in);
            s_skip_smc = 0;
            m = ea_mem(&ea);
            if (s->kind == OPK_IMM && s_dyn_imm_lin) emit_mov_mr(e, size, &m, emit_dyn_imm(e, s));
            else if (s->kind == OPK_IMM) emit_mov_mi(e, size, &m, s->imm);
            else if (is_high8(s)) { emit_high8_open(e, s->reg); emit_mov_mr(e, size, &m, s->reg & 3); emit_high8_close(e, s->reg); }
            else emit_mov_mr(e, size, &m, host_reg(s));
            emit_post_store(e, &ea, size);
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
        if (s->kind == OPK_IMM && s_dyn_imm_lin) emit_mov_rr(e, size, host_reg(d), emit_dyn_imm(e, s));
        else if (s->kind == OPK_IMM) emit_mov_ri(e, size, host_reg(d), s->imm);
        else emit_mov_rr(e, size, host_reg(d), host_reg(s));
        return;

    case OP_WAIT:
        /* Nothing to do unless CR0 has both TS and MP (#NM) or the x87 has
         * an unmasked exception pending (SW.ES: #MF, or FERR#); then the
         * whole WAIT is the interpreter's, out of line. Without a
         * coprocessor WAIT does nothing at all. */
        if (!s_cpu->has_fpu) return;
        fl_save(e, live_in);
        m = M(R_CPU, OFF_CR0); emit_mov_rm(e, 4, W_T0, &m);
        emit_alu_ri(e, 4, X64_ALU_AND, W_T0, X86_CR0_TS | X86_CR0_MP);
        emit_alu_ri(e, 4, X64_ALU_CMP, W_T0, X86_CR0_TS | X86_CR0_MP);
        slow_site(e, X64_CC_E);
        m = M(R_CPU, OFF_FPU_SW); emit_test_mi(e, 2, &m, 0x80);   /* SW.ES */
        slow_site(e, X64_CC_NE);
        slow_back(e, 0);
        return;

    case OP_OUT: {
        /* Through the port thunk; RFLAGS is free (every flag is live-in
         * at an OUT, so fl_save put them all in the slot). The port can
         * halt the machine or flip A20 (leave after the instruction) or
         * refuse it (#GP at it, cpu->exc set): both out of line. */
        int psize = s->size;
        fl_save(e, live_in);
        if (d->kind == OPK_IMM) emit_mov_ri(e, 4, W_T0, (d->imm & 0xFF) | ((uint32_t)psize << 16));
        else { emit_movzx_rr(e, 4, W_T0, 2, R_GPR(R_DX)); emit_alu_ri(e, 4, X64_ALU_OR, W_T0, (uint32_t)psize << 16); }
        if (psize == 4) emit_mov_rr(e, 4, W_T1, R_GPR(R_AX));
        else emit_movzx_rr(e, 4, W_T1, psize, R_GPR(R_AX));
        emit_call_rel32_to(e, s_port_thunk_off);
        emit_test_rr(e, 4, W_T0, W_T0);
        if (s_nleave >= LEAVE_MAX) { fprintf(stderr, "dbt: leave-site table overflow\n"); abort(); }
        s_leave[s_nleave++] = (leave_site_t){ emit_jcc_rel32(e, X64_CC_NE), s_cur_ip_after, s_cur_ip_start, s_cur_n_done };
        return;
    }

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

    case OP_LEA: {
        uint32_t amask = in->adsize == 4 ? 0xFFFFFFFFu : 0xFFFF;
        if (in->base < 0 && in->index < 0) { emit_mov_ri(e, size, host_reg(d), (uint32_t)in->disp & amask); return; }
        if (in->base >= 0 && in->index >= 0) m = x64_mi(R_GPR(in->base), R_GPR(in->index), in->scale, in->disp);
        else if (in->index >= 0) m = x64_mi(X64_NOREG, R_GPR(in->index), in->scale, in->disp);
        else m = x64_m(R_GPR(in->base), in->disp);
        if (size == 4 && in->adsize == 4) { emit_lea(e, 4, host_reg(d), &m); return; }
        emit_lea(e, 4, W_T0, &m);                                /* a 16-bit address or a 16-bit result: the low half */
        if (size == 4) emit_movzx_rr(e, 4, host_reg(d), 2, W_T0);   /* LEA r32 with a 16-bit address: zero-extended */
        else emit_mov_rr(e, size, host_reg(d), W_T0);
        return;
    }

    case OP_NOP: return;
    case OP_CBW: emit_cbw(e, in->opsize); return;
    case OP_CWD: emit_cwd(e, in->opsize); return;
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
            s_check_busy |= 1u << W_T2;
            emit_checks(e, &ea, 1, 1, live_out);
            s_check_busy &= ~(1u << W_T2);
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
        int val, w = s_flat ? 4 : in->opsize;
        if (d->kind == OPK_MEM) {
            emit_checks(e, &ea, w, 0, live_in); m = ea_mem(&ea);
            if (w == 4) emit_mov_rm(e, 4, W_T2, &m); else emit_movzx_rm(e, 4, W_T2, 2, &m);
            val = W_T2;
        }
        else {
            fl_save(e, live_in);
            if (d->kind == OPK_IMM) { emit_mov_ri(e, 4, W_T2, w == 4 ? d->imm : (d->imm & 0xFFFF)); val = W_T2; }
            else if (d->kind == OPK_SREG) { m = M(R_CPU, OFF_SEG_SEL(d->reg)); emit_movzx_rm(e, 4, W_T2, 2, &m); val = W_T2; }
            else if (d->reg == R_SP && s_model == X86_MODEL_8086) {
                /* 8086 PUSH SP stores the decremented value; 286+ the old one */
                m = M(X64_R12, -2); emit_lea(e, 4, W_T2, &m); val = W_T2;
            } else val = host_reg(d);
        }
        emit_push_w(e, val, w);
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
            int w = s_flat ? 4 : in->opsize;
            emit_checks_rw(e, &ea, w, 1, 0, live_in);
            ea_t sp = { s_flat ? R_MEM : emit_seg_ptr(e, S_SS, W_T3), X64_R12, S_SS };
            if (s_flat) emit_check_flat(e, &sp, 4, 0, 1);
            else {
                if (s_regs32 && !s_ss32) { emit_movzx_rr(e, 4, W_T2, 2, X64_R12); sp.off = W_T2; }
                emit_check_wrap(e, &sp, w);
            }
            m = ea_mem(&sp);
            emit_mov_rm(e, w, W_T2, &m);
            m = ea_mem(&ea);
            emit_mov_mr(e, w, &m, W_T2);
            m = M(X64_R12, w); emit_lea(e, 4, W_T2, &m);
            emit_mov_rr(e, sp_width(), X64_R12, W_T2);
            slow_back(e, 0);
            return;
        }
        fl_save(e, live_in);
        if (d->reg == R_SP) {                                    /* POP SP: the popped value is the final SP */
            emit_pop_stk(e, W_T2);
            emit_mov_rr(e, s_flat ? 4 : 2, X64_R12, W_T2);
        } else {
            emit_pop_w(e, host_reg(d), s_flat ? 4 : in->opsize);
        }
        slow_back(e, 0);
        return;
    }

    case OP_MOVS: case OP_STOS: case OP_LODS: case OP_CMPS: case OP_SCAS:
        if (in->rep) emit_rep_string(e, in, live_in);
        else emit_string1(e, in, live_in);
        slow_back(e, s_rf);
        return;

    case OP_PUSHA: emit_pusha32(e, live_in); slow_back(e, 0); return;
    case OP_POPA:  emit_popa32(e, live_in); slow_back(e, 0); return;
    case OP_LEAVE:                                               /* ESP = EBP; POP EBP */
        fl_save(e, live_in);
        emit_mov_rr(e, 4, X64_R12, X64_RBP);
        emit_pop32(e, X64_RBP);
        slow_back(e, 0);
        return;

    case OP_BT: case OP_BTS: case OP_BTR: case OP_BTC: {
        int bt = in->op == OP_BT ? X64_BT_BT : in->op == OP_BTS ? X64_BT_BTS : in->op == OP_BTR ? X64_BT_BTR : X64_BT_BTC;
        if (d->kind == OPK_MEM) {                                /* an immediate bit: within the operand */
            emit_checks(e, &ea, size, in->op != OP_BT, live_in);
            m = ea_mem(&ea);
            emit_bt_mi(e, size, bt, &m, (uint8_t)(s->imm & (size * 8 - 1)));
            fl_produce(X86_CF);
            slow_back(e, s_rf);
            return;
        }
        if (s->kind == OPK_IMM) emit_bt_ri(e, size, bt, host_reg(d), (uint8_t)(s->imm & (size * 8 - 1)));
        else emit_bt_rr(e, size, bt, host_reg(d), host_reg(s));
        fl_produce(X86_CF);
        return;
    }

    case OP_SHLD: case OP_SHRD: {
        /* 32-bit, an immediate count 1..31. CONTRACT (interp): CF = the last
         * bit out of the destination, OF = its sign changed, AF clear, SZP
         * from the result — the host's, but for OF, fixed in the slot when
         * live. */
        uint8_t cnt = (uint8_t)(in->imm2 & 31);
        int fix = (live_out & X86_OF) != 0;
        if (d->kind == OPK_MEM) {
            emit_checks(e, &ea, 4, 1, live_in);
            m = ea_mem(&ea);
            if (fix) emit_mov_rm(e, 4, W_T3, &m);
            if (in->op == OP_SHLD) emit_shld_mri(e, 4, &m, host_reg(s), cnt); else emit_shrd_mri(e, 4, &m, host_reg(s), cnt);
            fl_produce(ARITH);
            if (fix) { fl_save(e, ARITH); emit_alu_rm(e, 4, X64_ALU_XOR, W_T3, &m); }
            slow_back(e, s_rf);
        } else {
            if (fix) emit_mov_rr(e, 4, W_T3, host_reg(d));
            if (in->op == OP_SHLD) emit_shld_rri(e, 4, host_reg(d), host_reg(s), cnt); else emit_shrd_rri(e, 4, host_reg(d), host_reg(s), cnt);
            fl_produce(ARITH);
            if (fix) { fl_save(e, ARITH); emit_alu_rr(e, 4, X64_ALU_XOR, W_T3, host_reg(d)); }
        }
        if (fix) {                                               /* W_T3 = old ^ new: bit 31 is the sign change */
            x64_mem_t slot = M(R_CPU, OFF_JIT_FLAGS);
            emit_shift_ri(e, 4, X64_SH_SHR, W_T3, 31 - 11);
            emit_alu_ri(e, 4, X64_ALU_AND, W_T3, X86_OF);
            emit_alu_mi(e, 4, X64_ALU_AND, &slot, ~(uint32_t)X86_OF);
            emit_alu_mr(e, 4, X64_ALU_OR, &slot, W_T3);
        }
        return;
    }

    case OP_MUL: case OP_IMUL: case OP_IMUL3:
        if (in->op == OP_IMUL3 || (in->op == OP_IMUL && in->opcode2 == 0xAF)) {
            /* the product's low half; CONTRACT (interp, 286+): SZP from the
             * HIGH half, which the host does not give — native only while
             * those bits are dead, else the interpreter */
            if (live_out & (X86_SF | X86_ZF | X86_PF)) break;
            const x86_operand *src = in->op == OP_IMUL3 ? &in->ops[1] : &in->ops[1];
            if (src->kind == OPK_MEM) {
                emit_checks(e, &ea, size, 0, live_in);
                m = ea_mem(&ea);
                if (in->op == OP_IMUL3) emit_imul_rmi(e, size, host_reg(d), &m, in->imm2); else emit_imul_rm(e, size, host_reg(d), &m);
                fl_produce(X86_CF | X86_OF | X86_AF);
                slow_back(e, s_rf);
            } else {
                if (in->op == OP_IMUL3) emit_imul_rri(e, size, host_reg(d), host_reg(src), in->imm2); else emit_imul_rr(e, size, host_reg(d), host_reg(src));
                fl_produce(X86_CF | X86_OF | X86_AF);
            }
            return;
        }
        emit_mul16(e, in, &ea, live_in, live_out);
        return;

    case OP_DIV: case OP_IDIV:
        emit_div16(e, in, &ea, live_in);
        return;

    case OP_PUSHF: emit_pushf16(e, live_in); slow_back(e, 0); return;
    case OP_POPF:  emit_popf16(e, live_in); slow_back(e, 0); return;

    case OP_LDS: case OP_LES: {
        /* the far pointer as one dword (at FFFD..FFFF it straddles: the
         * interpreter's), then the register, then the segment */
        fl_save(e, live_in);
        if (s_paged) emit_checks_rw(e, &ea, 4, 0, 1, live_in);
        else {
            emit_alu_ri(e, 2, X64_ALU_CMP, ea.off, 0xFFFC);
            slow_site(e, X64_CC_A);
            if (s_blk->devread) emit_check_window(e, &ea);
        }
        m = ea_mem(&ea); emit_mov_rm(e, 4, W_T2, &m);
        emit_mov_rr(e, 2, host_reg(d), W_T2);
        emit_shift_ri(e, 4, X64_SH_SHR, W_T2, 16);
        emit_load_seg_real(e, in->op == OP_LDS ? S_DS : S_ES, W_T2);
        slow_back(e, 0);
        return;
    }

    case OP_XLAT: {
        /* AL = [seg: (BX + AL) & FFFF], or [EBX + AL] flat */
        emit_movzx_rr(e, 4, W_T2, 1, X64_AL);
        m = x64_mi(X64_RBX, W_T2, 0, 0); emit_lea(e, 4, W_T2, &m);
        if (!s_flat) emit_movzx_rr(e, 4, W_T2, 2, W_T2);
        int segp = s_flat ? R_MEM : emit_seg_ptr(e, in->seg, W_T1);
        int chk = s_flat || s_seg16 || s_paged || s_blk->devread;
        ea_t xe = { segp, W_T2, in->seg };
        if (chk) emit_checks(e, &xe, 1, 0, live_in);              /* (paged: xe is the translated address after) */
        m = ea_mem(&xe); emit_mov_rm(e, 1, X64_AL, &m);
        if (chk) slow_back(e, s_rf);
        return;
    }

    case OP_LAHF:
        if ((s_rf & (X86_SF | X86_ZF | X86_AF | X86_PF | X86_CF)) == (X86_SF | X86_ZF | X86_AF | X86_PF | X86_CF)) {
            emit_lahf(e);                                       /* bit 1 set, 3 and 5 clear: the host's too */
        } else {
            fl_save(e, live_in);
            m = M(R_CPU, OFF_JIT_FLAGS); emit_movzx_rm(e, 4, W_T2, 1, &m);
            emit_alu_ri(e, 4, X64_ALU_AND, W_T2, 0xD5);
            emit_alu_ri(e, 4, X64_ALU_OR, W_T2, 0x02);
            emit_high8_open(e, X64_AH);
            emit_mov_rr(e, 1, X64_AL, W_T2);
            emit_high8_close(e, X64_AH);
        }
        return;
    case OP_SAHF:
        emit_sahf(e);
        fl_produce(X86_SF | X86_ZF | X86_AF | X86_PF | X86_CF);
        return;

    case OP_MOVZX: case OP_MOVSX: {
        int ssize = s->size, zx = in->op == OP_MOVZX;
        if (s->kind == OPK_MEM) {
            emit_checks(e, &ea, ssize, 0, live_in);
            m = ea_mem(&ea);
            if (zx) emit_movzx_rm(e, size, host_reg(d), ssize, &m); else emit_movsx_rm(e, size, host_reg(d), ssize, &m);
            slow_back(e, s_rf);
        } else if (is_high8(s)) {
            /* movzx ax, bh: no REX needed, the encoder does it natively */
            if (zx) emit_movzx_rr(e, size, host_reg(d), 1, s->reg); else emit_movsx_rr(e, size, host_reg(d), 1, s->reg);
        } else {
            if (zx) emit_movzx_rr(e, size, host_reg(d), ssize, host_reg(s)); else emit_movsx_rr(e, size, host_reg(d), ssize, host_reg(s));
        }
        return;
    }

    default:
        break;
    }
    /* not inline after all: the interpreter */
    emit_helper_op(dbt, e, in, live_in);
}

/* ----------------------------------------------------------------------
 * Multi-slot stack frames (far CALL, INT): every slot checked before the
 * first store, so a slow path replays the whole instruction from an
 * untouched state. W_T3 = SS pointer, W_T2 = a slot's offset.
 * ---------------------------------------------------------------------- */
static void emit_frame_slot(emit_t *e, int32_t below) {           /* W_T2 = (SP - below) & FFFF */
    x64_mem_t m = M(X64_R12, -below);
    emit_lea(e, 4, W_T2, &m);
    emit_movzx_rr(e, 4, W_T2, 2, W_T2);
}
static void emit_frame_checks(emit_t *e, int nslots) {
    emit_seg_ptr(e, S_SS, W_T3);
    for (int k = 1; k <= nslots; k++) {
        emit_frame_slot(e, 2 * k);
        ea_t ea = { W_T3, W_T2, S_SS };
        emit_check_wrap(e, &ea, 2);
        emit_check_smc(e, &ea, 2);
    }
}
/* store a 16-bit register or immediate into slot k (1 = the first pushed) */
static void emit_frame_store_r(emit_t *e, int k, int reg) {
    emit_frame_slot(e, 2 * k);
    x64_mem_t m = x64_mi(W_T3, W_T2, 0, 0);
    emit_mov_mr(e, 2, &m, reg);
}
static void emit_frame_store_i(emit_t *e, int k, uint16_t imm) {
    emit_frame_slot(e, 2 * k);
    x64_mem_t m = x64_mi(W_T3, W_T2, 0, 0);
    emit_mov_mi(e, 2, &m, imm);
}
static void emit_frame_done(emit_t *e, int nslots) {              /* SP -= 2 * nslots */
    emit_frame_slot(e, 2 * nslots);
    emit_mov_rr(e, 2, X64_R12, W_T2);
}

/* CS := selector in `sel` (16 bits, any register but W_T0); real mode:
 * base = sel << 4. Limit, usable and D bit are already real mode's for
 * CS (never unreal). Leaves the base in W_T0. */
static void emit_load_cs(emit_t *e, int sel) {
    x64_mem_t m;
    emit_movzx_rr(e, 4, W_T0, 2, sel);
    m = M(R_CPU, OFF_SEG_SEL(S_CS)); emit_mov_mr(e, 4, &m, W_T0);            /* sel, attr = 0 */
    emit_shift_ri(e, 4, X64_SH_SHL, W_T0, 4);
    m = M(R_CPU, OFF_SEG_BASE(S_CS)); emit_mov_mr(e, 4, &m, W_T0);
}
/* R_KEY = key of base (W_T0, from emit_load_cs) + ip (`ip`, 16 bits) with
 * selector `sel`; A20 masked when off. sel and ip: any registers but W_T0/W_T1. */
static void emit_far_key(emit_t *e, int sel, int ip) {
    emit_movzx_rr(e, 4, W_T1, 2, ip);
    x64_mem_t m = x64_mi(W_T0, W_T1, 0, 0);
    emit_lea(e, 4, R_KEY, &m);
    if (!s_blk->v86 && s_cpu->a20_mask == 0xFFFFF) emit_alu_ri(e, 4, X64_ALU_AND, R_KEY, 0xFFFFF);
    emit_movzx_rr(e, 8, W_T1, 2, sel);
    emit_shift_ri(e, 8, X64_SH_SHL, W_T1, 32);
    emit_alu_rr(e, 8, X64_ALU_OR, R_KEY, W_T1);
    if (s_mode_bits) { emit_mov_ri(e, 8, W_T1, s_mode_bits); emit_alu_rr(e, 8, X64_ALU_OR, R_KEY, W_T1); }
}

/* Far transfers (real mode). The frame is pushed with every slot checked
 * first; a ptr16:16 in memory is read as one dword (a far pointer at
 * offset FFFD..FFFF straddles: the interpreter's). Flags are saved and
 * the block charged by the caller. */
static void emit_far_ender(x86_dbt *dbt, emit_t *e, const x86_insn *in, uint32_t ip_after) {
    const x86_cpu *cpu = s_cpu;
    x64_mem_t m;
    if (in->op == OP_RETF) {
        /* both slots fetched before SP moves (a 286 limit fault on either leaves SP intact) */
        emit_seg_ptr(e, S_SS, W_T3);
        int sp = X64_R12;
        if (s_regs32) { emit_movzx_rr(e, 4, W_T2, 2, X64_R12); sp = W_T2; }
        ea_t lo = { W_T3, sp, S_SS };
        emit_check_wrap(e, &lo, 2);
        m = M(sp, 2); emit_lea(e, 4, W_T1, &m); emit_movzx_rr(e, 4, W_T1, 2, W_T1);
        ea_t hi = { W_T3, W_T1, S_SS };
        emit_check_wrap(e, &hi, 2);
        m = ea_mem(&hi); emit_movzx_rm(e, 4, W_T0, 2, &m);                     /* selector */
        m = ea_mem(&lo); emit_movzx_rm(e, 4, W_T1, 2, &m);                     /* ip */
        int32_t adj = 4 + (in->ops[0].kind == OPK_IMM ? (int32_t)(in->ops[0].imm & 0xFFFF) : 0);
        m = M(X64_R12, adj); emit_lea(e, 4, W_T2, &m); emit_mov_rr(e, 2, X64_R12, W_T2);
        emit_mov_rr(e, 4, W_T3, W_T0);                                          /* sel out of W_T0's way */
        emit_load_cs(e, W_T3);
        emit_far_key(e, W_T3, W_T1);
        emit_dynamic_tail(dbt, e);
        return;
    }
    int is_imm = in->ops[0].kind == OPK_IMM;
    if (!is_imm) {
        /* ptr16:16 in memory: one dword read, W_T1 = sel:off; W_T0/W_T1 are the EA's */
        ea_t ea; emit_ea(e, in, &ea);
        emit_alu_ri(e, 2, X64_ALU_CMP, ea.off, 0xFFFC);
        slow_site(e, X64_CC_A);
        m = ea_mem(&ea); emit_mov_rm(e, 4, W_T1, &m);
        emit_mov_mr(e, 4, &(x64_mem_t){ R_CPU, X64_NOREG, 0, OFF_JIT_CUR_HIT }, W_T1);   /* parked: the frame needs every scratch */
    }
    if (in->op == OP_CALLF) {
        emit_frame_checks(e, 2);
        m = M(R_CPU, OFF_SEG_SEL(S_CS)); emit_movzx_rm(e, 4, W_T1, 2, &m);
        emit_frame_store_r(e, 1, W_T1);
        emit_frame_store_i(e, 2, (uint16_t)ip_after);
        emit_frame_done(e, 2);
    }
    if (is_imm) {
        uint16_t sel = (uint16_t)in->imm2, off = (uint16_t)in->ops[0].imm;
        emit_mov_ri(e, 4, W_T3, sel);
        emit_load_cs(e, W_T3);
        uint32_t lin = ((uint32_t)sel << 4) + off;
        if (!s_blk->v86) lin &= cpu->a20_mask;
        emit_edge(dbt, e, dbt_key(sel, lin) | s_mode_bits);
    } else {
        m = M(R_CPU, OFF_JIT_CUR_HIT); emit_mov_rm(e, 4, W_T1, &m);
        emit_mov_rr(e, 4, W_T3, W_T1);
        emit_shift_ri(e, 4, X64_SH_SHR, W_T3, 16);                              /* sel */
        emit_load_cs(e, W_T3);
        emit_far_key(e, W_T3, W_T1);
        emit_dynamic_tail(dbt, e);
    }
}

/* reg = FLAGS as the guest sees them: cpu->eflags with the arithmetic
 * bits from the slot (saved already) and DF from the host's RFLAGS.
 * Clobbers W_T0 and RFLAGS. */
static void emit_flags_image(emit_t *e, int reg) {
    x64_mem_t m;
    m = M(R_CPU, OFF_EFLAGS); emit_mov_rm(e, 4, reg, &m);
    emit_alu_ri(e, 4, X64_ALU_AND, reg, ~(uint32_t)(ARITH | X86_DF));
    m = M(R_CPU, OFF_JIT_FLAGS); emit_mov_rm(e, 4, W_T0, &m);
    emit_alu_ri(e, 4, X64_ALU_AND, W_T0, ARITH);
    emit_alu_rr(e, 4, X64_ALU_OR, reg, W_T0);
    emit_pushfq(e); emit_pop_r(e, W_T0);
    emit_alu_ri(e, 4, X64_ALU_AND, W_T0, X86_DF);
    emit_alu_rr(e, 4, X64_ALU_OR, reg, W_T0);
}

/* INT n / INT3 (real mode). The IVT entry is read before the frame is
 * pushed (measured), the pushed FLAGS are the pre-clear value with the
 * arithmetic bits from the slot, IF and TF are cleared, and the frame's
 * three slots are checked before any store. Landing on the HLE stub
 * segment is a refused key, so the run loop steps the service. */
static void emit_int_ender(x86_dbt *dbt, emit_t *e, const x86_insn *in) {
    uint32_t vec = in->op == OP_INT3 ? 3 : (in->ops[0].imm & 0xFF);
    x64_mem_t m;
    m = M(R_MEM, (int32_t)(vec * 4)); emit_mov_rm(e, 4, W_T0, &m);              /* sel:off */
    m = M(R_CPU, OFF_JIT_CUR_HIT); emit_mov_mr(e, 4, &m, W_T0);                 /* parked across the frame */
    emit_frame_checks(e, 3);
    emit_flags_image(e, W_T1);
    emit_frame_store_r(e, 1, W_T1);                                              /* FLAGS as the guest sees it */
    m = M(R_CPU, OFF_SEG_SEL(S_CS)); emit_movzx_rm(e, 4, W_T1, 2, &m);
    emit_frame_store_r(e, 2, W_T1);
    emit_frame_store_i(e, 3, (uint16_t)s_cur_ip_after);
    emit_frame_done(e, 3);
    m = M(R_CPU, OFF_EFLAGS); emit_alu_mi(e, 4, X64_ALU_AND, &m, ~(uint32_t)(X86_IF | X86_TF));
    m = M(R_CPU, (int32_t)offsetof(x86_cpu, int_inhibit)); emit_mov_mi(e, 1, &m, 0);
    m = M(R_CPU, OFF_JIT_CUR_HIT); emit_mov_rm(e, 4, W_T1, &m);
    emit_mov_rr(e, 4, W_T3, W_T1);
    emit_shift_ri(e, 4, X64_SH_SHR, W_T3, 16);                                  /* sel */
    emit_load_cs(e, W_T3);
    emit_far_key(e, W_T3, W_T1);
    emit_dynamic_tail(dbt, e);
}

/* ----------------------------------------------------------------------
 * String instructions (16-bit addressing, byte and word), on the host's
 * own: SI/DI become host pointers for the duration and are turned back
 * into offsets after. DF is the host's already. The checks come first —
 * a word at FFFFh, a store's bitmap byte, and for REP the whole range:
 * no wrap around the segment, no code bitmap bit under any store — and
 * a miss runs the instruction through the interpreter from the state
 * as it stands. Below the 386 the pointer registers are canonical; on
 * a 386 REP forms stay with the interpreter for now.
 * ---------------------------------------------------------------------- */

/* W_T1 = pointer of the source segment, W_T2 = of ES (registers the
 * string op needs kept: this is called first). Flat: both are R_MEM. */
static void emit_str_segs(emit_t *e, const x86_insn *in, int need_src) {
    if (s_flat) { if (need_src) emit_mov_rr(e, 8, W_T1, R_MEM); emit_mov_rr(e, 8, W_T2, R_MEM); return; }
    if (need_src) { if (in->seg == S_DS) emit_mov_rr(e, 8, W_T1, R_DSP); else emit_seg_ptr(e, in->seg, W_T1); }
    emit_seg_ptr(e, S_ES, W_T2);
}
/* On a 386 the upper halves of ESI/EDI must survive a 16-bit string op:
 * kept in the cpu's own register slots (stale during a block anyway),
 * and the registers themselves are only truncated once every check has
 * passed (a slow path must see them whole). */
static void emit_str_save_hi(emit_t *e) {
    if (!s_regs32) return;
    x64_mem_t m;
    m = M(R_CPU, OFF_R(R_SI)); emit_mov_mr(e, 4, &m, X64_RSI);
    m = M(R_CPU, OFF_R(R_DI)); emit_mov_mr(e, 4, &m, X64_RDI);
}
static void emit_str_trunc(emit_t *e, int si, int di) {
    if (!s_regs32) return;
    if (si) emit_movzx_rr(e, 4, X64_RSI, 2, X64_RSI);
    if (di) emit_movzx_rr(e, 4, X64_RDI, 2, X64_RDI);
}
/* reg (SI/DI) back to a 16-bit offset: host pointer minus base (in
 * `negbase`, negated), then the 386's upper half restored. Flag-free. */
static void emit_str_unptr(emit_t *e, int reg, int negbase) {
    x64_mem_t m = x64_mi(reg, negbase, 0, 0);
    emit_lea(e, 8, reg, &m);
    if (s_flat) { emit_mov_rr(e, 4, reg, reg); return; }         /* 32 bits, zero above */
    emit_movzx_rr(e, 4, reg, 2, reg);
    if (s_regs32) {
        m = M(R_CPU, OFF_R(reg == X64_RSI ? R_SI : R_DI));
        emit_mov_rm(e, 4, W_T0, &m);
        emit_mov_rr(e, 2, W_T0, reg);
        emit_mov_rr(e, 4, reg, W_T0);
    }
}

static void emit_string1(emit_t *e, const x86_insn *in, uint32_t live_in) {
    int size = in->ops[0].size;
    int rd_si = in->op == OP_MOVS || in->op == OP_LODS || in->op == OP_CMPS;
    int wr_di = in->op == OP_MOVS || in->op == OP_STOS;
    int use_di = in->op != OP_LODS;
    x64_mem_t m;
    fl_save(e, live_in);
    emit_str_segs(e, in, rd_si);
    if (s_flat) {
        if (rd_si) { ea_t ea = { R_MEM, X64_RSI, in->seg }; emit_check_flat(e, &ea, size, 0, 1); }
        if (use_di) { ea_t ea = { R_MEM, X64_RDI, S_ES }; emit_check_flat(e, &ea, size, wr_di, !wr_di || in->op == OP_MOVS ? 0 : 1); }
    } else {
        emit_str_save_hi(e);
        int si16 = X64_RSI, di16 = X64_RDI;
        if (s_regs32) { emit_movzx_rr(e, 4, W_T0, 2, X64_RSI); emit_movzx_rr(e, 4, W_T3, 2, X64_RDI); si16 = W_T0; di16 = W_T3; }
        /* both pointers and both offsets stay live across either check (seg16's borrows a scratch) */
        uint32_t busy0 = s_check_busy;
        s_check_busy |= (1u << W_T1) | (1u << W_T2) | (1u << si16) | (1u << di16);
        if (rd_si) { ea_t ea = { W_T1, si16, in->seg }; emit_check_wrap(e, &ea, size); }
        if (use_di) { ea_t ea = { W_T2, di16, S_ES }; emit_check_wrap(e, &ea, size); if (wr_di) emit_check_smc(e, &ea, size); }
        s_check_busy = busy0;
        emit_str_trunc(e, rd_si, use_di);
    }
    /* negated bases for the flag-free way back (CMPS/SCAS leave flags) */
    if (rd_si) { emit_mov_rr(e, 8, W_T0, W_T1); emit_g3_r(e, 8, X64_G3_NEG, W_T0); emit_mov_rr(e, 8, W_T1, W_T0); }
    if (use_di) { emit_mov_rr(e, 8, W_T0, W_T2); emit_g3_r(e, 8, X64_G3_NEG, W_T0); emit_mov_rr(e, 8, W_T2, W_T0); }
    /* W_T1 = -src base, W_T2 = -ES base: the pointers are reg - (-base) */
    if (rd_si) { emit_mov_rr(e, 8, W_T0, X64_RSI); emit_alu_rr(e, 8, X64_ALU_SUB, W_T0, W_T1); emit_mov_rr(e, 8, X64_RSI, W_T0); }
    if (use_di) { emit_mov_rr(e, 8, W_T0, X64_RDI); emit_alu_rr(e, 8, X64_ALU_SUB, W_T0, W_T2); emit_mov_rr(e, 8, X64_RDI, W_T0); }
    switch (in->op) {
    case OP_MOVS: emit_movs(e, size, 0); break;
    case OP_STOS: emit_stos(e, size, 0); break;
    case OP_LODS: emit_lods(e, size, 0); break;
    case OP_CMPS: emit_cmps(e, size, 0); break;
    default:      emit_scas(e, size, 0); break;
    }
    if (rd_si) emit_str_unptr(e, X64_RSI, W_T1);
    if (use_di) emit_str_unptr(e, X64_RDI, W_T2);
    (void)m;
    if (in->op == OP_CMPS || in->op == OP_SCAS) fl_produce(ARITH);
}

/* W_T3 = the 16-bit offset in reg (SI/DI) + disp: on a 386 the pinned
 * register has an upper half that is not part of the offset. */
static void emit_str_off(emit_t *e, int reg, int32_t disp, int extra) {
    x64_mem_t m;
    if (s_regs32 && !s_flat) {
        emit_movzx_rr(e, 4, W_T3, 2, reg);
        m = extra >= 0 ? x64_mi(W_T3, extra, 0, disp) : M(W_T3, disp);
    } else {
        m = extra >= 0 ? x64_mi(reg, extra, 0, disp) : M(reg, disp);
    }
    emit_lea(e, 8, W_T3, &m);
}

/* REP MOVS/STOS and REPE/REPNE CMPS/SCAS. The range is CX elements in
 * DF's direction; W_T0 = its byte count. On a 386 in a 16-bit block the
 * checks look at zero-extended copies, and only once they have passed are
 * the upper halves of ECX/ESI/EDI parked in the cpu's register slots and
 * the registers truncated for the host's REP; they come back after. */
static void emit_rep_string(emit_t *e, const x86_insn *in, uint32_t live_in) {
    int size = in->ops[0].size;
    int rd_si = in->op == OP_MOVS || in->op == OP_CMPS;
    int wr_di = in->op == OP_MOVS || in->op == OP_STOS;
    int cmp = in->op == OP_CMPS || in->op == OP_SCAS;
    x64_mem_t m;
    fl_save(e, live_in);
    emit_test_rr(e, s_flat ? 4 : 2, X64_RCX, X64_RCX);
    uint32_t none = emit_jcc_rel32(e, X64_CC_E);
    emit_str_segs(e, in, rd_si);
    /* byte count (64 bits: a flat ECX times four can pass 32) */
    if (size == 1) emit_mov_rr(e, 4, W_T0, X64_RCX);
    else { m = x64_mi(X64_NOREG, X64_RCX, size == 2 ? 1 : 2, 0); emit_lea(e, 8, W_T0, &m); }
    uint32_t top_off = s_flat ? FLAT_TOP : 0x10000;   /* offsets below this are the fast path's */
    /* seg16: both segments usable (else the interpreter's #GP), and below
     * every byte at or below the limit: the last one in DF's direction on
     * the way up, the first one on the way down (so no wrap either) */
    if (s_seg16) {
        if (rd_si) { m = M(R_CPU, OFF_SEG_USABLE(in->seg)); emit_alu_mi(e, 1, X64_ALU_CMP, &m, 0); slow_site(e, X64_CC_E); }
        m = M(R_CPU, OFF_SEG_USABLE(S_ES)); emit_alu_mi(e, 1, X64_ALU_CMP, &m, 0); slow_site(e, X64_CC_E);
    }
    /* direction */
    emit_pushfq(e); emit_pop_r(e, W_T3);
    emit_test_ri(e, 4, W_T3, X86_DF);
    uint32_t down = emit_jcc_rel32(e, X64_CC_NE);
    /* up: offset + count <= the top for each pointer; the bitmap under [ES:DI, +count) */
    if (s_seg16) {
        x64_mem_t lim;
        if (rd_si) { emit_str_off(e, X64_RSI, -1, W_T0); lim = M(R_CPU, OFF_SEG_LIMIT(in->seg)); emit_alu_rm(e, 4, X64_ALU_CMP, W_T3, &lim); slow_site(e, X64_CC_A); }
        emit_str_off(e, X64_RDI, -1, W_T0); lim = M(R_CPU, OFF_SEG_LIMIT(S_ES)); emit_alu_rm(e, 4, X64_ALU_CMP, W_T3, &lim); slow_site(e, X64_CC_A);
    } else {
        if (rd_si) { emit_str_off(e, X64_RSI, 0, W_T0); emit_alu_ri(e, 8, X64_ALU_CMP, W_T3, top_off); slow_site(e, X64_CC_A); }
        emit_str_off(e, X64_RDI, 0, W_T0); emit_alu_ri(e, 8, X64_ALU_CMP, W_T3, top_off); slow_site(e, X64_CC_A);
    }
    if (wr_di) { emit_str_off(e, X64_RDI, 0, -1); m = x64_mi(W_T2, W_T3, 0, (int32_t)X86_BM_DELTA); emit_lea(e, 8, W_T3, &m); }
    uint32_t to_scan = emit_jmp_rel32(e);
    /* down: offset + size >= count for each pointer; the bitmap under [ES:DI + size - count, +count) */
    emit_patch_rel32(e, down, emit_pos(e));
    if (s_seg16) {                                                             /* the highest byte, the first one here */
        x64_mem_t lim;
        if (rd_si) { emit_str_off(e, X64_RSI, size - 1, -1); lim = M(R_CPU, OFF_SEG_LIMIT(in->seg)); emit_alu_rm(e, 4, X64_ALU_CMP, W_T3, &lim); slow_site(e, X64_CC_A); }
        emit_str_off(e, X64_RDI, size - 1, -1); lim = M(R_CPU, OFF_SEG_LIMIT(S_ES)); emit_alu_rm(e, 4, X64_ALU_CMP, W_T3, &lim); slow_site(e, X64_CC_A);
    }
    if (rd_si) { emit_str_off(e, X64_RSI, size, -1); emit_alu_rr(e, 8, X64_ALU_CMP, W_T3, W_T0); slow_site(e, X64_CC_B); }
    emit_str_off(e, X64_RDI, size, -1); emit_alu_rr(e, 8, X64_ALU_CMP, W_T3, W_T0); slow_site(e, X64_CC_B);
    if (wr_di) {
        emit_alu_rr(e, 8, X64_ALU_SUB, W_T3, W_T0);                              /* DI + size - count */
        m = x64_mi(W_T2, W_T3, 0, (int32_t)X86_BM_DELTA); emit_lea(e, 8, W_T3, &m);
    }
    emit_patch_rel32(e, to_scan, emit_pos(e));
    if (wr_di) {
        /* the bitmap scan: W_T3 = first byte, W_T0 = count (recomputed after) */
        uint32_t top = emit_pos(e);
        emit_alu_ri(e, 8, X64_ALU_CMP, W_T0, 8);
        uint32_t tail = emit_jcc_rel8(e, X64_CC_B);
        m = M(W_T3, 0); emit_alu_mi(e, 8, X64_ALU_CMP, &m, 0);
        slow_site(e, X64_CC_NE);
        emit_alu_ri(e, 8, X64_ALU_ADD, W_T3, 8);
        emit_alu_ri(e, 8, X64_ALU_SUB, W_T0, 8);
        emit_patch_rel32(e, emit_jmp_rel32(e), top);
        emit_patch_rel8(e, tail, emit_pos(e));
        uint32_t tail_top = emit_pos(e);
        emit_test_rr(e, 8, W_T0, W_T0);
        uint32_t scanned = emit_jcc_rel8(e, X64_CC_E);
        m = M(W_T3, 0); emit_alu_mi(e, 1, X64_ALU_CMP, &m, 0);
        slow_site(e, X64_CC_NE);
        emit_alu_ri(e, 8, X64_ALU_ADD, W_T3, 1);
        emit_alu_ri(e, 8, X64_ALU_SUB, W_T0, 1);
        emit_patch_rel32(e, emit_jmp_rel32(e), tail_top);
        emit_patch_rel8(e, scanned, emit_pos(e));
    }
    /* every check passed: on a 386, the upper halves out of the way */
    int hi32 = s_regs32 && !s_flat;
    if (hi32) {
        emit_str_save_hi(e);
        m = M(R_CPU, OFF_R(R_CX)); emit_mov_mr(e, 4, &m, X64_RCX);
        emit_str_trunc(e, 1, 1);
        emit_movzx_rr(e, 4, X64_RCX, 2, X64_RCX);
    }
    /* pointers in, the op, pointers out (negated bases: no flags on the way back) */
    if (rd_si) { m = x64_mi(W_T1, X64_RSI, 0, 0); emit_lea(e, 8, X64_RSI, &m); emit_g3_r(e, 8, X64_G3_NEG, W_T1); }
    m = x64_mi(W_T2, X64_RDI, 0, 0); emit_lea(e, 8, X64_RDI, &m); emit_g3_r(e, 8, X64_G3_NEG, W_T2);
    switch (in->op) {
    case OP_MOVS: emit_movs(e, size, 0xF3); break;
    case OP_STOS: emit_stos(e, size, 0xF3); break;
    case OP_CMPS: emit_cmps(e, size, in->rep); break;
    default:      emit_scas(e, size, in->rep); break;
    }
    if (rd_si) emit_str_unptr(e, X64_RSI, W_T1);
    else if (hi32) {                                             /* SI untouched: its upper half back */
        m = M(R_CPU, OFF_R(R_SI)); emit_mov_rm(e, 4, W_T0, &m);
        emit_mov_rr(e, 2, W_T0, X64_RSI); emit_mov_rr(e, 4, X64_RSI, W_T0);
    }
    emit_str_unptr(e, X64_RDI, W_T2);
    if (hi32) {                                                  /* ECX = its upper half : the CX the REP left */
        m = M(R_CPU, OFF_R(R_CX)); emit_mov_rm(e, 4, W_T0, &m);
        emit_mov_rr(e, 2, W_T0, X64_RCX); emit_mov_rr(e, 4, X64_RCX, W_T0);
    }
    if (cmp) {                         /* the last comparison's flags, to the slot: the CX = 0 path left them there too */
        m = M(R_CPU, OFF_JIT_FLAGS);
        emit_pushfq(e); emit_pop_m(e, &m);
    }
    emit_patch_rel32(e, none, emit_pos(e));
}

/* ----------------------------------------------------------------------
 * MUL, IMUL, DIV, IDIV, shifts by CL, PUSHF/POPF, LDS/LES, XLAT, LAHF/SAHF
 * ---------------------------------------------------------------------- */

/* The slot's ZF := (reg == 0). Whatever RFLAGS held is in the slot
 * already; RFLAGS is free after. */
static void emit_slot_zf_from(emit_t *e, int size, int reg) {
    x64_mem_t m = M(R_CPU, OFF_JIT_FLAGS);
    emit_test_rr(e, size, reg, reg);
    emit_setcc_r(e, X64_CC_E, W_T0);
    emit_movzx_rr(e, 4, W_T0, 1, W_T0);
    emit_shift_ri(e, 4, X64_SH_SHL, W_T0, 6);
    emit_alu_mi(e, 4, X64_ALU_AND, &m, ~(uint32_t)X86_ZF);
    emit_alu_mr(e, 4, X64_ALU_OR, &m, W_T0);
}

/* One-operand MUL/IMUL r/m8, r/m16: the host's, whose CF/OF, SF and PF
 * are the interpreter's; its ZF is not (the interpreter's is the low
 * half's), so when ZF is live it is fixed in the slot. */
static void emit_mul16(emit_t *e, const x86_insn *in, ea_t *ea, uint32_t live_in, uint32_t live_out) {
    int size = in->ops[0].size, g3 = in->op == OP_MUL ? X64_G3_MUL : X64_G3_IMUL;
    const x86_operand *s = &in->ops[0];
    x64_mem_t m;
    if (s->kind == OPK_MEM) {
        emit_checks(e, ea, size, 0, live_in);
        m = ea_mem(ea); emit_g3_m(e, size, g3, &m);
    } else {
        int hi = is_high8(s);
        if (hi) {
            /* mul ah: the register is rotated into AL's place... but AL is
             * the other operand: copy to a scratch instead */
            emit_mov_rr(e, 4, W_T2, X64_RAX + (s->reg & 3));
            emit_shift_ri(e, 4, X64_SH_SHR, W_T2, 8);
            emit_g3_r(e, 1, g3, W_T2);
        } else {
            emit_g3_r(e, size, g3, host_reg(s));
        }
    }
    fl_produce(ARITH);
    if (live_out & X86_ZF) {
        fl_save(e, ARITH);
        emit_slot_zf_from(e, size, X64_RAX);
    }
    if (s->kind == OPK_MEM) slow_back(e, s_rf);
}

/* DIV/IDIV r/m8, r/m16 on the 286 and later: the host's, after checks
 * that keep it from faulting — a #DE (zero divisor, quotient too large)
 * is the interpreter's. Unsigned: exact (the high part below the
 * divisor). Signed: the fast path takes a dividend that is the sign
 * extension of its low half (what CBW/CWD leave) and not the one
 * overflowing pair. Flags are unchanged (CONTRACT, 186+); the checks
 * clobber RFLAGS, so they go to the slot first. */
static void emit_div16(emit_t *e, const x86_insn *in, ea_t *ea, uint32_t live_in) {
    int size = in->ops[0].size, sgn = in->op == OP_IDIV;
    const x86_operand *s = &in->ops[0];
    x64_mem_t m;
    int src;
    if (s->kind == OPK_MEM) { emit_checks(e, ea, size, 0, live_in); m = ea_mem(ea); if (size == 4) emit_mov_rm(e, 4, W_T2, &m); else emit_movzx_rm(e, 4, W_T2, size, &m); src = W_T2; }
    else {
        fl_save(e, live_in);
        if (is_high8(s)) { emit_mov_rr(e, 4, W_T2, X64_RAX + (s->reg & 3)); emit_shift_ri(e, 4, X64_SH_SHR, W_T2, 8); src = W_T2; }
        else src = host_reg(s);
    }
    if (!sgn) {
        if (size == 1) { emit_mov_rr(e, 4, W_T3, X64_RAX); emit_shift_ri(e, 4, X64_SH_SHR, W_T3, 8); emit_alu_rr(e, 1, X64_ALU_CMP, W_T3, src); }
        else emit_alu_rr(e, size, X64_ALU_CMP, X64_RDX, src);
        slow_site(e, X64_CC_AE);                                   /* high >= divisor: #DE (0 included) */
    } else {
        emit_test_rr(e, size, src, src);
        slow_site(e, X64_CC_E);                                    /* divisor 0 */
        /* the high half must be the sign extension of the low half */
        if (size == 1) {
            emit_movsx_rr(e, 4, W_T3, 1, X64_AL);
            emit_alu_rr(e, 2, X64_ALU_CMP, W_T3, X64_RAX);
        } else if (size == 2) {
            emit_movsx_rr(e, 4, W_T3, 2, X64_RAX);
            emit_shift_ri(e, 4, X64_SH_SAR, W_T3, 16);
            emit_alu_rr(e, 2, X64_ALU_CMP, W_T3, X64_RDX);
        } else {
            emit_mov_rr(e, 4, W_T3, X64_RAX);
            emit_shift_ri(e, 4, X64_SH_SAR, W_T3, 31);
            emit_alu_rr(e, 4, X64_ALU_CMP, W_T3, X64_RDX);
        }
        slow_site(e, X64_CC_NE);
        /* the one overflowing pair: MIN / -1 */
        emit_alu_ri(e, size, X64_ALU_CMP, src, size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFu);
        uint32_t ok = emit_jcc_rel8(e, X64_CC_NE);
        emit_alu_ri(e, size, X64_ALU_CMP, X64_RAX, size == 1 ? 0x80 : size == 2 ? 0x8000 : 0x80000000u);
        slow_site(e, X64_CC_E);
        emit_patch_rel8(e, ok, emit_pos(e));
    }
    emit_g3_r(e, size, sgn ? X64_G3_IDIV : X64_G3_DIV, src);
    s_rf = 0;                                                      /* undefined on the host; the slot is the truth */
    slow_back(e, 0);                                               /* (the #DE checks are sites whatever the operand) */
}

/* Shifts and rotates by CL (186+: the count masked to 5 bits, as the
 * host does). A zero count changes nothing, flags included, so the flags
 * are in the slot on both paths out. OF after more than one step is
 * the last step's on the guest: for SHL/ROL/RCL that is SF ^ CF of the
 * result, fixed in the slot when OF is live; SAR's is 0 on both. */
static void emit_shift_cl(emit_t *e, const x86_insn *in, ea_t *ea, uint32_t live_in, uint32_t live_out) {
    int size = in->ops[0].size, sh = sh_of_op[in->op];
    const x86_operand *d = &in->ops[0];
    x64_mem_t m, slot = M(R_CPU, OFF_JIT_FLAGS);
    int rcx = in->op == OP_RCL || in->op == OP_RCR;
    if (d->kind == OPK_MEM) emit_checks(e, ea, size, 1, live_in); else fl_save(e, live_in);
    emit_test_ri(e, 1, X64_CL, 0x1F);
    uint32_t zero = emit_jcc_rel32(e, X64_CC_E);
    int fix_of = (live_out & X86_OF) != 0 && in->op != OP_SAR;
    if (fix_of && in->op == OP_SHR) {                            /* the original's sign, for a count of one */
        if (d->kind == OPK_MEM) { m = ea_mem(ea); emit_load_val(e, size, W_T3, &m); emit_shift_ri(e, 4, X64_SH_SHR, W_T3, size * 8 - 1); }
        else { emit_mov_rr(e, 4, W_T3, is_high8(d) ? (d->reg & 3) : host_reg(d)); emit_shift_ri(e, 4, X64_SH_SHR, W_T3, is_high8(d) ? 15 : size * 8 - 1); }
        emit_alu_ri(e, 4, X64_ALU_AND, W_T3, 1);
    }
    if (rcx) fl_need_cf(e);
    if (d->kind == OPK_MEM) { m = ea_mem(ea); emit_shift_mcl(e, size, sh, &m); }
    else emit_shift_rcl(e, size, sh, host_reg(d));               /* a high byte and CL: no REX, native */
    int rot = in->op == OP_ROL || in->op == OP_ROR || rcx;
    if (!rot) { emit_pushfq(e); emit_pop_m(e, &slot); }
    else {                                                       /* a rotate writes CF and OF only: merge */
        emit_pushfq(e); emit_pop_r(e, W_T3);
        emit_alu_ri(e, 4, X64_ALU_AND, W_T3, X86_CF | X86_OF);
        emit_alu_mi(e, 4, X64_ALU_AND, &slot, ~(uint32_t)(X86_CF | X86_OF));
        emit_alu_mr(e, 4, X64_ALU_OR, &slot, W_T3);
    }
    if (fix_of) {
        /* OF, the last step's on the guest (the rotates leave SF alone,
         * so signs come from the value itself):
         *   SHL/SAL/ROL/RCL  MSB(result) ^ CF
         *   ROR/RCR          MSB(result) ^ the bit below it
         *   SHR              MSB(original) for a count of one, else 0 */
        int msb = is_high8(d) ? 15 : size * 8 - 1;
        if (in->op == OP_SHR) {
            emit_mov_rr(e, 4, W_T2, X64_RCX);
            emit_alu_ri(e, 4, X64_ALU_AND, W_T2, 0x1F);
            emit_alu_ri(e, 4, X64_ALU_CMP, W_T2, 1);
            uint32_t one = emit_jcc_rel8(e, X64_CC_E);
            emit_mov_ri(e, 4, W_T3, 0);
            emit_patch_rel8(e, one, emit_pos(e));
        } else {
            if (d->kind == OPK_MEM) { m = ea_mem(ea); emit_load_val(e, size, W_T3, &m); }
            else emit_mov_rr(e, 4, W_T3, is_high8(d) ? (d->reg & 3) : host_reg(d));
            if (in->op == OP_ROR || in->op == OP_RCR) {
                emit_mov_rr(e, 4, W_T2, W_T3);
                emit_shift_ri(e, 4, X64_SH_SHR, W_T3, msb);
                emit_shift_ri(e, 4, X64_SH_SHR, W_T2, msb - 1);
                emit_alu_rr(e, 4, X64_ALU_XOR, W_T3, W_T2);
            } else {
                emit_shift_ri(e, 4, X64_SH_SHR, W_T3, msb);
                emit_mov_rm(e, 4, W_T2, &slot);
                emit_alu_rr(e, 4, X64_ALU_XOR, W_T3, W_T2);           /* CF is bit 0 */
            }
        }
        emit_alu_ri(e, 4, X64_ALU_AND, W_T3, 1);
        emit_shift_ri(e, 4, X64_SH_SHL, W_T3, 11);
        emit_alu_mi(e, 4, X64_ALU_AND, &slot, ~(uint32_t)X86_OF);
        emit_alu_mr(e, 4, X64_ALU_OR, &slot, W_T3);
    }
    emit_patch_rel32(e, zero, emit_pos(e));
    s_rf = 0;
    slow_back(e, 0);
}

/* PUSHF with a 16-bit operand: FLAGS as the guest sees them, low word. */
static void emit_pushf16(emit_t *e, uint32_t live_in) {
    x64_mem_t m;
    fl_save(e, live_in | ARITH);                                   /* the value pushed: everything */
    emit_flags_image(e, W_T2);
    emit_push16(e, W_T2);
    (void)m;
}
/* POPF with a 16-bit operand, real mode: the low word of EFLAGS replaced
 * whole, then x86_flags_fixup's per-model masks (bit 1 set, 3 and 5
 * clear; 12-15 ones on the 8086/186, zeros on the 286, the 386 clears bit
 * 15 and VM). DF goes to the host's DF, the arithmetic bits to the slot. */
static void emit_popf16(emit_t *e, uint32_t live_in) {
    x64_mem_t m;
    fl_save(e, live_in);
    emit_pop16(e, W_T2);
    emit_movzx_rr(e, 4, W_T2, 2, W_T2);
    if (s_model >= X86_MODEL_386) {
        m = M(R_CPU, OFF_EFLAGS); emit_mov_rm(e, 4, W_T0, &m);
        emit_alu_ri(e, 4, X64_ALU_AND, W_T0, 0xFFFF0000u & ~(uint32_t)X86_VM);
        emit_alu_rr(e, 4, X64_ALU_OR, W_T2, W_T0);
        emit_alu_ri(e, 4, X64_ALU_AND, W_T2, ~0x8028u);
        emit_alu_ri(e, 4, X64_ALU_OR, W_T2, 0x2);
    } else if (s_model == X86_MODEL_286) {
        emit_alu_ri(e, 4, X64_ALU_AND, W_T2, 0x0FD7);
        emit_alu_ri(e, 4, X64_ALU_OR, W_T2, 0x2);
    } else {
        emit_alu_ri(e, 4, X64_ALU_AND, W_T2, 0xFFD7);
        emit_alu_ri(e, 4, X64_ALU_OR, W_T2, 0xF002);
    }
    m = M(R_CPU, OFF_EFLAGS); emit_mov_mr(e, 4, &m, W_T2);
    m = M(R_CPU, OFF_JIT_FLAGS); emit_mov_mr(e, 4, &m, W_T2);
    emit_alu_ri(e, 4, X64_ALU_AND, W_T2, X86_DF);
    emit_push_r(e, W_T2); emit_popfq(e);
    s_rf = 0;
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
        if (!s_regs32 || in->adsize == 4) {                      /* canonical CX, or ECX itself: RCX is it */
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
    default: {   /* LOOP family: CX (ECX) = CX - 1, taken when != 0 [&& ZF cond] */
        int aw = in->adsize == 4 ? 4 : 2;
        m = M(X64_RCX, -1);
        emit_lea(e, 4, W_T0, &m);
        emit_mov_rr(e, aw, X64_RCX, W_T0);                       /* 16-bit write: canonical stays canonical */
        if (in->op == OP_LOOP && (!s_regs32 || aw == 4)) {
            uint32_t done = emit_jrcxz_rel8(e);
            uint32_t taken = emit_jmp_rel32(e);
            emit_patch_rel8(e, done, emit_pos(e));
            return taken;
        }
        if (in->op == OP_LOOP) {
            fl_save(e, live);
            emit_test_rr(e, aw, X64_RCX, X64_RCX);
            return emit_jcc_rel32(e, X64_CC_NE);
        }
        /* LOOPE/LOOPNE: the guest's ZF and CX both. The flags go to the
         * slot first: both arms must agree on where they are. */
        int hc;
        fl_save(e, live | X86_ZF);
        emit_cond_setup(e, in->op == OP_LOOPE ? 4 : 5, live, &hc);
        uint32_t no = emit_jcc_rel8(e, hc ^ 1);                  /* ZF condition fails: not taken */
        emit_test_rr(e, aw, X64_RCX, X64_RCX);
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
    case OP_INT: case OP_INT3:
        emit_int_ender(dbt, e, in);
        break;
    case OP_CALLF: case OP_JMPF: case OP_RETF:
        emit_far_ender(dbt, e, in, ip_after);
        break;
    case OP_JMP: {
        int w = s_flat ? 4 : 2;
        if (in->ops[0].kind == OPK_IMM) { emit_edge(dbt, e, target_key(ip_after + in->ops[0].imm)); return; }
        if (in->ops[0].kind == OPK_MEM) {
            emit_ea(e, in, &ea);
            emit_checks(e, &ea, w, 0, 0);
            m = ea_mem(&ea);
            if (w == 4) emit_mov_rm(e, 4, W_T3, &m); else emit_movzx_rm(e, 4, W_T3, 2, &m);
        } else {
            if (w == 4) emit_mov_rr(e, 4, W_T3, host_reg(&in->ops[0])); else emit_movzx_rr(e, 4, W_T3, 2, host_reg(&in->ops[0]));
        }
        emit_dynamic_key(e, W_T3);
        emit_dynamic_tail(dbt, e);
        break;
    }
    case OP_CALL: {
        int dyn = in->ops[0].kind != OPK_IMM, w = s_flat ? 4 : 2;
        if (dyn && in->ops[0].kind == OPK_MEM) {
            emit_ea(e, in, &ea);
            emit_checks(e, &ea, w, 0, 0);
            m = ea_mem(&ea);
            if (w == 4) emit_mov_rm(e, 4, W_T3, &m); else emit_movzx_rm(e, 4, W_T3, 2, &m);
        } else if (dyn && !s_paged) {
            if (w == 4) emit_mov_rr(e, 4, W_T3, host_reg(&in->ops[0])); else emit_movzx_rr(e, 4, W_T3, 2, host_reg(&in->ops[0]));
        }
        emit_mov_ri(e, 4, W_T2, w == 4 ? ip_after : (uint16_t)ip_after);
        if (dyn) s_check_busy |= 1u << W_T3;                     /* the target rides across the push's checks */
        emit_push_stk(e, W_T2);
        s_check_busy &= ~(1u << W_T3);
        if (dyn && s_paged) {                                    /* (after: the translation used W_T3; not SP, see inline_ok_v86) */
            if (w == 4) emit_mov_rr(e, 4, W_T3, host_reg(&in->ops[0])); else emit_movzx_rr(e, 4, W_T3, 2, host_reg(&in->ops[0]));
        }
        if (dyn) { emit_dynamic_key(e, W_T3); emit_dynamic_tail(dbt, e); }
        else emit_edge(dbt, e, target_key(ip_after + in->ops[0].imm));
        break;
    }
    default: {   /* RET */
        (void)0;
        emit_pop_stk(e, W_T3);
        if (in->ops[0].kind == OPK_IMM) {
            m = M(X64_R12, (int32_t)(in->ops[0].imm & 0xFFFF));
            emit_lea(e, 4, W_T2, &m);
            emit_mov_rr(e, sp_width(), X64_R12, W_T2);
        }
        emit_dynamic_key(e, W_T3);
        emit_dynamic_tail(dbt, e);
        break;
    }
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
    s_flat = b->flat;
    s_seg16 = b->seg16;
    s_ss32 = b->ss32;
    s_paged = b->paged;
    s_pg_user = b->pg_user;
    s_skip_smc = 0;
    s_model = b->model;
    s_cpu = cpu;
    s_blk = b;
    s_rf = 0;
    s_nslow = 0; s_npost = 0; s_nleave = 0; s_nfault = 0; s_usable_ok = 0;
    /* what this backend emits inline so far: real-mode-shaped blocks
     * (real mode, segmented 16-bit PM) and flat ones, without paging;
     * everything else is helpers, and transfers end them */
    int inl = !b->all_helper && !getenv("X86_X64_HELPERS");

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
        /* This backend materializes eagerly: a store's side exit wants the
         * bits in fexit too (see dbt.h), so take the union. */
        uint32_t live_out = b->fmask[i] | b->fexit[i], live_in = b->live_in[i];
        int is_inline = inl && b->cls[i] == C_INLINE;
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
        if (is_inline) { s_dyn_imm_lin = b->dyn_lin[i]; emit_op(dbt, &e, in, live_in, live_out); s_dyn_imm_lin = 0; continue; }
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

    /* Slow paths of the checked instructions, and the marked-byte paths
     * of the plain stores. */
    emit_slow_chunks(dbt, &e);
    emit_post_chunks(&e);
    emit_leave_chunks(&e);
    emit_fault_chunks(&e);

    /* Budget-exhausted exit: nothing executed, next = this block. */
    emit_patch_rel32(&e, budget_patch, emit_pos(&e));
    m = M(R_CPU, OFF_EIP);
    emit_mov_mi(&e, 4, &m, b->start_ip);
    emit_jmp_rel32_to(&e, s_exit_eip_off);

    dbt->code_used = e.offset;
    return entry;
}
