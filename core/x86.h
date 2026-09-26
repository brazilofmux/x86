/* x86.h — 8086..80386 CPU state, flags, memory model, public interfaces
 *
 * Everything the interpreter and the DBT share lives here. Layout is
 * chosen so that generated code can reach any register at a fixed,
 * small offset from the cpu pointer, and so that the register file
 * indexes match the instruction encoding (AX=0 CX=1 DX=2 BX=3 SP=4
 * BP=5 SI=6 DI=7; ES=0 CS=1 SS=2 DS=3 FS=4 GS=5).
 */
#ifndef X86_H
#define X86_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <setjmp.h>
#include "x86_fpu.h"

/* The separator in a list of host paths (a diskette sequence): ':' as in
 * $PATH, but ';' on Windows, where ':' follows a drive letter. */
#if defined(_WIN32)
#define HOST_PATH_LIST_SEP ";"
#else
#define HOST_PATH_LIST_SEP ":"
#endif

/* ============================================================================
 * FLAGS
 * The low byte is the 8080 F register: CF=0 PF=2 AF=4 ZF=6 SF=7.
 * ========================================================================= */
#define X86_CF   0x0001
#define X86_F1   0x0002   /* reserved, always 1 */
#define X86_PF   0x0004
#define X86_AF   0x0010
#define X86_ZF   0x0040
#define X86_SF   0x0080
#define X86_TF   0x0100
#define X86_IF   0x0200
#define X86_DF   0x0400
#define X86_OF   0x0800
#define X86_IOPL 0x3000
#define X86_NT   0x4000
#define X86_RF   0x00010000
#define X86_VM   0x00020000
#define X86_AC   0x00040000   /* 486: alignment check (writable; how software tells a 486 from a 386) */
#define X86_ID   0x00200000   /* Pentium: writable, which is how software finds CPUID */

#define X86_ARITH_FLAGS (X86_CF|X86_PF|X86_AF|X86_ZF|X86_SF|X86_OF)

/* CPU models. Gate quirks on these, never on "is it a 386?" booleans. */
enum {
    X86_MODEL_8086 = 86,
    X86_MODEL_186  = 186,
    X86_MODEL_286  = 286,
    X86_MODEL_386  = 386,
    X86_MODEL_486  = 486,    /* no CPUID: EFLAGS.ID stays 0 */
    X86_MODEL_586  = 586,    /* a P54C Pentium: CPUID, RDTSC, CMPXCHG8B, MSRs, CR4 (PSE, TSD, DE, MCE) */
};

/* The Pentium's identity: CPUID 1's EAX, and EDX after reset. Family 5,
 * model 2 (the P54C), stepping 12 (C0). */
#define X86_586_SIGNATURE 0x0000052Cu

/* Register indexes (encoding order) */
enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };
enum { R_AL, R_CL, R_DL, R_BL, R_AH, R_CH, R_DH, R_BH };
enum { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS, S_NONE = 0xFF };

/* Per-segment cached state. In real mode base = sel<<4 and limit is
 * 0xFFFF; in PM the DPMI host fills these from the descriptor. */
/* attr packs the descriptor's access byte in bits 0-7 and its flags nibble
 * (G, D/B, L, AVL) in bits 8-11 — the two bytes the processor keeps once the
 * base and limit have been unpacked. `usable` is the hidden valid bit: a
 * segment register loaded with a null selector holds no descriptor at all. */
#define X86_AR_TYPE(a)    ((a) & 0x0F)
#define X86_AR_S(a)       (((a) >> 4) & 1)        /* 1 = code/data, 0 = system */
#define X86_AR_DPL(a)     (((a) >> 5) & 3)
#define X86_AR_P(a)       (((a) >> 7) & 1)
#define X86_AR_G(a)       (((a) >> 11) & 1)
#define X86_AR_DB(a)      (((a) >> 10) & 1)
#define X86_TYPE_CODE     0x08                    /* within a code/data type */
#define X86_TYPE_CONFORM  0x04                    /* code: conforming */
#define X86_TYPE_READABLE 0x02                    /* code: readable */
#define X86_TYPE_EXPDOWN  0x04                    /* data: expand-down */
#define X86_TYPE_WRITABLE 0x02                    /* data: writable */
#define X86_TYPE_ACCESSED 0x01

