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
 * and writes every present page; CR0.WP, which makes it honour R/W, came
 * with the 486. A fault records the linear address in CR2 and raises #PF
 * with an error code: bit 0 a protection violation (else not present),
 * bit 1 a write, bit 2 a user access.
 *
 * Outside an instruction — a BIOS service reading guest memory, or the
 * prefetch probing bytes it may not use — nothing is raised: the access
 * reads as open bus (X86_PG_BAD) and a probe notes the miss.
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

void x86_tlb_flush(x86_cpu *c) {
    memset(c->tlb, 0, sizeof c->tlb);
}

uint32_t x86_page_walk(x86_cpu *c, uint32_t lin, int write) {
    int user = !c->pg_super && x86_cpl(c) == 3;
    uint32_t err = (write ? 2u : 0u) | (user ? 4u : 0u);
    uint32_t pde_at = (c->cr3 & 0xFFFFF000u) | ((lin >> 20) & 0xFFCu);
    uint32_t pde = phys_rd32(c, pde_at);
    uint32_t pte_at = 0, pte = 0;
    int ok = (pde & 1) != 0;
    if (ok) {
        pte_at = (pde & 0xFFFFF000u) | ((lin >> 10) & 0xFFCu);
        pte = phys_rd32(c, pte_at);
        ok = (pte & 1) != 0;
    }
    if (ok && user) {
        uint32_t both = pde & pte;
        if (!(both & 4) || (write && !(both & 2))) { ok = 0; err |= 1; }
    }
    if (!ok) {
        if (c->pg_probe) { c->pg_miss = 1; return X86_PG_BAD; }
        if (!c->fault_armed) return X86_PG_BAD;
        c->cr2 = lin;
        x86_fault(c, X86_EXC_PF, err);
        return X86_PG_BAD;
    }
    phys_or8(c, pde_at, 0x20);                            /* accessed */
    phys_or8(c, pte_at, write ? 0x60 : 0x20);             /* accessed, dirty on a write */
    if (write) pte |= 0x40;
    struct x86_tlbe *t = &c->tlb[(lin >> 12) & 255];
    uint32_t both = pde & pte;
    t->tag = (lin & 0xFFFFF000u) | X86_TLB_V
           | ((both & 4) ? X86_TLB_U : 0)
           | ((both & 4) && (both & 2) ? X86_TLB_UW : 0)
           | ((pte & 0x40) ? X86_TLB_D : 0);
    t->phys = pte & 0xFFFFF000u;
    return t->phys | (lin & 0xFFF);
}
