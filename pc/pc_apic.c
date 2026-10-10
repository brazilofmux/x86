/* pc_apic.c — the Pentium's local APIC (the P54C's on-chip one).
 *
 * What a uniprocessor gets from it: a timer of its own (one-shot or
 * periodic, off the bus clock through a divider) and the interrupt
 * model Linux and the others prefer — vectors with priorities in the
 * processor instead of the 8259's fixed order. The 8259 pair stays: its
 * INTR reaches the processor through LINT0 in ExtINT mode ("virtual
 * wire", the way the BIOS leaves it), so everything that ran before
 * runs the same, and the APIC's own vectors are arbitrated above it. No
 * I/O APIC yet, so devices still interrupt through the 8259s; no IPIs
 * but to self.
 *
 * Registers at the base in MSR 1Bh (FEE00000h), one dword every 16
 * bytes, reached through the CPU's memory-mapped-device hook once the
 * guest has mapped the page. CPUID leaf 1 EDX bit 9 says it is there;
 * clearing the MSR's enable bit takes it away, CPUID bit and all.
 * Version 10h with four LVT entries (timer, LINT0, LINT1, error), the
 * P5's. The bus clock is 66.7 MHz, the Pentium 200's: 15 ns a tick. */
#include "pc.h"
#include <stdio.h>
#include <string.h>

#define LVT_MASKED   0x10000u
#define LVT_PERIODIC 0x20000u
#define SVR_ENABLE   0x100u

static int apic_off;                              /* -noapic */
static struct {
    int present, hw_enabled;
    uint32_t base;
    uint32_t id, tpr, ldr, dfr, svr, esr, icr_lo, icr_hi, dcr;
    uint32_t lvt[4];                              /* timer, LINT0, LINT1, error */
    uint32_t irr[8], isr[8], tmr[8];
    uint32_t timer_init;
    uint64_t timer_start_ns, timer_due_ns;        /* due: 0 when stopped */
} apic;

void pc_apic_off(void) { apic_off = 1; }
int  pc_apic_present(void) { return apic.present; }

static int highest(const uint32_t *r) {
    for (int i = 7; i >= 0; i--) if (r[i]) return i * 32 + 31 - __builtin_clz(r[i]);
    return -1;
}
static void set_bit(uint32_t *r, int v) { r[v >> 5] |= 1u << (v & 31); }
static void clr_bit(uint32_t *r, int v) { r[v >> 5] &= ~(1u << (v & 31)); }

/* The processor priority: the task priority, or the class of the
 * highest vector in service if that is higher. */
static uint32_t ppr(void) {
    int s = highest(apic.isr);
    return s >= 0 && (uint32_t)(s & 0xF0) > (apic.tpr & 0xF0) ? (uint32_t)(s & 0xF0) : apic.tpr;
}
/* The vector it would hand the processor now: the highest requested,
 * in a class above the processor priority. -1: none. */
static int deliverable(void) {
    if (!apic.hw_enabled || !(apic.svr & SVR_ENABLE)) return -1;
    int v = highest(apic.irr);
    return v >= 0 && (uint32_t)(v & 0xF0) > (ppr() & 0xF0) ? v : -1;
}
static void update(void) {
    if (deliverable() >= 0) pc.irq_pending |= 1 << 12; else pc.irq_pending &= ~(1 << 12);
    pc_intr_update();
}
static void request(int v) {
    if (v < 16) { apic.esr |= 0x40; return; }          /* received illegal vector */
    set_bit(apic.irr, v);
    update();
    if (pc.cpu) pc.cpu->jit_cur_hit = 1;                 /* taken at the next block boundary */
}

/* pc_poll's delivery: the vector, moved from requested to in service. */
int pc_apic_take(void) {
    int v = deliverable();
    if (v < 0) return -1;
    clr_bit(apic.irr, v);
    set_bit(apic.isr, v);
    update();
    if (pc.debug > 1) fprintf(stderr, "[apic] INT %02X @%llu\n", v, (unsigned long long)pc.cpu->insn_count);
    return v;
}
/* Does the 8259s' INTR reach the processor: no APIC, or one that is
 * off, or LINT0 unmasked in ExtINT mode? */
int pc_apic_extint_ok(void) {
    return !apic.present || !apic.hw_enabled || (!(apic.lvt[1] & LVT_MASKED) && ((apic.lvt[1] >> 8) & 7) == 7);
}

