/* pc_apic.c — the Pentium's local APICs (the P54C's on-chip one, one per
 * processor) and an I/O APIC.
 *
 * What a uniprocessor gets from it: a timer of its own (one-shot or
 * periodic, off the bus clock through a divider) and the interrupt
 * model Linux and the others prefer — vectors with priorities in the
 * processor instead of the 8259's fixed order. The 8259 pair stays: its
 * INTR reaches the bootstrap processor through LINT0 in ExtINT mode
 * ("virtual wire", the way the BIOS leaves it), so everything that ran
 * before runs the same, and the APIC's own vectors are arbitrated above
 * it. With more processors (-smp, pc_smp.c) the APICs carry the
 * interprocessor interrupts: INIT and STARTUP bring an application
 * processor up, fixed ones go to the physical ID or to whoever the
 * logical destination names, lowest-priority ones to the one with the
 * lowest processor priority.
 *
 * And an I/O APIC (an 82093AA at FEC00000h): 24 inputs, each with a
 * redirection entry naming the vector, edge or level, masked or not,
 * and the destination. The ISA lines are on inputs 1-15 (IRQ 0, the
 * PIT, on input 2 by the MP convention; input 0 carries the 8259's INTR
 * for ExtINT), the PCI cards' INTA-D on 16-19. A level input delivers
 * once, then holds its remote-IRR until the processor's EOI of that
 * vector, and delivers again if the line is still up. Both or neither:
 * -noapic.
 *
 * Registers at the base in MSR 1Bh (FEE00000h), one dword every 16
 * bytes, reached through the CPU's memory-mapped-device hook once the
 * guest has mapped the page — each processor sees its own there. CPUID
 * leaf 1 EDX bit 9 says it is there; clearing the MSR's enable bit
 * takes it away, CPUID bit and all. Version 10h with four LVT entries
 * (timer, LINT0, LINT1, error), the P5's. The bus clock is 66.7 MHz,
 * the Pentium 200's: 15 ns a tick. All of it runs under the machine
 * lock: the MMIO path holds it, the polls hold it. */
#include "pc.h"
#include <stdio.h>
#include <string.h>

#define LVT_MASKED   0x10000u
#define LVT_PERIODIC 0x20000u
#define SVR_ENABLE   0x100u

#define IOAPIC_BASE   0xFEC00000u
#define IOAPIC_ID     (PC_MAX_CPUS)                  /* past every processor's */
#define IOAPIC_INPUTS 24
static struct {
    uint32_t id, regsel;
    uint64_t rte[IOAPIC_INPUTS];                  /* redirection entries: mask bit 16, trigger 15, polarity 13, dest mode 11, delivery 8-10, vector 0-7; dest 56-63 */
    uint32_t lines, remote_irr;                   /* the inputs' levels; level entries delivered and not yet EOI'd */
} io;

static int apic_off;                              /* -noapic */
typedef struct lapic {
    x86_cpu *cpu;
    int present, hw_enabled, pending;             /* pending: a vector it would deliver now */
    uint32_t base;
    uint32_t id, tpr, ldr, dfr, svr, esr, icr_lo, icr_hi, dcr;
    uint32_t lvt[4];                              /* timer, LINT0, LINT1, error */
    uint32_t irr[8], isr[8], tmr[8];
    uint32_t timer_init;
    uint64_t timer_start_ns, timer_due_ns;        /* due: 0 when stopped */
} lapic;
static lapic la[PC_MAX_CPUS];
#define LA(c) (&la[(c)->cpu_id])

void pc_apic_off(void) { apic_off = 1; }
int  pc_apic_present(void) { return la[0].present; }

static int highest(const uint32_t *r) {
    for (int i = 7; i >= 0; i--) if (r[i]) return i * 32 + 31 - __builtin_clz(r[i]);
    return -1;
}
static void set_bit(uint32_t *r, int v) { r[v >> 5] |= 1u << (v & 31); }
static void clr_bit(uint32_t *r, int v) { r[v >> 5] &= ~(1u << (v & 31)); }

/* The processor priority: the task priority, or the class of the
 * highest vector in service if that is higher. */
