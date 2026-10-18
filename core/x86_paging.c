/* x86_paging.c — the 386's paging unit: two-level 4 KB page tables
 *
 * x86_lin (x86.h) answers from the TLB when it can; everything else comes
 * here. The walk reads the page directory entry at CR3 + (lin >> 22) * 4
 * and the page table entry it names, both physical (the A20 gate applies,
 * as it does to every physical address), checks the access against them,
 * sets the accessed bits (and dirty on a write) the way the processor
 * does — in memory, so the guest's page tables see them — and caches the
 * result.
 *
 * The 386's protection rules: a user access (CPL 3, not an implicit
 * supervisor access such as a descriptor-table read) needs U/S set at
 * both levels, and a user write needs R/W at both. The supervisor reads
 * and writes every present page — unless CR0.WP is set, on a 486, which
 * makes a supervisor write honour R/W too. A cached translation says
 * what a write may skip the walk for only through its D bit, so while WP
 * is set a read-only page's entry never gets D: the interpreter and
 * translated code both miss on a write there and come back here. A fault records the linear address in CR2 and raises #PF
 * with an error code: bit 0 a protection violation (else not present),
 * bit 1 a write, bit 2 a user access.
 *
 * Outside an instruction — a BIOS service reading guest memory, or the
 * prefetch probing bytes it may not use — nothing is raised: the access
 * reads as open bus (X86_PG_BAD) and a probe notes the miss. A probe has
 * no side effects at all; the fetch then touches, for real, the pages
 * the instruction actually occupies (x86_step), as translated code does
 * for its block's page (dbt_translate_block).
 */
#include "x86.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void x86_fault(x86_cpu *c, int vector, uint32_t err);

/* A page-table entry: one aligned load, and with other processors about
 * (x86_smp) an acquire — byte by byte, an entry another processor is
 * writing reads half old, half new. */
static uint32_t phys_rd32(x86_cpu *c, uint32_t p) {
    p &= c->a20_mask & ~3u;
    if (p + 3 >= c->mem_size) return 0xFFFFFFFFu;
    if (x86_smp) return __atomic_load_n((uint32_t *)(void *)&c->mem[p], __ATOMIC_ACQUIRE);
    return (uint32_t)c->mem[p] | (uint32_t)c->mem[p + 1] << 8 | (uint32_t)c->mem[p + 2] << 16 | (uint32_t)c->mem[p + 3] << 24;
}

/* Accessed and dirty bits: the processor sets them with a locked
 * read-modify-write, so an entry another processor is replacing keeps
 * its new value (a plain OR here put the old low byte back over it). */
static void phys_or8(x86_cpu *c, uint32_t p, uint8_t bits) {
    p &= c->a20_mask;
    if (p >= c->mem_size || (c->mem[p] & bits) == bits) return;
    if (x86_smp) __atomic_fetch_or(&c->mem[p], bits, __ATOMIC_SEQ_CST);
    else c->mem[p] |= bits;
    if (c->code_bitmap[p]) x86_store_hook(c, p);
}

/* The page tables' answer for a user access to LIN, without the side
 * effects of a walk (no accessed bits, no TLB fill, no fault): the
 * physical page address with A20 applied, or X86_PG_BAD when a user
 * could not read it. For the translator, deciding whether a code page is
 * mapped one-to-one. */
/* Pentium CR4.PSE: a PDE with PS set maps a 4 MB page itself — no page
 * table; its U/S, R/W, accessed and dirty bits are the PDE's. Without
 * PSE (and before the Pentium) the bit is ignored. */
static inline int pse_big(const x86_cpu *c, uint32_t pde) {
    return (pde & 0x80) && (c->cr4 & X86_CR4_PSE);
}

/* ... and whether a walk would still change the tables: *accessed says
 * both levels carry their accessed bit. A fetch of a cached block on a
 * page that lacks one must walk once (dbt_page_revive), as the
 * interpreter's fetch would, or the guest's page tables — and -V — see
 * a difference. */
uint32_t x86_page_peek_acc(x86_cpu *c, uint32_t lin, int user, int *accessed) {
    uint32_t need = user ? 5u : 1u;
    uint32_t pde = phys_rd32(c, (c->cr3 & 0xFFFFF000u) | ((lin >> 20) & 0xFFCu));
    *accessed = 0;
    if ((pde & need) != need) return X86_PG_BAD;
    if (pse_big(c, pde)) { *accessed = (pde & 0x20) != 0; return ((pde & 0xFFC00000u) | (lin & 0x003FF000u)) & c->a20_mask; }
    uint32_t pte = phys_rd32(c, (pde & 0xFFFFF000u) | ((lin >> 10) & 0xFFCu));
    if ((pte & need) != need) return X86_PG_BAD;
    *accessed = (pde & pte & 0x20) != 0;
    return (pte & 0xFFFFF000u) & c->a20_mask;
}
uint32_t x86_page_peek(x86_cpu *c, uint32_t lin, int user) {
    int acc;
    return x86_page_peek_acc(c, lin, user, &acc);
}