/* ---- Timer -------------------------------------------------------------- */
static uint64_t tick_ns(void) {
    static const int div[8] = { 2, 4, 8, 16, 32, 64, 128, 1 };
    return 15ull * (uint64_t)div[(apic.dcr & 3) | ((apic.dcr >> 1) & 4)];
}
static void timer_load(uint32_t v, uint64_t now) {
    apic.timer_init = v;
    apic.timer_start_ns = now;
    apic.timer_due_ns = v ? now + (uint64_t)v * tick_ns() : 0;
}
static uint32_t timer_count(uint64_t now) {
    if (!apic.timer_init) return 0;
    uint64_t e = (now - apic.timer_start_ns) / tick_ns();
    if (apic.lvt[0] & LVT_PERIODIC) return (uint32_t)(apic.timer_init - e % apic.timer_init);
    return e >= apic.timer_init ? 0 : (uint32_t)(apic.timer_init - e);
}
/* The timer's interrupt, when it is due. A periodic timer that fell
 * behind by more than a period (a stall) drops the missed ones, as the
 * 8259 path does for IRQ 0. */
void pc_apic_poll(uint64_t now) {
    if (!apic.timer_due_ns || now < apic.timer_due_ns) return;
    if (!(apic.lvt[0] & LVT_MASKED)) request(apic.lvt[0] & 0xFF);
    if (apic.lvt[0] & LVT_PERIODIC) {
        uint64_t p = (uint64_t)apic.timer_init * tick_ns();
        apic.timer_due_ns += p;
        if (now >= apic.timer_due_ns) apic.timer_due_ns = now + p;
    } else apic.timer_due_ns = 0;
}
uint64_t pc_apic_due(void) { return apic.timer_due_ns ? apic.timer_due_ns : UINT64_MAX; }

/* ---- Registers ---------------------------------------------------------- */
static void icr_write(uint32_t lo) {
    apic.icr_lo = lo & 0x000CDFFFu;
    int mode = (lo >> 8) & 7, shorthand = (lo >> 18) & 3, vec = lo & 0xFF;
    uint32_t dest = apic.icr_hi >> 24;
    int self = shorthand == 1 || shorthand == 2
            || (shorthand == 0 && ((lo & 0x800) ? (dest & (apic.ldr >> 24)) != 0 : dest == apic.id || dest == 0xFF));
    if (pc.debug > 1) fprintf(stderr, "[apic] ICR %08X:%08X (mode %d, %s)\n", apic.icr_hi, lo, mode, self ? "self" : "not self");
    if (!self) return;                                   /* no other processor to hear it */
    if (mode == 0 || mode == 1) request(vec);            /* fixed, lowest priority */
    /* NMI, SMI, INIT, STARTUP to self: nothing a uniprocessor's BIOS left running does */
}

int pc_apic_mmio_read(uint32_t phys, int size, uint32_t *val) {
    if (!apic.present || !apic.hw_enabled || phys - apic.base >= 0x1000) return 0;
    uint32_t off = (phys - apic.base) & ~0xFu, v = 0;
    if (off >= 0x100 && off < 0x180) v = apic.isr[(off - 0x100) >> 4];
    else if (off >= 0x180 && off < 0x200) v = apic.tmr[(off - 0x180) >> 4];
    else if (off >= 0x200 && off < 0x280) v = apic.irr[(off - 0x200) >> 4];
    else switch (off) {
    case 0x020: v = apic.id << 24; break;
    case 0x030: v = 0x00030010; break;                   /* version 10h, 4 LVT entries */
    case 0x080: v = apic.tpr; break;
    case 0x0A0: v = ppr(); break;
    case 0x0D0: v = apic.ldr; break;
    case 0x0E0: v = apic.dfr; break;
    case 0x0F0: v = apic.svr; break;
    case 0x280: v = apic.esr; break;
    case 0x300: v = apic.icr_lo; break;                  /* delivery status: always idle */
    case 0x310: v = apic.icr_hi; break;
    case 0x320: v = apic.lvt[0]; break;
    case 0x340: v = LVT_MASKED; break;                   /* no performance-counter entry on a P5 */
    case 0x350: v = apic.lvt[1]; break;
    case 0x360: v = apic.lvt[2]; break;
    case 0x370: v = apic.lvt[3]; break;
    case 0x380: v = apic.timer_init; break;
    case 0x390: v = timer_count(pc_now_ns()); break;
    case 0x3E0: v = apic.dcr; break;
    default: break;
    }
    v >>= 8 * (phys & 3);
    *val = size < 4 ? v & ((1u << (8 * size)) - 1) : v;
    return 1;
}