static uint32_t ppr(const lapic *a) {
    int s = highest(a->isr);
    return s >= 0 && (uint32_t)(s & 0xF0) > (a->tpr & 0xF0) ? (uint32_t)(s & 0xF0) : a->tpr;
}
/* The vector it would hand the processor now: the highest requested,
 * in a class above the processor priority. -1: none. */
static int deliverable(const lapic *a) {
    if (!a->hw_enabled || !(a->svr & SVR_ENABLE)) return -1;
    int v = highest(a->irr);
    return v >= 0 && (uint32_t)(v & 0xF0) > (ppr(a) & 0xF0) ? v : -1;
}
static void update(lapic *a) {
    a->pending = deliverable(a) >= 0;
    if (a->cpu == pc.cpus[0]) {
        if (a->pending) pc.irq_pending |= 1 << 12; else pc.irq_pending &= ~(1 << 12);
        pc_intr_update();
    } else a->cpu->intr_waiting = (uint8_t)a->pending;
}
static void io_eoi(int vec);
static void request(lapic *a, int v, int level) {
    if (v < 16) { a->esr |= 0x40; return; }            /* received illegal vector */
    set_bit(a->irr, v);
    if (level) set_bit(a->tmr, v); else clr_bit(a->tmr, v);
    update(a);
    a->cpu->jit_cur_hit = 1;                             /* taken at the next block boundary */
    pc_smp_wake(a->cpu->cpu_id);                         /* ... or now, if it sleeps */
}

/* pc_poll's delivery: the vector, moved from requested to in service. */
int pc_apic_take(x86_cpu *c) {
    lapic *a = LA(c);
    int v = deliverable(a);
    if (v < 0) return -1;
    clr_bit(a->irr, v);
    set_bit(a->isr, v);
    update(a);
    if (pc.debug > 1) fprintf(stderr, "[apic%d] INT %02X @%llu\n", c->cpu_id, v, (unsigned long long)c->insn_count);
    return v;
}
int pc_apic_pending(x86_cpu *c) { return LA(c)->pending; }
/* Does the 8259s' INTR reach the bootstrap processor: no APIC, or one
 * that is off, or LINT0 unmasked in ExtINT mode (or the I/O APIC's
 * input 0 set up so)? */
static int io_extint(void);
int pc_apic_extint_ok(void) {
    const lapic *a = &la[0];
    return !a->present || !a->hw_enabled || (!(a->lvt[1] & LVT_MASKED) && ((a->lvt[1] >> 8) & 7) == 7) || io_extint();
}

/* ---- Destinations -------------------------------------------------------- */
/* The processors a message reaches: by physical APIC ID (FFh: all) or
 * by logical destination (any whose LDR shares a bit, the flat model);
 * lowest-priority delivery picks the one of those with the lowest
 * processor priority. A mask of processor indices. */
static uint32_t targets(int logical, uint32_t dest, int lowest) {
    uint32_t m = 0;
    for (int i = 0; i < pc.ncpus; i++) {
        const lapic *a = &la[i];
        if (!a->present || !a->hw_enabled) continue;
        if (logical ? (dest & (a->ldr >> 24)) != 0 : (dest == 0xFF || dest == a->id)) m |= 1u << i;
    }
    if (lowest && m) {
        int best = -1;
        for (int i = 0; i < pc.ncpus; i++)
            if ((m >> i) & 1) if (best < 0 || (ppr(&la[i]) & 0xF0) < (ppr(&la[best]) & 0xF0)) best = i;
        m = 1u << best;
    }
    return m;
}
static void deliver_to(uint32_t mask, int vec, int level) {
    for (int i = 0; i < pc.ncpus; i++) if ((mask >> i) & 1) request(&la[i], vec, level);
}

