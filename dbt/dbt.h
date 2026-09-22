/* dbt.h — x86 Dynamic Binary Translator public interface.
 *
 * Lifted from ~/z80/dbt/dbt.h. The translator runs alongside the
 * interpreter (core/x86_interp.c, the oracle): blocks the backend can
 * translate run as native code; anything that changes CS, raises, or
 * talks to the host (INT, far transfers, IRET, DIV, I/O, HLT) ends the
 * block and the run loop steps the interpreter for that one instruction.
 * Instructions that are exact but rare go through a generic helper
 * that calls the interpreter's execute() on the pooled decoded form —
 * still inside the block, no exit.
 *
 * Block ABI (AArch64, dbt_a64.c). Guest state is PINNED in host
 * registers across blocks and chains:
 *   X19 = x86_cpu *cpu
 *   X20 = cpu->mem
 *   X21..X28 = AX CX DX BX SP BP SI DI — encoding order, so guest reg
 *              i lives in X21+i. Canonical zero-extended 16-bit while
 *              the model is < 386 (the interpreter never writes the
 *              upper halves there, so -V holds).
 *   X11 = JIT aux base (&dbt->aux: flag tables +0, helper pointers
 *         +AUX_HELPERS, block cache +AUX_CACHE)
 *   X12 = guest arithmetic flags: eflags & X86_ARITH_FLAGS, nothing else.
 *         cpu->eflags keeps IF/DF/TF/reserved bits; entry masks, exit
 *         merges.
 *   X13 = remaining instruction budget (signed). Every block entry does
 *         TBNZ X13,#63 → exit with its own key; every tail subtracts its
 *         instruction count. This is the interrupt-delivery hook: the
 *         run loop hands out a quantum, gets control back when it runs
 *         out, and delivers pending IRQs between blocks.
 *   X14 X15 X16 = host pointers mem + DS.base / SS.base / ES.base
 *   X17 = cpu->code_bitmap - cpu->mem (post-store SMC check)
 *   X0..X10 scratch; X0 = next block key at tails.
 * X11..X17 are caller-saved: helper-call sequences spill what the
 * helper reads and reload everything after.
 *
 * Block key = (cs_sel << 32) | linear, linear = (CS.base + IP) & a20.
 * The cache is direct-mapped 1:1 on linear over low memory
 * (X86_LOW_SIZE entries; the JIT refuses protected mode, so nothing is
 * translated from extended memory yet), so
 * the span-gated SMC sweep from z80 carries over unchanged; the 64-bit
 * tag keeps two CS values that alias one linear address apart (the
 * translation bakes in IP-relative constants).
 */
#ifndef DBT_H
#define DBT_H

#include "../core/x86.h"
#include "../core/x86_decode.h"
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

/* Apple Silicon W^X bracketing: every byte written into dbt->code_buf
 * must be inside a paired dbt_jit_writable_begin()/end(). */
#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#define DBT_JIT_MMAP_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT)
static inline void dbt_jit_writable_begin(void) { pthread_jit_write_protect_np(0); }
static inline void dbt_jit_writable_end(void)   { pthread_jit_write_protect_np(1); }
#else
#define DBT_JIT_MMAP_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS)
static inline void dbt_jit_writable_begin(void) { }
static inline void dbt_jit_writable_end(void)   { }
#endif

/* Block cache entry: 16 bytes so the JIT probe fetches it with one LDP.
 * Empty slots hold BLOCK_EMPTY_KEY; refused ("known untranslatable")
 * sentinels hold key | BLOCK_REFUSED_BIT with code == NULL — the tag
 * bit keeps the JIT-side probe from matching and branching to NULL. */
typedef struct {
    uint64_t key;
    uint8_t *code;
} x86_block_entry;
_Static_assert(sizeof(x86_block_entry) == 16, "x86_block_entry must be 16 bytes");

