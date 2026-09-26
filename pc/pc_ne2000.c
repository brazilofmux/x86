/* pc_ne2000.c — a Novell NE2000: National's DP8390 on the ISA bus
 *
 * -nic ne2000[,user]: at 300h, IRQ 3 — the jumpers most NE2000s and their
 * clones shipped with, and the defaults of the drivers that expect them
 * (Crynwr's packet driver, Windows for Workgroups', NT's, Linux 2.0's
 * ne.c probes there first). The card of its decade: every system this
 * machine runs has a driver for it. Behind it is what pc_net.c connects.
 *
 * The chip, as QEMU's ne2000.c models it (what today's drivers were last
 * tested against), and National's data sheet where QEMU is silent:
 * - 32 registers in four pages (CR bits 7:6 select), at base+00h-0Fh;
 *   base+10h-17h the data port, base+18h-1Fh the reset port (a read
 *   resets the chip: ISR says RST).
 * - The card's memory: the station address PROM at 0000h-001Fh (each byte
 *   twice, as a 16-bit card presents it, then 57h 57h — "NE2000"), and 32 KB
 *   of packet buffer at 4000h-BFFFh, which the host reaches only through
 *   remote DMA: RSAR the address, RBCR the count, CR's RD bits read or
 *   write, the data port moving a byte or (DCR.WTS) a word at a time. RDC
 *   in ISR when the count runs out.
 * - Transmit: CR.TXP sends TBCR bytes from page TPSR — at once; PTX in
 *   TSR, PTX in ISR.
 * - Receive: a ring of 256-byte pages from PSTART to PSTOP. A frame the
 *   address filter passes (PAR, broadcast with RCR.AB, multicast with AM
 *   and its bit in the MAR hash, anything with PRO) goes in at CURR behind
 *   a 4-byte header (status, next page, byte count), CURR advances, PRX
 *   in ISR. A ring with no room for a full frame between CURR and BNRY
 *   takes nothing: the frame waits in pc_net's queue until the driver
 *   moves BNRY on.
 * - The interrupt is ISR & IMR, a level on the IRQ line (the 8259 takes
 *   its rising edge, as on any ISA card; 8390 drivers loop on ISR, and
 *   restore IMR at the end of their handler, which raises a new edge for
 *   whatever came meanwhile).
 * No DMA to the PC's memory at all: everything crosses the data port, so
 * -V's port log covers it.
 */
#include "pc.h"
#include <string.h>

enum { IO_BASE = 0x300, IRQ = 3 };
enum { CR_STP = 0x01, CR_STA = 0x02, CR_TXP = 0x04, CR_RD = 0x38 };
enum { RD_READ = 0x08, RD_WRITE = 0x10 };
enum { ISR_PRX = 0x01, ISR_PTX = 0x02, ISR_RDC = 0x40, ISR_RST = 0x80 };
enum { RCR_AB = 0x04, RCR_AM = 0x08, RCR_PRO = 0x10 };
enum { PMEM_START = 0x4000, PMEM_END = 0xC000 };

static const uint8_t mac[6] = { 0x02, 0x44, 0x4D, 0x00, 0x00, 0x02 };   /* locally administered */

static struct {
    int enabled, line;
    uint8_t cr, isr, imr, rcr, tcr, dcr, tsr, rsr;
    uint8_t tpsr, bnry, curr;
    uint16_t pstart, pstop;         /* as addresses (page << 8) */
    uint16_t rsar, rbcr, tbcr;
    uint8_t par[6], mar[8];
    uint8_t mem[PMEM_END];
    uint64_t tx_packets, rx_packets;
} n;

static void update_irq(void) {
    int level = (n.isr & n.imr & 0x7F) != 0;
    if (level != n.line) { n.line = level; pc_irq_line(IRQ, level); }
}

static void chip_reset(void) {
    n.isr = ISR_RST;
    n.cr = CR_STP | 0x20;           /* stopped, remote DMA aborted */
    memset(n.mem, 0, 32);
    for (int i = 0; i < 6; i++) n.mem[2 * i] = n.mem[2 * i + 1] = mac[i];
    n.mem[28] = n.mem[29] = n.mem[30] = n.mem[31] = 0x57;
    update_irq();
}