/* ---- Timer -------------------------------------------------------------- */
static uint64_t tick_ns(const lapic *a) {
    static const int div[8] = { 2, 4, 8, 16, 32, 64, 128, 1 };
    return 15ull * (uint64_t)div[(a->dcr & 3) | ((a->dcr >> 1) & 4)];
}
static void timer_load(lapic *a, uint32_t v, uint64_t now) {
    a->timer_init = v;
    a->timer_start_ns = now;
    a->timer_due_ns = v ? now + (uint64_t)v * tick_ns(a) : 0;
}
static uint32_t timer_count(const lapic *a, uint64_t now) {
    if (!a->timer_init) return 0;
    uint64_t e = (now - a->timer_start_ns) / tick_ns(a);
    if (a->lvt[0] & LVT_PERIODIC) return (uint32_t)(a->timer_init - e % a->timer_init);
    return e >= a->timer_init ? 0 : (uint32_t)(a->timer_init - e);
}
/* The timer's interrupt, when it is due. A periodic timer that fell
 * behind by more than a period (a stall) drops the missed ones, as the
 * 8259 path does for IRQ 0. */
void pc_apic_poll(x86_cpu *c, uint64_t now) {
    lapic *a = LA(c);
    if (!a->timer_due_ns || now < a->timer_due_ns) return;
    if (!(a->lvt[0] & LVT_MASKED)) request(a, a->lvt[0] & 0xFF, 0);
    if (a->lvt[0] & LVT_PERIODIC) {
        uint64_t p = (uint64_t)a->timer_init * tick_ns(a);
        a->timer_due_ns += p;
        if (now >= a->timer_due_ns) a->timer_due_ns = now + p;
    } else a->timer_due_ns = 0;
}
uint64_t pc_apic_due(x86_cpu *c) { lapic *a = LA(c); return a->timer_due_ns ? a->timer_due_ns : UINT64_MAX; }

/* ---- Registers ---------------------------------------------------------- */
static void icr_write(lapic *a, uint32_t lo) {
    a->icr_lo = lo & 0x000CDFFFu;
    int mode = (lo >> 8) & 7, shorthand = (lo >> 18) & 3, vec = lo & 0xFF, self = a->cpu->cpu_id;
    uint32_t dest = a->icr_hi >> 24, m;
    switch (shorthand) {
    case 1:  m = 1u << self; break;
    case 2:  m = (1u << pc.ncpus) - 1; break;
    case 3:  m = ((1u << pc.ncpus) - 1) & ~(1u << self); break;
    default: m = targets((lo >> 11) & 1, dest, mode == 1); break;
    }
    if (pc.debug > 1) fprintf(stderr, "[apic%d] ICR %08X:%08X (mode %d, to %X)\n", self, a->icr_hi, lo, mode, m);
    switch (mode) {
    case 0: case 1:                                      /* fixed, lowest priority */
        deliver_to(m, vec, 0);
        break;
    case 5:                                              /* INIT (a level de-assert is nothing to us) */
        if ((lo & 0x8000) && !(lo & 0x4000)) break;
        for (int i = 0; i < pc.ncpus; i++) if (((m >> i) & 1) && i != self) pc_smp_init_ipi(i);
        break;
    case 6:                                              /* STARTUP: a waiting processor begins at vector:0000 */
        for (int i = 0; i < pc.ncpus; i++) if (((m >> i) & 1) && i != self) pc_smp_sipi(i, vec);
        break;
    default:                                             /* SMI, NMI: nothing a uniprocessor's BIOS left running does */
        break;
    }
}