typedef struct x86_seg {
    uint16_t sel;
    uint16_t attr;      /* PM: access byte + flags; 0 in real mode */
    uint8_t  usable;    /* 0 after a null selector: present but unusable */
    uint8_t  pad1[3];
    uint32_t base;
    uint32_t limit;     /* byte granular, inclusive */
    uint8_t  big;       /* D/B bit: default 32-bit operands/stack */
    uint8_t  pad[3];
} x86_seg;

#define X86_PGD_PAGES 0x110                 /* linear pages 0 .. 10FFFFh: all of V86's reach */
#define X86_PGD_NONE  1                     /* odd: no delta is (they are multiples of 4K) */

typedef struct x86_cpu {
    /* General registers, encoding order. 8-bit view via x86_r8 macros. */
    uint32_t r[8];
    uint32_t eip;
    uint32_t eflags;

    x86_seg  seg[6];

    /* Guest memory: flat host buffer. Physical address = linear & a20_mask,
     * and anything >= mem_size is open bus (reads 0xFF, writes dropped).
     * See x86_mem.c for the A20 mirror that lets the JIT skip the mask. */
    uint8_t *mem;
    uint32_t mem_size;
    uint32_t a20_mask;
    int      mem_fd;        /* backing object of the mirrored mapping, -1 if plain */
    uint8_t  mem_mirrored;  /* 1: HMA window aliases low memory while A20 is off */
    uint8_t  pad1[3];

    /* One byte per guest byte, nonzero while a translated block covers
     * it. Always allocated (all zero without a DBT) so the store path
     * below needs no NULL test. Same mirror layout as mem. */
    uint8_t *code_bitmap;
    void   (*smc_hook)(struct x86_cpu *, uint32_t phys);   /* DBT: a store hit code */
    /* Device memory (VGA planes). A store to a byte whose bitmap entry has
     * X86_BM_DEVICE is handed to device_store after it lands, from the
     * interpreter and from translated code alike; device_read, when set,
     * takes every interpreter read of the A0000-AFFFF window. */
    void   (*device_store)(struct x86_cpu *, uint32_t phys);
    uint8_t (*device_read)(struct x86_cpu *, uint32_t phys);
    /* Device fast path for translated byte stores into the device window
     * (planar VGA in its plain write mode, one plane enabled): the byte
     * goes to dev_wplane[off] and the window byte becomes dev_rplane[off],
     * which is all device_store would have done. NULL: call device_store.
     * The device keeps these current as its registers change. */
    uint8_t *dev_wplane, *dev_rplane;
    void   (*a20_hook)(struct x86_cpu *, int on);          /* DBT: the A20 gate changed */
    void   (*dev_hook)(struct x86_cpu *);                  /* DBT: device_read came or went (blocks bake it in) */
    void   (*trace_exc)(struct x86_cpu *, int vec, uint32_t err);   /* oracle tracing */

    /* Set while the very next HLE trap is the delivery of a CPU exception
     * rather than an interrupt or an INT instruction. The two are
     * indistinguishable once the gate has been taken — vector 8 is both
     * #DF and the timer IRQ — and a DPMI host must tell them apart, since
     * only a fault belongs to the client's exception handler. */
    uint8_t  exc_delivered;
    void    *dbt;           /* owning translator, NULL when interpreting only */
    void    *jit_aux;       /* DBT aux block base, reloaded by helper-call sequences */
    uint64_t jit_budget;    /* insn budget handed to the trampoline (exit stub math) */
    uint64_t jit_cnt_save;  /* pinned budget register parked across helper calls */
    uint8_t  pg_space;      /* DBT: id of the address space CR3 names (paged block keys carry it) */
    uint32_t jit_cur_lin;   /* linear address of the block making a helper call... */
    uint32_t jit_cur_hit;   /* ...set by the SMC sweep if that block got invalidated */
    uint64_t jit_flags;     /* x86-64 backend: the guest's arithmetic flags between blocks (an RFLAGS image; 64 bits: it is POPped into) */

    /* Descriptor tables. In real mode these sit unused; CR0.PE turns them on.
     * ldtr/tr keep the cached descriptor the same way the segment registers
     * do, because that is what the processor actually consults. */
    struct { uint32_t base; uint16_t limit; } gdtr, idtr;
    /* The real-mode CS still in place when CR0.PE was set: CPL is 0 until
     * a protected-mode CS load replaces it (x86_cpl), however its low bits
     * read — unreal-mode setups (HIMEMX) set PE, load DS/ES and clear PE
     * without ever touching CS. */
    uint16_t pe_cs_sel;
    uint32_t pe_cs_base;
    uint8_t  pe_window;
    x86_seg  ldtr, tr;
    uint32_t cr0;

    int      model;       /* X86_MODEL_* */
    uint8_t  pmode;       /* 0 = real mode, 1 = protected */
    uint8_t  halted;
    uint8_t  int_inhibit; /* one-instruction shadow after MOV SS / POP SS / STI */
    uint8_t  pad0;

    /* Pending exception raised during a step (X86_EXC_*), -1 if none */
    int      exc;
    uint32_t exc_err;
    /* Exceptions delivered, by vector, and the hottest sites (-s): what a
     * V86 monitor or DPMI host spends its time trapping. */
    uint64_t exc_count[32];
    struct { uint64_t key, n; } exc_site[1024];   /* key: vector << 48 | CS << 32 | EIP */
    /* Faults abort the instruction: x86_fault() longjmps here when armed
     * (x86_step, x86_exec_decoded) so no further state is committed. */
    jmp_buf  fault_jb;       /* _setjmp/_longjmp: plain setjmp saves the signal mask, a syscall per instruction on macOS */
    int      fault_armed;

    /* I/O port hooks (pc/ layer). NULL = open bus. */
    uint32_t (*io_read)(struct x86_cpu *, uint16_t port, int size);
    void     (*io_write)(struct x86_cpu *, uint16_t port, uint32_t val, int size);
    void     *io_ctx;

    /* High-level emulation trap: every IVT entry initially points at
     * hle_seg:vector, a segment of IRETs. Executing there calls hle()
     * instead of fetching, so BIOS/DOS services live in the host and a
     * guest that hooks a vector and chains still reaches them. The hook
     * performs the return itself (see pc/pc_bios.c). */
    uint16_t hle_seg;
    void   (*hle)(struct x86_cpu *, int vector);
    void    *hle_ctx;

    /* Stats */
    uint64_t insn_count;

    /* Paging (386): CR0.PG, CR2 the last faulting linear address, CR3 the
     * page directory. The TLB caches translations by linear page, direct
     * mapped; each entry carries what may be done without a walk (see
     * x86_lin). Last in the struct so the JIT's fixed offsets never move. */
    uint32_t cr2, cr3;
    uint32_t dr[8];
    uint8_t  pg_super;      /* nonzero: implicit supervisor access (descriptor tables, TSS, a ring-0 frame) */
    uint8_t  pg_probe;      /* nonzero: translate without faulting (instruction prefetch) */
    uint8_t  pg_miss;       /* set by a probe that found no mapping */
    uint8_t  pad2;
    struct x86_tlbe { uint32_t tag, phys; } tlb[256];
    /* The translator's view of the same translations, for V86 code (user
     * accesses below 10FFF0h): per linear page, physical page minus linear
     * page — added to an identity host address — or X86_PGD_NONE. Reads
     * need U at both levels; writes need U and R/W and the dirty bit
     * already set, so the first write to a page is the interpreter's.
     * Filled by the page walk, cleared with the TLB (then tlb_hook). */
    int64_t  pgd_r[X86_PGD_PAGES], pgd_w[X86_PGD_PAGES];
    void   (*tlb_hook)(struct x86_cpu *);   /* DBT: translations flushed (CR3, PG, A20) */

    /* The coprocessor (core/x86_fpu.c): a 387 beside a 386, a 486DX's
     * own. Without one, ESC opcodes only perform their bus cycle. */
    void   (*trace_task)(struct x86_cpu *, uint16_t from, uint16_t to, int reason);   /* NULL, or a debugging hook */
    /* A device's next event is due at this instruction count (the IDE
     * drive done with a block): translated code runs no further than
     * that before the run loop polls. 0 or past: no limit. */
    uint64_t next_event;
    uint8_t  has_fpu;
    x86_fpu  fpu;
    /* FERR#: an unmasked x87 exception with CR0.NE clear; the machine
     * turns it into IRQ 13 (pc/pc_bios.c). NULL on the -V shadow. */
    void   (*ferr_hook)(struct x86_cpu *);
    /* Pentium: CR4, and the time-stamp counter as an offset from its
     * clock (tsc_clock, below; translated code leaves the block for RDTSC,
     * and -V replays the real cpu's readings to the shadow). WRMSR 10h
     * moves the offset.
     * The performance-monitoring MSRs (CESR, CTR0, CTR1) hold what they
     * are given and count nothing. */
    uint32_t cr4;
    uint64_t tsc_base;
    uint64_t msr_perf[3];
    /* Would the machine interrupt the CPU now if IF were set (a request
     * the interrupt controller would pass on)? Asked by STI when it sets
     * IF: translated code takes interrupts only between blocks, and an
     * idle loop's "sti; nop; nop; cli" (Windows 2000's) never has IF set
     * at one, so STI sends the block back to the run loop instead, which
     * steps the shadowed instruction and delivers. NULL: never. */
    int    (*intr_ready)(struct x86_cpu *);
    /* The same answer, kept current by the machine (at each poll and when
     * a request, mask or in-service bit changes): what translated code's
     * inline STI reads, where calling intr_ready would cost a call. */
    uint8_t  intr_waiting;
    /* Memory-mapped devices (a PCI card's BAR): physical addresses at or
     * above mem_size + X86_MEM_SLACK go to these, one access of the
     * instruction's size (a register read that clears — an e1000's ICR —
     * must not be split into bytes). mmio_read returns 0 for an address
     * nothing answers (the bus floats: all ones). NULL: no such device,
     * and x86_rd/x86_wr do not even look. */
    int    (*mmio_read)(struct x86_cpu *, uint32_t phys, int size, uint32_t *val);
    void   (*mmio_write)(struct x86_cpu *, uint32_t phys, int size, uint32_t val);
    /* The TSC's clock: the machine's time, at the CPU's rate (pc_bios.c),
     * so it keeps counting while the CPU sits in HLT — as a Pentium's
     * does, and as Linux, whose clocksource it is, needs: counted in
     * instructions it stood still in an idle guest, and a sleep never
     * ended. NULL (the bare CPU: tools/sst and the like): instructions. */
    uint64_t (*tsc_clock)(struct x86_cpu *);
    /* A bus master wrote memory (pc_pci_dma_write: a network card's
     * descriptors and frames) — behind the CPU's back, so -V's shadow,
     * which has no devices, has to be resynced rather than compared. */
    uint8_t  dma_wrote;
} x86_cpu;

