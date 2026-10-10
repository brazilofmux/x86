/* pc_pci.c — a PCI bus: configuration mechanism #1, a host bridge, and the
 * devices that plug in
 *
 * Present only when asked for (-pci, or a PCI card such as -nic): the ISA
 * machine the DOS, Windows and NT suites boot stays one. With it, the
 * machine is what QEMU and VirtualBox present at 00:00.0, an Intel 440FX
 * host bridge (8086:1237), the IDE channel as a PIIX3 IDE function at
 * 00:01.0 (pc_ide.c), the display at 00:02.0 with -vbe (pc_vbe.c), plus
 * the cards (pc_e1000.c at 00:03.0, where VirtualBox puts its network
 * card).
 *
 * Configuration mechanism #1: a dword write to CF8h selects bus, device,
 * function and register (bit 31 enables); CFCh-CFFh read and write that
 * dword's bytes. An empty slot reads all ones, as does anything while bit
 * 31 is clear. Only bus 0 and function 0 of each device exist. What
 * software may write is what a real function lets it: the command
 * register, the cache line size and latency timer, the interrupt line,
 * and the base address registers — which keep the bits their size fixes
 * (write all ones, read back the size) and the type bits.
 *
 * POST assigns the cards' BARs and interrupt lines, as a PC BIOS does.
 * The BIOS32 Service Directory and the 32-bit PCI BIOS that protected-mode
 * software calls (tools/pcibios.asm) go in at F3000h: real instructions,
 * programming CF8h/CFCh like any other caller. The real-mode PCI BIOS,
 * INT 1Ah AH=B1h (pc_pci_int1a, from pc_bios.c's INT 1Ah), answers the
 * same functions from the host: it is what a boot loader asks whether the
 * machine has PCI at all (ReactOS's FreeLdr, which tells the kernel).
 */
#include "pc.h"
#include <stdio.h>
#include <string.h>
#include "pc_pcibios.h"

static struct {
    int present;
    uint32_t addr;                  /* CF8h */
    pc_pci_dev host;                /* 00:00.0 */
    pc_pci_dev *slot[32];
} pci;

void pc_pci_enable(void) { pci.present = 1; }
int  pc_pci_present(void) { return pci.present; }

void pc_pci_add(int dev, pc_pci_dev *d) {
    pci.present = 1;
    pci.slot[dev & 31] = d;
}

uint32_t pc_pci_bar(const pc_pci_dev *d, int i) {
    uint32_t v;
    memcpy(&v, d->cfg + 0x10 + 4 * i, 4);
    return v & (d->bar_io[i] ? ~3u : ~15u);
}

static void cfg_write8(pc_pci_dev *d, unsigned reg, uint8_t v) {
    if (reg >= 0x10 && reg < 0x28) {                     /* a BAR byte: the size's bits stay 0 */
        int i = (int)(reg - 0x10) / 4;
        if (!d->bar_size[i]) return;                      /* not implemented: reads 0 */
        uint32_t mask = ~(d->bar_size[i] - 1) & (d->bar_io[i] ? ~3u : ~15u);
        unsigned sh = 8 * ((reg - 0x10) & 3);
        uint32_t cur; memcpy(&cur, d->cfg + (reg & ~3u), 4);
        cur = (cur & ~(0xFFu << sh)) | ((uint32_t)v << sh);
        cur = (cur & mask) | (d->bar_io[i] ? 1u : 0u);
        memcpy(d->cfg + (reg & ~3u), &cur, 4);
    }
    else if (reg == 0x04) d->cfg[reg] = v & 0x07;         /* I/O, memory, bus master */
    else if (reg == 0x05) d->cfg[reg] = v & 0x04;         /* interrupt disable */
    else if (reg == 0x0C || reg == 0x0D || reg == 0x3C) d->cfg[reg] = v;
    else return;
    if (d->written) d->written(d, reg);
}

/* the selected function, or NULL for an empty slot */
static pc_pci_dev *selected(void) {
    if (!(pci.addr & 0x80000000u)) return NULL;
    uint32_t bus = (pci.addr >> 16) & 0xFF, dev = (pci.addr >> 11) & 0x1F, fn = (pci.addr >> 8) & 7;
    if (bus != 0 || fn != 0) return NULL;
    return pci.slot[dev];
}