int pc_apic_mmio_read(x86_cpu *c, uint32_t phys, int size, uint32_t *val) {
    lapic *a = LA(c);
    if (!a->present || !a->hw_enabled || phys - a->base >= 0x1000) return 0;
    uint32_t off = (phys - a->base) & ~0xFu, v = 0;
    if (off >= 0x100 && off < 0x180) v = a->isr[(off - 0x100) >> 4];
    else if (off >= 0x180 && off < 0x200) v = a->tmr[(off - 0x180) >> 4];
    else if (off >= 0x200 && off < 0x280) v = a->irr[(off - 0x200) >> 4];
    else switch (off) {
    case 0x020: v = a->id << 24; break;
    case 0x030: v = 0x00030010; break;                   /* version 10h, 4 LVT entries */
    case 0x080: v = a->tpr; break;
    case 0x0A0: v = ppr(a); break;
    case 0x0D0: v = a->ldr; break;
    case 0x0E0: v = a->dfr; break;
    case 0x0F0: v = a->svr; break;
    case 0x280: v = a->esr; break;
    case 0x300: v = a->icr_lo; break;                    /* delivery status: always idle */
    case 0x310: v = a->icr_hi; break;
    case 0x320: v = a->lvt[0]; break;
    case 0x340: v = LVT_MASKED; break;                   /* no performance-counter entry on a P5 */
    case 0x350: v = a->lvt[1]; break;
    case 0x360: v = a->lvt[2]; break;
    case 0x370: v = a->lvt[3]; break;
    case 0x380: v = a->timer_init; break;
    case 0x390: v = timer_count(a, pc_now_ns()); break;
    case 0x3E0: v = a->dcr; break;
    default: break;
    }
    v >>= 8 * (phys & 3);
    *val = size < 4 ? v & ((1u << (8 * size)) - 1) : v;
    return 1;
}

int pc_apic_mmio_write(x86_cpu *c, uint32_t phys, int size, uint32_t val) {
    lapic *a = LA(c);
    if (!a->present || !a->hw_enabled || phys - a->base >= 0x1000) return 0;
    uint32_t off = (phys - a->base) & ~0xFu;
    (void)size;
    if (pc.debug > 1 && off != 0xB0) fprintf(stderr, "[apic%d] %03X <- %08X @%llu\n", c->cpu_id, off, val, (unsigned long long)c->insn_count);
    switch (off) {
    case 0x020: a->id = (val >> 24) & 0xF; break;
    case 0x080: a->tpr = val & 0xFF; update(a); break;
    case 0x0B0: {                                        /* EOI: the highest in service; a level-triggered one tells the I/O APIC */
        int s = highest(a->isr);
        if (pc.debug > 1) fprintf(stderr, "[apic%d] EOI %02X @%llu\n", c->cpu_id, s & 0xFF, (unsigned long long)c->insn_count);
        if (s >= 0) {
            clr_bit(a->isr, s);
            if (a->tmr[s >> 5] & (1u << (s & 31))) { clr_bit(a->tmr, s); io_eoi(s); }
        }
        update(a);
        break;
    }
    case 0x0D0: a->ldr = val & 0xFF000000u; break;
    case 0x0E0: a->dfr = val | 0x0FFFFFFFu; break;
    case 0x0F0: a->svr = val & 0x1FF; update(a); break;
    case 0x280: a->esr = 0; break;
    case 0x300: icr_write(a, val); break;
    case 0x310: a->icr_hi = val & 0xFF000000u; break;
    case 0x320: a->lvt[0] = val & 0x300FFu; break;
    case 0x350: a->lvt[1] = val & 0x1A7FFu; pc_intr_update(); break;
    case 0x360: a->lvt[2] = val & 0x1A7FFu; break;
    case 0x370: a->lvt[3] = val & 0x100FFu; break;
    case 0x380: timer_load(a, val, pc_now_ns()); break;
    case 0x3E0: a->dcr = val & 0xB; break;
    default: break;                                      /* read-only, or nothing there */
    }
    return 1;
}

/* MSR 1Bh: the base, the BSP flag, the enable bit. */
static int msr_hook(x86_cpu *c, uint32_t msr, int write, uint64_t *v) {
    lapic *a = LA(c);
    if (msr != 0x1B || !a->present) return 0;
    if (!write) { *v = a->base | (c->cpu_id == 0 ? 0x100u : 0) | (a->hw_enabled ? 0x800u : 0); return 1; }
    a->base = (uint32_t)*v & 0xFFFFF000u;
    int en = ((uint32_t)*v >> 11) & 1;
    if (pc.debug && en != a->hw_enabled) fprintf(stderr, "[apic%d] %s by MSR 1Bh @%llu\n", c->cpu_id, en ? "enabled" : "disabled", (unsigned long long)c->insn_count);
    a->hw_enabled = en;
    c->has_apic = (uint8_t)en;
    update(a);
    return 1;
}

