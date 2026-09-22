/* dbt_common.c — architecture-neutral DBT machinery: init/run/cleanup,
 * the run loop, -V lockstep verification, helpers, stats.
 *
 * Lifted from ~/z80/dbt/dbt_common.c.
 *
 * Run-loop policy, per guest CS:IP:
 *   1. Cache lookup. Hit → trampoline into the native block (which
 *      chains onward until a probe misses or the budget runs out).
 *   2. Miss → translate. Success → insert and enter.
 *   3. Refusal (the first instruction is untranslatable) → step the
 *      interpreter once, retry at the next IP. A refused sentinel is
 *      cached so the next visit skips the translate attempt.
 */
#include "dbt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* NZCV nibble (N=8 Z=4 C=2 V=1) → x86 SF/ZF/CF/OF. The SUB variants
 * invert C (ARM carry = no borrow); INC/DEC drop CF so the emitter can
 * OR the preserved bit back in. */
static void build_tables(x86_jit_aux *aux) {
    for (int n = 0; n < 16; n++) {
        uint16_t f = 0;
        if (n & 8) f |= X86_SF;
        if (n & 4) f |= X86_ZF;
        if (n & 1) f |= X86_OF;
        uint16_t add = f | ((n & 2) ? X86_CF : 0);
        uint16_t sub = f | ((n & 2) ? 0 : X86_CF);
        aux->nzcv[0][n] = add;
        aux->nzcv[1][n] = sub;
        aux->nzcv[2][n] = add & ~X86_CF;
        aux->nzcv[3][n] = sub & ~X86_CF;
    }
    for (int b = 0; b < 256; b++)
        aux->parity[b] = (__builtin_popcount(b) & 1) ? 0 : X86_PF;
}

int dbt_jit_available(const x86_cpu *cpu) {
#if defined(__aarch64__)
    return cpu->mem_mirrored;
#else
    (void)cpu;
    return 0;
#endif
}

int dbt_init(x86_dbt *dbt, x86_cpu *cpu) {
    memset(dbt, 0, sizeof(*dbt));
    dbt->cpu = cpu;
    dbt->quantum = 1u << 20;
    dbt->verify_mem_every = 1;

    dbt->aux       = calloc(1, sizeof(x86_jit_aux));
    dbt->span      = calloc(BLOCK_CACHE_SIZE, sizeof(uint32_t));
    dbt->link_head = calloc(BLOCK_CACHE_SIZE, sizeof(uint32_t));
    dbt->link_pool = calloc(LINK_POOL_SIZE, sizeof(x86_link));
    dbt->insn_pool = calloc(INSN_POOL_SIZE, sizeof(x86_insn));
    if (!dbt->aux || !dbt->span || !dbt->link_head || !dbt->link_pool || !dbt->insn_pool) {
        fprintf(stderr, "dbt_init: out of memory\n");
        return -1;
    }
    build_tables(dbt->aux);
    dbt->aux->helpers[H_EXEC]       = (void *)dbt_h_exec;
    dbt->aux->helpers[H_POST_STORE] = (void *)x86_store_hook;   /* device memory, then code */

    cpu->dbt      = dbt;
    cpu->jit_aux  = dbt->aux;
    cpu->smc_hook = dbt_smc_store;
    cpu->a20_hook = dbt_a20_changed;
    dbt_cache_invalidate_all(dbt);

    dbt->code_buf = mmap(NULL, CODE_BUF_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                         DBT_JIT_MMAP_FLAGS, -1, 0);
    if (dbt->code_buf == MAP_FAILED) {
        fprintf(stderr, "dbt_init: cannot mmap %u-byte JIT buffer\n", CODE_BUF_SIZE);
        return -1;
    }
    dbt_jit_writable_begin();
    dbt_emit_trampoline(dbt);
    dbt_jit_writable_end();
    return 0;
}

/* X86_JIT_DUMP=<path>: at cleanup, write the raw code buffer and a
 * "key code_offset span" index so blocks can be disassembled
 * (objdump -D -b binary -m aarch64) and located from profiles. */