/* A card's interrupt pin: INTA-D, routed to its ISA IRQ at the 8259s (as
 * a PIIX's PIRQ router would), and wired to I/O APIC input 16 + ((slot +
 * pin) mod 4), as PIIX boards wire them. */
int pc_pci_card_pin(int dev) {
    pc_pci_dev *d = dev > 0 && dev < 32 ? pci.slot[dev] : NULL;
    return d && d->cfg[0x3D] >= 1 && d->cfg[0x3D] <= 4 ? d->cfg[0x3D] : 0;
}
void pc_pci_irq_line(pc_pci_dev *d, int level) {
    int slot = 0;
    for (int s = 1; s < 32; s++) if (pci.slot[s] == d) slot = s;
    int pin = pc_pci_card_pin(slot);
    pc_irq_line_to(d->irq, level, pin ? 16 + ((slot + pin - 1) & 3) : -1);
}

/* The cards' memory BARs, from the CPU's accesses above memory
 * (x86_cpu.mmio_read/mmio_write): a BAR decodes only while its card's
 * command register enables memory, as on the bus. */
static pc_pci_dev *mmio_find(uint32_t phys, int *bar, uint32_t *off) {
    for (int s = 1; s < 32; s++) {
        pc_pci_dev *d = pci.slot[s];
        if (!d || !d->mmio_read || !(d->cfg[4] & 2)) continue;
        for (int i = 0; i < 6; i++) {
            if (!d->bar_size[i] || d->bar_io[i]) continue;
            uint32_t base = pc_pci_bar(d, i);
            if (phys - base < d->bar_size[i]) { *bar = i; *off = phys - base; return d; }
        }
    }
    return NULL;
}
int pc_pci_mmio_read(x86_cpu *c, uint32_t phys, int size, uint32_t *val) {
    (void)c;
    int bar; uint32_t off;
    pc_pci_dev *d = mmio_find(phys, &bar, &off);
    if (!d) return 0;
    *val = d->mmio_read(d, bar, off, size);
    return 1;
}
void pc_pci_mmio_write(x86_cpu *c, uint32_t phys, int size, uint32_t val) {
    (void)c;
    int bar; uint32_t off;
    pc_pci_dev *d = mmio_find(phys, &bar, &off);
    if (d && d->mmio_write) d->mmio_write(d, bar, off, size, val);
}

/* POST: the host bridge; each card's BARs (I/O from C000h, memory from
 * FEB00000h down, each aligned to its size) and interrupt line, the way
 * a PC BIOS leaves them; the BIOS32 directory and the PCI BIOS */
void pc_pci_post(x86_cpu *c) {
    if (!pci.present) return;
    pc_ide_pci_attach();
    pc_vbe_pci_attach(c);
    pc_pci_dev *h = &pci.host;
    memset(h, 0, sizeof *h);
    static const uint8_t id[16] = {
        0x86, 0x80, 0x37, 0x12,     /* vendor 8086h, device 1237h: 82441FX */
        0x06, 0x00, 0x00, 0x02,     /* command: memory, bus master; status: medium DEVSEL */
        0x02, 0x00, 0x00, 0x06,     /* revision 2; class 06h (bridge), subclass 00h (host) */
        0x00, 0x00, 0x00, 0x00,     /* header type 0, single function */
    };
    memcpy(h->cfg, id, sizeof id);
    pci.slot[0] = h;
    pci.addr = 0;
    uint32_t io = 0xC000, mem = 0xFEC00000;
    for (int s = 1; s < 32; s++) {
        pc_pci_dev *d = pci.slot[s];
        if (!d) continue;
        if (d->reset) d->reset(d);
        for (int i = 0; i < 6; i++) {
            uint32_t sz = d->bar_size[i], base;
            if (!sz) continue;
            if (d->bar_fixed[i]) base = d->bar_fixed[i];
            else if (d->bar_io[i]) { io = (io + sz - 1) & ~(sz - 1); base = io; io += sz; }
            else { mem = (mem - sz) & ~(sz - 1); base = mem; }
            for (int b = 0; b < 4; b++) cfg_write8(d, 0x10 + 4 * (unsigned)i + (unsigned)b, (uint8_t)(base >> (8 * b)));
        }
        if (d->cfg[0x3D]) cfg_write8(d, 0x3C, d->irq);
        if (d->mmio_read) pc_mmio_install(c);
    }
    for (size_t i = 0; i < sizeof pcibios; i++) pc_wr8(c, PC_STUB_SEG, (uint16_t)(PC_STUB_PCIBIOS + i), pcibios[i]);
    uint8_t sum = 0;
    for (int i = 0; i < 16; i++) sum = (uint8_t)(sum + pcibios[i]);
    pc_wr8(c, PC_STUB_SEG, (uint16_t)(PC_STUB_PCIBIOS + 10), (uint8_t)(0 - sum));
}

