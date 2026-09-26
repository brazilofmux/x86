/* dbt_cache.c — direct-mapped translated-block cache, direct-link
 * registry, and span-gated SMC invalidation.
 *
 * Lifted from z80's dbt/block_cache.c (github.com/brazilofmux/z80). The cache is indexed on the guest
 * linear address modulo BLOCK_CACHE_SIZE: 1:1 over low memory, folded
 * over extended memory. The SMC sweep relies on the slot for linear p
 * holding, if anything of p's, the block starting at p, so its span says
 * exactly whether that block covers a stored byte — as long as the entry
 * is checked to really start at p and not at an address folding onto the
 * same slot. Two keys sharing a slot (another CS at the same linear
 * address, or another folded address) conflict and evict each other;
 * the eviction unlinks.
 */
#include "dbt.h"
#include <stdlib.h>
#include <string.h>

x86_block_entry *dbt_cache_lookup(x86_dbt *dbt, uint64_t key) {
    x86_block_entry *e = &dbt->aux->cache[dbt_slot(key)];
    if (e->key == key || e->key == (key | BLOCK_REFUSED_BIT)) {
        dbt->cache_hits++;
        return e;
    }
    dbt->cache_misses++;
    return NULL;
}

/* Drop the block in `slot` (if any): clear the entry and unlink every
 * site that branches straight to it. */
static void evict_slot(x86_dbt *dbt, uint32_t slot) {
    x86_block_entry *e = &dbt->aux->cache[slot];
    if (e->key == BLOCK_EMPTY_KEY) return;
    uint64_t old = e->key & ~BLOCK_REFUSED_BIT;
    e->key  = BLOCK_EMPTY_KEY;
    e->code = NULL;
    dbt_links_repatch(dbt, old, NULL);
}

void dbt_cache_insert(x86_dbt *dbt, uint64_t key, uint8_t *code) {
    uint32_t slot = dbt_slot(key);
    x86_block_entry *e = &dbt->aux->cache[slot];
    /* Another key in this slot: the old block's direct links would
     * otherwise keep running it after the probe stopped finding it. */
    if (e->key != BLOCK_EMPTY_KEY && (e->key & ~BLOCK_REFUSED_BIT) != key)
        evict_slot(dbt, slot);
    e->key  = code ? key : (key | BLOCK_REFUSED_BIT);
    /* Refusal is decided from the bytes at lin without tracking how
     * many were looked at — treat the sentinel as covering any store in
     * the window so it is always retried. */
    dbt->span[slot] = code ? dbt->last_block_bytes : 0xFFFFFFFFu;
    e->code = code;
    dbt_links_repatch(dbt, key, code);
}

/* NOTE: every caller also rewinds the code buffer (dbt_init; the
 * buffer-full path in dbt_translate_block). The link registry leans on
 * that: resetting the pool silently abandons all patch sites, sound
 * only because the code containing them is being discarded. */
void dbt_cache_invalidate_all(x86_dbt *dbt) {
    dbt->n_pcode = 0;
    memset(dbt->phys_alias, 0, sizeof dbt->phys_alias);
    memset(dbt->space_used, 0, sizeof dbt->space_used);   /* ids start over; the current space is 0 */
    dbt->space_next = 0;
    if (dbt->cpu && (dbt->cpu->cr0 & X86_CR0_PG)) dbt_space_current(dbt);
    /* An empty slot is all-ones in both words (its code pointer is never
     * read while the key says empty), and so is LINK_NONE: two memsets. */
    memset(dbt->aux->cache, 0xFF, sizeof dbt->aux->cache);
    memset(dbt->link_head, 0xFF, BLOCK_CACHE_SIZE * sizeof *dbt->link_head);
    dbt->link_used = 0;
    dbt->link_free = LINK_NONE;
    dbt_clear_code_bits(dbt->cpu);
    dbt->max_block_bytes = 0;
    if (dbt->insn_used > dbt->insn_high) dbt->insn_high = dbt->insn_used;
    dbt->insn_used = 0;
}

