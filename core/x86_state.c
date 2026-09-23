/* x86_state.c — CPU construction, reset, segment loading, dumps */

#include "x86.h"
#include <stdlib.h>
#include <string.h>

static void smc_hook_none(x86_cpu *c, uint32_t phys) { (void)c; (void)phys; }

void x86_init(x86_cpu *c, int model) {
    memset(c, 0, sizeof(*c));
    c->model = model;
    if (x86_mem_alloc(c) < 0) {         /* A20 gated off at power-on */
        fprintf(stderr, "x86_init: cannot allocate guest memory\n");
        exit(1);
    }
    c->smc_hook = smc_hook_none;
    c->exc = -1;
    x86_reset(c);
}

/* A store landed on a byte the bitmap marks: device memory first (it may
 * rewrite the byte), then code the DBT has translated. */
void x86_store_hook(x86_cpu *c, uint32_t p) {
    uint8_t b = c->code_bitmap[p];
    if ((b & X86_BM_DEVICE) && c->device_store) c->device_store(c, p);
    if ((b & (X86_BM_CODE | X86_BM_DESC)) && c->smc_hook) c->smc_hook(c, p);
}

void x86_free(x86_cpu *c) {
    x86_mem_free(c);
}

/* Real-mode segment load: base = sel << 4, limit 64K, 16-bit. Phase B
 * replaces this with descriptor lookup when c->pmode is set. */

/* ------------------------------------------------------------------------
 * Protected mode: descriptor tables and segment loading.
 *
 * Every rule here has a case in tools/pmoracle. Where the emulators we
 * compare against are permissive we follow the manual instead; those
 * places say so.
 * --------------------------------------------------------------------- */

/* CPL lives in the low two bits of CS, which is where the processor keeps
 * it — once CS has been loaded in protected mode. Right after CR0.PE is
 * set, CS still holds whatever real mode left (029Eh: low bits 2) and the
 * processor is at CPL 0 until the first far transfer. */
int x86_cpl(const x86_cpu *c) {
    if (!c->pmode) return 0;
    if (c->eflags & X86_VM) return 3;                 /* virtual-8086 mode runs at CPL 3 */
    if (c->pe_window && c->seg[S_CS].sel == c->pe_cs_sel && c->seg[S_CS].base == c->pe_cs_base) return 0;
    return c->seg[S_CS].sel & 3;
}

/* CR0.PE going 0 -> 1 (MOV CR0, LMSW): open the CPL-0 window. */
void x86_pe_set(x86_cpu *c) {
    c->pe_window = 1;
    c->pe_cs_sel = c->seg[S_CS].sel;
    c->pe_cs_base = c->seg[S_CS].base;
}

/* Fetch the 8 bytes of a descriptor. Returns 0 when the selector's index
 * falls outside its table, which is the caller's cue to raise #GP. */
int x86_read_desc(x86_cpu *c, uint16_t sel, uint32_t *lo, uint32_t *hi) {
    uint32_t base, off = sel & 0xFFF8;
    uint16_t limit;
    if (sel & 4) {
        /* LDTR is null out of reset, and a selector that needs it is then
         * simply invalid — both QEMU and Bochs instead read whatever lies
         * at linear 0. See tools/pmoracle/expected.py. */
        if (!c->ldtr.usable || !X86_AR_P(c->ldtr.attr)) return 0;
        base = c->ldtr.base;
        limit = (uint16_t)c->ldtr.limit;
    } else {
        base = c->gdtr.base;
        limit = c->gdtr.limit;
    }
    if (off + 7 > limit) return 0;
    *lo = x86_sup_rd(c, base, off, 4);
    *hi = x86_sup_rd(c, base, off + 4, 4);
    return 1;
}

void x86_unpack_desc(x86_seg *g, uint16_t sel, uint32_t lo, uint32_t hi) {
    g->sel = sel;
    g->base = (lo >> 16) | ((hi & 0xFF) << 16) | (hi & 0xFF000000u);
    g->limit = (lo & 0xFFFF) | (hi & 0x000F0000u);
    g->attr = (uint16_t)(((hi >> 8) & 0xFF) | (((hi >> 20) & 0x0F) << 8));
    if (X86_AR_G(g->attr)) g->limit = (g->limit << 12) | 0xFFF;
    g->big = X86_AR_DB(g->attr);
    g->usable = 1;
}

