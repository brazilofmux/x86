/* dbt_cache.c — direct-mapped translated-block cache, direct-link
 * registry, and span-gated SMC invalidation.
 *
 * Lifted from ~/z80/dbt/block_cache.c. The cache is indexed on the guest
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
    /* An empty slot is all-ones in both words (its code pointer is never
     * read while the key says empty), and so is LINK_NONE: two memsets. */
    memset(dbt->aux->cache, 0xFF, sizeof dbt->aux->cache);
    memset(dbt->link_head, 0xFF, BLOCK_CACHE_SIZE * sizeof *dbt->link_head);
    dbt->link_used = 0;
    dbt->link_free = LINK_NONE;
    dbt_clear_code_bits(dbt->cpu);
    dbt->max_block_bytes = 0;
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
static void invalidate_for_store(x86_dbt *dbt, uint32_t phys) {
    uint32_t window = dbt->max_block_bytes;
    if (dbt->smc_heat[phys] < 255) dbt->smc_heat[phys]++;
    for (uint32_t k = 0; k < window && k <= phys; k++) {
        uint32_t p = phys - k;
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
    dbt->smc_invalidations++;
    dbt->cpu->code_bitmap[phys] &= (uint8_t)~X86_BM_CODE;   /* a device bit stays */
}

/* Every translation is stale (A20 flipped, a code descriptor changed).
 * This runs from inside a helper thunk as often as not, so only the
 * cache and the link registry are wiped here — the code buffer that is
 * executing us is rewound by the next translate — and the running block
 * is told to leave. */
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

/* cpu->a20_hook. Every block key and every baked far-transfer mask is
 * stale now (OUT 92h/60h, from inside a helper thunk as often as not). */
void dbt_a20_changed(x86_cpu *cpu, int on) {
    x86_dbt *dbt = (x86_dbt *)cpu->dbt;
    (void)on;
    if (!dbt) return;
    flush_under_running_code(dbt);
    dbt->a20_flushes++;
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
