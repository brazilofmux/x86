/* dbt.h — x86 Dynamic Binary Translator public interface.
 *
 * Lifted from z80's dbt/dbt.h (github.com/brazilofmux/z80). The translator runs alongside the
 * interpreter (core/x86_interp.c, the oracle): blocks the backend can
 * translate run as native code; anything that changes CS, raises, or
 * talks to the host (INT, far transfers, IRET, DIV, I/O, HLT) ends the
 * block and the run loop steps the interpreter for that one instruction.
 * Instructions that are exact but rare go through a generic helper
 * that calls the interpreter's execute() on the pooled decoded form —
 * still inside the block, no exit. Protected-mode code the emitters do
 * not shape (16-bit PM outside KEY_SEG16, anything with A20 off) is
 * translated as nothing but such helpers.
 *
 * Two halves: dbt_translate.c plans a block (decode, classify, roles,
 * flag liveness — x86 semantics, host-independent) and a backend emits
 * it (dbt_arch_emit_block: dbt_a64.c, dbt_x64.c). The plan is dbt_block,
 * below.
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
 * Block key = mode bits | (cs_sel << 32) | linear, linear =
 * (CS.base + EIP) & a20 (see dbt_key). The cache is direct-mapped on
 * linear & (BLOCK_CACHE_SIZE-1): 1:1 over low memory, so real mode never
 * aliases, while extended memory folds onto it. A slot therefore only
 * ever holds a block starting at one of the addresses that fold there,
 * and everything that walks slots by address (the SMC sweep, the link
 * registry) checks the entry's own linear address. The 64-bit tag keeps
 * two CS values that alias one linear address apart (the translation
 * bakes in IP-relative constants).
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

#define BLOCK_CACHE_BITS   21
#define BLOCK_CACHE_SIZE   (1u << BLOCK_CACHE_BITS)   /* covers low memory 1:1 */
#define BLOCK_CACHE_MASK   (BLOCK_CACHE_SIZE - 1)
_Static_assert(BLOCK_CACHE_SIZE >= X86_LOW_SIZE, "real mode must not alias in the block cache");
#define BLOCK_EMPTY_KEY    0xFFFFFFFFFFFFFFFFull
#define BLOCK_REFUSED_BIT  0x8000000000000000ull

/* Key bits above the selector: how the bytes at `linear` decode. A
 * translation is only valid for the mode it was made in, and in
 * protected mode for the code segment's default size. */
#define KEY_PMODE          (1ull << 48)
#define KEY_BIG            (1ull << 49)   /* CS D bit: 32-bit default operand/address size */
#define KEY_FLAT           (1ull << 50)   /* CS, DS, ES, SS all base 0, limit 4G, 32-bit (dbt_seg_flat) */
#define KEY_SEG16          (1ull << 51)
#define KEY_V86            (1ull << 52)   /* virtual-8086 mode: real-mode-shaped code at CPL 3 */
#define KEY_IOPL3          (1ull << 53)   /* V86 at IOPL 3: CLI/STI/PUSHF behave as in real mode */
#define KEY_PAGED          (1ull << 54)   /* under paging: V86 through cpu->pgd_r/pgd_w, flat PM through cpu->tlb */
#define KEY_ESNULL         (1ull << 55)   /* flat, but ES is null (a monitor entered from V86): ES accesses are the interpreter's */
#define KEY_SPACE_SHIFT    58             /* paged keys: which address space (CR3) the block was translated in, */
#define KEY_SPACE_MASK     (0xFull << KEY_SPACE_SHIFT)   /* 16 at a time (dbt_tlb_flushed); outside the slot hash */
#define KEY_SS32           (1ull << 62)   /* segmented 16-bit code on a 32-bit stack (SS.B): push/pop move all of ESP */
#define KEY_A20OFF         (1ull << 57)   /* translated with the A20 gate off: far targets and wraps bake the 1 MB mask
                                             * (outside the slot hash: both states' blocks share a slot, and the SMC
                                             * sweep and code-page drops find either by address) */