#define BLOCK_CACHE_SIZE   X86_LOW_SIZE
#define BLOCK_EMPTY_KEY    0xFFFFFFFFFFFFFFFFull
#define BLOCK_REFUSED_BIT  0x8000000000000000ull

static inline uint64_t dbt_key(uint32_t cs_sel, uint32_t lin) { return ((uint64_t)cs_sel << 32) | lin; }
static inline uint32_t dbt_key_lin(uint64_t key) { return (uint32_t)key; }

#ifndef MAX_BLOCK_INSNS
#define MAX_BLOCK_INSNS    64
#endif

/* Direct block linking (see dbt_cache.c): every static edge is a
 * patchable B recorded here by target linear address. */
#define LINK_POOL_SIZE   (512 * 1024)
#define LINK_NONE        0xFFFFFFFFu
typedef struct {
    uint32_t site_off;
    uint32_t next;
} x86_link;

#define CODE_BUF_SIZE    (64 * 1024 * 1024)
#define INSN_POOL_SIZE   (1024 * 1024)       /* decoded insns kept for helper calls */

/* ---- JIT aux block ----
 * Translated code reaches these through ONE pinned base (X11) with
 * fixed offsets; the _Static_asserts below pin the layout.
 *   +AUX_PARITY   256-entry uint8: PF for a result byte (at 0 so the
 *                 lookup is one LDRB [X11, Wbyte, UXTW])
 *   +AUX_NZCV     four 16-entry uint16 tables: NZCV nibble → x86 flag bits
 *   +AUX_HELPERS  helper function pointers (LDR X9,[X11,#off]; BLR X9)
 *   +AUX_CACHE    block cache (ADD X, X11, #(AUX_CACHE>>12), LSL #12) */
#define AUX_PARITY    0x0000
#define AUX_NZCV      0x0100
#define AUX_NZCV_ADD  (AUX_NZCV + 0 * 32)
#define AUX_NZCV_SUB  (AUX_NZCV + 1 * 32)
#define AUX_NZCV_INC  (AUX_NZCV + 2 * 32)   /* ADD table with CF cleared */
#define AUX_NZCV_DEC  (AUX_NZCV + 3 * 32)   /* SUB table with CF cleared */
#define AUX_HELPERS   0x0800
#define AUX_CACHE     0x1000
_Static_assert((AUX_CACHE & 0xFFF) == 0, "AUX_CACHE must be reachable by ADD #imm12, LSL #12");

enum {
    H_EXEC = 0,        /* void (cpu, insn_index): interpreter's execute() */
    H_POST_STORE,      /* void (cpu, phys): SMC invalidation after a JIT store */
    H__COUNT
};

typedef struct {
    _Alignas(64) uint8_t parity[256];
    uint16_t nzcv[4][16];
    uint8_t  _pad0[AUX_HELPERS - AUX_NZCV - 4 * 16 * 2];
    void    *helpers[(AUX_CACHE - AUX_HELPERS) / 8];
    x86_block_entry cache[BLOCK_CACHE_SIZE];
} x86_jit_aux;
_Static_assert(offsetof(x86_jit_aux, nzcv)    == AUX_NZCV,    "aux nzcv offset");
_Static_assert(offsetof(x86_jit_aux, helpers) == AUX_HELPERS, "aux helpers offset");
_Static_assert(offsetof(x86_jit_aux, cache)   == AUX_CACHE,   "aux cache offset");

