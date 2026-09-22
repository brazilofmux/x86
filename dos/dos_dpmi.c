/* dos_dpmi.c — the DPMI host.
 *
 * We are the host, the way NTVDM was: a client asks through INT 2Fh
 * AX=1687h, far-calls the entry point we hand back, and wakes up in 32-bit
 * protected mode running on descriptors we built. Its own descriptors come
 * out of an LDT we own and hand out through INT 31h.
 *
 * There is no paging here and never will be (CLAUDE.md), so linear and
 * physical are the same address and the memory services are the DOS arena
 * wearing a different hat. The client runs at ring 0, which DPMI permits
 * and CWSDPMI also does; that keeps every INT out of the stack-switch path.
 *
 * Interrupts reach us the same way they do in real mode: every IDT gate
 * points into a code descriptor based at the HLE segment, so INT 21h in
 * protected mode lands on the same trap as INT 21h in real mode.
 */
#include "dos.h"
#include <string.h>
#include <stdio.h>

#define GDT_OFF   0x0000            /* within the host's block */
#define LDT_OFF   0x0400
#define IDT_OFF   0x0C00
#define TSS_OFF   0x1400
#define HOST_PARAS 0x160            /* 5.5 KB: tables plus slack */

#define SEL_HCODE 0x08              /* flat code, base 0 */
#define SEL_HDATA 0x10              /* flat data, base 0 */
#define SEL_HLE   0x18              /* code based at the HLE segment */
#define SEL_LDT   0x20
#define SEL_TSS   0x28
#define LDT_SLOTS 128

static struct {
    uint16_t seg;                   /* paragraph of the host's tables */
    int      active;                /* a client has switched to protected mode */
    uint8_t  used[LDT_SLOTS];
} dpmi;

static uint32_t lin(uint16_t seg, uint32_t off) { return ((uint32_t)seg << 4) + off; }

/* Write one descriptor, given the fields rather than the packed form. */
static void set_desc(x86_cpu *c, uint32_t at, uint32_t base, uint32_t limit,
                     uint8_t access, uint8_t flags) {
    if (limit > 0xFFFFF) { limit >>= 12; flags |= 0x80; }      /* granular */
    x86_wr(c, at, 0, 0xFFFFFFFFu, 2, limit & 0xFFFF);
    x86_wr(c, at, 2, 0xFFFFFFFFu, 2, base & 0xFFFF);
    x86_wr(c, at, 4, 0xFFFFFFFFu, 1, (base >> 16) & 0xFF);
    x86_wr(c, at, 5, 0xFFFFFFFFu, 1, access);
    x86_wr(c, at, 6, 0xFFFFFFFFu, 1, (uint8_t)(((limit >> 16) & 0x0F) | (flags & 0xF0)));
    x86_wr(c, at, 7, 0xFFFFFFFFu, 1, (base >> 24) & 0xFF);
}

static uint32_t gdt_at(int i) { return lin(dpmi.seg, GDT_OFF + (uint32_t)i * 8); }
static uint32_t ldt_at(int i) { return lin(dpmi.seg, LDT_OFF + (uint32_t)i * 8); }

/* Build the tables once, at DOS init: they cost a few KB of the arena and
 * mean the mode switch itself has nothing to allocate. */
void dpmi_init(x86_cpu *c) {
    memset(&dpmi, 0, sizeof dpmi);
    dpmi.seg = dos_mem_alloc(HOST_PARAS, 8, NULL);      /* owner 8: the host itself */
    if (!dpmi.seg) { fprintf(stderr, "dpmi: no memory for host tables\n"); return; }
    for (uint32_t i = 0; i < HOST_PARAS * 16u; i++) x86_phys_wr8(c, lin(dpmi.seg, i), 0);

    set_desc(c, gdt_at(1), 0, 0xFFFFFFFFu, 0x9A, 0x40);                 /* flat code */
    set_desc(c, gdt_at(2), 0, 0xFFFFFFFFu, 0x92, 0x40);                 /* flat data */
    set_desc(c, gdt_at(3), (uint32_t)PC_HLE_SEG << 4, 0xFFFF, 0x9A, 0x40);  /* the trap segment */
    set_desc(c, gdt_at(4), lin(dpmi.seg, LDT_OFF), LDT_SLOTS * 8 - 1, 0x82, 0x00);
    set_desc(c, gdt_at(5), lin(dpmi.seg, TSS_OFF), 0x67, 0x89, 0x00);

    /* Every vector traps to the host, exactly as the real-mode IVT does. */
    for (int v = 0; v < 256; v++) {
        uint32_t at = lin(dpmi.seg, IDT_OFF + (uint32_t)v * 8);
        uint32_t off = (uint32_t)v;                    /* offset within the trap segment */
        x86_wr(c, at, 0, 0xFFFFFFFFu, 2, off & 0xFFFF);
        x86_wr(c, at, 2, 0xFFFFFFFFu, 2, SEL_HLE);
        x86_wr(c, at, 4, 0xFFFFFFFFu, 1, 0);
        x86_wr(c, at, 5, 0xFFFFFFFFu, 1, 0xEE);        /* present, DPL 3, 32-bit trap gate */
        x86_wr(c, at, 6, 0xFFFFFFFFu, 2, (off >> 16) & 0xFFFF);
    }
}

