/* dbt_cache.c — direct-mapped translated-block cache, direct-link
 * registry, and span-gated SMC invalidation.
 *
 * Lifted from ~/z80/dbt/block_cache.c. The cache is 1:1 on the guest
 * linear address (X86_MEM_SIZE entries), which the SMC sweep relies on:
 * the slot for linear p can only ever hold the block starting at p, so
 * its span says exactly whether that block covers a stored byte. Two CS
 * values aliasing one linear address (different 64-bit keys) conflict
 * on the slot and evict each other — rare, and the eviction unlinks.
 */
#include "dbt.h"
#include <string.h>

x86_block_entry *dbt_cache_lookup(x86_dbt *dbt, uint64_t key) {
    x86_block_entry *e = &dbt->aux->cache[dbt_key_lin(key)];
    if (e->key == key || e->key == (key | BLOCK_REFUSED_BIT)) {
        dbt->cache_hits++;
        return e;
    }
    dbt->cache_misses++;
    return NULL;
}

/* Drop the block in slot `lin` (if any): clear the entry and unlink
 * every site that branches straight to it. */
static void evict_slot(x86_dbt *dbt, uint32_t lin) {
    x86_block_entry *e = &dbt->aux->cache[lin];
    if (e->key == BLOCK_EMPTY_KEY) return;
    e->key  = BLOCK_EMPTY_KEY;
    e->code = NULL;
    dbt_links_repatch(dbt, lin, NULL);
}

void dbt_cache_insert(x86_dbt *dbt, uint64_t key, uint8_t *code) {
    uint32_t lin = dbt_key_lin(key);
    x86_block_entry *e = &dbt->aux->cache[lin];
    /* A different CS aliasing this linear address: the old block's
     * direct links would otherwise keep running it with the wrong CS. */
    if (e->key != BLOCK_EMPTY_KEY && (e->key & ~BLOCK_REFUSED_BIT) != key)
        evict_slot(dbt, lin);
    e->key  = code ? key : (key | BLOCK_REFUSED_BIT);
    /* Refusal is decided from the bytes at lin without tracking how
     * many were looked at — treat the sentinel as covering any store in
     * the window so it is always retried. */
    dbt->span[lin] = code ? dbt->last_block_bytes : 0xFFFFFFFFu;
    e->code = code;
    dbt_links_repatch(dbt, lin, code);
}

/* NOTE: every caller also rewinds the code buffer (dbt_init; the
 * buffer-full path in dbt_translate_block). The link registry leans on
 * that: resetting the pool silently abandons all patch sites, sound
 * only because the code containing them is being discarded. */
void dbt_cache_invalidate_all(x86_dbt *dbt) {
    for (uint32_t i = 0; i < BLOCK_CACHE_SIZE; i++) {
        dbt->aux->cache[i].key  = BLOCK_EMPTY_KEY;
        dbt->aux->cache[i].code = NULL;
        dbt->link_head[i]       = LINK_NONE;
    }
    dbt->link_used = 0;
    dbt->link_free = LINK_NONE;
    memset(dbt->cpu->code_bitmap, 0, X86_MEM_SIZE);
    dbt->max_block_bytes = 0;
    dbt->insn_used = 0;
}

int dbt_link_record(x86_dbt *dbt, uint32_t lin, uint32_t site_off) {
    uint32_t i;
    if (dbt->link_free != LINK_NONE) {
        i = dbt->link_free;
        dbt->link_free = dbt->link_pool[i].next;
    } else if (dbt->link_used < LINK_POOL_SIZE) {
        i = dbt->link_used++;
    } else {
        return 0;
    }
    dbt->link_pool[i].site_off = site_off;
    dbt->link_pool[i].next     = dbt->link_head[lin];
    dbt->link_head[lin] = i;
    dbt->links_created++;
    return 1;
}