int dbt_link_record(x86_dbt *dbt, uint64_t key, uint32_t site_off) {
    uint32_t i, slot = dbt_slot(key);
    if (dbt->link_free != LINK_NONE) {
        i = dbt->link_free;
        dbt->link_free = dbt->link_pool[i].next;
    } else if (dbt->link_used < LINK_POOL_SIZE) {
        i = dbt->link_used++;
    } else {
        return 0;
    }
    dbt->link_pool[i].key      = key;
    dbt->link_pool[i].site_off = site_off;
    dbt->link_pool[i].next     = dbt->link_head[slot];
    dbt->link_head[slot] = i;
    dbt->links_created++;
    return 1;
}

/* Point every site that names `key` at `code`, or, with code == NULL
 * (the block is gone), unlink those sites and free their records. Sites
 * naming other keys that share the slot are left alone either way. */
void dbt_links_repatch(x86_dbt *dbt, uint64_t key, uint8_t *code) {
    uint32_t *pi = &dbt->link_head[dbt_slot(key)];
    if (*pi == LINK_NONE) return;
    dbt_jit_writable_begin();
    while (*pi != LINK_NONE) {
        x86_link *l = &dbt->link_pool[*pi];
        if (l->key != key) { pi = &l->next; continue; }
        dbt_arch_patch_link(dbt, l->site_off, code);
        if (code) {
            dbt->links_patched++;
            pi = &l->next;
        } else {
            /* Unlink AND free: an invalidated target's sites almost
             * always belong to blocks dying in the same SMC wave, and
             * their re-translations re-record fresh sites. */
            uint32_t i = *pi;
            *pi = l->next;
            l->next = dbt->link_free;
            dbt->link_free = i;
            dbt->links_unpatched++;
        }
    }
    dbt_jit_writable_end();
}

/* As dbt_mark_block_bytes, leaving out the [lo, hi) pairs in skip —
 * immediates the block reads from memory at run time. Nothing is ever
 * cleared here: a byte another translation baked in keeps its mark. */
void dbt_mark_block_bytes_except(x86_dbt *dbt, uint32_t start, uint32_t end, const uint32_t *skip, uint32_t nskip) {
    uint32_t bytes = end - start;
    if (bytes > dbt->max_block_bytes) dbt->max_block_bytes = bytes;
    dbt->last_block_bytes = bytes;
    uint8_t *bm = dbt->cpu->code_bitmap;
    for (uint32_t a = start; a < end; a++) {
        int skipped = 0;
        for (uint32_t k = 0; k < nskip; k++)
            if (a >= skip[2 * k] && a < skip[2 * k + 1]) { skipped = 1; break; }
        if (!skipped) bm[a] |= X86_BM_CODE;
    }
    if (start < dbt->bm_lo) dbt->bm_lo = start;
    if (end > dbt->bm_hi) dbt->bm_hi = end;
}

void dbt_mark_block_bytes(x86_dbt *dbt, uint32_t start, uint32_t end) {
    uint32_t bytes = end - start;
    if (bytes > dbt->max_block_bytes) dbt->max_block_bytes = bytes;
    dbt->last_block_bytes = bytes;
    uint8_t *bm = dbt->cpu->code_bitmap;    /* X86_MEM_SLACK past mem_size absorbs a block at the very top */
    for (uint32_t a = start; a < end; a++) bm[a] |= X86_BM_CODE;
    if (start < dbt->bm_lo) dbt->bm_lo = start;
    if (end > dbt->bm_hi) dbt->bm_hi = end;
}

/* A store landed on a byte some cached block covers. Invalidate every
 * block whose start lies in [phys - max_block_bytes + 1, phys] and
 * whose span reaches phys; clear the bitmap byte last, once no block
 * covers it (the rule paid for in blood: never leave a bitmap byte set
 * after its covering blocks are gone, never clear it before the sweep). */
/* Invalidate every block whose start lies in [lin - max_block_bytes + 1,
 * lin] and whose span reaches lin, under each mode variant. */