static void dbt_dump_code(x86_dbt *dbt) {
    const char *path = getenv("X86_JIT_DUMP");
    if (!path || !dbt->code_buf) return;
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(dbt->code_buf, 1, dbt->code_used, f); fclose(f); }
    char idx[4096];
    snprintf(idx, sizeof idx, "%s.idx", path);
    f = fopen(idx, "w");
    if (!f) return;
    fprintf(f, "# code_buf=%p exit_stub=%u\n", (void *)dbt->code_buf, dbt->exit_stub_off);
    for (uint32_t i = 0; i < BLOCK_CACHE_SIZE; i++) {
        const x86_block_entry *be = &dbt->aux->cache[i];
        if (be->key == BLOCK_EMPTY_KEY || !be->code) continue;
        fprintf(f, "%04X:%05X %u %u\n", (unsigned)(be->key >> 32), (unsigned)be->key,
                (unsigned)(be->code - dbt->code_buf), dbt->span[i]);
    }
    fclose(f);
}

void dbt_cleanup(x86_dbt *dbt) {
    dbt_dump_code(dbt);
    if (dbt->cpu) {
        dbt->cpu->dbt = NULL;
        dbt->cpu->smc_hook = NULL;
        dbt_clear_code_bits(dbt->cpu);
    }
    if (dbt->code_buf && dbt->code_buf != MAP_FAILED) munmap(dbt->code_buf, CODE_BUF_SIZE);
    if (dbt->shadow_live) x86_free(&dbt->shadow);
    free(dbt->aux); free(dbt->span); free(dbt->link_head); free(dbt->link_pool); free(dbt->insn_pool);
    memset(dbt, 0, sizeof(*dbt));
}

/* ----------------------------------------------------------------------
 * Helpers called from translated code
 * ---------------------------------------------------------------------- */

/* Generic slow path: run the interpreter's execute() on a pooled decoded
 * instruction. The call site has synced every pinned register the
 * interpreter reads (all of them) and reloads them all afterwards; eip
 * already points past the instruction. Nothing that raises or moves CS
 * is ever routed here (can_translate), so exc stays -1. */
void dbt_h_exec(x86_cpu *cpu, uint32_t insn_index) {
    x86_dbt *dbt = (x86_dbt *)cpu->dbt;
    x86_exec_decoded(cpu, &dbt->insn_pool[insn_index]);
}

/* ----------------------------------------------------------------------
 * -V shadow-verify support
 * ---------------------------------------------------------------------- */
static int cpu_regs_equal(const x86_cpu *a, const x86_cpu *b) {
    for (int i = 0; i < 8; i++) if (a->r[i] != b->r[i]) return 0;
    if (a->eip != b->eip || a->eflags != b->eflags) return 0;
    for (int i = 0; i < 6; i++)
        if (a->seg[i].sel != b->seg[i].sel || a->seg[i].base != b->seg[i].base) return 0;
    if (a->halted != b->halted) return 0;
    return 1;
}

static void dump_regs(FILE *out, const char *tag, const x86_cpu *c) {
    fprintf(out, "  %s: ", tag);
    x86_dump((x86_cpu *)c, out);
}

static void verify_first_diff(const x86_cpu *jit, const x86_cpu *interp) {
    static const char *rn[8] = { "AX","CX","DX","BX","SP","BP","SI","DI" };
    static const char *sn[6] = { "ES","CS","SS","DS","FS","GS" };
    for (int i = 0; i < 8; i++)
        if (jit->r[i] != interp->r[i])
            fprintf(stderr, "    %s differs: jit=%08X interp=%08X\n", rn[i], jit->r[i], interp->r[i]);
    if (jit->eip != interp->eip)
        fprintf(stderr, "    IP differs: jit=%08X interp=%08X\n", jit->eip, interp->eip);
    if (jit->eflags != interp->eflags)
        fprintf(stderr, "    FLAGS differ: jit=%08X interp=%08X (xor %08X)\n",
                jit->eflags, interp->eflags, jit->eflags ^ interp->eflags);
    for (int i = 0; i < 6; i++)
        if (jit->seg[i].sel != interp->seg[i].sel)
            fprintf(stderr, "    %s differs: jit=%04X interp=%04X\n", sn[i], jit->seg[i].sel, interp->seg[i].sel);
    if (jit->halted != interp->halted)
        fprintf(stderr, "    halted differs: jit=%d interp=%d\n", jit->halted, interp->halted);
}

