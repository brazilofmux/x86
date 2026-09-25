/* pc_e1000.c — an Intel 82545EM Gigabit Ethernet controller (8086:100F)
 *
 * -nic e1000: on the PCI bus at 00:03.0 (where VirtualBox puts it), INTA on
 * IRQ 11. Xinu's driver (and Linux's e1000, later) programs it; so far it
 * has nothing behind it — what it transmits is counted and dropped, and
 * nothing is received — which is enough for a system that brings its
 * network up and lives without a DHCP answer.
 *
 * The registers are the 128 KB space of BAR0; this model decodes them
 * through the I/O BAR (BAR2, 32 bytes): IOADDR at +0 selects a register,
 * IODATA at +4 reads or writes it. (BAR0 is assigned but not yet decoded:
 * a driver that uses memory-mapped registers needs that next.) Registers
 * with no behaviour here hold what they were given; the statistics
 * (4000h-40FFh) read as counts and clear when read.
 *
 * What has behaviour:
 * - CTRL.RST resets the MAC (it comes back at once); STATUS says the link
 *   is up at 1000 Mb/s, full duplex.
 * - RAL0/RAH0 hold the MAC address after reset (the EEPROM's, loaded by
 *   the hardware), and EERD reads the EEPROM: the address in words 0-2,
 *   the checksum making words 0-3Fh sum to BABAh.
 * - MDIC reaches the PHY at address 1, a Marvell 88E1011: link up,
 *   autonegotiation complete (at once), reset and restart bits that clear
 *   themselves. Other PHY addresses answer with MDIC's error bit.
 * - ICR (read, and cleared by reading), ICS, IMS, IMC: the interrupt is
 *   ICR & IMS, a level on IRQ 11 (the 8259 takes its rising edge).
 * - Transmit: a write to TDT, with TCTL.EN, sends the descriptors from TDH
 *   up to it — legacy descriptors, and extended data ones (context
 *   descriptors are skipped); a packet ends at EOP. Each descriptor with
 *   RS gets DD written back into its status; then TXDW, and TXQE when the
 *   ring is empty. The descriptors and buffers are read by DMA
 *   (pc_pci_dma_read), the write-backs written the same way.
 * - Receive: the ring is set up and waits.
 */
#include "pc.h"
#include <stdio.h>
#include <string.h>

enum {
    CTRL = 0x0000, STATUS = 0x0008, EECD = 0x0010, EERD = 0x0014, MDIC = 0x0020,
    ICR = 0x00C0, ICS = 0x00C8, IMS = 0x00D0, IMC = 0x00D8,
    RCTL = 0x0100, TCTL = 0x0400,
    RDBAL = 0x2800, RDBAH = 0x2804, RDLEN = 0x2808, RDH = 0x2810, RDT = 0x2818,
    TDBAL = 0x3800, TDBAH = 0x3804, TDLEN = 0x3808, TDH = 0x3810, TDT = 0x3818,
    GPTC = 0x4080, TPT = 0x40D4, RAL0 = 0x5400, RAH0 = 0x5404,
};
enum { ICR_TXDW = 1, ICR_TXQE = 2, ICR_LSC = 4 };

#define REGS (0x20000 / 4)

static const uint8_t mac[6] = { 0x02, 0x44, 0x4D, 0x00, 0x00, 0x01 };   /* locally administered */

static struct {
    int enabled;
    pc_pci_dev pci;
    uint32_t ioaddr;                /* IOADDR: the register IODATA reaches */
    uint32_t r[REGS];
    uint16_t phy[32];
    uint16_t eeprom[64];
    uint8_t  pkt[16384]; uint32_t pkt_len;   /* a packet being gathered from descriptors */
    int line;
    uint64_t tx_packets, tx_bytes;
} e;

#define R(off) e.r[(off) / 4]

static void update_irq(void) {
    int level = (R(ICR) & R(IMS)) && !(e.pci.cfg[5] & 0x04);
    if (level != e.line) { e.line = level; pc_irq_line(e.pci.irq, level); }
}

static void phy_reset(void) {
    memset(e.phy, 0, sizeof e.phy);
    e.phy[0x00] = 0x1140;           /* control: autonegotiation on, full duplex, 1000 */
    e.phy[0x01] = 0x796D;           /* status: link up, autonegotiation complete */
    e.phy[0x02] = 0x0141;           /* ID: Marvell 88E1011 */
    e.phy[0x03] = 0x0C20;
    e.phy[0x04] = 0x0DE1;           /* our abilities */
    e.phy[0x05] = 0x41E1;           /* the link partner's */
    e.phy[0x06] = 0x0001;
    e.phy[0x09] = 0x0E00;           /* 1000BASE-T control */
    e.phy[0x0A] = 0x3C00;           /* ... status */
    e.phy[0x0F] = 0x3000;
    e.phy[0x10] = 0x0360;           /* M88: PHY specific control */
    e.phy[0x11] = 0xAC00;           /* ... status: 1000, full duplex, resolved, link */
    e.phy[0x14] = 0x0D60;
}