/* The accessed bit is set in the descriptor itself the first time it is
 * loaded, not merely in the cached copy. */
void x86_set_accessed(x86_cpu *c, uint16_t sel, uint32_t hi) {
    uint32_t base = (sel & 4) ? c->ldtr.base : c->gdtr.base;
    if (hi & (X86_TYPE_ACCESSED << 8)) return;
    x86_sup_wr(c, base, (sel & 0xFFF8) + 4, 4, hi | (X86_TYPE_ACCESSED << 8));
}

/* Load a data or stack segment register. CS goes through the control
 * transfer paths instead, which have their own rules. */
static void load_seg_pm(x86_cpu *c, int s, uint16_t sel) {
    int cpl = x86_cpl(c), rpl = sel & 3;
    uint32_t lo, hi;

    if ((sel & 0xFFFC) == 0) {
        /* Null is legal in a data register and leaves it unusable; in SS it
         * is #GP(0), because there is no such thing as no stack. */
        if (s == S_SS) x86_fault(c, X86_EXC_GP, 0);
        c->seg[s].sel = sel;
        c->seg[s].base = 0; c->seg[s].limit = 0; c->seg[s].attr = 0;
        c->seg[s].big = 0; c->seg[s].usable = 0;
        return;
    }
    if (!x86_read_desc(c, sel, &lo, &hi)) x86_fault(c, X86_EXC_GP, sel & 0xFFFC);
    uint16_t attr = (uint16_t)(((hi >> 8) & 0xFF) | (((hi >> 20) & 0x0F) << 8));
    int type = X86_AR_TYPE(attr);

    if (!X86_AR_S(attr)) x86_fault(c, X86_EXC_GP, sel & 0xFFFC);   /* a system descriptor */

    if (s == S_CS) {
        /* Only the host places CS directly (an HLE service returning, a DPMI
         * trampoline entering client code); the instructions go through
         * load_cs_pm with their own rules. What must hold regardless: it is
         * code, and it is there. CPL becomes the selector's RPL. */
        if (!(type & X86_TYPE_CODE)) x86_fault(c, X86_EXC_GP, sel & 0xFFFC);
        if (!X86_AR_P(attr)) x86_fault(c, X86_EXC_NP, sel & 0xFFFC);
    } else if (s == S_SS) {
        /* The stack is the strict one: writable data, and both RPL and DPL
         * equal to CPL. A read-only segment that any other register accepts
         * is rejected here. */
        if ((type & X86_TYPE_CODE) || !(type & X86_TYPE_WRITABLE))
            x86_fault(c, X86_EXC_GP, sel & 0xFFFC);
        if (rpl != cpl || X86_AR_DPL(attr) != cpl)
            x86_fault(c, X86_EXC_GP, sel & 0xFFFC);
        if (!X86_AR_P(attr)) x86_fault(c, X86_EXC_SS, sel & 0xFFFC);
    } else {
        /* Code is only a legal data segment if it can be read. */
        if ((type & X86_TYPE_CODE) && !(type & X86_TYPE_READABLE))
            x86_fault(c, X86_EXC_GP, sel & 0xFFFC);
        /* Conforming code is reachable from anywhere; everything else has to
         * be at least as privileged as the more privileged of CPL and RPL. */
        int conforming = (type & X86_TYPE_CODE) && (type & X86_TYPE_CONFORM);
        if (!conforming) {
            int need = cpl > rpl ? cpl : rpl;
            if (need > X86_AR_DPL(attr)) x86_fault(c, X86_EXC_GP, sel & 0xFFFC);
        }
        if (!X86_AR_P(attr)) x86_fault(c, X86_EXC_NP, sel & 0xFFFC);
    }
    x86_unpack_desc(&c->seg[s], sel, lo, hi);
    x86_set_accessed(c, sel, hi);
}