static void dump_mem_diff(const uint8_t *a, const uint8_t *b, const char *na, const char *nb) {
    int shown = 0;
    for (uint32_t i = 0; i < X86_LOW_SIZE && shown < 8; i++) {
        if (a[i] != b[i]) {
            fprintf(stderr, "    mem[%06X] differs: %s=%02X %s=%02X\n", i, na, a[i], nb, b[i]);
            shown++;
        }
    }
}

static void dump_block_bytes(const x86_cpu *c, uint64_t key, const char *tag) {
    uint32_t lin = dbt_key_lin(key);
    fprintf(stderr, "    block bytes (%s):", tag);
    for (int i = 0; i < 32; i++) {
        if ((i & 0xF) == 0) fprintf(stderr, "\n      %05X:", lin + i);
        fprintf(stderr, " %02X", c->mem[lin + i]);
    }
    fprintf(stderr, "\n");
}

static void shadow_smc_none(x86_cpu *c, uint32_t p) { (void)c; (void)p; }

/* Copy the real cpu into the shadow: registers, memory, A20. The shadow
 * keeps its own mem/bitmap (never marked, so the hook never fires) and
 * never sees the DBT. */
static void shadow_resync(x86_dbt *dbt) {
    x86_cpu *cpu = dbt->cpu, *sh = &dbt->shadow;
    uint8_t *mem = sh->mem, *bm = sh->code_bitmap;
    int fd = sh->mem_fd; uint8_t mirrored = sh->mem_mirrored;
    uint32_t sh_a20 = sh->a20_mask;
    *sh = *cpu;
    sh->mem = mem; sh->code_bitmap = bm; sh->mem_fd = fd; sh->mem_mirrored = mirrored;
    sh->dbt = NULL; sh->jit_aux = NULL;
    sh->smc_hook = shadow_smc_none;
    /* Devices belong to the real machine: the shadow sees their memory as
     * plain bytes and never reaches back into their state. */
    sh->device_store = NULL;
    sh->device_read = NULL;
    sh->a20_hook = NULL;
    /* The shadow's HMA window must alias the same way before the copy,
     * or the real HMA bytes land in the shadow's low 64 KB (or vice versa). */
    sh->a20_mask = sh_a20;
    x86_set_a20(sh, cpu->a20_mask != 0xFFFFFu);
    memcpy(sh->mem, cpu->mem, X86_LOW_SIZE);   /* low memory only: the JIT refuses PM, so nothing above it moves */
}

typedef void (*trampoline_fn)(x86_cpu *cpu, uint8_t *mem, void *block, void *aux, uint64_t budget);

