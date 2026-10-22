/* strtest: the interpreter's REP MOVS/STOS fast path (do_string_bulk,
 * core/x86_interp.c) against its element loop. Each random case runs
 * twice from the same state — once as it is, once with x86_nwatch set,
 * which keeps do_string on the element loop — and memory, registers,
 * flags and CR2 must come out the same.
 *
 * Real mode: 16- and 32-bit addressing (big limits, as unreal mode has),
 * offsets at page ends and at FFFFh, overlapping pointers in either
 * direction, A20 on and off. Paged protected mode (a 486, CR0.WP either
 * way): pages missing, read-only or mapped elsewhere, so a chunk's first
 * element faults — the #PF goes through the IDT to a HLT — and the
 * accessed and dirty bits are compared with the rest of memory. Some
 * cases mark bytes as translated code (scattered, or whole pages, as a
 * kernel poisoning its freed init text meets them): a hook logs each
 * store that lands on one and clears its mark, as the translator does,
 * and both runs must hook the same bytes in the same order.
 *
 *   strtest [-n trials] [-s seed]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/x86.h"

static uint64_t s_rnd = 88172645463325252ull;
static uint32_t rnd(void) { s_rnd ^= s_rnd << 13; s_rnd ^= s_rnd >> 7; s_rnd ^= s_rnd << 17; return (uint32_t)s_rnd; }

#define SPAN  0x220000u      /* everything a case can touch: data, tables, stack */
#define CODE  0x20100u
#define PD    0x1F0000u
#define PT    0x1F1000u
#define GDT   0x1F8000u
#define IDT   0x1F8100u
#define STUB  0x1F9000u
#define STACK 0x1FF000u

static void put32(x86_cpu *c, uint32_t at, uint32_t v) { memcpy(c->mem + at, &v, 4); }

#define HOOK_MAX 65536
static uint32_t s_hooked[HOOK_MAX], s_nhooked;
static void smc_log(x86_cpu *c, uint32_t p) {
    if (s_nhooked < HOOK_MAX) s_hooked[s_nhooked] = p;
    s_nhooked++;
    c->code_bitmap[p] &= (uint8_t)~X86_BM_CODE;
}

/* Paged flat protected mode at CPL 0: identity-mapped 4 MB with some data
 * pages missing, read-only or elsewhere; every vector a gate to a HLT. */
static void pm_setup(x86_cpu *c) {
    c->pmode = 1;
    c->cr0 |= 1 | X86_CR0_PG | ((rnd() & 1) ? X86_CR0_WP : 0);
    static const uint16_t sels[6] = { 0x10, 0x08, 0x10, 0x10, 0, 0 };
    for (int s = 0; s < 6; s++) {
        x86_seg *g = &c->seg[s];
        memset(g, 0, sizeof *g);
        g->sel = sels[s];
        if (!sels[s]) continue;
        g->usable = 1; g->limit = 0xFFFFFFFFu; g->big = 1;
        g->attr = (uint16_t)((s == S_CS ? 0x9B : 0x93) | 0xC00);
    }
    for (int s = 0; s < 2; s++) {                        /* DS and ES: sometimes based, sometimes limited */
        x86_seg *g = &c->seg[s == 0 ? S_DS : S_ES];
        if (rnd() & 1) g->base = rnd() & 0xFFFFF;
        if (!(rnd() % 3)) g->limit = 0x100000 + (rnd() & 0xFFFFF);
    }
    static const uint32_t gdt[6] = { 0, 0, 0x0000FFFF, 0x00CF9B00, 0x0000FFFF, 0x00CF9300 };
    memcpy(c->mem + GDT, gdt, sizeof gdt);
    c->gdtr.base = GDT; c->gdtr.limit = 0x17;
    for (int v = 0; v < 32; v++) { put32(c, IDT + 8 * v, (STUB & 0xFFFF) | (0x08u << 16)); put32(c, IDT + 8 * v + 4, (STUB & 0xFFFF0000u) | 0x8E00u); }
    c->idtr.base = IDT; c->idtr.limit = 32 * 8 - 1;
    c->mem[STUB] = 0xF4;
    memset(c->mem + PD, 0, 0x1000);
    put32(c, PD, PT | 0x07);
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t e = (i << 12) | 0x07, r = rnd() % 32;
        int fixed = i == (CODE >> 12) || i >= (PD >> 12);
        if (!fixed && r == 0) e = 0;                                   /* missing */
        else if (!fixed && r == 1) e &= ~2u;                           /* read-only */
        else if (!fixed && r == 2) e = ((rnd() % 0x1E0) << 12) | 0x07; /* elsewhere */
        put32(c, PT + 4 * i, e);
    }
    c->cr3 = PD;
    x86_tlb_flush(c);
    c->r[R_SP] = STACK;
}

