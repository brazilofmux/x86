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

static void page_record(x86_dbt *dbt, uint64_t key, uint8_t *code, uint32_t span);
static int32_t pcode_find(x86_dbt *dbt, uint32_t lin_page, uint8_t sp);
static void page_evict(x86_dbt *dbt, int32_t i, int park);

void dbt_cache_insert(x86_dbt *dbt, uint64_t key, uint8_t *code) {
    uint32_t slot = dbt_slot(key);
    x86_block_entry *e = &dbt->aux->cache[slot];
    if (code && e->key == key && e->code == code) return;   /* already in: a block unparked for this miss (dbt_page_revive) */
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
    if (code && (key & KEY_PAGED)) page_record(dbt, key, code, dbt->span[slot]);
}

/* NOTE: every caller also rewinds the code buffer (dbt_init; the
 * buffer-full path in dbt_translate_block). The link registry leans on
 * that: resetting the pool silently abandons all patch sites, sound
 * only because the code containing them is being discarded. */
static void pcode_reset(x86_dbt *dbt) {
    memset(dbt->pcode_hash, 0xFF, sizeof dbt->pcode_hash);
    memset(dbt->pcode_space, 0xFF, sizeof dbt->pcode_space);
    for (int32_t i = 0; i < DBT_PCODE_MAX; i++) dbt->pcode[i].snext = i + 1 < DBT_PCODE_MAX ? i + 1 : -1;
    dbt->pcode_free = 0;
    dbt->n_pcode = 0;
    dbt->pblk_used = 0;
    dbt->pblk_full = 0;
    memset(dbt->cr3s, 0, sizeof dbt->cr3s);
}