/* Remote DMA: the card's memory as the data port sees it */
static uint8_t mem_rd(uint32_t a) {
    if (a < 32 || (a >= PMEM_START && a < PMEM_END)) return n.mem[a];
    return 0xFF;
}
static void mem_wr(uint32_t a, uint8_t v) {
    if (a >= PMEM_START && a < PMEM_END) n.mem[a] = v;
}
static void dma_advance(int len) {
    n.rsar = (uint16_t)(n.rsar + len);
    if (n.rsar == n.pstop) n.rsar = n.pstart;   /* a read out of the ring wraps with it */
    if (n.rbcr <= len) { n.rbcr = 0; n.isr |= ISR_RDC; update_irq(); }
    else n.rbcr = (uint16_t)(n.rbcr - len);
}

static void transmit(void) {
    uint32_t a = (uint32_t)n.tpsr << 8, len = n.tbcr;
    if (a >= PMEM_END) a -= PMEM_END - PMEM_START;
    if (a + len <= PMEM_END && len) {
        n.tx_packets++;
        if (pc_net_present()) pc_net_send(n.mem + a, len);
    }
    n.tsr = 0x01;                   /* PTX */
    n.isr |= ISR_PTX;
    n.cr &= (uint8_t)~CR_TXP;
    update_irq();
}

/* The multicast hash: the top six bits of the destination's CRC-32,
 * computed most significant bit first (the 8390's own) */
static int mcast_ok(const uint8_t *a) {
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < 6; i++) {
        uint8_t b = a[i];
        for (int k = 0; k < 8; k++, b >>= 1) {
            int carry = (int)((crc >> 31) ^ (b & 1));
            crc <<= 1;
            if (carry) crc = (crc ^ 0x04C11DB6u) | 1;
        }
    }
    int idx = (int)(crc >> 26);
    return (n.mar[idx >> 3] >> (idx & 7)) & 1;
}

/* A frame from the wire (pc_net's deliver): 1 taken or filtered out, 0
 * no room in the ring yet */
static int receive(const uint8_t *buf, uint32_t size) {
    if (!n.enabled || (n.cr & CR_STP) || size < 14) return 1;
    if (!(n.rcr & RCR_PRO)) {
        static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        if (!memcmp(buf, bcast, 6)) { if (!(n.rcr & RCR_AB)) return 1; }
        else if (buf[0] & 1) { if (!(n.rcr & RCR_AM) || !mcast_ok(buf)) return 1; }
        else if (memcmp(buf, n.par, 6)) return 1;
    }
    uint8_t pad[60];
    if (size < 60) { memcpy(pad, buf, size); memset(pad + size, 0, 60 - size); buf = pad; size = 60; }
    if (size > 1514) return 1;
    if (n.pstop <= n.pstart || n.pstop > PMEM_END) return 1;
    uint32_t index = (uint32_t)n.curr << 8, boundary = (uint32_t)n.bnry << 8;
    if (index >= PMEM_END) index = n.pstart;
    uint32_t avail = index < boundary ? boundary - index : (uint32_t)(n.pstop - n.pstart) - (index - boundary);
    if (avail < 1514 + 4) return 0;             /* full: the frame waits */
    uint32_t total = size + 4;                  /* the count includes the header */
    uint32_t next = index + ((total + 4 + 255) & ~255u);
    if (next >= n.pstop) next -= (uint32_t)(n.pstop - n.pstart);
    n.rsr = (uint8_t)(0x01 | (buf[0] & 1 ? 0x20 : 0));   /* PRX; PHY: multicast or broadcast */
    n.mem[index] = n.rsr;
    n.mem[index + 1] = (uint8_t)(next >> 8);
    n.mem[index + 2] = (uint8_t)total;
    n.mem[index + 3] = (uint8_t)(total >> 8);
    index += 4;
    while (size) {
        uint32_t len = n.pstop - index < size ? n.pstop - index : size;
        memcpy(n.mem + index, buf, len);
        buf += len; size -= len; index += len;
        if (index == n.pstop) index = n.pstart;
    }
    n.curr = (uint8_t)(next >> 8);
    n.rx_packets++;
    n.isr |= ISR_PRX;
    update_irq();
    return 1;
}

static void cr_write(uint8_t v) {
    n.cr = v;
    if (v & CR_STP) return;
    n.isr &= (uint8_t)~ISR_RST;
    if ((v & (RD_READ | RD_WRITE)) && !(v & 0x20) && n.rbcr == 0) { n.isr |= ISR_RDC; update_irq(); }
    if (v & CR_TXP) transmit();
    if (v & CR_STA) pc_net_rx_drain();          /* started: what waited can come in */
}