static void mac_reset(void) {
    memset(e.r, 0, sizeof e.r);
    R(CTRL) = 0x00000240;           /* SLU, speed 1000 */
    R(STATUS) = 0x00000083;         /* full duplex, link up, 1000 Mb/s */
    R(EECD) = 0x00000100;           /* the EEPROM is present */
    R(0x1000) = 0x00000030;         /* PBA: 48 KB receive */
    R(RAL0) = (uint32_t)mac[0] | (uint32_t)mac[1] << 8 | (uint32_t)mac[2] << 16 | (uint32_t)mac[3] << 24;
    R(RAH0) = (uint32_t)mac[4] | (uint32_t)mac[5] << 8 | 0x80000000u;   /* address valid */
    phy_reset();
    e.pkt_len = 0;
    update_irq();
}

static void eeprom_init(void) {
    memset(e.eeprom, 0, sizeof e.eeprom);
    for (int i = 0; i < 3; i++) e.eeprom[i] = (uint16_t)(mac[2 * i] | mac[2 * i + 1] << 8);
    e.eeprom[0x0A] = 0x4408;        /* init control 1 */
    e.eeprom[0x0B] = 0x1001;        /* subsystem ID */
    e.eeprom[0x0C] = 0x8086;        /* subsystem vendor */
    e.eeprom[0x0D] = 0x100F;        /* device ID */
    e.eeprom[0x0E] = 0x8086;
    uint16_t sum = 0;
    for (int i = 0; i < 0x3F; i++) sum = (uint16_t)(sum + e.eeprom[i]);
    e.eeprom[0x3F] = (uint16_t)(0xBABA - sum);
}

static uint64_t ring_base(uint32_t lo, uint32_t hi) { return (uint64_t)R(hi) << 32 | (R(lo) & ~15u); }

/* TDT moved: send what lies between TDH and it */
static void transmit(void) {
    if (!(R(TCTL) & 2)) return;                          /* TCTL.EN */
    x86_cpu *c = pc.cpu;
    uint32_t n = R(TDLEN) / 16;
    if (!c || !n) return;
    uint64_t base = ring_base(TDBAL, TDBAH);
    int done = 0;
    for (uint32_t guard = 0; R(TDH) != R(TDT) && guard < n; guard++) {
        uint32_t h = R(TDH) % n;
        uint8_t d[16];
        pc_pci_dma_read(c, base + 16u * h, d, 16);
        uint64_t buf; memcpy(&buf, d, 8);
        uint32_t lower, upper; memcpy(&lower, d + 8, 4); memcpy(&upper, d + 12, 4);
        uint8_t cmd = (uint8_t)(lower >> 24);
        int ext = (cmd & 0x20) != 0;                     /* DEXT */
        int dtyp = (lower >> 20) & 15;
        if (!(ext && dtyp == 0)) {                       /* not a context descriptor: data */
            uint32_t len = ext ? lower & 0xFFFFF : lower & 0xFFFF;
            if (e.pkt_len + len > sizeof e.pkt) len = (uint32_t)sizeof e.pkt - e.pkt_len;
            pc_pci_dma_read(c, buf, e.pkt + e.pkt_len, len);
            e.pkt_len += len;
            if (cmd & 0x01) {                            /* EOP: the packet is whole — to nowhere, so far */
                e.tx_packets++; e.tx_bytes += e.pkt_len;
                R(GPTC)++; R(TPT)++;
                if (pc.debug) fprintf(stderr, "[e1000] sent %u bytes (dropped: no network behind the card)\n", e.pkt_len);
                e.pkt_len = 0;
            }
        }
        if (cmd & 0x08) {                                /* RS: report status */
            upper |= 1;                                  /* DD */
            pc_pci_dma_write(c, base + 16u * h + 12, &upper, 4);
        }
        R(TDH) = (h + 1) % n;
        done = 1;
    }
    if (done) {
        R(ICR) |= ICR_TXDW | (R(TDH) == R(TDT) ? ICR_TXQE : 0);
        update_irq();
    }
}

static uint32_t reg_read(uint32_t off) {
    off &= 0x1FFFC;
    uint32_t v = R(off);
    if (off == ICR) { R(ICR) = 0; update_irq(); }       /* read to clear */
    else if (off >= 0x4000 && off < 0x4100) R(off) = 0;  /* statistics clear when read */
    return v;
}