static inline uint64_t x86_tsc_raw(x86_cpu *c) { return c->tsc_clock ? c->tsc_clock(c) : c->insn_count; }
static inline uint64_t x86_tsc(x86_cpu *c) { return x86_tsc_raw(c) + c->tsc_base; }

/* 8-bit register access: AL..BL are the low bytes of r[0..3], AH..BH are
 * byte 1 of r[0..3]. Little-endian host assumed (all targets are). */
static inline uint8_t *x86_r8p(x86_cpu *c, int i) {
    return (uint8_t *)&c->r[i & 3] + (i >> 2);
}
static inline uint32_t x86_get_r8(x86_cpu *c, int i)  { return *x86_r8p(c, i); }
static inline uint32_t x86_get_r16(x86_cpu *c, int i) { return c->r[i] & 0xFFFF; }
static inline uint32_t x86_get_r32(x86_cpu *c, int i) { return c->r[i]; }
static inline void x86_set_r8(x86_cpu *c, int i, uint32_t v)  { *x86_r8p(c, i) = (uint8_t)v; }
static inline void x86_set_r16(x86_cpu *c, int i, uint32_t v) { c->r[i] = (c->r[i] & 0xFFFF0000u) | (v & 0xFFFF); }
static inline void x86_set_r32(x86_cpu *c, int i, uint32_t v) { c->r[i] = v; }