static uint8_t reg_read(unsigned r) {
    int page = n.cr >> 6;
    if (r == 0) return n.cr;
    if (page == 0) switch (r) {
        case 0x03: return n.bnry;
        case 0x04: return n.tsr;
        case 0x07: return n.isr;
        case 0x08: return (uint8_t)n.rsar;       /* CRDA: the remote DMA's address */
        case 0x09: return (uint8_t)(n.rsar >> 8);
        case 0x0C: return n.rsr;
        default: return 0;                       /* CLDA, NCR, FIFO, the tally counters: nothing to count */
    }
    if (page == 1) {
        if (r <= 6) return n.par[r - 1];
        if (r == 7) return n.curr;
        return n.mar[r - 8];
    }
    if (page == 2) switch (r) {
        case 0x01: return (uint8_t)(n.pstart >> 8);
        case 0x02: return (uint8_t)(n.pstop >> 8);
        case 0x04: return n.tpsr;
        case 0x0C: return n.rcr;
        case 0x0D: return n.tcr;
        case 0x0E: return n.dcr;
        case 0x0F: return n.imr;
        default: return 0;
    }
    return 0;
}

static void reg_write(unsigned r, uint8_t v) {
    int page = n.cr >> 6;
    if (r == 0) { cr_write(v); return; }
    if (page == 0) switch (r) {
        case 0x01: n.pstart = (uint16_t)(v << 8); return;
        case 0x02: n.pstop = (uint16_t)(v << 8); return;
        case 0x03: n.bnry = v; pc_net_rx_drain(); return;   /* the driver took frames: room */
        case 0x04: n.tpsr = v; return;
        case 0x05: n.tbcr = (uint16_t)((n.tbcr & 0xFF00) | v); return;
        case 0x06: n.tbcr = (uint16_t)((n.tbcr & 0x00FF) | v << 8); return;
        case 0x07: n.isr &= (uint8_t)~(v & 0x7F); update_irq(); return;   /* ones clear (not RST) */
        case 0x08: n.rsar = (uint16_t)((n.rsar & 0xFF00) | v); return;
        case 0x09: n.rsar = (uint16_t)((n.rsar & 0x00FF) | v << 8); return;
        case 0x0A: n.rbcr = (uint16_t)((n.rbcr & 0xFF00) | v); return;
        case 0x0B: n.rbcr = (uint16_t)((n.rbcr & 0x00FF) | v << 8); return;
        case 0x0C: n.rcr = v; return;
        case 0x0D: n.tcr = v; return;
        case 0x0E: n.dcr = v; return;
        case 0x0F: n.imr = v; update_irq(); return;
        default: return;
    }
    if (page == 1) {
        if (r <= 6) n.par[r - 1] = v;
        else if (r == 7) { n.curr = v; pc_net_rx_drain(); }
        else n.mar[r - 8] = v;
    }
}

int pc_ne2000_port_read(uint16_t port, int size, uint32_t *val) {
    if (!n.enabled || port < IO_BASE || port >= IO_BASE + 0x20) return 0;
    unsigned o = port - IO_BASE;
    if (o < 0x10) *val = reg_read(o);
    else if (o < 0x18) {                         /* the data port */
        int w = size == 4 ? 4 : (n.dcr & 1) ? 2 : 1;
        uint32_t a = w > 1 ? n.rsar & ~1u : n.rsar, v = 0;
        for (int i = 0; i < w; i++) v |= (uint32_t)mem_rd(a + (uint32_t)i) << (8 * i);
        *val = v;
        dma_advance(w);
    } else { chip_reset(); *val = 0; }           /* the reset port */
    return 1;
}

int pc_ne2000_port_write(uint16_t port, uint32_t val, int size) {
    if (!n.enabled || port < IO_BASE || port >= IO_BASE + 0x20) return 0;
    unsigned o = port - IO_BASE;
    if (o < 0x10) reg_write(o, (uint8_t)val);
    else if (o < 0x18) {
        int w = size == 4 ? 4 : (n.dcr & 1) ? 2 : 1;
        uint32_t a = w > 1 ? n.rsar & ~1u : n.rsar;
        for (int i = 0; i < w; i++) mem_wr(a + (uint32_t)i, (uint8_t)(val >> (8 * i)));
        dma_advance(w);
    }
    return 1;                                    /* a write to the reset port: nothing */
}

void pc_ne2000_enable(void) {
    n.enabled = 1;
    chip_reset();
    pc_net_attach(receive);
}