int main(int argc, char **argv) {
    int n = 20000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) s_rnd = strtoull(argv[++i], 0, 0) | 1;
        else { fprintf(stderr, "usage: strtest [-n trials] [-s seed]\n"); return 2; }
    }
    static uint8_t mem0[SPAN], memA[SPAN], bm0[SPAN], bmA[SPAN];
    static uint32_t hookedA[HOOK_MAX];
    x86_watch_phys[0] = 0x7FFFFFF0u;                     /* (watching: nothing a case reaches) */
    int bad = 0, faults = 0;
    uint64_t hooks = 0;
    for (int t = 0; t < 2 * n; t++) {
        int pm = t & 1;
        x86_cpu c;
        x86_init(&c, pm ? X86_MODEL_486 : X86_MODEL_386);
        for (uint32_t i = 0; i < SPAN; i += 4) put32(&c, i, rnd());
        x86_set_a20(&c, pm || (rnd() & 1));
        int a32;
        if (pm) {
            pm_setup(&c);
            c.eip = CODE;
            a32 = !(rnd() % 6) ? 0 : 1;
        } else {
            x86_load_seg(&c, S_CS, CODE >> 4);
            c.eip = 0;
            uint16_t dsel = (uint16_t)(rnd() % 0xFFFF), esel = (rnd() & 3) ? (uint16_t)(rnd() % 0xFFFF) : dsel;
            if (rnd() & 1) esel = (uint16_t)(dsel + (rnd() & 0x3F) - 0x20);
            x86_load_seg(&c, S_DS, dsel);
            x86_load_seg(&c, S_ES, esel);
            int unreal = rnd() & 1;
            if (unreal) { c.seg[S_DS].limit = (rnd() & 1) ? 0xFFFFFFFFu : 0x1FFFFF; c.seg[S_ES].limit = (rnd() & 1) ? 0xFFFFFFFFu : 0x10FFFF; }
            a32 = unreal && (rnd() & 1);
            x86_load_seg(&c, S_SS, 0x3000); c.r[R_SP] = 0xFFF0;
        }
        static const uint8_t ops[6] = { 0xA4, 0xA5, 0xA5, 0xAA, 0xAB, 0xAB };
        int k = rnd() % 6, i = 0;
        uint8_t *code = c.mem + CODE;
        if (pm ? !a32 : a32) code[i++] = 0x67;
        if (pm ? k == 1 || k == 4 : k == 2 || k == 5) code[i++] = 0x66;   /* the other operand size */
        code[i++] = 0xF3;
        code[i++] = ops[k];
        code[i++] = 0xF4;
        uint32_t am = a32 ? 0xFFFFFFFFu : 0xFFFF, lim = pm ? 0x1C0000 : 0x180000;
        uint32_t si, di;
        switch (rnd() % 5) {
        case 0: si = ((rnd() % 0x1C0) << 12) - (rnd() & 0x1F); break;
        case 1: si = 0xFFFF - (rnd() & 0x3F); break;
        case 2: si = rnd() & 0xFFFF; break;
        default: si = a32 ? rnd() % lim : rnd() & 0xFFFF; break;
        }
        switch (rnd() % 4) {
        case 0: di = si + (rnd() & 7) - 3; break;                      /* overlapping */
        case 1: di = ((rnd() % 0x1C0) << 12) - (rnd() & 0x1F); break;
        case 2: di = 0xFFFF - (rnd() & 0x3F); break;
        default: di = a32 ? rnd() % lim : rnd() & 0xFFFF; break;
        }
        c.r[R_SI] = (si & am) | (a32 ? 0 : rnd() & 0xFFFF0000u);
        c.r[R_DI] = (di & am) | (a32 ? 0 : rnd() & 0xFFFF0000u);
        uint32_t cx = (rnd() & 3) ? rnd() % 300 : rnd() % 9000;
        c.r[R_CX] = a32 ? cx : (cx & 0xFFFF) | (rnd() & 0xFFFF0000u);
        c.r[R_AX] = rnd();
        if (rnd() & 1) c.eflags |= X86_DF; else c.eflags &= ~X86_DF;
        c.smc_hook = smc_log;
        if (!(rnd() % 3)) {                              /* translated code under the destination, or near it */
            uint32_t at = (di & am) & 0x1FF000u, pages = 1 + rnd() % 3;
            if (pm) at = (c.seg[S_ES].base + at) & 0x1FF000u;   /* (paged: often not where it maps, which is fine) */
            if (rnd() & 1) { for (uint32_t i = 0; i < pages * 4096 && at + i < 0x1E0000; i++) c.code_bitmap[at + i] |= X86_BM_CODE; }
            else for (int k = 0; k < 40; k++) { uint32_t a = (at + rnd() % (pages * 4096)) % 0x1E0000; c.code_bitmap[a] |= X86_BM_CODE; }
        }

        x86_cpu c0 = c;
        memcpy(mem0, c.mem, SPAN);
        memcpy(bm0, c.code_bitmap, SPAN);
        s_nhooked = 0;
        x86_step(&c);                                    /* the fast path where it applies */
        x86_cpu cA = c;
        memcpy(memA, c.mem, SPAN);
        memcpy(bmA, c.code_bitmap, SPAN);
        uint32_t nhookedA = s_nhooked;
        memcpy(hookedA, s_hooked, sizeof hookedA);
        uint8_t *m = c.mem, *b = c.code_bitmap;
        c = c0; c.mem = m; c.code_bitmap = b;
        memcpy(c.mem, mem0, SPAN);
        memcpy(c.code_bitmap, bm0, SPAN);
        x86_tlb_flush(&c);
        s_nhooked = 0;
        x86_nwatch = 1;                                  /* the element loop throughout */
        x86_step(&c);
        x86_nwatch = 0;
        faults += pm && c.eip == STUB;
        int diff = memcmp(memA, c.mem, SPAN) != 0 || memcmp(bmA, c.code_bitmap, SPAN) != 0
                || nhookedA != s_nhooked || memcmp(hookedA, s_hooked, (nhookedA < HOOK_MAX ? nhookedA : HOOK_MAX) * sizeof hookedA[0]) != 0;
        hooks += s_nhooked;
        for (int r = 0; r < 8; r++) diff |= cA.r[r] != c.r[r];
        diff |= cA.eip != c.eip || cA.eflags != c.eflags || cA.seg[S_CS].sel != c.seg[S_CS].sel || cA.cr2 != c.cr2;
        if (diff && bad++ < 10) {
            fprintf(stderr, "case %d (%s): op %02X a32 %d df %d si %08X di %08X cx %08X ds %08X/%08X es %08X/%08X a20 %d\n",
                    t, pm ? "paged" : "real", ops[k], a32, !!(c0.eflags & X86_DF), c0.r[R_SI], c0.r[R_DI], c0.r[R_CX],
                    c0.seg[S_DS].base, c0.seg[S_DS].limit, c0.seg[S_ES].base, c0.seg[S_ES].limit, c0.a20_mask == 0xFFFFFFFFu);
            fprintf(stderr, "   code-store hooks: fast %u, elements %u\n", nhookedA, s_nhooked);
            fprintf(stderr, "   fast: si %08X di %08X cx %08X eip %08X cr2 %08X | elements: si %08X di %08X cx %08X eip %08X cr2 %08X\n",
                    cA.r[R_SI], cA.r[R_DI], cA.r[R_CX], cA.eip, cA.cr2, c.r[R_SI], c.r[R_DI], c.r[R_CX], c.eip, c.cr2);
            for (uint32_t q = 0; q < SPAN; q++)
                if (memA[q] != c.mem[q]) { fprintf(stderr, "   memory first differs at %06X: fast %02X, elements %02X, before %02X\n", q, memA[q], c.mem[q], mem0[q]); break; }
        }
        x86_free(&c);
    }
    printf("strtest: %d real-mode and %d paged cases (%d faulted, %llu stores on code), %d differ\n", n, n, faults, (unsigned long long)hooks, bad);
    return bad != 0;
}