static inline uint32_t x86_get_reg(x86_cpu *c, int i, int size) {
    return size == 1 ? x86_get_r8(c, i) : size == 2 ? x86_get_r16(c, i) : x86_get_r32(c, i);
}
static inline void x86_set_reg(x86_cpu *c, int i, int size, uint32_t v) {
    if (size == 1) x86_set_r8(c, i, v); else if (size == 2) x86_set_r16(c, i, v); else x86_set_r32(c, i, v);
}

/* ============================================================================
 * Physical memory. Everything funnels through these so the A20 gate,
 * the open-bus region and (later) the SMC write hook have one home.
 * ========================================================================= */
/* The code bitmap carries three things: X86_BM_CODE, a translated block
 * covers the byte; X86_BM_DESC, the byte belongs to a descriptor that a
 * translated block's key depends on (both the DBT's); and X86_BM_DEVICE,
 * the byte is device memory whose store has side effects. Either way a
 * store lands, then any nonzero entry sends it to x86_store_hook. */
#define X86_BM_CODE   0x01
#define X86_BM_DESC   0x02
#define X86_BM_DEVICE 0x80
#define X86_BM_EMPTY  0x40                  /* no memory here: reads FFh, a store vanishes (an empty bus) */
void x86_store_hook(struct x86_cpu *c, uint32_t phys);