static void sweep_blocks_at(x86_dbt *dbt, uint32_t lin) {
    uint32_t window = dbt->max_block_bytes;
    for (uint32_t k = 0; k < window && k <= lin; k++) {
        uint32_t p = lin - k;
        for (int m = 0; m < KEY_MODE_VARIANTS; m++) {
            uint32_t slot = dbt_slot_mode(p, dbt_key_modes[m]);
            x86_block_entry *e = &dbt->aux->cache[slot];
            if (e->key == BLOCK_EMPTY_KEY || dbt_key_lin(e->key) != p || k >= dbt->span[slot]) continue;
            uint64_t old = e->key & ~BLOCK_REFUSED_BIT;
            e->key  = BLOCK_EMPTY_KEY;
            e->code = NULL;
            dbt_links_repatch(dbt, old, NULL);
            /* The block that is executing right now (a JIT store or a helper
             * op inside it): the thunk sees this flag and leaves the block
             * before its next, now stale, instruction. */
            if (p == dbt->cpu->jit_cur_lin) dbt->cpu->jit_cur_hit = 1;
        }
    }
}

/* A store landed on a byte some cached block covers. Sweep the blocks
 * keyed at it — and at its linear alias, when a remapped V86 code page
 * sits on it — then clear the bitmap byte, last, once no block covers it
 * (the rule paid for in blood: never leave a bitmap byte set after its
 * covering blocks are gone, never clear it before the sweep). */
static void invalidate_for_store(x86_dbt *dbt, uint32_t phys) {
    uint8_t now = dbt_smc_window(dbt->cpu);
    if (dbt->smc_win[phys] != now) { dbt->smc_win[phys] = now; dbt->smc_heat[phys] = 0; }
    if (dbt->smc_heat[phys] < 255) dbt->smc_heat[phys]++;
    sweep_blocks_at(dbt, phys);
    uint32_t alias = dbt->phys_alias[phys >> 12];   /* a linear page + 1: past 16 bits for NT's kernel at 80000000h */
    if (alias) sweep_blocks_at(dbt, ((uint32_t)(alias - 1) << 12) | (phys & 0xFFF));
    dbt->smc_invalidations++;
    dbt->cpu->code_bitmap[phys] &= (uint8_t)~X86_BM_CODE;   /* a device bit stays */
}

static void flush_under_running_code(x86_dbt *dbt) {
    dbt_cache_invalidate_all(dbt);
    dbt->flush_pending = 1;
    dbt->cpu->jit_cur_hit = 1;
}

/* A protected-mode key names CS by selector, and the translation bakes in
 * what the descriptor said then: base, limit, D bit. Watch the descriptor's
 * 8 bytes; a store to any of them (a DPMI set-base, a client writing its
 * LDT) flushes everything. Descriptors change rarely and never in a loop. */
void dbt_watch_cs_desc(x86_dbt *dbt) {
    x86_cpu *cpu = dbt->cpu;
    uint16_t sel = cpu->seg[S_CS].sel;
    uint32_t at = ((sel & 4) ? cpu->ldtr.base : cpu->gdtr.base) + (sel & 0xFFF8u);
    if (at >= cpu->mem_size - 8) return;
    for (uint32_t i = 0; i < 8; i++) cpu->code_bitmap[at + i] |= X86_BM_DESC;
    if (at < dbt->bm_lo) dbt->bm_lo = at;
    if (at + 8 > dbt->bm_hi) dbt->bm_hi = at + 8;
}

/* cpu->smc_hook: reached from x86_phys_wr8 (interpreter, helpers, host
 * services) whenever the bitmap byte is set. */
void dbt_smc_store(x86_cpu *cpu, uint32_t phys) {
    x86_dbt *dbt = (x86_dbt *)cpu->dbt;
    /* X86_SMC_TRACE=N: the first N stores that hit translated code, with
     * where the guest was — what a retranslation storm is made of */
    static int trace = -1;
    if (trace < 0) { const char *t = getenv("X86_SMC_TRACE"); trace = t ? atoi(t) : 0; }
    if (trace > 0) { trace--; fprintf(stderr, "[smc] store %06X from %04X:%08X\n", phys, cpu->seg[S_CS].sel, cpu->eip); }
    if (cpu->code_bitmap[phys] & X86_BM_DESC) {
        flush_under_running_code(dbt);     /* clears every CODE and DESC bit */
        dbt->desc_flushes++;
        return;
    }
    invalidate_for_store(dbt, phys);
}