/* Power-on, as the BIOS leaves it: enabled, ID = the processor's number,
 * software-disabled with the spurious vector at FFh, LINT0 wired through
 * (ExtINT) on the bootstrap processor and masked on the others, LINT1
 * the NMI line, the rest masked — "virtual wire mode". */
void pc_apic_init(x86_cpu *c) {
    lapic *a = LA(c);
    memset(a, 0, sizeof *a);
    a->cpu = c;
    a->present = c->model >= X86_MODEL_586 && !apic_off;
    c->has_apic = (uint8_t)a->present;
    c->msr_hook = a->present ? msr_hook : NULL;
    if (!a->present) return;
    a->base = 0xFEE00000u;
    a->hw_enabled = 1;
    a->id = c->cpu_id;
    a->svr = 0xFF;
    a->dfr = 0xFFFFFFFFu;
    a->lvt[0] = LVT_MASKED;
    a->lvt[1] = c->cpu_id == 0 ? 0x700 : 0x700 | LVT_MASKED;
    a->lvt[2] = 0x400;
    a->lvt[3] = LVT_MASKED;
    if (c->cpu_id == 0) {
        memset(&io, 0, sizeof io);
        io.id = IOAPIC_ID;
        for (int n = 0; n < IOAPIC_INPUTS; n++) io.rte[n] = LVT_MASKED;
        io.rte[0] = 0x700;                            /* input 0, the 8259s' INTR, as ExtINT: the BIOS's virtual wire through the I/O APIC too */
    }
}

/* ---- The I/O APIC -------------------------------------------------------- */
static void io_deliver(int n) {
    uint64_t e = io.rte[n];
    int mode = (int)((e >> 8) & 7), level = (int)((e >> 15) & 1), vec = (int)(e & 0xFF);
    uint32_t m = targets((int)((e >> 11) & 1), (uint32_t)(e >> 56), mode == 1);
    if (!m) m = 1;                                       /* nobody named: the bootstrap processor */
    if (pc.debug > 1) fprintf(stderr, "[ioapic] input %d -> vector %02X%s to %X\n", n, vec, level ? " (level)" : "", m);
    if (mode == 0 || mode == 1) deliver_to(m, vec, level);
    else if (pc.debug) fprintf(stderr, "[ioapic] input %d: delivery mode %d, nothing to do\n", n, mode);
}
/* A level input: deliver while it is up and not already at the processor. */
static void io_service(int n) {
    uint64_t e = io.rte[n];
    if ((e & LVT_MASKED) || !((e >> 15) & 1)) return;
    if (((io.lines >> n) & 1) && !((io.remote_irr >> n) & 1)) { io.remote_irr |= 1u << n; io_deliver(n); }
}
/* A line's level on input N: an edge entry takes the rising edge, a
 * level entry the level. */
void pc_ioapic_set(int n, int level) {
    if (!la[0].present || n < 0 || n >= IOAPIC_INPUTS) return;
    int was = (io.lines >> n) & 1;
    if (level) io.lines |= 1u << n; else io.lines &= ~(1u << n);
    uint64_t e = io.rte[n];
    if (e & LVT_MASKED) return;
    if ((e >> 15) & 1) io_service(n);
    else if (level && !was) io_deliver(n);
}
void pc_ioapic_edge(int n) { pc_ioapic_set(n, 1); pc_ioapic_set(n, 0); }
/* The processor's EOI of a level-triggered vector: the entries holding it
 * let go, and deliver again if their line is still up. */
static void io_eoi(int vec) {
    for (int n = 0; n < IOAPIC_INPUTS; n++)
        if (((io.remote_irr >> n) & 1) && (io.rte[n] & 0xFF) == (uint32_t)vec) { io.remote_irr &= ~(1u << n); io_service(n); }
}
/* Input 0 unmasked in ExtINT mode: the 8259s' vector reaches the processor this way too. */
static int io_extint(void) { return la[0].present && !(io.rte[0] & LVT_MASKED) && ((io.rte[0] >> 8) & 7) == 7; }