/* ---- Paging --------------------------------------------------------------
 * With CR0.PG set every linear address goes through the page tables. A TLB
 * entry's tag is the linear page with its low bits saying what the entry
 * allows without a walk: TLB_V valid, TLB_U user access, TLB_UW user
 * write, TLB_D dirty already set (so a write needs no update). Anything
 * the tag does not allow walks (x86_page_walk), which sets accessed and
 * dirty, checks the 386's rules — a user access needs U/S in both levels
 * and a user write R/W in both; the supervisor writes anywhere, CR0.WP
 * being a 486 thing — and raises #PF. */
#define X86_CR0_PG  0x80000000u
#define X86_CR0_ET  0x00000010u   /* 486: reads as 1 */
#define X86_CR0_WP  0x00010000u   /* 486: supervisor writes honour read-only pages */
#define X86_CR0_AM  0x00040000u   /* 486: EFLAGS.AC checks alignment at CPL 3 */
/* Pentium CR4. VME and PVI (bits 0, 1) are not implemented, so CPUID does
 * not claim VME and loading them is #GP, as any reserved bit is. */
#define X86_CR4_TSD 0x00000004u   /* RDTSC is CPL 0 only */
#define X86_CR4_DE  0x00000008u   /* DR4/DR5 are #UD instead of DR6/DR7 */
#define X86_CR4_PSE 0x00000010u   /* a PDE with PS (bit 7) maps 4 MB */
#define X86_CR4_MCE 0x00000040u   /* machine checks enabled (none ever happen) */
#define X86_CR4_P5  (X86_CR4_TSD | X86_CR4_DE | X86_CR4_PSE | X86_CR4_MCE)
#define X86_TLB_V   0x001u
#define X86_TLB_U   0x002u
#define X86_TLB_UW  0x004u
#define X86_TLB_D   0x008u
#define X86_TLB_MEM 0x010u                   /* the physical page is plain RAM (not past memory, not the VGA read window): translated code may use it */
#define X86_TLB_WMEM 0x020u                  /* ...or at least take a pure store there: plain RAM, or the VGA window, where the
                                              * store lands and the code bitmap hands it to the device (as unpaged flat code does) */