/* Forget every translated byte and watched descriptor, keeping device
 * marks. Only the range ever marked is walked: the bitmap spans all of
 * memory, and writing it whole would fault in 17 MB per flush. */
void dbt_clear_code_bits(x86_cpu *cpu) {
    x86_dbt *dbt = (x86_dbt *)cpu->dbt;
    uint8_t *bm = cpu->code_bitmap;
    for (uint32_t i = dbt->bm_lo; i < dbt->bm_hi; i++) bm[i] &= (uint8_t)~(X86_BM_CODE | X86_BM_DESC);
    dbt->bm_lo = cpu->mem_size;
    dbt->bm_hi = 0;
}

/* cpu->a20_hook. A block's key says which A20 state it was translated in
 * (KEY_A20OFF: its far targets and wraps baked that mask), so a flip
 * needs no flush — the next lookup simply wants the other state's
 * blocks, and this state's stay for when the gate flips back. HIMEM's
 * memory test flips it thousands of times; flushing everything each time
 * was 10 of the 12 seconds of an MS-DOS boot. The block running now
 * leaves after the instruction (OUT 92h/60h, from a helper thunk). */
void dbt_a20_changed(x86_cpu *cpu, int on) {
    x86_dbt *dbt = (x86_dbt *)cpu->dbt;
    (void)on;
    if (!dbt) return;
    cpu->jit_cur_hit = 1;
    dbt->a20_flushes++;
}

/* cpu->dev_hook. Real-mode-shaped and segmented blocks check their reads
 * against the VGA window only when translated with a device answering
 * reads there (the planar VGA), so turning it on or off retranslates. */
void dbt_dev_changed(x86_cpu *cpu) {
    x86_dbt *dbt = (x86_dbt *)cpu->dbt;
    if (!dbt) return;
    dbt->wipe_dev++;
    flush_under_running_code(dbt);
}

/* Drop code page I's paged blocks (those of its address space) and the
 * entry's alias. The caller compacts the list. A page's blocks sit in
 * 4096 consecutive slots per key mode (its linear address XOR the folded
 * mode bits): V86, flat 32-bit PM, and segmented 16-bit PM. */
static void drop_code_page(x86_dbt *dbt, uint32_t i) {
    static const uint32_t modes[3] = { (uint32_t)(KEY_V86 >> KEY_MODE_SHIFT),
                                       (uint32_t)((KEY_PMODE | KEY_BIG | KEY_FLAT) >> KEY_MODE_SHIFT),
                                       (uint32_t)((KEY_PMODE | KEY_SEG16) >> KEY_MODE_SHIFT) };
    uint32_t lin = dbt->pcode[i].lin_page << 12, was = dbt->pcode[i].phys_page << 12;
    uint64_t space = (uint64_t)dbt->pcode[i].space << KEY_SPACE_SHIFT;
    if (dbt->phys_alias[was >> 12] == dbt->pcode[i].lin_page + 1) dbt->phys_alias[was >> 12] = 0;
    for (int m = 0; m < 3; m++)
        for (uint32_t k = 0; k < 4096; k++) {
            uint32_t slot = dbt_slot_mode(lin + k, modes[m]);
            x86_block_entry *e = &dbt->aux->cache[slot];
            if (e->key == BLOCK_EMPTY_KEY || !(e->key & KEY_PAGED) || dbt_key_lin(e->key) != lin + k
                || (e->key & KEY_SPACE_MASK) != space) continue;
            evict_slot(dbt, slot);
        }
    if ((dbt->cpu->jit_cur_lin & 0xFFFFF000u) == lin) dbt->cpu->jit_cur_hit = 1;
    dbt->tlb_page_drops++;
}

/* The space CR3 names, registered if new — the oldest one making room,
 * its blocks gone — and made cpu->pg_space. */