static void reg_write(uint32_t off, uint32_t v) {
    off &= 0x1FFFC;
    switch (off) {
    case CTRL:
        if (v & (1u << 26)) { mac_reset(); return; }     /* RST: done at once */
        R(CTRL) = v & ~(1u << 26);
        return;
    case STATUS: return;                                 /* read-only */
    case EERD:
        if (v & 1) {                                     /* START: the word at bits 15:8, done at once */
            unsigned a = (v >> 8) & 0xFF;
            R(EERD) = (uint32_t)(a < 64 ? e.eeprom[a] : 0xFFFF) << 16 | a << 8 | 0x10;
        }
        return;
    case MDIC: {
        unsigned phyad = (v >> 21) & 31, reg = (v >> 16) & 31, op = (v >> 26) & 3;
        uint32_t r = v & 0x0FFFFFFF;
        if (phyad != 1) r |= 1u << 30;                   /* no PHY there: error */
        else if (op == 1) {                              /* write */
            uint16_t d = (uint16_t)v;
            if (reg == 0x00) {
                if (d & 0x8000) { phy_reset(); d &= 0x7FFF; }   /* reset: clears itself */
                d &= (uint16_t)~0x0200;                  /* restart autonegotiation: complete at once */
            }
            if (reg != 0x01 && reg != 0x02 && reg != 0x03 && reg != 0x05 && reg != 0x0A && reg != 0x11)
                e.phy[reg] = d;                          /* status and ID registers are read-only */
        } else if (op == 2) r = (r & 0xFFFF0000u) | e.phy[reg];   /* read */
        R(MDIC) = r | 1u << 28;                          /* ready */
        return;
    }
    case ICR: R(ICR) &= ~v; update_irq(); return;        /* writing ones clears them */
    case ICS: R(ICR) |= v; update_irq(); return;
    case IMS: R(IMS) |= v; update_irq(); return;
    case IMC: R(IMS) &= ~v; update_irq(); return;
    case TDT: R(TDT) = v & 0xFFFF; transmit(); return;
    case TDH: case RDH: case RDT: R(off) = v & 0xFFFF; return;
    case TDLEN: case RDLEN: R(off) = v & 0xFFF80; return;
    case TCTL: R(TCTL) = v; transmit(); return;
    default: R(off) = v; return;
    }
}

static int io_read(pc_pci_dev *d, uint16_t port, int size, uint32_t *val) {
    uint32_t base = pc_pci_bar(d, 2);
    if (port < base || port >= base + 32) return 0;
    unsigned o = port - base;
    uint32_t v = o < 4 ? e.ioaddr : o < 8 ? reg_read(e.ioaddr) : 0xFFFFFFFFu;
    *val = size == 4 ? v : (v >> (8 * (o & 3))) & (size == 2 ? 0xFFFF : 0xFF);
    return 1;
}

static int io_write(pc_pci_dev *d, uint16_t port, uint32_t val, int size) {
    uint32_t base = pc_pci_bar(d, 2);
    if (port < base || port >= base + 32) return 0;
    unsigned o = port - base;
    if (size != 4) return 1;                             /* IOADDR and IODATA take dwords */
    if (o == 0) e.ioaddr = val;
    else if (o == 4) reg_write(e.ioaddr, val);
    return 1;
}

static void reset(pc_pci_dev *d) { (void)d; e.ioaddr = 0; mac_reset(); }
static void written(pc_pci_dev *d, unsigned reg) { (void)d; if (reg == 0x05) update_irq(); }   /* interrupt disable */

void pc_e1000_enable(void) {
    e.enabled = 1;
    pc_pci_dev *d = &e.pci;
    memset(d, 0, sizeof *d);
    static const uint8_t id[16] = {
        0x86, 0x80, 0x0F, 0x10,     /* 8086:100F, the 82545EM (copper) */
        0x00, 0x00, 0x20, 0x02,     /* command 0 (POST leaves I/O and mastering to the driver); status: 66 MHz, medium DEVSEL */
        0x00, 0x00, 0x00, 0x02,     /* revision 0; class 02h (network), 00h (Ethernet) */
        0x10, 0x40, 0x00, 0x00,     /* cache line 64 bytes, latency 64, header 0 */
    };
    memcpy(d->cfg, id, sizeof id);
    d->cfg[0x2C] = 0x86; d->cfg[0x2D] = 0x80;          /* subsystem 8086:1001 */
    d->cfg[0x2E] = 0x01; d->cfg[0x2F] = 0x10;
    d->cfg[0x3D] = 0x01;                                 /* INTA */
    d->cfg[0x3E] = 0xFF; d->cfg[0x3F] = 0x00;            /* min grant, max latency */
    d->bar_size[0] = 0x20000;                            /* the registers, memory-mapped (not decoded yet) */
    d->bar_size[2] = 32; d->bar_io[2] = 1;               /* ... and through IOADDR/IODATA */
    d->irq = 11;
    d->reset = reset;
    d->written = written;
    d->io_read = io_read;
    d->io_write = io_write;
    eeprom_init();
    pc_pci_add(3, d);
}

void pc_e1000_poll(void) { }                             /* nothing arrives: no network behind the card yet */