static uint32_t io_reg_read(uint32_t r) {
    if (r == 0 || r == 2) return io.id << 24;                     /* ID; arbitration ID */
    if (r == 1) return 0x00170011;                                /* version 11h, 24 entries */
    if (r >= 0x10 && r < 0x10 + 2 * IOAPIC_INPUTS) {
        int n = (int)(r - 0x10) >> 1;
        uint64_t e = io.rte[n];
        if (r & 1) return (uint32_t)(e >> 32);
        return (uint32_t)e | (((io.remote_irr >> n) & 1) ? 0x4000u : 0);   /* remote IRR; delivery status: idle */
    }
    return 0;
}
static void io_reg_write(uint32_t r, uint32_t v) {
    if (r == 0) { io.id = (v >> 24) & 0xF; return; }
    if (r < 0x10 || r >= 0x10 + 2 * IOAPIC_INPUTS) return;
    int n = (int)(r - 0x10) >> 1;
    uint64_t e = io.rte[n];
    if (r & 1) e = (e & 0xFFFFFFFFu) | ((uint64_t)(v & 0xFF000000u) << 32);
    else e = (e & 0xFFFFFFFF00000000ull) | (v & 0x1AFFFu);
    io.rte[n] = e;
    if (pc.debug > 1 && !(r & 1)) fprintf(stderr, "[ioapic] entry %d <- %05X (vector %02X%s%s)\n", n, (unsigned)(e & 0x1FFFF), (unsigned)(e & 0xFF), (e >> 15) & 1 ? ", level" : "", e & LVT_MASKED ? ", masked" : "");
    if (!(e & LVT_MASKED)) io_service(n);                         /* an unmasked level line that is already up */
    pc_intr_update();
}
int pc_ioapic_mmio_read(uint32_t phys, int size, uint32_t *val) {
    if (!la[0].present || phys - IOAPIC_BASE >= 0x1000) return 0;
    uint32_t off = phys - IOAPIC_BASE, v = 0;
    if (off == 0x00) v = io.regsel;
    else if (off == 0x10) v = io_reg_read(io.regsel);
    v >>= 8 * (phys & 3);
    *val = size < 4 ? v & ((1u << (8 * size)) - 1) : v;
    return 1;
}
int pc_ioapic_mmio_write(uint32_t phys, int size, uint32_t val) {
    if (!la[0].present || phys - IOAPIC_BASE >= 0x1000) return 0;
    uint32_t off = phys - IOAPIC_BASE;
    (void)size;
    if (off == 0x00) io.regsel = val & 0xFF;
    else if (off == 0x10) io_reg_write(io.regsel, val);
    return 1;
}

/* ---- The MP tables ------------------------------------------------------
 * Intel MultiProcessor Specification 1.4: the floating pointer structure
 * ("_MP_") at F000:F000 in the ROM, one of the places an OS looks, and
 * the configuration table after it — the processors with their local
 * APICs, the buses, the I/O APIC and what is wired to its inputs, the
 * two local interrupt lines (ExtINT into LINT0, NMI into LINT1). Linux
 * takes the local APIC's timer only once a table (or ACPI) has told it
 * the APIC is part of a configuration; without one it stays in "virtual
 * wire mode with no configuration" on the 8259 and the PIT. */