/* Allocate `count` consecutive LDT slots. Returns the first selector, or 0. */
static uint16_t ldt_alloc(int count) {
    for (int i = 1; i + count <= LDT_SLOTS; i++) {
        int k = 0;
        while (k < count && !dpmi.used[i + k]) k++;
        if (k < count) { i += k; continue; }
        for (k = 0; k < count; k++) dpmi.used[i + k] = 1;
        return (uint16_t)((i << 3) | 4);               /* TI = 1, RPL 0 */
    }
    return 0;
}

static int ldt_index(uint16_t sel) {
    if (!(sel & 4)) return -1;
    int i = sel >> 3;
    return (i > 0 && i < LDT_SLOTS) ? i : -1;
}

/* INT 2Fh AX=1687h: yes, there is a host, and here is the way in. */
void dpmi_int2f_1687(x86_cpu *c) {
    x86_set_r16(c, R_AX, 0);                           /* available */
    x86_set_r16(c, R_BX, 1);                           /* 32-bit clients supported */
    x86_set_r8(c, R_CL, 3);                            /* 386 */
    x86_set_r16(c, R_DX, 0x005A);                      /* version 0.90 */
    x86_set_r16(c, R_SI, 0);                           /* no private data needed */
    x86_load_seg(c, S_ES, PC_HLE_SEG);
    x86_set_r16(c, R_DI, PC_HLE_DPMI_ENTRY);
}

/* The client far-called the entry point. Build its descriptors, turn on
 * protected mode, and return to it on the other side. */
void dpmi_mode_switch(x86_cpu *c, int vector) {
    (void)vector;
    uint16_t ccs = pc_rd16(c, c->seg[S_SS].sel, (uint16_t)(c->r[R_SP] + 2));
    uint16_t cip = pc_rd16(c, c->seg[S_SS].sel, (uint16_t)c->r[R_SP]);
    uint16_t cds = c->seg[S_DS].sel, css = c->seg[S_SS].sel;
    uint32_t csp = c->r[R_SP] + 4;                     /* drop the far return address */

    if (!dpmi.seg) { c->eflags |= X86_CF; return; }

    /* AX on the way in says which kind of client this is, and that decides
     * the D/B bit: give 16-bit code a 32-bit CS and it decodes its own
     * instructions wrongly from the first one. */
    int is32 = x86_get_r16(c, R_AX) & 1;
    uint8_t f = is32 ? 0x40 : 0x00;

    uint16_t sel_cs = ldt_alloc(1), sel_ds = ldt_alloc(1);
    uint16_t sel_ss = ldt_alloc(1), sel_psp = ldt_alloc(1);
    if (!sel_cs || !sel_ds || !sel_ss || !sel_psp) { c->eflags |= X86_CF; return; }
    set_desc(c, ldt_at(ldt_index(sel_cs)),  (uint32_t)ccs << 4, 0xFFFF, 0x9A, f);
    set_desc(c, ldt_at(ldt_index(sel_ds)),  (uint32_t)cds << 4, 0xFFFF, 0x92, f);
    set_desc(c, ldt_at(ldt_index(sel_ss)),  (uint32_t)css << 4, 0xFFFF, 0x92, f);
    set_desc(c, ldt_at(ldt_index(sel_psp)), (uint32_t)dos.psp << 4, 0xFF, 0x92, f);

    c->gdtr.base = lin(dpmi.seg, GDT_OFF);  c->gdtr.limit = 6 * 8 - 1;
    c->idtr.base = lin(dpmi.seg, IDT_OFF);  c->idtr.limit = 256 * 8 - 1;
    c->cr0 |= 1;
    c->pmode = 1;
    x86_load_seg(c, S_DS, SEL_HDATA);                  /* something valid while we set up */
    c->ldtr.sel = SEL_LDT;
    c->ldtr.base = lin(dpmi.seg, LDT_OFF);
    c->ldtr.limit = LDT_SLOTS * 8 - 1;
    c->ldtr.attr = 0x82;
    c->ldtr.usable = 1;
    c->tr.sel = SEL_TSS;
    c->tr.base = lin(dpmi.seg, TSS_OFF);
    c->tr.limit = 0x67;
    c->tr.attr = 0x8B;
    c->tr.usable = 1;

    x86_load_seg(c, S_SS, sel_ss);
    c->r[R_SP] = csp;
    x86_load_seg(c, S_DS, sel_ds);
    x86_load_seg(c, S_ES, sel_psp);
    x86_load_seg(c, S_CS, sel_cs);
    c->eip = cip;
    c->eflags &= ~X86_CF;
    dpmi.active = 1;
    pc.returned = 1;                                   /* we placed CS:IP ourselves */
    if (pc.debug) fprintf(stderr, "[dpmi] client in %d-bit protected mode at %04X:%04X\n", is32 ? 32 : 16, sel_cs, cip);
}

