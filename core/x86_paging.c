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

void x86_fault(x86_cpu *c, int vector, uint32_t err);

static uint32_t phys_rd32(x86_cpu *c, uint32_t p) {
    p &= c->a20_mask & ~3u;
    if (p + 3 >= c->mem_size) return 0xFFFFFFFFu;
    return (uint32_t)c->mem[p] | (uint32_t)c->mem[p + 1] << 8 | (uint32_t)c->mem[p + 2] << 16 | (uint32_t)c->mem[p + 3] << 24;
}

static void phys_or8(x86_cpu *c, uint32_t p, uint8_t bits) {
    p &= c->a20_mask;
    if (p >= c->mem_size || (c->mem[p] & bits) == bits) return;
    c->mem[p] |= bits;
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

uint32_t x86_page_peek(x86_cpu *c, uint32_t lin, int user) {
    uint32_t need = user ? 5u : 1u;
    uint32_t pde = phys_rd32(c, (c->cr3 & 0xFFFFF000u) | ((lin >> 20) & 0xFFCu));
    if ((pde & need) != need) return X86_PG_BAD;
    if (pse_big(c, pde)) return ((pde & 0xFFC00000u) | (lin & 0x003FF000u)) & c->a20_mask;
    uint32_t pte = phys_rd32(c, (pde & 0xFFFFF000u) | ((lin >> 10) & 0xFFCu));
    if ((pte & need) != need) return X86_PG_BAD;
    return (pte & 0xFFFFF000u) & c->a20_mask;
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
static uint32_t tlb_mem_bits(const x86_cpu *c, uint32_t phys) {
    uint32_t p = phys & c->a20_mask;
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

/* 486 WP: a read-only page (R/W clear at either level) takes no write
 * through a cached translation, supervisor or not. */
static int wp_ro(const x86_cpu *c, uint32_t both) {
    return (c->cr0 & X86_CR0_WP) && c->model >= X86_MODEL_486 && !(both & 2);
}

uint32_t x86_page_walk(x86_cpu *c, uint32_t lin, int write) {
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
    phys_or8(c, pde_at, 0x20);                            /* accessed */
    phys_or8(c, pte_at, write ? 0x60 : 0x20);             /* accessed, dirty on a write */
    if (write) pte |= 0x40;
    struct x86_tlbe *t = &c->tlb[(lin >> 12) & 255];
    uint32_t both = pde & pte;
    t->tag = (lin & 0xFFFFF000u) | X86_TLB_V
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