void dbt_space_current(x86_dbt *dbt) {
    x86_cpu *cpu = dbt->cpu;
    uint32_t cr3 = cpu->cr3 & 0xFFFFF000u;
    for (int i = 0; i < DBT_SPACES; i++)
        if (dbt->space_used[i] && dbt->space_cr3[i] == cr3) { cpu->pg_space = (uint8_t)i; return; }
    uint8_t id = dbt->space_next;
    dbt->space_next = (uint8_t)((id + 1) % DBT_SPACES);
    if (dbt->space_used[id]) {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < dbt->n_pcode; i++) {
            if (dbt->pcode[i].space == id) { drop_code_page(dbt, i); continue; }
            dbt->pcode[kept++] = dbt->pcode[i];
        }
        dbt->n_pcode = kept;
        dbt->space_evictions++;
    }
    dbt->space_used[id] = 1;
    dbt->space_cr3[id] = cr3;
    cpu->pg_space = id;
}

/* cpu->tlb_hook: the page tables may map differently now (CR3 load, PG
 * toggled, A20). A CR3 reload is how a memory manager flushes after any
 * remap (JEMM with NOINVLPG does it for every A20 emulation), and a VCPI
 * client switches spaces on every call down to DOS, so dropping
 * translations wholesale thrashes: DOOM under EMM386 retranslated itself
 * every time DOS/4GW reflected an interrupt. Only the code pages of the
 * space now current are re-peeked, and only those that moved lose their
 * blocks; another space's are checked when it is current again (its
 * tables may have changed meanwhile, but using them takes a CR3 load,
 * which comes back here). With paging off no paged block is reachable,
 * and nothing is dropped. */
void dbt_tlb_flushed(x86_cpu *cpu) {
    x86_dbt *dbt = (x86_dbt *)cpu->dbt;
    if (!dbt) return;
    dbt->tlb_flushes++;
    if (!(cpu->cr0 & X86_CR0_PG)) return;
    dbt_space_current(dbt);
    uint8_t sp = cpu->pg_space;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < dbt->n_pcode; i++) {
        if (dbt->pcode[i].space != sp
            || x86_page_peek(cpu, dbt->pcode[i].lin_page << 12, dbt->pcode[i].user) == dbt->pcode[i].phys_page << 12) {
            dbt->pcode[kept++] = dbt->pcode[i];
            continue;
        }
        drop_code_page(dbt, i);
    }
    dbt->n_pcode = kept;
}

/* A paged block is being translated on LIN_PAGE, which maps to PHYS_PAGE
 * (both page numbers) in the current space: note it for dbt_tlb_flushed.
 * 0 if the list is full — the caller then does not translate. */
int dbt_note_code_page(x86_dbt *dbt, uint32_t lin_page, uint32_t phys_page, int user) {
    uint8_t sp = dbt->cpu->pg_space;
    for (uint32_t i = 0; i < dbt->n_pcode; i++)
        if (dbt->pcode[i].lin_page == lin_page && dbt->pcode[i].space == sp) {
            dbt->pcode[i].phys_page = phys_page;
            dbt->pcode[i].user = (uint8_t)user;
            return 1;
        }
    if (dbt->n_pcode == DBT_PCODE_MAX) return 0;
    dbt->pcode[dbt->n_pcode].lin_page = lin_page;
    dbt->pcode[dbt->n_pcode].phys_page = phys_page;
    dbt->pcode[dbt->n_pcode].user = (uint8_t)user;
    dbt->pcode[dbt->n_pcode].space = sp;
    dbt->n_pcode++;
    return 1;
}

/* The host wrote guest memory directly (loader, DOS file reads): run
 * the same invalidation a guest store would have. */
void dbt_host_wrote(x86_cpu *cpu, uint32_t phys, uint32_t len) {
    if (!cpu->dbt) return;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t p = (phys + i) & cpu->a20_mask;
        if (p < cpu->mem_size && (cpu->code_bitmap[p] & X86_BM_CODE)) dbt_smc_store(cpu, p);
    }
}