int dbt_run(x86_dbt *dbt) {
    x86_cpu *cpu = dbt->cpu;
    trampoline_fn trampoline = (trampoline_fn)(void *)dbt->code_buf;

    if (dbt->verify && !dbt->shadow_live) {
        x86_init(&dbt->shadow, cpu->model);
        dbt->shadow_live = 1;
        shadow_resync(dbt);
    }

    uint64_t runs = 0;
    uint32_t poll_countdown = 0;
    for (;;) {
        /* Host events (timer tick, keys, screen) between block runs —
         * every time the JIT hands control back after a quantum or a
         * fallback, rate-limited on the fallback path. Anything the poll
         * did to the cpu (an IRQ delivered, HLT ended) is a host-side
         * change the shadow must copy. */
        if (dbt->poll && (cpu->halted || poll_countdown-- == 0)) {
            poll_countdown = 256;
            if (dbt->poll(cpu) && dbt->verify) shadow_resync(dbt);
        }
        if (cpu->halted) return 0;
        if (dbt->insn_limit && cpu->insn_count >= dbt->insn_limit) return 0;

        /* The block cache is indexed 1:1 on a linear address in low memory,
         * which is all real mode can reach. Protected mode is both outside
         * the backend's repertoire and outside that index — a DPMI client
         * runs from extended memory — so it never reaches a lookup, and
         * every instruction goes to the interpreter below. */
        x86_block_entry *be = NULL;
        uint8_t *code = NULL;
        uint64_t key = 0;
        if (!cpu->pmode) {
            key = dbt_cpu_key(cpu);
            be = dbt_cache_lookup(dbt, key);
            code = be ? be->code : NULL;
            if (!be) {
                dbt_jit_writable_begin();
                code = dbt_translate_block(dbt, key);
                dbt_jit_writable_end();
                dbt_cache_insert(dbt, key, code);
                if (code) dbt->blocks_translated++;
            }
        }

        if (code) {
            if (dbt->verify) {
                uint64_t insns_before = cpu->insn_count;
                x86_cpu pre = *cpu;

                int pre_regs_ok = cpu_regs_equal(cpu, &dbt->shadow);
                if (!pre_regs_ok) {
                    fprintf(stderr, "\n[verify] lockstep broken BEFORE JIT block at %04X:%04X\n",
                            cpu->seg[S_CS].sel, cpu->eip);
                    dump_regs(stderr, "real  ", cpu);
                    dump_regs(stderr, "shadow", &dbt->shadow);
                    verify_first_diff(cpu, &dbt->shadow);
                    return -1;
                }

                dbt->jit_block_entries++;
                trampoline(cpu, cpu->mem, code, dbt->aux, dbt->quantum);
                poll_countdown = 0;
                if (cpu->exc >= 0) x86_deliver_exception(cpu);
                uint64_t jit_insns = cpu->insn_count - insns_before;

                static int vtrace = -1;
                if (vtrace < 0) vtrace = getenv("X86_VTRACE") != NULL;
                for (uint64_t i = 0; i < jit_insns; i++) {
                    if (vtrace) { fprintf(stderr, "[shadow exc=%d armed=%d] ", dbt->shadow.exc, dbt->shadow.fault_armed); x86_dump(&dbt->shadow, stderr); }
                    if (x86_step(&dbt->shadow) != 0) break;
                }
                dbt->verify_blocks_checked++;
                runs++;

                int regs_ok = cpu_regs_equal(cpu, &dbt->shadow);
                int mem_ok = 1;
                if (dbt->verify_mem_every <= 1 || (runs % (uint64_t)dbt->verify_mem_every) == 0)
                    mem_ok = memcmp(cpu->mem, dbt->shadow.mem, X86_LOW_SIZE) == 0;
                if (!regs_ok || !mem_ok) {
                    fprintf(stderr, "\n[verify] divergence after JIT run from %04X:%04X (%llu insns)\n",
                            pre.seg[S_CS].sel, pre.eip, (unsigned long long)jit_insns);
                    dump_regs(stderr, "pre   ", &pre);
                    dump_regs(stderr, "JIT   ", cpu);
                    dump_regs(stderr, "shadow", &dbt->shadow);
                    if (!regs_ok) verify_first_diff(cpu, &dbt->shadow);
                    if (!mem_ok) dump_mem_diff(cpu->mem, dbt->shadow.mem, "jit", "shadow");
                    dump_block_bytes(cpu, key, "entry block, real mem");
                    return -1;
                }
                continue;
            }
            dbt->jit_block_entries++;
            trampoline(cpu, cpu->mem, code, dbt->aux, dbt->quantum);
            poll_countdown = 0;
            if (cpu->exc >= 0) x86_deliver_exception(cpu);
            continue;
        }

        /* Refused: one interpreter step. Timed (sampled 1 in 16) since on
         * a running system these are the host-service traps. */
        dbt->interp_fallback_insns++;
        if (!cpu->pmode && cpu->seg[S_CS].sel != cpu->hle_seg) {
            /* dynamic histogram: what did we hand to the interpreter? */
            uint8_t fb[16];
            for (int k = 0; k < 16; k++) fb[k] = x86_phys_rd8(cpu, cpu->seg[S_CS].base + ((cpu->eip + k) & 0xFFFF));
            x86_dec_ctx dc = { fb, cpu->model, 0 }; x86_insn di;
            if (x86_decode(&dc, &di)) dbt->fallback_by_op[di.op]++;
        }
        int timed = (dbt->interp_fallback_insns & 15) == 0;
        struct timespec t0, t1;
        if (timed) clock_gettime(CLOCK_MONOTONIC, &t0);
        int rc = x86_step(cpu);
        if (timed) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            dbt->interp_fallback_ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull
                                     + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
        }
        if (rc < 0) {
            fprintf(stderr, "dbt_run: interpreter stopped at %04X:%04X\n", cpu->seg[S_CS].sel, cpu->eip);
            return -1;
        }
        if (dbt->verify) {
            /* The fallback ran on the reference interpreter — nothing to
             * verify, and it may have been a host service whose side
             * effects must not happen twice. Re-sync the shadow. */
            shadow_resync(dbt);
        }
        /* Poll after the step as well, not just after a translated block.
         * A fallback is nearly always an HLE service, and the instant it
         * returns is when IF comes back: with INT inlined, every block in
         * a poll loop ends with the INT that cleared IF, so a block-only
         * poll never sees interrupts enabled and pending IRQs would never
         * be delivered. Affordable since pc_poll became a clock read —
         * but not per instruction: in protected mode, which the backend does
         * not translate, every instruction is a fallback, and a clock read
         * each was a fifth of DOOM's time. There the countdown runs on. */
        if (!cpu->pmode) poll_countdown = 0;
    }
}