#define MP_FPS_OFF 0xF000
#define MP_CT_OFF  0xF010
static int mp_put(uint8_t *t, int n, const void *p, int len) { memcpy(t + n, p, (size_t)len); return n + len; }
static int mp_put32(uint8_t *t, int n, uint32_t v) { for (int i = 0; i < 4; i++) t[n + i] = (uint8_t)(v >> (8 * i)); return n + 4; }
static uint8_t mp_sum(const uint8_t *p, int len) { uint8_t s = 0; for (int i = 0; i < len; i++) s = (uint8_t)(s + p[i]); return s; }
void pc_apic_mp_table(x86_cpu *c) {
    if (!la[0].present) return;
    uint8_t t[512], fps[16];
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
    n = mp_put32(t, n, la[0].base);                      /* local APIC address */
    n += 4;                                              /* extended table length, checksum, reserved */
    /* the processors: APIC ID = number, version 10h, enabled, the first
     * the bootstrap one; CPUID leaf 1's signature and feature flags */
    for (int i = 0; i < pc.ncpus; i++) {
        t[n++] = 0; t[n++] = (uint8_t)la[i].id; t[n++] = 0x10; t[n++] = (uint8_t)(i == 0 ? 0x03 : 0x01);
        n = mp_put32(t, n, X86_586_SIGNATURE);
        n = mp_put32(t, n, (c->has_fpu ? 1u : 0u) | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 7 | 1u << 8 | 1u << 9);
        n += 8; entries++;
    }
    /* the buses: PCI as bus 0 when there is one, ISA after it */
    int isa = 0;
    if (pci) { t[n++] = 1; t[n++] = 0; n = mp_put(t, n, "PCI   ", 6); entries++; isa = 1; }
    t[n++] = 1; t[n++] = (uint8_t)isa; n = mp_put(t, n, "ISA   ", 6); entries++;
    /* the I/O APIC, and its inputs: the 8259s' INTR (ExtINT) on 0, the
     * PIT's IRQ 0 on 2, the other ISA lines on their own numbers (edge,
     * active high, as the bus has them), each PCI card's interrupt pin
     * on 16 + ((slot + pin) mod 4), level, active low */
    t[n++] = 2; t[n++] = IOAPIC_ID; t[n++] = 0x11; t[n++] = 1; n = mp_put32(t, n, IOAPIC_BASE); entries++;
    t[n++] = 3; t[n++] = 3; t[n++] = 0; t[n++] = 0; t[n++] = (uint8_t)isa; t[n++] = 0; t[n++] = IOAPIC_ID; t[n++] = 0; entries++;
    for (int irq = 0; irq < 16; irq++) {
        if (irq == 2) continue;
        t[n++] = 3; t[n++] = 0; t[n++] = 0; t[n++] = 0; t[n++] = (uint8_t)isa; t[n++] = (uint8_t)irq; t[n++] = IOAPIC_ID; t[n++] = (uint8_t)(irq ? irq : 2); entries++;
    }
    if (pci)
        for (int slot = 1; slot < 32; slot++) {
            int pin = pc_pci_card_pin(slot);
            if (!pin) continue;
            t[n++] = 3; t[n++] = 0; t[n++] = 0x0F; t[n++] = 0; t[n++] = 0; t[n++] = (uint8_t)((slot << 2) | (pin - 1)); t[n++] = IOAPIC_ID; t[n++] = (uint8_t)(16 + ((slot + pin - 1) & 3)); entries++;
        }
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

/* -s with -smp: where the interrupts stand at the end. */
void pc_apic_dump(FILE *out) {
    uint64_t now = pc_now_ns();
    for (int i = 0; i < pc.ncpus; i++) {
        const lapic *a = &la[i];
        if (!a->present) continue;
        fprintf(out, "  apic%d: %s, pending %d, irr %02X, isr %02X, tpr %02X, svr %03X, ldr %02X, lvt timer %05X, timer %s%lld us, lint0 %05X\n",
                i, a->cpu->halted ? "halted" : "running", a->pending, highest(a->irr) & 0xFF, highest(a->isr) & 0xFF, a->tpr, a->svr, a->ldr >> 24,
                a->lvt[0], a->timer_due_ns ? "due in " : "off", a->timer_due_ns ? (long long)((int64_t)(a->timer_due_ns - now) / 1000) : 0, a->lvt[1]);
    }
    fprintf(out, "  ioapic: lines %06X, remote irr %06X\n", io.lines, io.remote_irr);
    for (int n = 0; n < IOAPIC_INPUTS; n++)
        if (!(io.rte[n] & LVT_MASKED)) fprintf(out, "    input %d: vector %02X%s dest %02X%s\n", n, (unsigned)(io.rte[n] & 0xFF),
                                               (io.rte[n] >> 15) & 1 ? " level" : "", (unsigned)(io.rte[n] >> 56), (io.rte[n] >> 11) & 1 ? " logical" : "");
}