int pc_apic_mmio_write(uint32_t phys, int size, uint32_t val) {
    if (!apic.present || !apic.hw_enabled || phys - apic.base >= 0x1000) return 0;
    uint32_t off = (phys - apic.base) & ~0xFu;
    (void)size;
    if (pc.debug > 1 && off != 0xB0) fprintf(stderr, "[apic] %03X <- %08X @%llu\n", off, val, (unsigned long long)pc.cpu->insn_count);
    switch (off) {
    case 0x020: apic.id = (val >> 24) & 0xF; break;
    case 0x080: apic.tpr = val & 0xFF; update(); break;
    case 0x0B0: { int s = highest(apic.isr); if (s >= 0) clr_bit(apic.isr, s); update(); break; }
    case 0x0D0: apic.ldr = val & 0xFF000000u; break;
    case 0x0E0: apic.dfr = val | 0x0FFFFFFFu; break;
    case 0x0F0: apic.svr = val & 0x1FF; update(); break;
    case 0x280: apic.esr = 0; break;
    case 0x300: icr_write(val); break;
    case 0x310: apic.icr_hi = val & 0xFF000000u; break;
    case 0x320: apic.lvt[0] = val & 0x300FFu; break;
    case 0x350: apic.lvt[1] = val & 0x1A7FFu; pc_intr_update(); break;
    case 0x360: apic.lvt[2] = val & 0x1A7FFu; break;
    case 0x370: apic.lvt[3] = val & 0x100FFu; break;
    case 0x380: timer_load(val, pc_now_ns()); break;
    case 0x3E0: apic.dcr = val & 0xB; break;
    default: break;                                      /* read-only, or nothing there */
    }
    return 1;
}

/* MSR 1Bh: the base, the BSP flag, the enable bit. */
static int msr_hook(x86_cpu *c, uint32_t msr, int write, uint64_t *v) {
    if (msr != 0x1B || !apic.present) return 0;
    if (!write) { *v = apic.base | 0x100 | (apic.hw_enabled ? 0x800u : 0); return 1; }
    apic.base = (uint32_t)*v & 0xFFFFF000u;
    int en = ((uint32_t)*v >> 11) & 1;
    if (pc.debug && en != apic.hw_enabled) fprintf(stderr, "[apic] %s by MSR 1Bh @%llu\n", en ? "enabled" : "disabled", (unsigned long long)c->insn_count);
    apic.hw_enabled = en;
    c->has_apic = (uint8_t)en;
    update();
    return 1;
}

/* Power-on, as the BIOS leaves it: enabled, ID 0, software-disabled with
 * the spurious vector at FFh, LINT0 wired through (ExtINT), LINT1 the
 * NMI line, the rest masked — "virtual wire mode". */
void pc_apic_init(x86_cpu *c) {
    memset(&apic, 0, sizeof apic);
    apic.present = c->model >= X86_MODEL_586 && !apic_off;
    c->has_apic = (uint8_t)apic.present;
    c->msr_hook = apic.present ? msr_hook : NULL;
    if (!apic.present) return;
    apic.base = 0xFEE00000u;
    apic.hw_enabled = 1;
    apic.svr = 0xFF;
    apic.dfr = 0xFFFFFFFFu;
    apic.lvt[0] = LVT_MASKED;
    apic.lvt[1] = 0x700;
    apic.lvt[2] = 0x400;
    apic.lvt[3] = LVT_MASKED;
}

/* ---- The MP tables ------------------------------------------------------
 * Intel MultiProcessor Specification 1.4: the floating pointer structure
 * ("_MP_") at F000:F000 in the ROM, one of the places an OS looks, and
 * the configuration table after it — this processor with its local
 * APIC, the buses, the two local interrupt lines (ExtINT into LINT0,
 * NMI into LINT1), no I/O APIC yet. Linux takes the local APIC's timer
 * only once a table (or ACPI) has told it the APIC is part of a
 * configuration; without one it stays in "virtual wire mode with no
 * configuration" on the 8259 and the PIT. */