/* Is a physical page one translated code may address straight? */
static int plain_ram(const x86_cpu *c, uint32_t phys) {
    phys &= c->a20_mask;
    return phys + 0xFFFu < c->mem_size && (phys & 0xFFFF0000u) != 0xA0000u;
}

/* Can translated code store to a physical page straight? Plain RAM, and
 * also the VGA window: the byte lands in memory and the code bitmap's
 * device bit hands it to the VGA — only a read there must be the
 * device's (the latches). DOOM under WIN386's DPMI draws through paging. */
int x86_nwatch = -1;
uint32_t x86_watch_phys[8];
static uint32_t watch_seq;
static uint32_t watch_pages[8]; static int nwatch_pages;
static void watch_init(void) {
    x86_nwatch = 0;
    const char *e = getenv("X86_WATCH_PHYS");
    while (e && *e && x86_nwatch < 8) {
        char *end; uint32_t v = (uint32_t)strtoul(e, &end, 16);
        if (end == e) break;
        x86_watch_phys[x86_nwatch++] = v & ~3u;
        e = *end ? end + 1 : end;
    }
    /* X86_WATCH_PAGE=hex[,hex...]: whole pages — every store and atomic
     * to them, every load of them (consecutive repeats of one load, a
     * spin, are counted, not printed) */
    e = getenv("X86_WATCH_PAGE");
    while (e && *e && nwatch_pages < 8) {
        char *end; uint32_t v = (uint32_t)strtoul(e, &end, 16);
        if (end == e) break;
        watch_pages[nwatch_pages++] = v & 0xFFFFF000u;
        e = *end ? end + 1 : end;
    }
    if (nwatch_pages && !x86_nwatch) x86_nwatch = 1, x86_watch_phys[0] = 0xFFFFFFF0u;   /* (a dword no access reaches) */
}
static int watch_hit(uint32_t phys, int size) {
    for (int i = 0; i < x86_nwatch; i++)
        if (phys < x86_watch_phys[i] + 4 && phys + (uint32_t)size > x86_watch_phys[i]) return 1;
    for (int i = 0; i < nwatch_pages; i++) if ((phys & 0xFFFFF000u) == watch_pages[i]) return 1;
    return 0;
}
int x86_watch_page(uint32_t phys) {
    if (x86_nwatch < 0) watch_init();
    for (int i = 0; i < x86_nwatch; i++) if ((x86_watch_phys[i] & 0xFFFFF000u) == (phys & 0xFFFFF000u)) return 1;
    for (int i = 0; i < nwatch_pages; i++) if ((phys & 0xFFFFF000u) == watch_pages[i]) return 1;
    return 0;
}
/* phys 0xFFFFFFFF: translate lin (without faulting) first. */
int x86_locked_fallback(void);
void x86_watch_store(x86_cpu *c, uint32_t lin, uint32_t phys, int size, uint32_t v) {
    if (x86_nwatch <= 0) return;
    if (phys == 0xFFFFFFFFu) {
        int probe = c->pg_probe; c->pg_probe = 1;
        phys = x86_lin(c, lin, 0);
        c->pg_probe = probe;
        if (phys == X86_PG_BAD) return;
    }
    int atomic = size & 0x100; size &= 0xFF;                 /* bit 8: a host-atomic RMW (the value is what is there after) */
    if (watch_hit(phys, size))
        fprintf(stderr, "[pw %6u] cpu%d phys %08X <- %0*X (%d bytes at %08X%s) at lin %08X efl %08X eax %08X\n",
                __atomic_fetch_add(&watch_seq, 1, __ATOMIC_SEQ_CST), c->cpu_id, phys & ~3u,
                size * 2, v, size, phys, atomic ? ", atomic" : x86_locked_fallback() ? ", LOCKED FALLBACK" : "",
                c->seg[S_CS].base + c->eip, c->eflags, c->r[R_AX]);
}
void x86_watch_load(x86_cpu *c, uint32_t lin, int size, uint32_t v) {
    if (x86_nwatch <= 0) return;
    int probe = c->pg_probe; c->pg_probe = 1;
    uint32_t phys = x86_lin(c, lin, 0);
    c->pg_probe = probe;
    if (phys == X86_PG_BAD) return;
    if (!watch_hit(phys, size)) return;
    static struct { uint32_t phys, v, eip, n; } last[8];     /* per processor: the load before, repeated n times */
    int id = c->cpu_id & 7;
    uint32_t eip = c->seg[S_CS].base + c->eip;
    if (last[id].n && last[id].phys == phys && last[id].v == v && last[id].eip == eip) { last[id].n++; return; }
    if (last[id].n > 1) fprintf(stderr, "[pr      ] cpu%d   ... the load before, %u times\n", id, last[id].n);
    last[id].phys = phys; last[id].v = v; last[id].eip = eip; last[id].n = 1;
    fprintf(stderr, "[pr %6u] cpu%d phys %08X -> %0*X (%d bytes at %08X) at lin %08X\n",
            __atomic_fetch_add(&watch_seq, 1, __ATOMIC_SEQ_CST), c->cpu_id, phys & ~3u,
            size * 2, v, size, phys, eip);
}