#define X86_PG_BAD  0xFFFFFFFFu              /* a probe's miss: reads as open bus */
uint32_t x86_page_walk(struct x86_cpu *c, uint32_t lin, int write);
void     x86_tlb_flush(struct x86_cpu *c);
uint32_t x86_page_peek(struct x86_cpu *c, uint32_t lin, int user);   /* LIN's physical page, no side effects */
uint32_t x86_tlb_from_pgd(struct x86_cpu *c, uint32_t lin);          /* a low page's delta, into the TLB too */
int      x86_cpl(const struct x86_cpu *c);
/* 486: alignment checking is on (CR0.AM, EFLAGS.AC, CPL 3). Translated
 * code does not check, so the run loop gives such code to the interpreter. */
static inline int x86_ac_live(const struct x86_cpu *c);

static inline uint32_t x86_lin(x86_cpu *c, uint32_t lin, int write) {
    if (!(c->cr0 & X86_CR0_PG)) return lin;
    /* Low pages answer from the translator's tables first, so the
     * interpreter and translated V86 code share one cache state there (a
     * -V shadow walking a page the JIT still holds would set an accessed
     * bit the JIT run never set). An entry there allows any access of its
     * kind: present and user-readable, or user-writable and dirty. */
    struct x86_tlbe *t = &c->tlb[(lin >> 12) & 255];
    if ((t->tag & 0xFFFFF000u) == (lin & 0xFFFFF000u) && (t->tag & X86_TLB_V)) {
        uint32_t need = write ? X86_TLB_D : 0;
        if (!c->pg_super && x86_cpl(c) == 3) need |= write ? X86_TLB_UW : X86_TLB_U;
        if ((t->tag & need) == need) return t->phys | (lin & 0xFFF);
    }
    if ((lin >> 12) < X86_PGD_PAGES) {
        int64_t d = write ? c->pgd_w[lin >> 12] : c->pgd_r[lin >> 12];
        if (!(d & 1)) return x86_tlb_from_pgd(c, lin);
    }
    return x86_page_walk(c, lin, write);
}

static inline int x86_ac_live(const x86_cpu *c) {
    return (c->cr0 & X86_CR0_AM) && (c->eflags & X86_AC) && c->model >= X86_MODEL_486 && x86_cpl(c) == 3;
}
static inline uint8_t x86_phys_rd8(x86_cpu *c, uint32_t lin) {
    uint32_t p = x86_lin(c, lin, 0);
    if (p == X86_PG_BAD) return 0xFF;
    p &= c->a20_mask;
    if (c->device_read && p - 0xA0000u < 0x10000u) return c->device_read(c, p);
    return p < c->mem_size ? c->mem[p] : 0xFF;
}
static inline void x86_phys_wr8(x86_cpu *c, uint32_t lin, uint8_t v) {
    uint32_t p = x86_lin(c, lin, 1);
    if (p == X86_PG_BAD) return;
    p &= c->a20_mask;
    if (p < c->mem_size) {
        c->mem[p] = v;
        if (c->code_bitmap[p]) x86_store_hook(c, p);
    }
}

/* Implicit supervisor accesses — descriptor tables, the TSS, a frame on an
 * inner stack — are supervisor to the paging unit whatever the CPL. */
static inline uint32_t x86_rd(x86_cpu *c, uint32_t base, uint32_t off, uint32_t offmask, int size);
static inline void x86_wr(x86_cpu *c, uint32_t base, uint32_t off, uint32_t offmask, int size, uint32_t v);
static inline uint32_t x86_sup_rd(x86_cpu *c, uint32_t base, uint32_t off, int size) {
    uint8_t s = c->pg_super; c->pg_super = 1;
    uint32_t v = x86_rd(c, base, off, 0xFFFFFFFFu, size);
    c->pg_super = s;
    return v;
}
static inline void x86_sup_wr(x86_cpu *c, uint32_t base, uint32_t off, int size, uint32_t v) {
    uint8_t s = c->pg_super; c->pg_super = 1;
    x86_wr(c, base, off, 0xFFFFFFFFu, size, v);
    c->pg_super = s;
}