int pc_pci_port_read(uint16_t port, int size, uint32_t *val) {
    if (!pci.present) return 0;
    if (port == 0xCF8 && size == 4) { *val = pci.addr; return 1; }
    if (port >= 0xCFC && port <= 0xCFF) {
        pc_pci_dev *d = selected();
        unsigned reg = (pci.addr & 0xFC) + (port & 3u);
        uint32_t v = 0;
        for (int i = 0; i < size; i++) v |= (uint32_t)(d && reg + (unsigned)i < 256 ? d->cfg[reg + (unsigned)i] : 0xFF) << (8 * i);
        *val = v;
        if (pc.debug > 1 && d) fprintf(stderr, "[pci] %02X:%02X.%u cfg %02X -> %0*X\n", (pci.addr >> 16) & 0xFF, (pci.addr >> 11) & 0x1F, (pci.addr >> 8) & 7, reg, 2 * size, v);
        return 1;
    }
    /* the cards' I/O BARs */
    for (int s = 1; s < 32; s++) {
        pc_pci_dev *d = pci.slot[s];
        if (d && d->io_read && (d->cfg[4] & 1) && d->io_read(d, port, size, val)) return 1;
    }
    return 0;
}

int pc_pci_port_write(uint16_t port, uint32_t val, int size) {
    if (!pci.present) return 0;
    if (port == 0xCF8 && size == 4) {
        pci.addr = val & 0x80FFFFFCu;
        if (pc.debug > 1) fprintf(stderr, "[pci] CF8 <- %08X\n", val);
        return 1;
    }
    if (port >= 0xCFC && port <= 0xCFF) {
        pc_pci_dev *d = selected();
        unsigned reg = (pci.addr & 0xFC) + (port & 3u);
        if (d) for (int i = 0; i < size; i++) cfg_write8(d, reg + (unsigned)i, (uint8_t)(val >> (8 * i)));
        return 1;
    }
    for (int s = 1; s < 32; s++) {
        pc_pci_dev *d = pci.slot[s];
        if (d && d->io_write && (d->cfg[4] & 1) && d->io_write(d, port, val, size)) return 1;
    }
    return 0;
}

/* A card's bus-master access to memory: physical addresses (no paging,
 * no A20 folding past what the address says), and a write reaches the
 * code bitmap as a CPU store would, so translated code it overwrites is
 * retranslated. Out-of-memory bytes read as ones and are dropped. */
void pc_pci_dma_read(x86_cpu *c, uint64_t phys, void *buf, uint32_t len) {
    uint8_t *b = buf;
    for (uint32_t i = 0; i < len; i++) b[i] = phys + i < c->mem_size ? c->mem[phys + i] : 0xFF;
}
void pc_pci_dma_write(x86_cpu *c, uint64_t phys, const void *buf, uint32_t len) {
    const uint8_t *b = buf;
    c->dma_wrote = 1;
    for (uint32_t i = 0; i < len; i++) {
        if (phys + i >= c->mem_size) break;
        uint32_t p = (uint32_t)(phys + i);
        c->mem[p] = b[i];
        if (c->code_bitmap[p]) x86_store_hook(c, p);
    }
}

/* ---- the real-mode PCI BIOS: INT 1Ah AH=B1h ------------------------------ */