static uint32_t tlb_mem_bits(const x86_cpu *c, uint32_t phys) {
    uint32_t p = phys & c->a20_mask;
    if (__builtin_expect(x86_nwatch != 0, 0) && x86_watch_page(phys)) return 0;   /* watched: every access through the interpreter */
    if (plain_ram(c, phys)) return X86_TLB_MEM | X86_TLB_WMEM;
    return p + 0xFFFu < c->mem_size ? X86_TLB_WMEM : 0;
}

/* A low page answered from the translator's delta tables (x86_lin) goes
 * into the TLB as well, which is where translated protected-mode code
 * looks: the same translation, the permissions the tables imply. */
uint32_t x86_tlb_from_pgd(x86_cpu *c, uint32_t lin) {
    uint32_t page = lin >> 12;
    uint32_t phys = (uint32_t)((int64_t)(lin & 0xFFFFF000u) + c->pgd_r[page]);
    struct x86_tlbe *t = &c->tlb[page & 255];
    t->tag = (lin & 0xFFFFF000u) | X86_TLB_V | X86_TLB_U
           | (!(c->pgd_w[page] & 1) ? X86_TLB_UW | X86_TLB_D : 0)
           | tlb_mem_bits(c, phys);
    t->phys = phys;
    return phys | (lin & 0xFFF);
}

void x86_tlb_flush(x86_cpu *c) {
    memset(c->tlb, 0, sizeof c->tlb);
    for (int i = 0; i < X86_PGD_PAGES; i++) c->pgd_r[i] = c->pgd_w[i] = X86_PGD_NONE;
    if (c->tlb_hook) c->tlb_hook(c);
}
/* A CR3 load: with CR4.PGE (the Pentium Pro's) the entries a global PTE
 * or 4 MB PDE made stay — a kernel's own pages, the same in every address
 * space, no longer refilled after every process switch. The low-page
 * delta tables are user pages: they go. */
void x86_tlb_flush_cr3(x86_cpu *c) {
    if (!(c->cr4 & X86_CR4_PGE)) { x86_tlb_flush(c); return; }
    for (int i = 0; i < 256; i++) if (!(c->tlb[i].tag & X86_TLB_G)) c->tlb[i].tag = 0;
    for (int i = 0; i < X86_PGD_PAGES; i++) c->pgd_r[i] = c->pgd_w[i] = X86_PGD_NONE;
    if (c->tlb_hook) c->tlb_hook(c);
}
/* INVLPG: the page's entry, global or not (once the whole TLB: every
 * later access re-walked, which software could not tell apart — until
 * global pages, which a whole flush would throw away at every shootdown) */
void x86_tlb_flush_page(x86_cpu *c, uint32_t lin) {
    struct x86_tlbe *t = &c->tlb[(lin >> 12) & 255];
    if (!((t->tag ^ lin) & 0xFFFFF000u)) t->tag = 0;
    uint32_t page = lin >> 12;
    if (page < X86_PGD_PAGES) c->pgd_r[page] = c->pgd_w[page] = X86_PGD_NONE;
    if (c->tlb_hook) c->tlb_hook(c);
}

/* 486 WP: a read-only page (R/W clear at either level) takes no write
 * through a cached translation, supervisor or not. */
static int wp_ro(const x86_cpu *c, uint32_t both) {
    return (c->cr0 & X86_CR0_WP) && c->model >= X86_MODEL_486 && !(both & 2);
}

