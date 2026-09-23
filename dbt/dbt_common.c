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
    dbt->verify_mem_every = 256;
    if (getenv("X86_PMPROF")) {
        dbt->pmprof = 1;
        dbt->pmprof_after = strtoull(getenv("X86_PMPROF"), NULL, 0);
        dbt->pm_hits = calloc(X86_MEM_SIZE >> 4, sizeof(uint32_t));
    }

    dbt->aux       = calloc(1, sizeof(x86_jit_aux));
    dbt->span      = calloc(BLOCK_CACHE_SIZE, sizeof(uint32_t));
    dbt->link_head = calloc(BLOCK_CACHE_SIZE, sizeof(uint32_t));
    dbt->link_pool = calloc(LINK_POOL_SIZE, sizeof(x86_link));
    dbt->insn_pool = calloc(INSN_POOL_SIZE, sizeof(x86_insn));
    dbt->insn_hits = calloc(INSN_POOL_SIZE, sizeof(uint32_t));
    dbt->insn_tag  = calloc(INSN_POOL_SIZE, 1);
    dbt->insn_lin  = calloc(INSN_POOL_SIZE, sizeof(uint32_t));
    dbt->smc_heat  = calloc(X86_MEM_SIZE + X86_MEM_SLACK, 1);     /* touched only where SMC happens */
    if (!dbt->aux || !dbt->span || !dbt->link_head || !dbt->link_pool || !dbt->insn_pool || !dbt->smc_heat) {
        fprintf(stderr, "dbt_init: out of memory\n");
        return -1;
    }
    build_tables(dbt->aux);
    dbt->aux->helpers[H_EXEC]       = (void *)dbt_h_exec;
    dbt->aux->helpers[H_POST_STORE] = (void *)dbt_h_post_store;
    dbt->aux->helpers[H_OUT]        = (void *)dbt_h_out;

    dbt->bm_lo    = cpu->mem_size;         /* nothing marked yet */
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

static uint8_t dev_record(x86_cpu *c, uint32_t p);

void dbt_cleanup(x86_dbt *dbt) {
    dbt_dump_code(dbt);
    if (dbt->cpu) {
        dbt_clear_code_bits(dbt->cpu);
        dbt->cpu->dbt = NULL;
        dbt->cpu->smc_hook = NULL;
    }
    if (dbt->code_buf && dbt->code_buf != MAP_FAILED) munmap(dbt->code_buf, CODE_BUF_SIZE);
    if (dbt->shadow_live) x86_free(&dbt->shadow);
    if (dbt->cpu && dbt->cpu->device_read == dev_record) dbt->cpu->device_read = dbt->dev_read_real;
    free(dbt->devlog);
    free(dbt->smc_heat);
    free(dbt->aux); free(dbt->span); free(dbt->link_head); free(dbt->link_pool); free(dbt->insn_pool);
    free(dbt->insn_hits); free(dbt->insn_tag); free(dbt->insn_lin);
    memset(dbt, 0, sizeof(*dbt));
}

/* ----------------------------------------------------------------------
 * Helpers called from translated code
 * ---------------------------------------------------------------------- */

/* Generic slow path: run the interpreter's execute() on a pooled decoded
 * instruction. The call site has synced every pinned register the
 * interpreter reads (all of them) and reloads them all afterwards; eip
 * already points past the instruction. Nothing that moves CS is routed
 * here. A fault leaves cpu->exc set and eip at the instruction; the exec
 * thunk sees it and leaves the block for the run loop to deliver it. */
void dbt_h_exec(x86_cpu *cpu, uint32_t insn_index) {
    x86_dbt *dbt = (x86_dbt *)cpu->dbt;
    const x86_insn *in = &dbt->insn_pool[insn_index];
    dbt->helper_by_op[in->op]++;
    dbt->insn_hits[insn_index]++;
    x86_exec_decoded(cpu, in);
}

/* A translated store of 1, 2 or 4 bytes (count in bits 31:28, 0 meaning
 * 1) found a nonzero code-bitmap entry somewhere under it: run the store
 * hook — device memory, then SMC — for each byte, as the interpreter's
 * byte-wise stores would. */
void dbt_h_post_store(x86_cpu *cpu, uint32_t arg) {
    uint32_t phys = arg & 0x0FFFFFFFu, n = arg >> 28;
    if (!n) n = 1;
    for (uint32_t i = 0; i < n; i++)
        if (cpu->code_bitmap[phys + i]) x86_store_hook(cpu, phys + i);
}