/* Segment-relative access with in-segment offset wrap. offmask is
 * 0xFFFF for 16-bit addressing and 0xFFFFFFFF for 32-bit; a word that
 * straddles the wrap point really does touch base+FFFF and base+0000. */
#define X86_MEM_SLACK 0x10000u      /* (as below: readable slack past the end of memory) */
/* A device's register, when the access lands on one (x86_cpu.mmio_read):
 * within one page, translated once, above memory. 1 if it did. */
static inline int x86_mmio_rd(x86_cpu *c, uint32_t lin, int size, uint32_t *v) {
    if ((lin & 0xFFF) + (uint32_t)size > 0x1000) return 0;
    uint32_t p = x86_lin(c, lin, 0);
    if (p == X86_PG_BAD || p < c->mem_size + X86_MEM_SLACK) return 0;
    if (!c->mmio_read(c, p, size, v)) *v = 0xFFFFFFFFu;
    if (size < 4) *v &= (1u << (8 * size)) - 1;
    return 1;
}
static inline int x86_mmio_wr(x86_cpu *c, uint32_t lin, int size, uint32_t v) {
    if ((lin & 0xFFF) + (uint32_t)size > 0x1000) return 0;
    uint32_t p = x86_lin(c, lin, 1);
    if (p == X86_PG_BAD || p < c->mem_size + X86_MEM_SLACK) return 0;
    c->mmio_write(c, p, size, v);
    return 1;
}
static inline uint32_t x86_rd(x86_cpu *c, uint32_t base, uint32_t off, uint32_t offmask, int size) {
    uint32_t v = 0;
    if (__builtin_expect(c->mmio_read != NULL, 0) && x86_mmio_rd(c, base + (off & offmask), size, &v)) return v;
    for (int i = 0; i < size; i++)
        v |= (uint32_t)x86_phys_rd8(c, base + ((off + i) & offmask)) << (8 * i);
    return v;
}
static inline void x86_wr(x86_cpu *c, uint32_t base, uint32_t off, uint32_t offmask, int size, uint32_t v) {
    if (__builtin_expect(c->mmio_write != NULL, 0) && x86_mmio_wr(c, base + (off & offmask), size, v)) return;
    for (int i = 0; i < size; i++)
        x86_phys_wr8(c, base + ((off + i) & offmask), (uint8_t)(v >> (8 * i)));
}

/* Guest memory geometry. Low memory is 1 MB + the 64 KB HMA — everything
 * real mode can address, and the range the block cache is indexed on.
 * Extended memory sits above it, contiguous, for the DPMI host to hand
 * out; with no paging (CLAUDE.md) linear == physical, so a client's flat
 * selector is just an offset into this buffer. X86_MEM_SLACK is readable
 * slack past the end so a decode or a straddling access at the top never
 * faults.
 *
 * The size is the run's (cpu->mem_size, -mem N: N MB in all): at least
 * X86_MEM_SIZE — 16 MB of extended memory, what the translators' flat
 * fast paths assume is always there — and at most X86_MEM_MAX, which
 * the bitmap's fixed distance and the translator's tables are built for.
 * x86_set_mem_size before x86_init chooses. */
#define X86_LOW_SIZE  0x110000u
#define X86_EXT_SIZE  0x1000000u                     /* the default and least: 16 MB */
#define X86_MEM_SIZE  (X86_LOW_SIZE + X86_EXT_SIZE)
#define X86_MEM_MAX   (0x10000000u + 0x100000u)      /* 257 MB: 256 MB of extended memory, and the first 1 MB */
#define X86_MEM_SLACK 0x10000u
/* The code bitmap sits at this fixed distance from guest memory when the
 * mirrored layout is in use (x86_mem.c: one reservation holds both), so
 * translated code reaches a store's bitmap byte as [host_addr + delta]
 * with no register; disp32-reachable, and clear of the memory's span.
 *
 * On POSIX hosts it lies BELOW the memory, and above the memory the
 * reservation runs on to X86_FLAT_SPAN: 64 KB of read-only slack filled
 * with FFh (the interpreter's open bus past mem_size), then no access at
 * all, so any 32-bit flat offset lands either in memory, in the slack,
 * or on a page that faults. The x86-64 backend's flat accesses rely on
 * that fault instead of a range check (dbt_x64.c, fastmem). Windows keeps
 * the bitmap above the memory and the explicit checks. */