/* configuration space at bus/device/function BH:BL, register REG: SIZE
 * bytes, all ones where nothing answers */
static pc_pci_dev *function_at(unsigned bus, unsigned devfn) {
    return bus == 0 && (devfn & 7) == 0 ? pci.slot[devfn >> 3] : NULL;
}
static uint32_t cfg_read(pc_pci_dev *d, unsigned reg, int size) {
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)(d && reg + (unsigned)i < 256 ? d->cfg[reg + (unsigned)i] : 0xFF) << (8 * i);
    return v;
}

/* Returns 1 when the function was one of the PCI BIOS's (AL): CF and AH
 * as the PCI BIOS Specification 2.1 has them — AH 0 on success, else the
 * error, 81h for a function it has not got. */
int pc_pci_int1a(x86_cpu *c) {
    if (!pci.present) return 0;
    enum { OK = 0x00, NOT_SUPP = 0x81, BAD_VENDOR = 0x83, NOT_FOUND = 0x86, BAD_REG = 0x87 };
    unsigned al = x86_get_r8(c, R_AL);
    uint8_t ah = NOT_SUPP;
    unsigned bus = x86_get_r8(c, R_BH), devfn = x86_get_r8(c, R_BL);
    unsigned reg = x86_get_r16(c, R_DI);
    switch (al) {
    case 0x01:                                           /* installation check */
        x86_set_r8(c, R_AL, 0x01);                       /* mechanism #1 */
        x86_set_r16(c, R_BX, 0x0210);                    /* version 2.10 */
        x86_set_r8(c, R_CL, 0x00);                       /* the last bus */
        c->r[R_DX] = 0x20494350;                         /* "PCI " */
        ah = OK;
        break;
    case 0x02: case 0x03: {                              /* find device (vendor DX, device CX), find class (ECX), the SI-th */
        unsigned n = x86_get_r16(c, R_SI);
        if (al == 0x02 && x86_get_r16(c, R_DX) == 0xFFFF) { ah = BAD_VENDOR; break; }
        ah = NOT_FOUND;
        for (unsigned s = 0; s < 32 && ah != OK; s++) {
            pc_pci_dev *d = pci.slot[s];
            if (!d) continue;
            int hit = al == 0x02 ? cfg_read(d, 0, 4) == ((c->r[R_CX] & 0xFFFF) << 16 | x86_get_r16(c, R_DX))
                                 : (cfg_read(d, 8, 4) >> 8) == (c->r[R_CX] & 0xFFFFFF);
            if (!hit) continue;
            if (n--) continue;
            x86_set_r8(c, R_BH, 0); x86_set_r8(c, R_BL, (uint8_t)(s << 3));
            ah = OK;
        }
        break;
    }
    case 0x08: case 0x09: case 0x0A: {                   /* read byte, word, dword at DI */
        int size = al == 0x08 ? 1 : al == 0x09 ? 2 : 4;
        if (reg > 255 || (reg & (unsigned)(size - 1))) { ah = BAD_REG; break; }
        uint32_t v = cfg_read(function_at(bus, devfn), reg, size);
        if (size == 1) x86_set_r8(c, R_CL, v); else if (size == 2) x86_set_r16(c, R_CX, v); else c->r[R_CX] = v;
        ah = OK;
        break;
    }
    case 0x0B: case 0x0C: case 0x0D: {                   /* write byte, word, dword */
        int size = al == 0x0B ? 1 : al == 0x0C ? 2 : 4;
        if (reg > 255 || (reg & (unsigned)(size - 1))) { ah = BAD_REG; break; }
        pc_pci_dev *d = function_at(bus, devfn);
        if (d) for (int i = 0; i < size; i++) cfg_write8(d, reg + (unsigned)i, (uint8_t)(c->r[R_CX] >> (8 * i)));
        ah = OK;
        break;
    }
    default: break;                                      /* special cycle, the IRQ routing table: not here */
    }
    x86_set_r8(c, R_AH, ah);
    if (ah == OK) c->eflags &= ~X86_CF; else c->eflags |= X86_CF;
    return 1;
}