/* OUT from inside a block, through the port thunk (which saves only the
 * caller-saved pinned registers: no spill, no reload). Returns 0 to go
 * on with the block, 1 to leave after the instruction — the port halted
 * the machine (the oracle's exit port), or flipped A20 and with it the
 * cache under the running block — and 2 for #GP, cpu->exc set, to leave
 * at the instruction. Ports never touch the guest registers or flags. */
uint32_t dbt_h_out(x86_cpu *cpu, uint32_t port_size, uint32_t value) {
    x86_dbt *dbt = (x86_dbt *)cpu->dbt;
    uint32_t port = port_size & 0xFFFF;
    int size = (int)(port_size >> 16);
    dbt->helper_by_op[OP_OUT]++;
    if (!x86_io_permitted(cpu, port, size)) { cpu->exc = X86_EXC_GP; cpu->exc_err = 0; return 2; }
    cpu->jit_cur_hit = 0;
    if (cpu->io_write) cpu->io_write(cpu, (uint16_t)port, value, size);
    return (cpu->halted || cpu->jit_cur_hit) ? 1 : 0;
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

static void dump_mem_diff(const uint8_t *a, const uint8_t *b, uint32_t size, const char *na, const char *nb) {
    int shown = 0;
    for (uint32_t i = 0; i < size && shown < 8; i++) {
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
static uint32_t shadow_io_read(x86_cpu *c, uint16_t port, int size) { (void)c; (void)port; (void)size; return 0xFFFFFFFFu; }
static void shadow_io_write(x86_cpu *c, uint16_t port, uint32_t v, int size) { (void)c; (void)port; (void)v; (void)size; }

/* -V: the real cpu's device reads go through dev_record, which logs what
 * the device answered; the shadow's go through dev_replay, which hands
 * the same answers back in the same order. A VGA read depends on plane
 * and latch state the shadow does not have (DOOM's I_ReadScreen copies
 * the screen out plane by plane), so this is the only way it can agree. */
static uint8_t dev_record(x86_cpu *c, uint32_t p) {
    x86_dbt *dbt = (x86_dbt *)c->dbt;
    uint8_t v = dbt->dev_read_real(c, p);
    if (dbt->devlog_n == dbt->devlog_cap) {
        dbt->devlog_cap = dbt->devlog_cap ? dbt->devlog_cap * 2 : 65536;
        dbt->devlog = realloc(dbt->devlog, dbt->devlog_cap);
    }
    dbt->devlog[dbt->devlog_n++] = v;
    return v;
}
static x86_dbt *s_replay_dbt;      /* the shadow has no dbt pointer of its own */
static uint8_t dev_replay(x86_cpu *c, uint32_t p) {
    x86_dbt *dbt = s_replay_dbt;
    if (dbt->devlog_pos < dbt->devlog_n) return dbt->devlog[dbt->devlog_pos++];
    return c->mem[p];               /* more reads than the real cpu made: a divergence -V will report */
}
/* Before the real cpu runs: route its device reads through the recorder
 * (the device layer installs and removes its hook on mode changes). */
static void dev_log_arm(x86_dbt *dbt) {
    x86_cpu *cpu = dbt->cpu;
    if (cpu->device_read && cpu->device_read != dev_record) {
        dbt->dev_read_real = cpu->device_read;
        cpu->device_read = dev_record;
    }
    dbt->devlog_n = dbt->devlog_pos = 0;
}

static int at_hle(const x86_cpu *c) {
    return c->hle && c->seg[S_CS].base == ((uint32_t)c->hle_seg << 4);
}

/* How much of memory the shadow tracks. Real mode cannot address past
 * the HMA — only host services write there — so until translated
 * protected-mode code has run, low memory is all that needs copying or
 * comparing; the full 17 MB per resync made -V unusable on real-mode
 * programs that trap to the host often. */
static uint32_t shadow_span(const x86_dbt *dbt) {
    return dbt->shadow_ext ? dbt->cpu->mem_size : X86_LOW_SIZE;
}

/* Copy the real cpu's registers into the shadow, leaving its memory. The
 * shadow keeps its own mem/bitmap (never marked, so the hook never fires),
 * never sees the DBT, and never reaches a device, a port or the host. */
static void shadow_copy_regs(x86_dbt *dbt) {
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
    sh->dev_wplane = sh->dev_rplane = NULL;
    sh->device_read = cpu->device_read ? dev_replay : NULL;
    s_replay_dbt = dbt;
    sh->a20_hook = NULL;
    sh->io_read = shadow_io_read;
    sh->io_write = shadow_io_write;
    sh->hle = NULL;
    sh->trace_exc = NULL;
    /* The shadow's HMA window must alias the same way before any copy,
     * or the real HMA bytes land in the shadow's low 64 KB (or vice versa). */
    sh->a20_mask = sh_a20;
    x86_set_a20(sh, cpu->a20_mask != 0xFFFFFu);
}

/* Device memory — the VGA window, while planar VGA claims it — holds
 * whatever the device made of a store, which the shadow cannot know, so
 * it is left out of the comparison and handed over after each check. */
#define DEV_LO 0xA0000u
#define DEV_HI 0xB0000u
static int shadow_mem_equal(const x86_dbt *dbt, uint32_t span) {
    const uint8_t *a = dbt->cpu->mem, *b = dbt->shadow.mem;
    if (!dbt->cpu->device_store) return memcmp(a, b, span) == 0;
    return memcmp(a, b, DEV_LO) == 0 && memcmp(a + DEV_HI, b + DEV_HI, span - DEV_HI) == 0;
}

/* Registers and memory. */
static void shadow_resync(x86_dbt *dbt) {
    shadow_copy_regs(dbt);
    memcpy(dbt->shadow.mem, dbt->cpu->mem, shadow_span(dbt));
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
            if (dbt->poll(cpu) && dbt->verify) dbt->shadow_stale = 1;
        }
        if (cpu->halted) return 0;
        if (dbt->insn_limit && cpu->insn_count >= dbt->insn_limit) return 0;

        uint64_t key = dbt_cpu_key(cpu);
        x86_block_entry *be = dbt_cache_lookup(dbt, key);
        uint8_t *code = be ? be->code : NULL;
        if (!be) {
            dbt_jit_writable_begin();
            code = dbt_translate_block(dbt, key);
            dbt_jit_writable_end();
            dbt_cache_insert(dbt, key, code);
            if (code) dbt->blocks_translated++;
        }

        if (code) {
            if (dbt->verify) {
                if (cpu->pmode && !dbt->shadow_ext) { dbt->shadow_ext = 1; dbt->shadow_stale = 1; }
                if (dbt->shadow_stale) { shadow_resync(dbt); dbt->shadow_stale = 0; }
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
                dev_log_arm(dbt);
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

                /* An OUT inside the run halted the machine: only the real
                 * cpu has ports, so the shadow, which ran the same
                 * instructions, is told. */
                if (cpu->halted) dbt->shadow.halted = 1;
                int regs_ok = cpu_regs_equal(cpu, &dbt->shadow);
                /* An OUT inside the run flipped A20: the shadow's ports are
                 * stubs, so it did not follow. Registers are compared as is;
                 * memory resyncs before the next check. */
                int a20_moved = dbt->shadow.a20_mask != cpu->a20_mask;
                /* Low memory after every run; all of it — 17 MB, too much per
                 * block — every verify_mem_every runs (-M 1 for
                 * every run, to localise a divergence the sampling found). */
                int mem_ok = shadow_mem_equal(dbt, X86_LOW_SIZE);
                if (mem_ok && dbt->shadow_ext && (dbt->verify_mem_every <= 1 || (runs % (uint64_t)dbt->verify_mem_every) == 0))
                    mem_ok = shadow_mem_equal(dbt, cpu->mem_size);
                if (cpu->device_store) memcpy(dbt->shadow.mem + DEV_LO, cpu->mem + DEV_LO, DEV_HI - DEV_LO);
                if (a20_moved) { mem_ok = 1; dbt->shadow_stale = 1; }
                if (!regs_ok || !mem_ok) {
                    fprintf(stderr, "\n[verify] divergence after JIT run from %04X:%04X (%llu insns)\n",
                            pre.seg[S_CS].sel, pre.eip, (unsigned long long)jit_insns);
                    dump_regs(stderr, "pre   ", &pre);
                    dump_regs(stderr, "JIT   ", cpu);
                    dump_regs(stderr, "shadow", &dbt->shadow);
                    if (!regs_ok) verify_first_diff(cpu, &dbt->shadow);
                    if (!mem_ok) dump_mem_diff(cpu->mem, dbt->shadow.mem, shadow_span(dbt), "jit", "shadow");
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
        if (dbt->pmprof && cpu->insn_count >= dbt->pmprof_after && cpu->pmode && !(cpu->hle && cpu->seg[S_CS].base == ((uint32_t)cpu->hle_seg << 4))) {
            uint32_t lin = cpu->seg[S_CS].base + cpu->eip;
            uint8_t fb[16];
            for (int k = 0; k < 16; k++) fb[k] = x86_phys_rd8(cpu, lin + (uint32_t)k);
            x86_dec_ctx dc = { fb, cpu->model, (uint8_t)X86_AR_DB(cpu->seg[S_CS].attr) }; x86_insn di;
            if (x86_decode(&dc, &di)) {
                dbt->pm_ops[di.op][di.opsize == 4][di.adsize == 4]++;
                dbt->pm_insns++;
                if (di.ea_valid) { dbt->pm_mem++; if (di.index >= 0 || (di.adsize == 4 && di.rm == 4 && di.mod != 3)) dbt->pm_sib++; }
                if (di.seg_override != S_NONE) dbt->pm_segovr++;
                if (di.rep) dbt->pm_rep++;
            }
            if (lin < cpu->mem_size) dbt->pm_hits[lin >> 4]++;
        }
        int hle_step = at_hle(cpu);
        if (!hle_step) {
            /* dynamic histogram: what did we hand to the interpreter? */
            uint8_t fb[16];
            uint32_t m = cpu->pmode ? 0xFFFFFFFFu : 0xFFFFu;
            for (int k = 0; k < 16; k++) fb[k] = x86_phys_rd8(cpu, cpu->seg[S_CS].base + ((cpu->eip + (uint32_t)k) & m));
            x86_dec_ctx dc = { fb, cpu->model, cpu->pmode ? cpu->seg[S_CS].big : 0 }; x86_insn di;
            if (x86_decode(&dc, &di)) dbt->fallback_by_op[di.op]++;
        }
        if (dbt->verify) dev_log_arm(dbt);
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
        if (dbt->verify && !dbt->shadow_stale) {
            /* The fallback ran on the reference interpreter — nothing to
             * verify. An ordinary instruction is replayed on the shadow so
             * that it stays in step without a copy of memory; its registers
             * are then taken from the real cpu, which alone saw the ports
             * and devices. A host service must not run twice: the shadow is
             * re-synced instead, once a translated block next needs it. */
            if (hle_step) {
                dbt->shadow_stale = 1;
            } else {
                x86_step(&dbt->shadow);
                shadow_copy_regs(dbt);
                if (dbt->shadow.a20_mask != cpu->a20_mask) dbt->shadow_stale = 1;
            }
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

static int cmp_u64_desc(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* X86_PMPROF: the protected-mode work list. */
static void print_pmprof(x86_dbt *dbt, FILE *out) {
    if (!dbt->pmprof || !dbt->pm_insns) return;
    double n = (double)dbt->pm_insns;
    fprintf(out, "  protected mode: %llu insns; memory operand %.1f%%, SIB %.1f%%, seg override %.1f%%, REP %.1f%%\n",
            (unsigned long long)dbt->pm_insns, 100.0 * (double)dbt->pm_mem / n, 100.0 * (double)dbt->pm_sib / n,
            100.0 * (double)dbt->pm_segovr / n, 100.0 * (double)dbt->pm_rep / n);
    uint64_t w[2][2] = {{0}};
    for (int i = 0; i < OP__COUNT; i++)
        for (int o = 0; o < 2; o++) for (int a = 0; a < 2; a++) w[o][a] += dbt->pm_ops[i][o][a];
    fprintf(out, "    operand/address size: 16/16 %.1f%%  16/32 %.1f%%  32/16 %.1f%%  32/32 %.1f%%\n",
            100.0 * (double)w[0][0] / n, 100.0 * (double)w[0][1] / n, 100.0 * (double)w[1][0] / n, 100.0 * (double)w[1][1] / n);
    fprintf(out, "    ops (cumulative %%):");
    double cum = 0;
    for (int k = 0; k < 40; k++) {
        int best = -1; uint64_t bv = 0;
        for (int i = 0; i < OP__COUNT; i++) {
            uint64_t v = dbt->pm_ops[i][0][0] + dbt->pm_ops[i][0][1] + dbt->pm_ops[i][1][0] + dbt->pm_ops[i][1][1];
            if (v > bv) { bv = v; best = i; }
        }
        if (best < 0) break;
        cum += (double)bv / n;
        x86_insn tmp = { .op = (uint8_t)best };
        char buf[64];
        fprintf(out, "%s %s %.1f", k % 8 ? "" : "\n     ", x86_disasm(&tmp, buf, sizeof buf), 100.0 * cum);
        memset(dbt->pm_ops[best], 0, sizeof dbt->pm_ops[best]);
    }
    fprintf(out, "\n");
    /* How concentrated: the share of execution in the hottest 16-byte lines. */
    size_t lines = X86_MEM_SIZE >> 4, used = 0;
    uint64_t *v = malloc(lines * sizeof *v);
    for (size_t i = 0; i < lines; i++) if (dbt->pm_hits[i]) v[used++] = dbt->pm_hits[i];
    qsort(v, used, sizeof *v, cmp_u64_desc);
    fprintf(out, "    code lines touched: %zu (%zu KB);", used, used * 16 / 1024);
    uint64_t acc = 0; size_t marks[] = { 10, 50, 100, 500, 1000, 5000 };
    for (size_t i = 0, m = 0; i < used && m < 6; i++) {
        acc += v[i];
        if (i + 1 == marks[m]) { fprintf(out, " top %zu: %.1f%%", marks[m], 100.0 * (double)acc / n); m++; }
    }
    fprintf(out, "\n");
    /* ...and where they are, with their bytes. */
    for (int k = 0; k < 12; k++) {
        size_t bi = 0; uint32_t bv = 0;
        for (size_t i = 0; i < lines; i++) if (dbt->pm_hits[i] > bv) { bv = dbt->pm_hits[i]; bi = i; }
        if (!bv) break;
        fprintf(out, "      %08zX %5.1f%% ", bi << 4, 100.0 * (double)bv / n);
        for (int b = 0; b < 16; b++) fprintf(out, " %02X", x86_phys_rd8(dbt->cpu, (uint32_t)(bi << 4) + (uint32_t)b));
        fprintf(out, "\n");
        dbt->pm_hits[bi] = 0;
    }
    free(v);
}

void dbt_print_stats(x86_dbt *dbt, FILE *out) {
    print_pmprof(dbt, out);
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
    if (dbt->desc_flushes) fprintf(out, "  CS descriptor flushes:  %llu\n", (unsigned long long)dbt->desc_flushes);
    fprintf(out, "  links created/patched/unpatched: %llu / %llu / %llu\n",
            (unsigned long long)dbt->links_created, (unsigned long long)dbt->links_patched,
            (unsigned long long)dbt->links_unpatched);
    fprintf(out, "  max block bytes:        %u\n", (unsigned)dbt->max_block_bytes);
    fprintf(out, "  code used:              %u bytes, %u pooled insns\n", dbt->code_used, dbt->insn_used);
    fprintf(out, "  helper calls by op (dynamic):");
    for (int n = 0; n < 14; n++) {
        int best = -1;
        for (int i = 0; i < OP__COUNT; i++)
            if (dbt->helper_by_op[i] && (best < 0 || dbt->helper_by_op[i] > dbt->helper_by_op[best])) best = i;
        if (best < 0) break;
        x86_insn tmp = { .op = (uint8_t)best };
        char buf[64];
        fprintf(out, " %s:%llu", x86_disasm(&tmp, buf, sizeof buf), (unsigned long long)dbt->helper_by_op[best]);
        dbt->helper_by_op[best] = 0;
    }
    fprintf(out, "\n");
    if (getenv("X86_HELPER_DETAIL")) {
        /* The hottest pooled instructions themselves, with where they were
         * emitted from: [pm] all-helper block, [flat] helper in a flat
         * block, [slow] a flat inline op's out-of-range path. */
        static const char *tags[3] = { "pm", "flat", "slow" };
        uint32_t n = strtoul(getenv("X86_HELPER_DETAIL"), NULL, 0);
        if (!n) n = 40;
        fprintf(out, "  hottest helper insns:\n");
        for (uint32_t k = 0; k < n; k++) {
            uint32_t best = 0; int found = 0;
            for (uint32_t i = 0; i < dbt->insn_used; i++)
                if (dbt->insn_hits[i] && (!found || dbt->insn_hits[i] > dbt->insn_hits[best])) { best = i; found = 1; }
            if (!found) break;
            char buf[64];
            fprintf(out, "    %10u [%-4s] %06X %s\n", dbt->insn_hits[best], tags[dbt->insn_tag[best]],
                    dbt->insn_lin[best], x86_disasm(&dbt->insn_pool[best], buf, sizeof buf));
            dbt->insn_hits[best] = 0;
        }
    }
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