/* INT 31h. */
void dpmi_int31(x86_cpu *c, int vector) {
    (void)vector;
    uint16_t ax = x86_get_r16(c, R_AX);
    c->eflags &= ~X86_CF;
    switch (ax) {
    case 0x0000: {                                     /* allocate LDT descriptors */
        uint16_t sel = ldt_alloc(x86_get_r16(c, R_CX));
        if (!sel) { x86_set_r16(c, R_AX, 8); c->eflags |= X86_CF; break; }
        int n = x86_get_r16(c, R_CX);
        for (int k = 0; k < n; k++)
            set_desc(c, ldt_at(ldt_index(sel) + k), 0, 0, 0x92, 0x40);
        x86_set_r16(c, R_AX, sel);
        break;
    }
    case 0x0001: {                                     /* free a descriptor */
        int i = ldt_index(x86_get_r16(c, R_BX));
        if (i < 0) { x86_set_r16(c, R_AX, 0x8022); c->eflags |= X86_CF; break; }
        dpmi.used[i] = 0;
        break;
    }
    case 0x0003:                                       /* selector increment */
        x86_set_r16(c, R_AX, 8);
        break;
    case 0x0006: {                                     /* get segment base */
        int i = ldt_index(x86_get_r16(c, R_BX));
        if (i < 0) { x86_set_r16(c, R_AX, 0x8022); c->eflags |= X86_CF; break; }
        uint32_t at = ldt_at(i);
        uint32_t base = (uint32_t)x86_rd(c, at, 2, 0xFFFFFFFFu, 2)
                      | ((uint32_t)x86_rd(c, at, 4, 0xFFFFFFFFu, 1) << 16)
                      | ((uint32_t)x86_rd(c, at, 7, 0xFFFFFFFFu, 1) << 24);
        x86_set_r16(c, R_CX, (uint16_t)(base >> 16));
        x86_set_r16(c, R_DX, (uint16_t)base);
        break;
    }
    case 0x0007: {                                     /* set segment base */
        int i = ldt_index(x86_get_r16(c, R_BX));
        if (i < 0) { x86_set_r16(c, R_AX, 0x8022); c->eflags |= X86_CF; break; }
        uint32_t at = ldt_at(i);
        uint32_t base = ((uint32_t)x86_get_r16(c, R_CX) << 16) | x86_get_r16(c, R_DX);
        x86_wr(c, at, 2, 0xFFFFFFFFu, 2, base & 0xFFFF);
        x86_wr(c, at, 4, 0xFFFFFFFFu, 1, (base >> 16) & 0xFF);
        x86_wr(c, at, 7, 0xFFFFFFFFu, 1, (base >> 24) & 0xFF);
        break;
    }
    case 0x0008: {                                     /* set segment limit */
        int i = ldt_index(x86_get_r16(c, R_BX));
        if (i < 0) { x86_set_r16(c, R_AX, 0x8022); c->eflags |= X86_CF; break; }
        uint32_t at = ldt_at(i);
        uint32_t limit = ((uint32_t)x86_get_r16(c, R_CX) << 16) | x86_get_r16(c, R_DX);
        uint8_t flags = (uint8_t)x86_rd(c, at, 6, 0xFFFFFFFFu, 1) & 0xF0;
        if (limit > 0xFFFFF) { limit >>= 12; flags |= 0x80; }
        x86_wr(c, at, 0, 0xFFFFFFFFu, 2, limit & 0xFFFF);
        x86_wr(c, at, 6, 0xFFFFFFFFu, 1, (uint8_t)(((limit >> 16) & 0x0F) | flags));
        break;
    }
    case 0x0009: {                                     /* set access rights */
        int i = ldt_index(x86_get_r16(c, R_BX));
        if (i < 0) { x86_set_r16(c, R_AX, 0x8022); c->eflags |= X86_CF; break; }
        uint32_t at = ldt_at(i);
        uint16_t rights = x86_get_r16(c, R_CX);
        x86_wr(c, at, 5, 0xFFFFFFFFu, 1, rights & 0xFF);
        uint8_t flags = (uint8_t)x86_rd(c, at, 6, 0xFFFFFFFFu, 1) & 0x0F;
        x86_wr(c, at, 6, 0xFFFFFFFFu, 1, (uint8_t)(flags | ((rights >> 8) & 0xF0)));
        break;
    }
    case 0x0400:                                       /* version */
        x86_set_r16(c, R_AX, 0x005A);                  /* 0.90 */
        x86_set_r16(c, R_BX, 0x0001);                  /* 32-bit, no paging */
        x86_set_r8(c, R_CL, 3);
        x86_set_r16(c, R_DX, 0xFFFF);
        break;
    default:
        if (pc.debug) fprintf(stderr, "[dpmi] unimplemented INT 31h AX=%04X\n", ax);
        x86_set_r16(c, R_AX, 0x8001);
        c->eflags |= X86_CF;
        break;
    }
}