#define MP_FPS_OFF 0xF000
#define MP_CT_OFF  0xF010
static int mp_put(uint8_t *t, int n, const void *p, int len) { memcpy(t + n, p, (size_t)len); return n + len; }
static int mp_put32(uint8_t *t, int n, uint32_t v) { for (int i = 0; i < 4; i++) t[n + i] = (uint8_t)(v >> (8 * i)); return n + 4; }
static uint8_t mp_sum(const uint8_t *p, int len) { uint8_t s = 0; for (int i = 0; i < len; i++) s = (uint8_t)(s + p[i]); return s; }
void pc_apic_mp_table(x86_cpu *c) {
    if (!apic.present) return;
    uint8_t t[256], fps[16];
    int n = 0, entries = 0, pci = pc_pci_present();
    memset(t, 0, sizeof t);
    n = mp_put(t, n, "PCMP", 4);
    n += 2;                                              /* base table length, below */
    t[n++] = 4;                                          /* spec revision 1.4 */
    n++;                                                 /* checksum, below */
    n = mp_put(t, n, "DOSMNSTR", 8);                     /* OEM ID */
    n = mp_put(t, n, "PC          ", 12);                /* product ID */
    n = mp_put32(t, n, 0); n += 2;                       /* OEM table: none */
    n += 2;                                              /* entry count, below */
    n = mp_put32(t, n, apic.base);                       /* local APIC address */
    n += 4;                                              /* extended table length, checksum, reserved */
    /* the processor: APIC ID 0, version 10h, enabled, the bootstrap one;
     * CPUID leaf 1's signature and feature flags */
    t[n++] = 0; t[n++] = (uint8_t)apic.id; t[n++] = 0x10; t[n++] = 0x03;
    n = mp_put32(t, n, X86_586_SIGNATURE);
    n = mp_put32(t, n, (c->has_fpu ? 1u : 0u) | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 7 | 1u << 8 | 1u << 9);
    n += 8; entries++;
    /* the buses: PCI as bus 0 when there is one, ISA after it */
    int isa = 0;
    if (pci) { t[n++] = 1; t[n++] = 0; n = mp_put(t, n, "PCI   ", 6); entries++; isa = 1; }
    t[n++] = 1; t[n++] = (uint8_t)isa; n = mp_put(t, n, "ISA   ", 6); entries++;
    /* the local interrupts: the 8259s' INTR as ExtINT into every processor's
     * LINT0, the NMI line into LINT1 (polarity and trigger as the bus has them) */
    t[n++] = 4; t[n++] = 3; t[n++] = 0; t[n++] = 0; t[n++] = (uint8_t)isa; t[n++] = 0; t[n++] = 0xFF; t[n++] = 0; entries++;
    t[n++] = 4; t[n++] = 1; t[n++] = 0; t[n++] = 0; t[n++] = (uint8_t)isa; t[n++] = 0; t[n++] = 0xFF; t[n++] = 1; entries++;
    t[4] = (uint8_t)n; t[5] = (uint8_t)(n >> 8);
    t[34] = (uint8_t)entries; t[35] = (uint8_t)(entries >> 8);
    t[7] = (uint8_t)(0 - mp_sum(t, n));
    memset(fps, 0, sizeof fps);
    memcpy(fps, "_MP_", 4);
    uint32_t ct = ((uint32_t)PC_HLE_SEG << 4) + MP_CT_OFF;
    for (int i = 0; i < 4; i++) fps[4 + i] = (uint8_t)(ct >> (8 * i));
    fps[8] = 1;                                          /* length: 16 bytes */
    fps[9] = 4;                                          /* spec revision 1.4 */
    fps[11] = 0;                                         /* feature byte 1: a configuration table follows */
    fps[12] = 0;                                         /* feature byte 2: no IMCR — virtual wire mode */
    fps[10] = (uint8_t)(0 - mp_sum(fps, 16));
    for (int i = 0; i < 16; i++) pc_wr8(c, PC_HLE_SEG, (uint16_t)(MP_FPS_OFF + i), fps[i]);
    for (int i = 0; i < n; i++) pc_wr8(c, PC_HLE_SEG, (uint16_t)(MP_CT_OFF + i), t[i]);
}