#if defined(_WIN32)
#define X86_BM_DELTA  ((int64_t)0x20000000)
#else
#define X86_BM_DELTA  (-(int64_t)0x20000000)
#endif
#define X86_FLAT_SPAN (0x100000000ull + 0x20000u)     /* memory + slack + guard: past every 32-bit offset and a straddle */
_Static_assert((X86_BM_DELTA < 0 ? -X86_BM_DELTA : X86_BM_DELTA) >= (int64_t)X86_MEM_MAX + X86_MEM_SLACK,
               "the bitmap must lie clear of guest memory");

void x86_set_mem_size(uint32_t bytes);          /* the next x86_init's memory, X86_MEM_SIZE..X86_MEM_MAX */
int  x86_mem_alloc(x86_cpu *c);
void x86_mem_free(x86_cpu *c);
int  x86_set_a20(x86_cpu *c, int on);

/* Exceptions (vector numbers) */
enum {
    X86_EXC_DE = 0, X86_EXC_DB = 1, X86_EXC_BP = 3, X86_EXC_OF = 4,
    X86_EXC_BR = 5, X86_EXC_UD = 6, X86_EXC_NM = 7, X86_EXC_DF = 8,
    X86_EXC_TS = 10, X86_EXC_NP = 11, X86_EXC_SS = 12, X86_EXC_GP = 13,
    X86_EXC_PF = 14, X86_EXC_AC = 17,
};

/* ============================================================================
 * Public API (core/x86_state.c, core/x86_interp.c)
 * ========================================================================= */
void x86_init(x86_cpu *c, int model);
void x86_free(x86_cpu *c);
void x86_reset(x86_cpu *c);
void x86_load_seg(x86_cpu *c, int s, uint16_t sel);
int  x86_cpl(const x86_cpu *c);
void x86_pe_set(x86_cpu *c);                 /* CR0.PE 0 -> 1 from real mode */
void x86_real_limits(x86_cpu *c);            /* all segment limits back to 64K */
int  x86_read_desc(x86_cpu *c, uint16_t sel, uint32_t *lo, uint32_t *hi);
void x86_unpack_desc(x86_seg *g, uint16_t sel, uint32_t lo, uint32_t hi);
void x86_set_accessed(x86_cpu *c, uint16_t sel, uint32_t hi);   /* real mode: base = sel<<4 */
void x86_dump(x86_cpu *c, FILE *f);

/* Execute one instruction. Returns 0 on success, 1 if halted, or a
 * negative code when the instruction could not be executed. */
int  x86_step(x86_cpu *c);

/* Run an already-decoded instruction's semantics with c->eip pointing
 * past it — the DBT's generic slow path. Does not count the instruction
 * or dispatch exceptions; the caller has excluded anything that raises. */
struct x86_insn;
void x86_exec_decoded(x86_cpu *c, const struct x86_insn *in);
void x86_deliver_exception(x86_cpu *c);   /* cpu->exc pending after a JIT run */

/* Deliver interrupt/exception vector n (pushes flags/CS/IP, loads vector). */
void x86_interrupt(x86_cpu *c, int vector, int is_sw);

/* Raise a fault from inside an instruction: records it and, when a step
 * is armed, abandons the instruction (longjmp). Never returns if armed. */
void x86_fault(x86_cpu *c, int vector, uint32_t err);
int  x86_io_permitted(x86_cpu *c, uint32_t port, int size);   /* the I/O permission check, without raising */

/* Flags after POPF/IRET are model dependent: 8086 forces 12-15 set,
 * 286 real mode forces them clear, 386 allows IOPL/NT. */
uint32_t x86_flags_fixup(x86_cpu *c, uint32_t f);

#endif /* X86_H */