void dbt_cache_invalidate_all(x86_dbt *dbt) {
    pcode_reset(dbt);
    memset(dbt->phys_alias, 0, sizeof dbt->phys_alias);
    dbt->alias_used = 0;
    memset(dbt->space_used, 0, sizeof dbt->space_used);   /* ids start over */
    memset(dbt->space_kernel, 0, sizeof dbt->space_kernel);
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
    /* paged modes' blocks sit by address space too: each one in use */
    uint8_t spaces[DBT_SPACES]; int nsp = 0;
    for (int s = 0; s < DBT_SPACES; s++) if (s == 0 || dbt->space_used[s]) spaces[nsp++] = (uint8_t)s;
    for (uint32_t k = 0; k < window && k <= lin; k++) {
        uint32_t p = lin - k;
        for (int m = 0; m < KEY_MODE_VARIANTS; m++)
        for (int si = 0; si < (dbt_key_mode_paged(dbt_key_modes[m]) ? nsp : 1); si++) {
            uint32_t slot = dbt_slot_hash(p, dbt_key_modes[m], spaces[si]);
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
    for (uint32_t a = dbt->phys_alias[phys >> 12]; a; a = dbt->alias_pool[a - 1].next)
        sweep_blocks_at(dbt, (dbt->alias_pool[a - 1].lin_page << 12) | (phys & 0xFFF));
    /* A parked page on it — at its own address, or an alias — would bring
     * its blocks back unswept: they go */
    for (uint32_t a = dbt->phys_alias[phys >> 12], lp = phys >> 12;; lp = dbt->alias_pool[a - 1].lin_page, a = dbt->alias_pool[a - 1].next) {
        for (int s = 0; s < DBT_SPACES; s++) {
            if (!dbt->space_used[s]) continue;
            int32_t i = pcode_find(dbt, lp, (uint8_t)s);
            if (i >= 0 && dbt->pcode[i].parked) { page_evict(dbt, i, 0); dbt->pcode[i].parked = 0; }
        }
        if (!a) break;
    }
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

static uint32_t pcode_bucket(uint32_t lin_page, uint8_t space) {
    return ((lin_page * 2654435761u) ^ ((uint32_t)space * 0x9E3779B9u)) >> 18 & (DBT_PCODE_HASH - 1);
}
/* The entry for (space, linear page), or -1 */
static int32_t pcode_find(x86_dbt *dbt, uint32_t lin_page, uint8_t sp) {
    for (int32_t i = dbt->pcode_hash[pcode_bucket(lin_page, sp)]; i >= 0; i = dbt->pcode[i].hnext)
        if (dbt->pcode[i].lin_page == lin_page && dbt->pcode[i].space == sp) return i;
    return -1;
}
/* Entry I leaves the hash and goes back on the free list; the caller has
 * already unlinked it from its space's list. */
static void pcode_release(x86_dbt *dbt, int32_t i) {
    int32_t *pp = &dbt->pcode_hash[pcode_bucket(dbt->pcode[i].lin_page, dbt->pcode[i].space)];
    while (*pp != i) pp = &dbt->pcode[*pp].hnext;
    *pp = dbt->pcode[i].hnext;
    dbt->pcode[i].snext = dbt->pcode_free;
    dbt->pcode_free = i;
    dbt->n_pcode--;
}

enum { PB_LIVE, PB_PARKED, PB_DEAD };

/* A block just went into the cache: on its code page's list */
static void page_record(x86_dbt *dbt, uint64_t key, uint8_t *code, uint32_t span) {
    int32_t i = pcode_find(dbt, dbt_key_lin(key) >> 12, (uint8_t)((key & KEY_SPACE_MASK) >> KEY_SPACE_SHIFT));
    if (i < 0 || dbt->pblk_used == DBT_PBLK_MAX) { dbt->pblk_full = 1; return; }   /* untracked: scans from now on */
    uint32_t n = dbt->pblk_used++;
    dbt->pblk[n].key = key;
    dbt->pblk[n].code_off = (uint32_t)(code - dbt->code_buf);
    dbt->pblk[n].span = span;
    dbt->pblk[n].state = PB_LIVE;
    dbt->pblk[n].next = dbt->pcode[i].blocks;
    dbt->pcode[i].blocks = (int32_t)n;
}

/* Every block of LIN's page in space SP still in the cache, whatever its
 * record says: the slow way, for blocks that went unrecorded. A page's
 * blocks sit in 4096 consecutive slots per key mode: V86, flat 32-bit
 * PM, and segmented 16-bit PM. */
static void scan_evict_page(x86_dbt *dbt, uint32_t lin, uint8_t sp) {
    static const uint32_t modes[4] = { (uint32_t)(KEY_V86 >> KEY_MODE_SHIFT),
                                       (uint32_t)((KEY_PMODE | KEY_BIG | KEY_FLAT) >> KEY_MODE_SHIFT),
                                       (uint32_t)((KEY_PMODE | KEY_SEG16) >> KEY_MODE_SHIFT),
                                       (uint32_t)((KEY_PMODE | KEY_BIG | KEY_SEG16) >> KEY_MODE_SHIFT) };
    uint64_t space = (uint64_t)sp << KEY_SPACE_SHIFT;
    for (int m = 0; m < 4; m++)
        for (uint32_t k = 0; k < 4096; k++) {
            uint32_t slot = dbt_slot_hash(lin + k, modes[m], sp);
            x86_block_entry *e = &dbt->aux->cache[slot];
            if (e->key == BLOCK_EMPTY_KEY || !(e->key & KEY_PAGED) || dbt_key_lin(e->key) != lin + k
                || (e->key & KEY_SPACE_MASK) != space) continue;
            evict_slot(dbt, slot);
        }
}

/* Page I's blocks leave the cache: kept to come back (PARK: the live ones
 * become parked), or for good (all of them dead). Dead records unlink. */
static void page_evict(x86_dbt *dbt, int32_t i, int park) {
    int32_t *pp = &dbt->pcode[i].blocks;
    while (*pp >= 0) {
        uint32_t n = (uint32_t)*pp;
        if (dbt->pblk[n].state == PB_LIVE) {
            uint32_t slot = dbt_slot(dbt->pblk[n].key);
            x86_block_entry *e = &dbt->aux->cache[slot];
            if (e->key == dbt->pblk[n].key && e->code == dbt->code_buf + dbt->pblk[n].code_off) {
                evict_slot(dbt, slot);
                dbt->pblk[n].state = park ? PB_PARKED : PB_DEAD;
            } else dbt->pblk[n].state = PB_DEAD;
        } else if (!park) dbt->pblk[n].state = PB_DEAD;
        if (dbt->pblk[n].state == PB_DEAD) { *pp = dbt->pblk[n].next; continue; }
        pp = &dbt->pblk[n].next;
    }
    uint32_t lin = dbt->pcode[i].lin_page << 12;
    if (dbt->pblk_full) scan_evict_page(dbt, lin, dbt->pcode[i].space);
    if ((dbt->cpu->jit_cur_lin & 0xFFFFF000u) == lin) dbt->cpu->jit_cur_hit = 1;
}

/* Drop code page I's blocks, parked ones included; its alias stays listed
 * until the next wipe. The caller unlinks the entry (pcode_release) or
 * points it somewhere new. */
static void drop_code_page(x86_dbt *dbt, uint32_t i) {
    page_evict(dbt, (int32_t)i, 0);
    dbt->pcode[i].parked = 0;
    dbt->tlb_page_drops++;
}

/* Page I is not mapped in the page tables now current, or not the way
 * its blocks were translated: out of reach, kept for a CR3 that does map
 * it — or for this one, once the page is faulted in. */
static void park_page(x86_dbt *dbt, int32_t i) {
    if (dbt->pcode[i].parked) return;
    page_evict(dbt, i, 1);
    dbt->pcode[i].parked = 1;
    dbt->pages_parked++;
}

/* Page I maps as it did: its parked blocks back into the cache, their
 * link sites patched again (links into them were unrecorded when they
 * left: those sites reach them through the cache probe). */
static void unpark_page(x86_dbt *dbt, int32_t i) {
    if (!dbt->pcode[i].parked) return;
    int32_t *pp = &dbt->pcode[i].blocks;
    while (*pp >= 0) {
        uint32_t n = (uint32_t)*pp;
        if (dbt->pblk[n].state == PB_PARKED) {
            uint64_t key = dbt->pblk[n].key;
            uint32_t slot = dbt_slot(key);
            x86_block_entry *e = &dbt->aux->cache[slot];
            if (e->key != BLOCK_EMPTY_KEY && (e->key & ~BLOCK_REFUSED_BIT) == key) dbt->pblk[n].state = PB_DEAD;   /* translated again meanwhile */
            else {
                if (e->key != BLOCK_EMPTY_KEY) evict_slot(dbt, slot);
                uint8_t *code = dbt->code_buf + dbt->pblk[n].code_off;
                e->key = key;
                e->code = code;
                dbt->span[slot] = dbt->pblk[n].span;
                dbt_links_repatch(dbt, key, code);
                dbt->pblk[n].state = PB_LIVE;
                dbt->blocks_reinstated++;
            }
        }
        if (dbt->pblk[n].state == PB_DEAD) { *pp = dbt->pblk[n].next; continue; }
        pp = &dbt->pblk[n].next;
    }
    dbt->pcode[i].parked = 0;
    dbt->pages_unparked++;
}

/* Space ID's code pages and their blocks go, and the id is free. The
 * CR3s that used it choose again at their next load. */
static void space_free(x86_dbt *dbt, uint8_t id) {
    while (dbt->pcode_space[id] >= 0) {
        int32_t i = dbt->pcode_space[id];
        drop_code_page(dbt, (uint32_t)i);
        dbt->pcode_space[id] = dbt->pcode[i].snext;
        pcode_release(dbt, i);
    }
    for (int c = 0; c < DBT_CR3S; c++) {
        if (dbt->cr3s[c].kspace == id) dbt->cr3s[c].kspace = 0xFF;
        if (dbt->cr3s[c].uspace == id) dbt->cr3s[c].uspace = 0xFF;
    }
    dbt->space_used[id] = dbt->space_kernel[id] = 0;
    dbt->space_evictions++;
}

/* An id for a new space: the oldest makes room, but never KEEP */
static uint8_t space_alloc(x86_dbt *dbt, int keep) {
    uint8_t id;
    do { id = dbt->space_next; dbt->space_next = (uint8_t)((id + 1) % DBT_SPACES); } while (id == keep);
    if (dbt->space_used[id]) space_free(dbt, id);
    dbt->space_used[id] = 1;
    return id;
}

/* Does kernel space K fit the page tables now: does every page it has
 * code on map as it did? A page that maps nowhere means another layout
 * — a VCPI client's tables beside its server's — and K does not fit
 * (without DROP, neither does a remapped one). With DROP a page mapped
 * somewhere else has moved within this layout: its blocks go, K stays. */
static void park_page(x86_dbt *dbt, int32_t i);
static void unpark_page(x86_dbt *dbt, int32_t i);
static int kspace_fits(x86_dbt *dbt, uint8_t k, int drop) {
    int32_t *pp = &dbt->pcode_space[k];
    while (*pp >= 0) {
        int32_t i = *pp;
        int acc;
        uint32_t p = x86_page_peek_acc(dbt->cpu, dbt->pcode[i].lin_page << 12, 0, &acc);
        if (p == dbt->pcode[i].phys_page << 12) {
            /* mapped as it was: in reach — unless its accessed bits are
             * clear (a fresh page table), when its first fetch must walk:
             * parked until then (dbt_page_revive) */
            if (drop) { if (acc) unpark_page(dbt, i); else park_page(dbt, i); }
            pp = &dbt->pcode[i].snext;
            continue;
        }
        if (p == X86_PG_BAD || !drop) return 0;
        drop_code_page(dbt, (uint32_t)i);
        *pp = dbt->pcode[i].snext;
        pcode_release(dbt, i);
    }
    return 1;
}

/* How many of user space U's pages map somewhere other than where their
 * blocks were translated (0: U fits the page tables now, up to LIMIT).
 * A page mapped nowhere does not count against it — a process just
 * forked or exec'd has faulted in almost nothing — it is parked
 * (uspace_enter). */
static uint32_t uspace_conflicts(x86_dbt *dbt, uint8_t u, uint32_t limit) {
    uint32_t n = 0;
    for (int32_t i = dbt->pcode_space[u]; i >= 0 && n < limit; i = dbt->pcode[i].snext) {
        uint32_t p = x86_page_peek(dbt->cpu, dbt->pcode[i].lin_page << 12, dbt->pcode[i].user);
        if (p != X86_PG_BAD && p != dbt->pcode[i].phys_page << 12) n++;
    }
    return n;
}
/* U is current: its pages mapped as they were are in reach, those not
 * mapped parked, those mapped elsewhere — other code at that address in
 * this process — dropped */
static void uspace_enter(x86_dbt *dbt, uint8_t u) {
    int32_t *pp = &dbt->pcode_space[u];
    while (*pp >= 0) {
        int32_t i = *pp;
        int acc;
        uint32_t p = x86_page_peek_acc(dbt->cpu, dbt->pcode[i].lin_page << 12, dbt->pcode[i].user, &acc);
        if (p == dbt->pcode[i].phys_page << 12) { if (acc) unpark_page(dbt, i); else park_page(dbt, i); }   /* (accessed bits clear: parked, see kspace_fits) */
        else if (p == X86_PG_BAD) park_page(dbt, i);
        else {
            drop_code_page(dbt, (uint32_t)i);
            *pp = dbt->pcode[i].snext;
            pcode_release(dbt, i);
            continue;
        }
        pp = &dbt->pcode[i].snext;
    }
}

/* CR3's entry in the table, made if new (the least recently used goes) */
static int cr3_entry(x86_dbt *dbt, uint32_t cr3) {
    int lru = 0;
    for (int c = 0; c < DBT_CR3S; c++) {
        if (dbt->cr3s[c].used && dbt->cr3s[c].cr3 == cr3) { dbt->cr3s[c].lru = ++dbt->cr3_clock; return c; }
        if (!dbt->cr3s[c].used || (dbt->cr3s[lru].used && dbt->cr3s[c].lru < dbt->cr3s[lru].lru)) lru = c;
    }
    dbt->cr3s[lru].used = 1;
    dbt->cr3s[lru].cr3 = cr3;
    dbt->cr3s[lru].lru = ++dbt->cr3_clock;
    dbt->cr3s[lru].kspace = dbt->cr3s[lru].uspace = 0xFF;
    return lru;
}

/* The page tables CR3 names are current: the code spaces its code runs
 * in. Kernel (cpu->pg_kspace): the one it had if that still fits, else
 * any other that fits, else a new one. User (cpu->pg_space): the same,
 * trying next the space of the process that ran just before — a child
 * forked from it, or itself before an exec — whose pages it has not
 * faulted in are parked until it does. */
void dbt_space_current(x86_dbt *dbt) {
    x86_cpu *cpu = dbt->cpu;
    int c = cr3_entry(dbt, cpu->cr3 & 0xFFFFF000u);
    uint8_t prev = cpu->pg_space;
    uint8_t k = dbt->cr3s[c].kspace;
    if (k == 0xFF || !kspace_fits(dbt, k, 1)) {
        uint8_t was = k;
        k = 0xFF;
        for (int j = 0; j < DBT_SPACES && k == 0xFF; j++)
            if (j != was && dbt->space_used[j] && dbt->space_kernel[j] && kspace_fits(dbt, (uint8_t)j, 0)) k = (uint8_t)j;
        if (k == 0xFF) {
            k = space_alloc(dbt, dbt->cr3s[c].uspace == 0xFF ? -1 : dbt->cr3s[c].uspace);
            dbt->space_kernel[k] = 1;
        }
        if (was != 0xFF) dbt->kspace_moves++;
        dbt->cr3s[c].kspace = k;
    }
    /* User: the first space with nothing mapped elsewhere — its own, the
     * last process's, any — else its own (or a free id, or the last
     * process's), whose pages mapped elsewhere go. Library pages sit at
     * random addresses in each process, so two processes' code overlaps
     * here and there: moving to another space for every such page took
     * thousands of moves and evicted spaces wholesale. */
    uint8_t own = dbt->cr3s[c].uspace, u = 0xFF;
    #define USER_SPACE(j) (dbt->space_used[j] && !dbt->space_kernel[j])
    if (own != 0xFF && uspace_conflicts(dbt, own, 1) == 0) u = own;
    else if (prev != own && USER_SPACE(prev) && uspace_conflicts(dbt, prev, 1) == 0) u = prev;
    else {
        for (int j = 0; j < DBT_SPACES && u == 0xFF; j++)
            if (j != own && j != prev && USER_SPACE(j) && uspace_conflicts(dbt, (uint8_t)j, 1) == 0) u = (uint8_t)j;
        if (u == 0xFF && own != 0xFF) u = own;
        if (u == 0xFF) {
            for (int j = 0; j < DBT_SPACES && u == 0xFF; j++)
                if (!dbt->space_used[j]) { u = (uint8_t)j; dbt->space_used[j] = 1; dbt->space_kernel[j] = 0; }
            if (u == 0xFF) u = USER_SPACE(prev) ? prev : space_alloc(dbt, k);
        }
    }
    #undef USER_SPACE
    if (u != own) { if (own != 0xFF) dbt->uspace_moves++; dbt->cr3s[c].uspace = u; }
    uspace_enter(dbt, u);
    cpu->pg_space = u;
    cpu->pg_kspace = k;
}

/* cpu->tlb_hook: the page tables may map differently now (CR3 load, PG
 * toggled, A20, INVLPG). A CR3 reload is how a memory manager flushes
 * after any remap (JEMM with NOINVLPG does it for every A20 emulation),
 * and a VCPI client switches page tables on every call down to DOS, so
 * dropping translations wholesale thrashes. The code spaces the current
 * tables fit are chosen again (dbt_space_current): their pages re-peeked,
 * those that moved dropped (kernel) or the tables moved to a space they
 * fit (user), those not mapped parked. With paging off no paged block is
 * reachable, and nothing is dropped. */
void dbt_tlb_flushed(x86_cpu *cpu) {
    x86_dbt *dbt = (x86_dbt *)cpu->dbt;
    if (!dbt) return;
    dbt->tlb_flushes++;
    if (!(cpu->cr0 & X86_CR0_PG)) return;
    dbt_space_current(dbt);
}

/* LIN_PAGE maps onto PHYS_PAGE: on its alias list. 0 if the pool is
 * full — the caller then does not translate. */
int dbt_note_alias(x86_dbt *dbt, uint32_t lin_page, uint32_t phys_page) {
    for (uint32_t a = dbt->phys_alias[phys_page]; a; a = dbt->alias_pool[a - 1].next)
        if (dbt->alias_pool[a - 1].lin_page == lin_page) return 1;
    if (dbt->alias_used == DBT_ALIAS_MAX) return 0;
    uint32_t n = dbt->alias_used++;
    dbt->alias_pool[n].lin_page = lin_page;
    dbt->alias_pool[n].next = dbt->phys_alias[phys_page];
    dbt->phys_alias[phys_page] = n + 1;
    return 1;
}

/* A miss on a paged key: if its page is parked — out of reach since the
 * page tables were loaded, as not mapped then — and now maps as it did
 * (faulted in since), its blocks come back without translating; the
 * wanted one, if it is among them, is the answer. A page mapped elsewhere
 * has other code at that address: its parked blocks go. NULL: translate. */
uint8_t *dbt_page_revive(x86_dbt *dbt, uint64_t key) {
    int32_t i = pcode_find(dbt, dbt_key_lin(key) >> 12, (uint8_t)((key & KEY_SPACE_MASK) >> KEY_SPACE_SHIFT));
    if (i < 0 || !dbt->pcode[i].parked) return NULL;
    uint32_t p = x86_page_peek(dbt->cpu, dbt->pcode[i].lin_page << 12, dbt->pcode[i].user);
    if (p != dbt->pcode[i].phys_page << 12) { drop_code_page(dbt, (uint32_t)i); return NULL; }
    /* the fetch the interpreter would make here: a walk that sets the
     * page's accessed bits (and fills the TLB), once, at first execution */
    (void)x86_phys_rd8(dbt->cpu, dbt_key_lin(key));
    unpark_page(dbt, i);
    uint32_t slot = dbt_slot(key);
    x86_block_entry *e = &dbt->aux->cache[slot];
    if (e->key != key || !e->code) return NULL;
    dbt->last_block_bytes = dbt->span[slot];
    return e->code;
}

/* A paged block is being translated on LIN_PAGE, which maps to PHYS_PAGE
 * (both page numbers) in space SP (the key's: a user space, or a kernel
 * space): note it for dbt_tlb_flushed. A page noted before at another
 * physical page loses the blocks translated there. 0 if the list is full
 * — the caller then does not translate. */
int dbt_note_code_page(x86_dbt *dbt, uint32_t lin_page, uint32_t phys_page, int user, uint8_t sp) {
    int32_t i = pcode_find(dbt, lin_page, sp);
    if (i >= 0) {
        if (dbt->pcode[i].phys_page != phys_page || dbt->pcode[i].parked) drop_code_page(dbt, (uint32_t)i);
        dbt->pcode[i].phys_page = phys_page;
        dbt->pcode[i].user = (uint8_t)user;
        return 1;
    }
    i = dbt->pcode_free;
    if (i < 0) return 0;
    dbt->pcode_free = dbt->pcode[i].snext;
    dbt->pcode[i].lin_page = lin_page;
    dbt->pcode[i].phys_page = phys_page;
    dbt->pcode[i].user = (uint8_t)user;
    dbt->pcode[i].space = sp;
    dbt->pcode[i].blocks = -1;
    dbt->pcode[i].parked = 0;
    int32_t *head = &dbt->pcode_hash[pcode_bucket(lin_page, sp)];
    dbt->pcode[i].hnext = *head;
    *head = i;
    dbt->pcode[i].snext = dbt->pcode_space[sp];
    dbt->pcode_space[sp] = i;
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