/* Real mode sets the selector and base and leaves the cached limit
 * alone, which is what unreal mode is: HIMEMX loads DS/ES with 4 GB in
 * protected mode, returns, and moves XMS with 32-bit offsets. We keep a
 * limit only when it is above 64K (nothing real-mode relies on a smaller
 * one surviving) and never for CS; the DPMI host resets its own on the
 * way down (x86_real_limits). */
void x86_load_seg(x86_cpu *c, int s, uint16_t sel) {
    if (c->pmode && !(c->eflags & X86_VM)) { load_seg_pm(c, s, sel); return; }
    c->seg[s].sel = sel;
    c->seg[s].base = (uint32_t)sel << 4;
    /* virtual-8086 mode: every load is a 64K segment, whatever came before */
    if (s == S_CS || c->seg[s].limit < 0xFFFF || (c->eflags & X86_VM)) c->seg[s].limit = 0xFFFF;
    c->seg[s].big = 0;
    c->seg[s].attr = 0;
    c->seg[s].usable = 1;
}

/* Every segment back to 64K, as a DPMI host's descriptors would leave
 * them before it clears PE. */
void x86_real_limits(x86_cpu *c) {
    for (int s = 0; s < 6; s++) c->seg[s].limit = 0xFFFF;
}

uint32_t x86_flags_fixup(x86_cpu *c, uint32_t f) {
    f |= X86_F1;
    f &= ~0x28u;                                  /* bits 3 and 5 always 0 */
    switch (c->model) {
    case X86_MODEL_8086:
    case X86_MODEL_186:
        f |= 0xF000; f &= 0xFFFF; break;          /* 12-15 read as 1 */
    case X86_MODEL_286:
        if (!c->pmode) f &= 0x0FFF; else f &= 0x7FFF; break;
    default:
        /* 386: bit 15 clear, RF/VM allowed; bits 18-31 are reserved and
         * read back as whatever they were (the 386EX shows them set) */
        f &= ~0x8000u;
        if (!c->pmode) f &= ~X86_VM;
        break;
    }
    return f;
}

void x86_reset(x86_cpu *c) {
    memset(c->r, 0, sizeof c->r);
    c->eflags = x86_flags_fixup(c, 0);
    c->pmode = 0;
    c->pe_window = 0;
    x86_real_limits(c);
    c->cr0 = 0; c->cr2 = 0; c->cr3 = 0;
    c->pg_super = c->pg_probe = 0;
    x86_tlb_flush(c);
    c->halted = 0;
    c->int_inhibit = 0;
    c->exc = -1;
    for (int i = 0; i < 6; i++) x86_load_seg(c, i, 0);
    x86_load_seg(c, S_CS, 0xFFFF);
    c->eip = 0;
    if (c->model >= X86_MODEL_286) {              /* 286+: CS base F0000/FFFF0000, IP FFF0 */
        c->seg[S_CS].sel = 0xF000;
        c->seg[S_CS].base = c->model >= X86_MODEL_386 ? 0xFFFF0000u : 0xFF0000u;
        c->eip = 0xFFF0;
    }
}

void x86_dump(x86_cpu *c, FILE *f) {
    static const char *rn[8] = { "AX","CX","DX","BX","SP","BP","SI","DI" };
    static const char *sn[6] = { "ES","CS","SS","DS","FS","GS" };
    for (int i = 0; i < 8; i++) fprintf(f, "%s=%08X%c", rn[i], c->r[i], i == 7 ? '\n' : ' ');
    for (int i = 0; i < 6; i++) fprintf(f, "%s=%04X%c", sn[i], c->seg[i].sel, i == 5 ? ' ' : ' ');
    fprintf(f, "IP=%08X FL=%08X [%c%c%c%c%c%c%c%c%c]\n", c->eip, c->eflags,
            c->eflags & X86_OF ? 'O' : '.', c->eflags & X86_DF ? 'D' : '.',
            c->eflags & X86_IF ? 'I' : '.', c->eflags & X86_TF ? 'T' : '.',
            c->eflags & X86_SF ? 'S' : '.', c->eflags & X86_ZF ? 'Z' : '.',
            c->eflags & X86_AF ? 'A' : '.', c->eflags & X86_PF ? 'P' : '.',
            c->eflags & X86_CF ? 'C' : '.');
}