typedef struct {
    x86_cpu *cpu;
    x86_jit_aux *aux;              /* calloc'd: ~17 MB, lazily committed */

    /* Byte span of each cached block, parallel to aux->cache. Read only
     * by the C-side SMC sweeps; 0xFFFFFFFF for refused sentinels. */
    uint32_t *span;

    /* Direct-link registry, keyed by target linear address. */
    uint32_t *link_head;           /* BLOCK_CACHE_SIZE entries */
    x86_link *link_pool;
    uint32_t  link_used;
    uint32_t  link_free;

    uint8_t *code_buf;
    uint32_t code_used;
    uint32_t exit_stub_off;        /* B here with X0 = next key */

    /* Decoded instructions referenced by H_EXEC helper calls. Bump
     * allocated; reset together with the code buffer. */
    x86_insn *insn_pool;
    uint32_t  insn_used;

    /* Instruction budget per trampoline entry; the run loop delivers
     * host events (timer, keyboard) when it comes back. */
    uint64_t quantum;
    uint64_t insn_limit;           /* stop (rc 0) once insn_count passes this; 0 = never */
    int    (*poll)(x86_cpu *);     /* host events between block runs; returns 1 if it changed cpu state */

    /* Stats */
    uint64_t blocks_translated;
    uint64_t cache_hits, cache_misses;
    uint64_t interp_fallback_insns;
    uint64_t interp_fallback_ns;
    uint64_t helper_insns;         /* class-B ops emitted (translation time) */
    uint64_t jit_block_entries;
    uint64_t smc_invalidations;
    uint64_t a20_flushes;
    uint64_t verify_blocks_checked;
    uint64_t links_created, links_patched, links_unpatched;
    uint64_t refused_by_op[OP__COUNT];   /* which op ended/refused blocks */
    uint64_t fallback_by_op[OP__COUNT];  /* which op the interpreter actually ran (dynamic) */
    uint32_t max_block_bytes;
    uint32_t last_block_bytes;
    int      flush_pending;         /* A20 changed under running code: rewind the code buffer at the next translate */

    int trace;
    int verify;                    /* -V: lockstep interp shadow, diff each block run */
    int verify_mem_every;          /* compare guest memory every N block runs (0 = each) */

    x86_cpu shadow;                /* verify only; has its own memory */
    int shadow_live;
} x86_dbt;

/* Public API (dbt_common.c) */
int  dbt_init(x86_dbt *dbt, x86_cpu *cpu);
int  dbt_run(x86_dbt *dbt);
void dbt_cleanup(x86_dbt *dbt);
int  dbt_jit_available(const x86_cpu *cpu);
void dbt_print_stats(x86_dbt *dbt, FILE *out);

/* Cache management (dbt_cache.c) */
x86_block_entry *dbt_cache_lookup(x86_dbt *dbt, uint64_t key);
void             dbt_cache_insert(x86_dbt *dbt, uint64_t key, uint8_t *code);
void             dbt_cache_invalidate_all(x86_dbt *dbt);
int  dbt_link_record(x86_dbt *dbt, uint32_t lin, uint32_t site_off);
void dbt_links_repatch(x86_dbt *dbt, uint32_t lin, uint8_t *code);
void dbt_mark_block_bytes(x86_dbt *dbt, uint32_t start, uint32_t end);
void dbt_smc_store(x86_cpu *cpu, uint32_t phys);          /* cpu->smc_hook */
void dbt_host_wrote(x86_cpu *cpu, uint32_t phys, uint32_t len);
void dbt_a20_changed(x86_cpu *cpu, int on);

/* Backend hooks (dbt_a64.c) */
uint8_t *dbt_translate_block(x86_dbt *dbt, uint64_t key);
void     dbt_emit_trampoline(x86_dbt *dbt);
void     dbt_arch_patch_link(x86_dbt *dbt, uint32_t site_off, uint8_t *target);

int      dbt_classify_op(const x86_insn *in);   /* 0 refuse, 1 inline, 2 helper */

/* Helpers called from translated code (dbt_common.c) */
void dbt_h_exec(x86_cpu *cpu, uint32_t insn_index);

/* Current block key for the cpu's CS:IP. */
static inline uint64_t dbt_cpu_key(const x86_cpu *c) {
    uint32_t lin = (c->seg[S_CS].base + (c->eip & 0xFFFF)) & c->a20_mask;
    return dbt_key(c->seg[S_CS].sel, lin);
}

#endif /* DBT_H */