/* SMP: set accessed/dirty BITS in the entry at P, which read EXPECT, as
 * the processor does — a locked update of an entry still present and
 * unchanged. Another processor may have rewritten it since the walk read
 * it (NT's memory manager trims a working set, turning a PTE into a
 * transition entry, whose bits 5 and 6 are part of its protection): a
 * blind OR there corrupts the software entry. 0: it changed, walk again. */
static int pte_mark(x86_cpu *c, uint32_t p, uint32_t expect, uint32_t bits) {
    p &= c->a20_mask & ~3u;
    if (p + 3 >= c->mem_size || (expect & bits) == bits) return 1;
    uint32_t *w = (uint32_t *)(void *)&c->mem[p];
    if (!__atomic_compare_exchange_n(w, &expect, expect | bits, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return 0;
    if (c->code_bitmap[p]) x86_store_hook(c, p);
    return 1;
}

uint32_t x86_page_walk(x86_cpu *c, uint32_t lin, int write) {
  again:;
    int user = !c->pg_super && x86_cpl(c) == 3;
    uint32_t err = (write ? 2u : 0u) | (user ? 4u : 0u);
    uint32_t pde_at = (c->cr3 & 0xFFFFF000u) | ((lin >> 20) & 0xFFCu);
    uint32_t pde = phys_rd32(c, pde_at);
    uint32_t pte_at = 0, pte = 0;
    int ok = (pde & 1) != 0;
    int big = ok && pse_big(c, pde);
    if (big) {
        /* the PDE is the whole translation: it stands in for the PTE below
         * (both = pde & pte = pde; its A and D bits are the ones set) */
        pte_at = pde_at;
        pte = (pde & 0xFFC00000u) | (lin & 0x003FF000u) | (pde & 0xFFFu);
    } else if (ok) {
        pte_at = (pde & 0xFFFFF000u) | ((lin >> 10) & 0xFFCu);
        pte = phys_rd32(c, pte_at);
        ok = (pte & 1) != 0;
    }
    if (ok && user) {
        uint32_t both = pde & pte;
        if (!(both & 4) || (write && !(both & 2))) { ok = 0; err |= 1; }
    } else if (ok && write && (c->cr0 & X86_CR0_WP) && c->model >= X86_MODEL_486 && !(pde & pte & 2)) {
        ok = 0; err |= 1;                                 /* 486 WP: a supervisor write to a read-only page */
    }
    if (!ok) {
        if (c->pg_probe) { c->pg_miss = 1; return X86_PG_BAD; }
        if (!c->fault_armed) return X86_PG_BAD;
        c->cr2 = lin;
        x86_fault(c, X86_EXC_PF, err);
        return X86_PG_BAD;
    }
    /* A probe (the prefetch) looks without touching: the accessed bits
     * and the caches are the business of the access that uses the byte. */
    if (c->pg_probe) return ((pte & 0xFFFFF000u) | (lin & 0xFFF));
    if (x86_smp) {
        if (big) { if (!pte_mark(c, pde_at, pde, write ? 0x60 : 0x20)) goto again; }
        else {
            if (!pte_mark(c, pde_at, pde, 0x20)) goto again;
            if (!pte_mark(c, pte_at, pte, write ? 0x60 : 0x20)) goto again;
        }
    } else {
        phys_or8(c, pde_at, 0x20);                        /* accessed */
        phys_or8(c, pte_at, write ? 0x60 : 0x20);         /* accessed, dirty on a write */
    }
    if (write) pte |= 0x40;
    struct x86_tlbe *t = &c->tlb[(lin >> 12) & 255];
    uint32_t both = pde & pte;
    t->tag = (lin & 0xFFFFF000u) | X86_TLB_V
           | ((c->cr4 & X86_CR4_PGE) && (pte & 0x100) ? X86_TLB_G : 0)   /* (a 4 MB page's G is its PDE's, in pte here) */
           | ((both & 4) ? X86_TLB_U : 0)
           | ((both & 4) && (both & 2) ? X86_TLB_UW : 0)
           | ((pte & 0x40) && !(wp_ro(c, pde & pte)) ? X86_TLB_D : 0)
           | tlb_mem_bits(c, pte & 0xFFFFF000u);
    t->phys = pte & 0xFFFFF000u;
    /* the translator's tables, low linear pages only, user permissions */
    uint32_t page = lin >> 12;
    uint32_t phys = t->phys & c->a20_mask;
    if (page < X86_PGD_PAGES && (both & 4) && phys + 0xFFFu < c->mem_size) {
        int64_t d = (int64_t)phys - (int64_t)(lin & 0xFFFFF000u);
        c->pgd_r[page] = d;
        if ((both & 2) && (pte & 0x40)) c->pgd_w[page] = d;
    }
    return t->phys | (lin & 0xFFF);
}