#define KEY_DSNULL         (1ull << 56)   /* ...and the same for DS */   /* 16-bit CS and SS, expand-up data segments: real-mode-shaped code with limits (dbt_seg16_ok) */

/* Monotonic nanoseconds, cheap enough to take around every block run:
 * the AArch64 virtual counter, scaled; clock_gettime elsewhere. */
#include <time.h>
static inline uint64_t dbt_now_ns(void) {
#if defined(__aarch64__)
    static uint64_t mult;
    uint64_t v;
    if (!mult) {
        uint64_t f;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
        mult = f ? (1000000000ull << 32) / f : 1ull << 32;
    }
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return (uint64_t)(((unsigned __int128)v * mult) >> 32);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static inline uint64_t dbt_key(uint32_t cs_sel, uint32_t lin) { return ((uint64_t)cs_sel << 32) | lin; }
static inline uint32_t dbt_key_lin(uint64_t key) { return (uint32_t)key; }
/* Cache slot: the linear address, with the key's mode bits (48..52:
 * PMODE, BIG, FLAT, SEG16) folded into bits 16..20 so the same code
 * seen under two segment shapes — DOS/4GW's dispatcher enters on the
 * client's 32-bit stack and switches to its own 16-bit one — holds
 * both translations instead of evicting one with the other. Real-mode
 * keys (no mode bits) stay 1:1 over low memory. */
#define KEY_MODE_SHIFT 48
#define KEY_MODE_MASK  0x1Fu
static inline uint32_t dbt_slot_mode(uint32_t lin, uint32_t mode) { return (lin ^ (mode << 16)) & BLOCK_CACHE_MASK; }
static inline uint32_t dbt_slot(uint64_t key) { return dbt_slot_mode((uint32_t)key, (uint32_t)(key >> KEY_MODE_SHIFT) & KEY_MODE_MASK); }
/* Every mode-bit combination a key can carry: real; PM 16-bit; PM 32-bit;
 * flat; segmented 16-bit. The SMC sweep probes each. */
#define KEY_MODE_VARIANTS 6
static const uint32_t dbt_key_modes[KEY_MODE_VARIANTS] = { 0, 1, 3, 7, 9, 16 };

#ifndef MAX_BLOCK_INSNS
#define MAX_BLOCK_INSNS    64
#endif

/* Direct block linking (see dbt_cache.c): every static edge is a
 * patchable B recorded here under its target's cache slot, with the
 * full target key — slots are shared, and a site must only ever be
 * patched to the block it names. */
#define LINK_POOL_SIZE   (512 * 1024)
#define LINK_NONE        0xFFFFFFFFu
typedef struct {
    uint64_t key;
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
    H_POST_STORE,      /* void (cpu, phys | bytes << 28): device and SMC hooks after a JIT store */
    H_OUT,             /* uint32 (cpu, port | size << 16, value): OUT; 0 go on, 1 leave after it, 2 #GP */
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
    x86_jit_aux *aux;              /* calloc'd: ~32 MB, lazily committed */

    /* Byte span of each cached block, parallel to aux->cache. Read only
     * by the C-side SMC sweeps; 0xFFFFFFFF for refused sentinels. */
    uint32_t *span;

    /* Direct-link registry, one list per cache slot. */
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
    uint32_t  insn_high;                 /* high-water mark of insn_used across flushes (stats scan this far) */
    uint32_t *insn_hits;           /* per pooled insn: dynamic helper calls (stats) */
    uint8_t  *insn_tag;            /* per pooled insn: 0 all-helper PM block, 1 flat block helper, 2 flat slow path */
    uint32_t *insn_lin;            /* per pooled insn: its linear address (stats) */

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
    /* Code pages paged blocks were translated on (V86 or flat protected
     * mode), with the physical page each mapped to then: a TLB flush
     * re-peeks them and drops the blocks of any that moved. */
#define DBT_PCODE_MAX 2048
    struct { uint32_t lin_page, phys_page; uint8_t user, space; } pcode[DBT_PCODE_MAX];
    uint32_t n_pcode;
    /* The address spaces paged blocks were translated in: CR3 values, by
     * the id their keys carry (KEY_SPACE). A VCPI client switches between
     * its page tables and its server's on every call down to DOS; the
     * other space's blocks wait, unchecked, until it is current again. */
#define DBT_SPACES 16
    uint32_t space_cr3[DBT_SPACES];
    uint8_t  space_used[DBT_SPACES], space_next;
    uint64_t space_evictions;
    /* A remapped code page (UMB code, a memory manager mapped high): the
     * block keys are linear, the code bitmap and every SMC report
     * physical. phys_alias[physical page] is the linear page + 1 whose
     * blocks a store there must also sweep. One alias per physical page;
     * a second linear page onto the same one is not translated. */
    uint32_t phys_alias[X86_MEM_MAX >> 12];
    uint64_t smc_hot_refusals;      /* blocks ended before a patched instruction */
    uint64_t tlb_flushes;           /* TLB flushes seen (CR3, PG, A20)... */
    uint64_t tlb_page_drops;        /* ...and code pages whose blocks went because the page moved */
    uint64_t desc_flushes;
    uint64_t verify_blocks_checked;
    uint64_t links_created, links_patched, links_unpatched;
    uint64_t refused_by_op[OP__COUNT];   /* which op ended/refused blocks */
    uint64_t fallback_by_op[OP__COUNT];  /* which op the interpreter actually ran (dynamic) */
    uint64_t fallback_by_class[6];       /* ...and where: real, V86, PM flat, PM paged not flat, PM other, inhibited */
    struct { uint64_t key, n; } fb_site[4096];   /* X86_FALLBACK_SITES: where (mode:CS:EIP), open-addressed */
    int      fb_sites;
    uint64_t helper_by_op[OP__COUNT];    /* which op helper calls ran (dynamic) — the promotion list */
    /* Where the run's time went, exactly (dbt_now_ns is a counter read):
     * translated code (helper calls from it included), host services
     * (HLE steps: DOS, BIOS, DPMI), plain interpreter steps, translation,
     * the host poll (keyboard, screen, timer, idle sleeps), -V's shadow.
     * And the guest instructions each retired. */
    int      phases;               /* X86_PHASES: take the times below (off by default: clock reads cost) */
    uint64_t t_jit, t_svc, t_interp, t_xlate, t_poll, t_verify, t_run;
    uint64_t n_jit, n_svc, n_interp;
    /* X86_PMPROF=1: what protected-mode code actually executes, to decide
     * what the backend learns first. Indexed [op][opsize==4][adsize==4]. */
    int      pmprof;
    uint64_t pmprof_after;              /* X86_PMPROF=N: start counting at instruction N */
    uint64_t pm_ops[OP__COUNT][2][2];
    uint64_t pm_insns, pm_mem, pm_sib, pm_segovr, pm_rep;
    uint32_t *pm_hits;                  /* per 16-byte line of linear memory */
    uint32_t max_block_bytes;
    uint32_t bm_lo, bm_hi;         /* bitmap range holding CODE/DESC marks: [lo, hi) */
    /* SMC heat: how often a store to each guest byte has invalidated
     * translated code (saturating). A byte at SMC_VOLATILE or above is
     * patched data living in an instruction — an immediate that code
     * rewrites before running it (DOOM's column drawers) — and a flat
     * block reads such an immediate from memory at run time instead of
     * baking it in, leaving the bytes unmarked. */
    uint8_t *smc_heat;
    uint8_t *smc_win;               /* the window (insn_count >> SMC_WINDOW_SHIFT) a byte's heat belongs to */
    uint32_t last_block_bytes;
    int      flush_pending;         /* A20 changed under running code: rewind the code buffer at the next translate */

    int trace;
    int verify;                    /* -V: lockstep interp shadow, diff each block run */
    int shadow_stale;              /* the machine moved without the shadow; resync before the next check */
    int shadow_ext;                /* translated PM code has run: the shadow tracks all of memory, not just low */
    /* Device reads (planar VGA) seen by the real cpu since the last check,
     * replayed in order to the shadow, which has no device of its own. */
    uint8_t (*dev_read_real)(x86_cpu *, uint32_t);
    uint8_t *devlog;
    uint32_t devlog_n, devlog_cap, devlog_pos;
    /* the same for port reads (IN inside a run: a V86 helper) */
    uint32_t (*io_read_real)(x86_cpu *, uint16_t, int);
    uint32_t *iolog;
    uint32_t iolog_n, iolog_cap, iolog_pos;
    int verify_mem_every;          /* compare guest memory every N block runs (0 = each) */

    FILE    *golden;               /* X86_GOLDEN: the translation log */
    uint64_t golden_hash, golden_n;

    x86_cpu shadow;                /* verify only; has its own memory */
    int shadow_live;
} x86_dbt;

/* Public API (dbt_common.c) */
int  dbt_init(x86_dbt *dbt, x86_cpu *cpu);
int  dbt_run(x86_dbt *dbt);
void dbt_cleanup(x86_dbt *dbt);
int  dbt_jit_available(const x86_cpu *cpu);
void dbt_print_stats(x86_dbt *dbt, FILE *out);
/* X86_SAMPLE=N: sample the host PC every N microseconds (SIGPROF) and,
 * at exit, report where the time went: translated blocks by guest
 * address, the run-time's own functions by name. */
void dbt_sample_start(void);
void dbt_sample_report(x86_dbt *dbt, FILE *out);

/* Cache management (dbt_cache.c) */
x86_block_entry *dbt_cache_lookup(x86_dbt *dbt, uint64_t key);
void             dbt_cache_insert(x86_dbt *dbt, uint64_t key, uint8_t *code);
void             dbt_cache_invalidate_all(x86_dbt *dbt);
int  dbt_link_record(x86_dbt *dbt, uint64_t key, uint32_t site_off);
void dbt_links_repatch(x86_dbt *dbt, uint64_t key, uint8_t *code);
#define SMC_VOLATILE 4
/* Heat decays: it counts invalidations within one window of 2^26 guest
 * instructions (and the one before). A patching loop crosses
 * SMC_VOLATILE in a window; code reloaded now and then — a program
 * EXECed again, FreeCOM swapping itself back in — never does, and so is
 * never left to the interpreter for good. */
#define SMC_WINDOW_SHIFT 26
static inline uint8_t dbt_smc_window(const x86_cpu *c) { return (uint8_t)(c->insn_count >> SMC_WINDOW_SHIFT); }
static inline int dbt_smc_hot(const uint8_t *heat, const uint8_t *win, uint32_t p, uint8_t now) {
    return heat[p] >= SMC_VOLATILE && (uint8_t)(now - win[p]) <= 1;
}
void dbt_mark_block_bytes(x86_dbt *dbt, uint32_t start, uint32_t end);
void dbt_mark_block_bytes_except(x86_dbt *dbt, uint32_t start, uint32_t end, const uint32_t *skip, uint32_t nskip);
void dbt_watch_cs_desc(x86_dbt *dbt);                     /* PM: flush if CS's descriptor is rewritten */
void dbt_smc_store(x86_cpu *cpu, uint32_t phys);          /* cpu->smc_hook */
void dbt_clear_code_bits(x86_cpu *cpu);                   /* forget translations, keep device marks */
void dbt_host_wrote(x86_cpu *cpu, uint32_t phys, uint32_t len);
void dbt_a20_changed(x86_cpu *cpu, int on);
void dbt_dev_changed(x86_cpu *cpu);
void             dbt_tlb_flushed(x86_cpu *cpu);
int              dbt_note_code_page(x86_dbt *dbt, uint32_t lin_page, uint32_t phys_page, int user);
void             dbt_space_current(x86_dbt *dbt);   /* register CR3's space, set cpu->pg_space */

/* X86_GOLDEN=<path>: translate-only mode (dbt_common.c). The interpreter
 * runs the program; before every instruction the key it stands at is
 * translated, once, and the emitted bytes are hashed to the file as one
 * line per translation: key, length, hash. Two builds of the translator
 * must agree on every block, byte for byte (tools/golden-diff.py) — the
 * check a refactor of the translator is held to, and one that needs no
 * host of the backend's architecture: the emitters only write bytes.
 * The machine's clock is the instruction counter meanwhile (pc.vclock),
 * so the timer, and with it the block sequence, repeats run to run. */
int  dbt_golden_open(x86_dbt *dbt, const char *path);
void dbt_golden_step(x86_dbt *dbt);
void dbt_golden_close(x86_dbt *dbt, FILE *out);

/* ---- A block plan: the front end's decisions, the backend's input ----
 * dbt_translate.c decodes the block at a key, classifies each instruction,
 * gives it a role, runs the backward flag-liveness pass and finds patched
 * immediates; dbt_arch_emit_block turns the plan into host code. Nothing
 * in the plan depends on the host. */
enum { C_REFUSE = 0, C_INLINE, C_HELPER };                 /* an instruction's class */
enum { ROLE_PLAIN = 0, ROLE_UNCOND, ROLE_COND, ROLE_HELPER_END };   /* and its role in the block */

/* Superblocks keep translating through conditionals (side exits) only
 * while the block is shorter than this many guest bytes. */
#define SUPERBLOCK_BYTE_CAP 48

typedef struct {
    uint64_t key;
    uint64_t mode_bits;         /* the key's mode bits: every static edge carries them */
    int      model;             /* cpu->model */
    /* The block's shape, from the key and the cpu. */
    uint8_t  flat, seg16, ss32, v86, paged, iopl3, pg_user, esnull, dsnull;
    uint8_t  devread;           /* a device answers reads in the VGA window (planar VGA) */
    uint8_t  regs32;            /* 386: the pinned registers hold all 32 bits */
    uint8_t  wrap_exact;        /* < 286: word accesses at offset FFFF wrap in-segment */
    uint8_t  all_helper;        /* a protected-mode block of nothing but helpers (plan_pm) */
    uint8_t  ends_dynamic;      /* all_helper: ends after a near transfer; the helper set EIP */
    uint32_t code_delta;        /* paged: physical - linear of the code page, mod 2^32 */
    uint32_t start_ip, end_ip;  /* guest bytes covered: [start_ip, end_ip) within CS */
    uint32_t n_ops;
    uint32_t ip_afters[MAX_BLOCK_INSNS];
    uint8_t  cls[MAX_BLOCK_INSNS];        /* C_INLINE or C_HELPER (refusals end the block) */
    uint8_t  role[MAX_BLOCK_INSNS];       /* ROLE_* */
    uint32_t fmask[MAX_BLOCK_INSNS];      /* arithmetic flags live AFTER op i (live-out) */
    uint32_t fexit[MAX_BLOCK_INSNS];      /* and only on op i's own side exit (a store's SMC sweep): bits
                                           * the block never reads, so a backend may emit them on that
                                           * cold path instead. fmask | fexit is the eager mask. */
    uint32_t live_in[MAX_BLOCK_INSNS];    /* and BEFORE it: what bookkeeping emitted ahead of op i must not clobber */
    uint32_t dyn_lin[MAX_BLOCK_INSNS];    /* nonzero: read op i's immediate from this physical address */
    x86_insn decs[MAX_BLOCK_INSNS];       /* last: everything before it is cleared per block */
} dbt_block;

/* Predicates both halves need. */
static inline int dbt_near_transfer(int op) {
    return op == OP_JMP || op == OP_CALL || op == OP_RET || op == OP_JCC || op == OP_JCXZ
        || op == OP_LOOP || op == OP_LOOPE || op == OP_LOOPNE;
}
static inline int dbt_loads_segment(const x86_insn *in) {
    switch (in->op) {
    case OP_MOVSEG: case OP_POP: return in->ops[0].kind == OPK_SREG;
    case OP_LES: case OP_LDS: case OP_LSS: case OP_LFS: case OP_LGS: return 1;
    default: return 0;
    }
}

/* Front end (dbt_translate.c): plan the block at key and have the
 * backend emit it; NULL when the run loop should step the interpreter. */
uint8_t *dbt_translate_block(x86_dbt *dbt, uint64_t key);

/* Backend hooks (dbt_a64.c, dbt_x64.c: one of them is built, BACKEND in the Makefile) */
uint8_t *dbt_arch_emit_block(x86_dbt *dbt, const dbt_block *b);   /* host code for a plan (never NULL) */
/* Can the backend emit this instruction inline in a block of b's shape
 * (flat, seg16, v86/real, paged; b->model)? Asked by the front end for
 * every instruction the interpreter does not claim; a no makes it a
 * helper. b->decs is not filled in yet when this is called. */
int      dbt_arch_can_inline(const dbt_block *b, const x86_insn *in);
void     dbt_emit_trampoline(x86_dbt *dbt);
void     dbt_arch_patch_link(x86_dbt *dbt, uint32_t site_off, uint8_t *target);

/* Classes for tools/jittest's fuzzer (dbt_translate.c): 0 refuse, 1 inline, 2 helper. */
int      dbt_classify_op(const x86_insn *in);
int      dbt_classify_op_pm(const x86_insn *in);
int      dbt_classify_op_seg16(const x86_insn *in);

/* Helpers called from translated code (dbt_common.c) */
void dbt_h_exec(x86_cpu *cpu, uint32_t insn_index);
void dbt_h_post_store(x86_cpu *cpu, uint32_t arg);
uint32_t dbt_h_out(x86_cpu *cpu, uint32_t port_size, uint32_t value);

/* The flat model: base 0, limit 4G, 32-bit, and for data segments
 * writable and expanding up. A block translated under KEY_FLAT addresses
 * memory as mem + offset with no base add and no limit check (no access
 * through these segments can fault), and ends after anything that loads
 * a segment register, so the assumption holds for as long as it runs;
 * everything else that changes a segment returns to the run loop, which
 * recomputes the key. */
static inline int dbt_seg_flat(const x86_seg *g, int code) {
    if (!g->usable || g->base != 0 || g->limit != 0xFFFFFFFFu || !g->big || !X86_AR_S(g->attr)) return 0;
    if (code) return (g->attr & X86_TYPE_CODE) != 0;
    return !(g->attr & X86_TYPE_CODE) && (g->attr & X86_TYPE_WRITABLE) && !(g->attr & X86_TYPE_EXPDOWN);
}

/* Segmented 16-bit protected mode (a KEY_SEG16 block): the real-mode
 * translator's shape — 16-bit IP and SP, offsets through pinned segment
 * bases — with each access checked against the segment's limit, read
 * from the cpu at run time (DOS/4GW's data segments change under the
 * same block). Needs a 16-bit code segment and a 16-bit expand-up stack;
 * DS and ES may be null (an access through one is #GP: the check sees
 * `usable`) but not expand-down, whose limit reads the other way. */
static inline int dbt_seg16_data_ok(const x86_seg *g) {
    if (!g->usable) return 1;
    if (!X86_AR_S(g->attr)) return 0;
    if (g->attr & X86_TYPE_CODE) return (g->attr & X86_TYPE_READABLE) != 0;
    return !(g->attr & X86_TYPE_EXPDOWN);
}
static inline int dbt_seg16_ok(const x86_cpu *c) {
    const x86_seg *cs = &c->seg[S_CS], *ss = &c->seg[S_SS];
    if (!cs->usable || cs->big || !X86_AR_S(cs->attr) || !(cs->attr & X86_TYPE_CODE)) return 0;
    if (!ss->usable || !X86_AR_S(ss->attr) || (ss->attr & X86_TYPE_CODE)
        || !(ss->attr & X86_TYPE_WRITABLE) || (ss->attr & X86_TYPE_EXPDOWN)) return 0;
    /* A 32-bit stack (DOS/4GW's 16-bit code on its client's flat stack):
     * under paging every access goes through the TLB; without it the block
     * addresses mem + base + ESP, so the segment must lie in memory. */
    if (ss->big && !(c->cr0 & X86_CR0_PG) && (uint64_t)ss->base + ss->limit >= c->mem_size) return 0;
    return dbt_seg16_data_ok(&c->seg[S_DS]) && dbt_seg16_data_ok(&c->seg[S_ES]);
}

extern int dbt_seg16_enabled;   /* X86_NO_SEG16 clears it: segmented 16-bit PM blocks stay all-helper (A/B) */

/* Mode bits of a key for the current segments. */
static inline uint64_t dbt_cpu_mode_bits(const x86_cpu *c) {
    uint64_t a20 = c->a20_mask != 0xFFFFFFFFu ? KEY_A20OFF : 0;
    if (!c->pmode) return a20;
    uint64_t b = KEY_PMODE | a20 | (c->seg[S_CS].big ? KEY_BIG : 0);
    /* Flat: CS and SS flat, DS and ES flat or null — entering a monitor
     * from V86 mode nulls DS/ES/FS/GS, and its handler addresses through
     * SS until it loads its own (KEY_DSNULL/KEY_ESNULL: accesses through
     * a null one are the interpreter's #GP). */
    if (dbt_seg_flat(&c->seg[S_CS], 1) && dbt_seg_flat(&c->seg[S_SS], 0)
        && (dbt_seg_flat(&c->seg[S_DS], 0) || !c->seg[S_DS].usable)
        && (dbt_seg_flat(&c->seg[S_ES], 0) || !c->seg[S_ES].usable)) {
        b |= KEY_FLAT | (c->seg[S_ES].usable ? 0 : KEY_ESNULL) | (c->seg[S_DS].usable ? 0 : KEY_DSNULL);
        if (c->cr0 & X86_CR0_PG) b |= KEY_PAGED | (uint64_t)c->pg_space << KEY_SPACE_SHIFT;
    }
    else if (dbt_seg16_enabled && dbt_seg16_ok(c)) {
        b |= KEY_SEG16 | (c->seg[S_SS].big ? KEY_SS32 : 0);
        if (c->cr0 & X86_CR0_PG) b |= KEY_PAGED | (uint64_t)c->pg_space << KEY_SPACE_SHIFT;
    }
    return b;
}

/* Current block key for the cpu's CS:EIP. Real mode masks for A20 and
 * the 16-bit IP; protected mode is only translated with A20 on, where
 * linear = CS.base + EIP exactly and the exit stub can recover EIP as
 * linear - CS.base. */
static inline uint64_t dbt_cpu_key(const x86_cpu *c) {
    if (c->eflags & X86_VM) {
        /* V86: CS.base + IP, not A20-masked (under paging the linear
         * address is what the page tables see; the mask is physical) */
        uint64_t b = KEY_V86 | ((c->eflags & X86_IOPL) == X86_IOPL ? KEY_IOPL3 : 0)
                   | ((c->cr0 & X86_CR0_PG) ? KEY_PAGED | (uint64_t)c->pg_space << KEY_SPACE_SHIFT : 0)
                   | (c->a20_mask != 0xFFFFFFFFu ? KEY_A20OFF : 0);
        return dbt_key(c->seg[S_CS].sel, c->seg[S_CS].base + (c->eip & 0xFFFF)) | b;
    }
    if (c->pmode) return dbt_key(c->seg[S_CS].sel, c->seg[S_CS].base + c->eip) | dbt_cpu_mode_bits(c);
    uint32_t lin = (c->seg[S_CS].base + (c->eip & 0xFFFF)) & c->a20_mask;
    return dbt_key(c->seg[S_CS].sel, lin) | dbt_cpu_mode_bits(c);   /* real mode: KEY_A20OFF or nothing */
}

#endif /* DBT_H */