void dbt_print_stats(x86_dbt *dbt, FILE *out) {
    fprintf(out, "  blocks translated:      %llu\n", (unsigned long long)dbt->blocks_translated);
    fprintf(out, "  cache hits/misses:      %llu / %llu\n",
            (unsigned long long)dbt->cache_hits, (unsigned long long)dbt->cache_misses);
    fprintf(out, "  JIT block entries:      %llu\n", (unsigned long long)dbt->jit_block_entries);
    fprintf(out, "  interp fallback insns:  %llu  (~%.1f ms host, ~%.0f ns each; sampled)\n",
            (unsigned long long)dbt->interp_fallback_insns,
            (double)dbt->interp_fallback_ns * 16.0 / 1e6,
            dbt->interp_fallback_insns ? (double)dbt->interp_fallback_ns * 16.0 / (double)dbt->interp_fallback_insns : 0.0);
    fprintf(out, "  helper-class ops:       %llu (at translation)\n", (unsigned long long)dbt->helper_insns);
    fprintf(out, "  SMC invalidations:      %llu\n", (unsigned long long)dbt->smc_invalidations);
    if (dbt->a20_flushes) fprintf(out, "  A20 cache flushes:      %llu\n", (unsigned long long)dbt->a20_flushes);
    fprintf(out, "  links created/patched/unpatched: %llu / %llu / %llu\n",
            (unsigned long long)dbt->links_created, (unsigned long long)dbt->links_patched,
            (unsigned long long)dbt->links_unpatched);
    fprintf(out, "  max block bytes:        %u\n", (unsigned)dbt->max_block_bytes);
    fprintf(out, "  code used:              %u bytes, %u pooled insns\n", dbt->code_used, dbt->insn_used);
    fprintf(out, "  interp fallbacks by op (dynamic):");
    for (int n = 0; n < 12; n++) {
        int best = -1;
        for (int i = 0; i < OP__COUNT; i++)
            if (dbt->fallback_by_op[i] && (best < 0 || dbt->fallback_by_op[i] > dbt->fallback_by_op[best])) best = i;
        if (best < 0) break;
        x86_insn tmp = { .op = (uint8_t)best };
        char buf[64];
        fprintf(out, " %s:%llu", x86_disasm(&tmp, buf, sizeof buf), (unsigned long long)dbt->fallback_by_op[best]);
        dbt->fallback_by_op[best] = 0;
    }
    fprintf(out, "\n");
    /* Which ops the backend refused or ended blocks on — the to-do list. */
    int any = 0;
    for (int i = 0; i < OP__COUNT; i++) if (dbt->refused_by_op[i]) { any = 1; break; }
    if (any) {
        fprintf(out, "  block enders/refusals by op:");
        for (int n = 0; n < 12; n++) {
            int best = -1;
            for (int i = 0; i < OP__COUNT; i++)
                if (dbt->refused_by_op[i] && (best < 0 || dbt->refused_by_op[i] > dbt->refused_by_op[best])) best = i;
            if (best < 0) break;
            x86_insn tmp = { .op = (uint8_t)best };
            char buf[64];
            fprintf(out, " %s:%llu", x86_disasm(&tmp, buf, sizeof buf), (unsigned long long)dbt->refused_by_op[best]);
            dbt->refused_by_op[best] = 0;
        }
        fprintf(out, "\n");
    }
}