void dbt_links_repatch(x86_dbt *dbt, uint32_t lin, uint8_t *code) {
    uint32_t i = dbt->link_head[lin];
    if (i == LINK_NONE) return;
    dbt_jit_writable_begin();
    if (code) {
        for (; i != LINK_NONE; i = dbt->link_pool[i].next) {
            dbt_arch_patch_link(dbt, dbt->link_pool[i].site_off, code);
            dbt->links_patched++;
        }
    } else {
        /* Unlink AND free the list: an invalidated target's sites almost
         * always belong to blocks dying in the same SMC wave, and their
         * re-translations re-record fresh sites. */
        while (i != LINK_NONE) {
            uint32_t next = dbt->link_pool[i].next;
            dbt_arch_patch_link(dbt, dbt->link_pool[i].site_off, NULL);
            dbt->link_pool[i].next = dbt->link_free;
            dbt->link_free = i;
            dbt->links_unpatched++;
            i = next;
        }
        dbt->link_head[lin] = LINK_NONE;
    }
    dbt_jit_writable_end();
}

void dbt_mark_block_bytes(x86_dbt *dbt, uint32_t start, uint32_t end) {
    uint32_t bytes = end - start;
    if (bytes > dbt->max_block_bytes) dbt->max_block_bytes = bytes;
    dbt->last_block_bytes = bytes;
    uint8_t *bm = dbt->cpu->code_bitmap;    /* slack past X86_MEM_SIZE absorbs top-of-memory blocks */
    for (uint32_t a = start; a < end; a++) bm[a] = 1;
}

/* A store landed on a byte some cached block covers. Invalidate every
 * block whose start lies in [phys - max_block_bytes + 1, phys] and
 * whose span reaches phys; clear the bitmap byte last, once no block
 * covers it (the rule paid for in blood: never leave a bitmap byte set
 * after its covering blocks are gone, never clear it before the sweep). */
static void invalidate_for_store(x86_dbt *dbt, uint32_t phys) {
    uint32_t window = dbt->max_block_bytes;
    for (uint32_t k = 0; k < window && k <= phys; k++) {
        uint32_t p = phys - k;
        x86_block_entry *e = &dbt->aux->cache[p];
        if (e->key == BLOCK_EMPTY_KEY || k >= dbt->span[p]) continue;
        e->key  = BLOCK_EMPTY_KEY;
        e->code = NULL;
        dbt_links_repatch(dbt, p, NULL);
        /* The block that is executing right now (a JIT store or a helper
         * op inside it): the thunk sees this flag and leaves the block
         * before its next, now stale, instruction. */
        if (p == dbt->cpu->jit_cur_lin) dbt->cpu->jit_cur_hit = 1;
    }
    dbt->smc_invalidations++;
    dbt->cpu->code_bitmap[phys] = 0;
}

/* cpu->smc_hook: reached from x86_phys_wr8 (interpreter, helpers, host
 * services) whenever the bitmap byte is set. */
void dbt_smc_store(x86_cpu *cpu, uint32_t phys) {
    invalidate_for_store((x86_dbt *)cpu->dbt, phys);
}

/* cpu->a20_hook. Every block key and every baked far-transfer mask is
 * stale now. This runs from inside a helper thunk (OUT 92h/60h) as
 * often as not, so only the cache and the link registry are wiped here
 * — the code buffer that is executing us is rewound by the next
 * translate — and the running block is told to leave. */
void dbt_a20_changed(x86_cpu *cpu, int on) {
    x86_dbt *dbt = (x86_dbt *)cpu->dbt;
    (void)on;
    if (!dbt) return;
    dbt_cache_invalidate_all(dbt);
    dbt->flush_pending = 1;
    cpu->jit_cur_hit = 1;
    dbt->a20_flushes++;
}

/* The host wrote guest memory directly (loader, DOS file reads): run
 * the same invalidation a guest store would have. */
void dbt_host_wrote(x86_cpu *cpu, uint32_t phys, uint32_t len) {
    if (!cpu->dbt) return;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t p = (phys + i) & cpu->a20_mask;
        if (p < cpu->mem_size && cpu->code_bitmap[p]) dbt_smc_store(cpu, p);
    }
}
