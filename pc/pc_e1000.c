/* pc_e1000.c — an Intel 82545EM Gigabit Ethernet controller (8086:100F)
 *
 * -nic e1000: on the PCI bus at 00:03.0 (where VirtualBox puts it), INTA on
 * IRQ 11. Xinu's driver and Linux's e1000 program it. Behind it is what
 * pc_net.c connects (-nic e1000,user: libslirp's NAT network), or nothing:
 * then what it sends is counted and dropped, and nothing arrives.
 *
 * The registers are the 128 KB space of BAR0, memory-mapped (Linux's way
 * in; a narrower access reads its lanes of a register, writes by merging)
 * and through the I/O BAR (BAR2, 32 bytes: IOADDR at +0 selects a
 * register, IODATA at +4 reads or writes it — Xinu's). Registers with no
 * behaviour here hold what they were given; the statistics (4000h-40FFh)
 * read as counts and clear when read.
 *
 * What has behaviour:
 * - CTRL.RST resets the MAC (it comes back at once); STATUS says the link
 *   is up at 1000 Mb/s, full duplex.
 * - The EEPROM (the address in words 0-2, the checksum making words 0-3Fh
 *   sum to BABAh): read through EERD, or bit-banged through EECD's
 *   Microwire wires as Linux does. RAL0/RAH0 hold the address after reset.
 * - MDIC reaches the PHY at address 1, a Marvell 88E1011: link up,
 *   autonegotiation complete (at once), reset and restart bits that clear
 *   themselves. Other PHY addresses answer with MDIC's error bit.
 * - ICR (read, and cleared by reading), ICS, IMS, IMC: the interrupt is
 *   ICR & IMS, a level on IRQ 11 (the 8259 takes its rising edge).
 * - Transmit: a write to TDT, with TCTL.EN, sends the descriptors from TDH
 *   up to it — legacy descriptors (with their checksum insertion), context
 *   descriptors (the offloads for the packets after them), and extended
 *   data ones; a packet ends at EOP. Its TCP/UDP and IP checksums are
 *   filled in as the packet's options ask, and with TSE it goes out as
 *   segments of the context's MSS (tx_packet). Each descriptor with RS
 *   gets DD written back; then TXDW, and TXQE when the ring is empty.
 * - Receive: frames the address filter passes (rx_accept) go into the
 *   buffers of the descriptors from RDH up to RDT, FCS and all (rx_frame),
 *   then RXT0; a frame with no room waits in pc_net's queue until RDT
 *   moves. No interrupt moderation: ITR and the delay timers are ignored.
 * Descriptors and buffers are read and written by DMA (pc_pci_dma_read,
 * pc_pci_dma_write): at once, from inside the register write that starts
 * them, or from pc_poll for what arrives.
 */
#include "pc.h"
#include <stdio.h>
#include <string.h>
#include <zlib.h>

enum {
    CTRL = 0x0000, STATUS = 0x0008, EECD = 0x0010, EERD = 0x0014, MDIC = 0x0020,
    ICR = 0x00C0, ICS = 0x00C8, IMS = 0x00D0, IMC = 0x00D8,
    RCTL = 0x0100, TCTL = 0x0400,
    RDBAL = 0x2800, RDBAH = 0x2804, RDLEN = 0x2808, RDH = 0x2810, RDT = 0x2818,
    TDBAL = 0x3800, TDBAH = 0x3804, TDLEN = 0x3808, TDH = 0x3810, TDT = 0x3818,
    GPRC = 0x4074, GPTC = 0x4080, TPR = 0x40D0, TPT = 0x40D4,
    MTA = 0x5200, RAL0 = 0x5400, RAH0 = 0x5404,
};
enum { ICR_TXDW = 1, ICR_TXQE = 2, ICR_LSC = 4, ICR_RXDMT0 = 0x10, ICR_RXT0 = 0x80 };
enum { RCTL_EN = 1 << 1, RCTL_UPE = 1 << 3, RCTL_MPE = 1 << 4, RCTL_BAM = 1 << 15, RCTL_BSEX = 1 << 25, RCTL_SECRC = 1 << 26 };

#define REGS (0x20000 / 4)

static const uint8_t mac[6] = { 0x02, 0x44, 0x4D, 0x00, 0x00, 0x01 };   /* locally administered */

static struct {
    int enabled;
    pc_pci_dev pci;
    uint32_t ioaddr;                /* IOADDR: the register IODATA reaches */
    uint32_t r[REGS];
    uint16_t phy[32];
    uint16_t eeprom[64];
    uint8_t  pkt[0x10000 + 256]; uint32_t pkt_len;   /* a packet being gathered from descriptors (TSO: up to 64K) */
    struct {                        /* the last context descriptor's offload setup */
        uint8_t ipcss, ipcso, tucss, tucso, tucmd, hdr_len;
        uint16_t ipcse, tucse, mss;
    } ctx;
    uint8_t popts;                  /* the packet's IXSM/TXSM, from its first data descriptor */
    int tse;                        /* ... and whether it is to be segmented */
    int legacy_ic; uint8_t cso, css;   /* a legacy descriptor's checksum insertion */
    int line;
    uint64_t tx_packets, tx_bytes, rx_packets, rx_bytes;
} e;

#define R(off) e.r[(off) / 4]

#define TR(...) do { if (pc.debug > 1) fprintf(stderr, "[e1000] " __VA_ARGS__); } while (0)
static void update_irq(void) {
    int level = (R(ICR) & R(IMS)) && !(e.pci.cfg[5] & 0x04);
    if (level != e.line) TR("irq %d icr %X ims %X\n", level, R(ICR), R(IMS));
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

/* The EEPROM's 64 words, laid out as an 82545EM's (QEMU's template): the
 * MAC in words 0-2, the part number, the init control words, the IDs, and
 * word 3Fh making the sum BABAh, which Linux checks before anything else. */
static void eeprom_init(void) {
    static const uint16_t tpl[64] = {
        0x0000, 0x0000, 0x0000, 0x0000, 0xffff, 0x0000, 0x0000, 0x0000,
        0x3000, 0x1000, 0x6403, 0x100F, 0x8086, 0x100F, 0x8086, 0x3040,
        0x0008, 0x2000, 0x7e14, 0x0048, 0x1000, 0x00d8, 0x0000, 0x2700,
        0x6cc9, 0x3150, 0x0722, 0x040b, 0x0984, 0x0000, 0xc000, 0x0706,
        0x1008, 0x0000, 0x0f04, 0x7fff, 0x4d01, 0xffff, 0xffff, 0xffff,
        0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
        0x0100, 0x4000, 0x121c, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
        0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0x0000,
    };
    memcpy(e.eeprom, tpl, sizeof tpl);
    for (int i = 0; i < 3; i++) e.eeprom[i] = (uint16_t)(mac[2 * i] | mac[2 * i + 1] << 8);
    uint16_t sum = 0;
    for (int i = 0; i < 0x3F; i++) sum = (uint16_t)(sum + e.eeprom[i]);
    e.eeprom[0x3F] = (uint16_t)(0xBABA - sum);
}

/* EECD: the EEPROM behind four wires, as a driver bit-bangs it (Linux's
 * e1000 on an 82545: Microwire, 64 words). Chip select up starts a
 * command; on each rising clock the driver's DI goes in — after 9 bits
 * (start bit, opcode 110b for READ, a 6-bit address) the word comes out
 * on DO, a bit per clock, most significant first. REQ is granted at once,
 * and the EEPROM is present. (QEMU's model, the one the drivers were
 * tested against.) */
static struct { uint32_t old, val_in; int bits_in, bit_out, reading; } ee;
static void eecd_write(uint32_t v) {
    uint32_t old = ee.old;
    ee.old = v & 0x07;                                   /* SK, CS, DI as last written (DO is the EEPROM's:
                                                          * a driver writes back what it read, DO and all) */
    R(EECD) = (R(EECD) & ~0x4Fu) | (v & 0x40);          /* REQ */
    if (!(v & 2)) return;                                /* chip not selected */
    if ((v ^ old) & 2) { ee.val_in = 0; ee.bits_in = 0; ee.bit_out = 0; ee.reading = 0; }   /* CS rises: a new command */
    if (!((v ^ old) & 1)) return;                        /* no clock edge */
    if (!(v & 1)) { ee.bit_out++; return; }              /* falling edge: the next bit out */
    ee.val_in = ee.val_in << 1 | ((v >> 2) & 1);
    if (++ee.bits_in == 9 && !ee.reading) {
        ee.bit_out = (int)((ee.val_in & 0x3F) << 4) - 1;
        ee.reading = ((ee.val_in >> 6) & 7) == 6;       /* start bit + READ */
    }
}
static uint32_t eecd_read(void) {
    uint32_t r = 0x180 | ee.old | (R(EECD) & 0x40);    /* present, granted */
    if (!ee.reading || ((e.eeprom[(ee.bit_out >> 4) & 0x3F] >> ((ee.bit_out & 15) ^ 15)) & 1)) r |= 0x08;   /* DO */
    return r;
}

static uint64_t ring_base(uint32_t lo, uint32_t hi) { return (uint64_t)R(hi) << 32 | (R(lo) & ~15u); }

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put_be16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* The checksum the hardware inserts: the ones'-complement sum of the
 * bytes from css through cse (0: to the end), whatever the field already
 * holds included (the driver leaves the pseudo-header's sum there, or 0),
 * stored complemented at so. */
static void put_sum(uint8_t *f, uint32_t n, uint32_t so, uint32_t css, uint32_t cse) {
    if (cse && cse < n) n = cse + 1;
    if (css >= n || so + 1 >= n) return;
    uint32_t sum = 0;
    for (uint32_t i = css; i < n; i += 2) sum += (uint32_t)f[i] << 8 | (i + 1 < n ? f[i + 1] : 0);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    sum = ~sum & 0xFFFF;
    put_be16(f + so, sum ? sum : 0xFFFF);
}

static void tx_frame(const uint8_t *f, uint32_t len) {
    TR("tx %u proto %02X\n", len, len > 23 ? f[23] : 0);
    e.tx_packets++; e.tx_bytes += len;
    R(GPTC)++; R(TPT)++;
    if (pc_net_present()) pc_net_send(f, len);
}

/* A whole packet gathered: its offloads done, onto the wire. With TSE, the
 * payload after the context's hdr_len bytes goes out mss bytes at a time,
 * each behind a copy of the header fixed up as the next segment's: IP
 * length (and IPv4 id), TCP sequence number, FIN and PSH on the last only,
 * and the TCP/UDP length added into the pseudo-header sum the driver left
 * (QEMU's e1000, which Linux's driver is exercised against). */
static void tx_packet(void) {
    uint8_t *p = e.pkt;
    uint32_t n = e.pkt_len;
    int ipv4 = e.ctx.tucmd & 0x02, tcp = e.ctx.tucmd & 0x01;
    if (e.legacy_ic) { put_sum(p, n, e.cso, e.css, 0); tx_frame(p, n); return; }
    if (!e.tse || !e.ctx.mss || e.ctx.hdr_len >= n) {
        if (e.popts & 0x02) put_sum(p, n, e.ctx.tucso, e.ctx.tucss, e.ctx.tucse);
        if (e.popts & 0x01) put_sum(p, n, e.ctx.ipcso, e.ctx.ipcss, e.ctx.ipcse);
        tx_frame(p, n);
        return;
    }
    static uint8_t seg[0x10000 + 256];
    uint32_t hl = e.ctx.hdr_len, pay = n - hl, css = e.ctx.ipcss, tss = e.ctx.tucss;
    if (tss + 20 > hl || css + 20 > hl) return;          /* nonsense setup: the packet is lost */
    uint32_t seq0 = (uint32_t)be16(p + tss + 4) << 16 | be16(p + tss + 6);
    uint16_t id0 = be16(p + css + 4), ph0 = be16(p + e.ctx.tucso);
    uint8_t flags0 = p[tss + 13];
    for (uint32_t off = 0, k = 0; off < pay; off += e.ctx.mss, k++) {
        uint32_t sz = pay - off < e.ctx.mss ? pay - off : e.ctx.mss, len = hl + sz;
        int last = off + sz == pay;
        memcpy(seg, p, hl);
        memcpy(seg + hl, p + hl + off, sz);
        if (ipv4) { put_be16(seg + css + 2, len - css); put_be16(seg + css + 4, (uint16_t)(id0 + k)); }
        else put_be16(seg + css + 4, len - css - 40);   /* IPv6: payload length */
        if (tcp) {
            uint32_t seq = seq0 + off;
            put_be16(seg + tss + 4, seq >> 16); put_be16(seg + tss + 6, seq);
            if (!last) seg[tss + 13] = flags0 & (uint8_t)~0x09;   /* FIN, PSH: the last segment's */
        } else put_be16(seg + tss + 4, len - tss);      /* UDP length */
        uint32_t ph = ph0 + (len - tss);                 /* the pseudo-header's length */
        ph = (ph & 0xFFFF) + (ph >> 16);
        put_be16(seg + e.ctx.tucso, ph);
        if (e.popts & 0x02) put_sum(seg, len, e.ctx.tucso, tss, 0);
        if (e.popts & 0x01) { if (ipv4) put_be16(seg + e.ctx.ipcso, 0); put_sum(seg, len, e.ctx.ipcso, css, e.ctx.ipcse); }
        tx_frame(seg, len);
    }
}

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
        if (ext && dtyp == 0) {                          /* context: the offloads for the packets after it */
            e.ctx.ipcss = d[0]; e.ctx.ipcso = d[1]; e.ctx.ipcse = (uint16_t)(d[2] | d[3] << 8);
            e.ctx.tucss = d[4]; e.ctx.tucso = d[5]; e.ctx.tucse = (uint16_t)(d[6] | d[7] << 8);
            e.ctx.tucmd = cmd; e.ctx.hdr_len = d[13]; e.ctx.mss = (uint16_t)(d[14] | d[15] << 8);
        } else {                                         /* data */
            uint32_t len = ext ? lower & 0xFFFFF : lower & 0xFFFF;
            if (e.pkt_len == 0) {                        /* a packet's first descriptor says what it needs */
                e.legacy_ic = !ext && (cmd & 0x04);
                e.cso = d[10]; e.css = d[13];
                e.popts = ext ? d[13] : 0;
                e.tse = ext && (cmd & 0x04);
            }
            if (e.pkt_len + len > sizeof e.pkt) len = (uint32_t)sizeof e.pkt - e.pkt_len;
            pc_pci_dma_read(c, buf, e.pkt + e.pkt_len, len);
            e.pkt_len += len;
            if (cmd & 0x01) { tx_packet(); e.pkt_len = 0; }   /* EOP: the packet is whole */
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

/* Receive filtering: promiscuous; broadcast with BAM; multicast with MPE
 * or its bit in the 4096-bit table (MTA, hashed on the address's last
 * bits as RCTL.MO says); unicast to one of the 16 valid addresses. */
static int rx_accept(const uint8_t *f) {
    uint32_t rctl = R(RCTL);
    if (rctl & RCTL_UPE) return 1;
    if (f[0] & 1) {
        static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        if ((rctl & RCTL_BAM) && !memcmp(f, bcast, 6)) return 1;
        if (rctl & RCTL_MPE) return 1;
        static const int mo[4] = { 4, 3, 2, 0 };
        uint32_t h = ((uint32_t)(f[5] << 8 | f[4]) >> mo[(rctl >> 12) & 3]) & 0xFFF;
        return (R(MTA + 4 * (h >> 5)) >> (h & 31)) & 1;
    }
    for (int i = 0; i < 16; i++) {
        uint32_t lo = R(RAL0 + 8 * i), hi = R(RAH0 + 8 * i);
        if (!(hi & 0x80000000u)) continue;
        uint8_t a[6] = { (uint8_t)lo, (uint8_t)(lo >> 8), (uint8_t)(lo >> 16), (uint8_t)(lo >> 24), (uint8_t)hi, (uint8_t)(hi >> 8) };
        if (!memcmp(f, a, 6)) return 1;
    }
    return 0;
}

/* One frame from the wire into the receive ring: padded to the minimum
 * 60 bytes, the FCS after it unless RCTL.SECRC strips it (Linux's driver
 * takes 4 bytes off every frame), across as many descriptors' buffers as
 * it needs from RDH up to RDT; each written back with its length and DD,
 * the last with EOP (and IXSM: no checksum is claimed). Then RXT0, and
 * RXDMT0 once the free descriptors are down to RCTL.RDMTS's share. 0: no
 * room — the frame waits (pc_net's queue) for the driver to give more. */
static int rx_frame(const uint8_t *frame, uint32_t flen) {
    if (!(R(RCTL) & RCTL_EN) || flen < 14) return 1;     /* not receiving: the frame is lost */
    if (!rx_accept(frame)) return 1;
    x86_cpu *c = pc.cpu;
    uint32_t n = R(RDLEN) / 16;
    if (!c || !n) return 1;
    static uint8_t f[16384 + 64];
    if (flen > 16384) return 1;
    memcpy(f, frame, flen);
    if (flen < 60) { memset(f + flen, 0, 60 - flen); flen = 60; }
    if (!(R(RCTL) & RCTL_SECRC)) {
        uint32_t fcs = (uint32_t)crc32(0, f, flen);
        f[flen] = (uint8_t)fcs; f[flen + 1] = (uint8_t)(fcs >> 8); f[flen + 2] = (uint8_t)(fcs >> 16); f[flen + 3] = (uint8_t)(fcs >> 24);
        flen += 4;
    }
    static const uint32_t bsize[4] = { 2048, 1024, 512, 256 };
    uint32_t bs = bsize[(R(RCTL) >> 16) & 3];
    if ((R(RCTL) & RCTL_BSEX) && bs != 2048) bs *= 16;
    uint32_t need = (flen + bs - 1) / bs;
    uint32_t avail = (R(RDT) % n + n - R(RDH) % n) % n;
    TR("rx %u need %u avail %u rdh %u rdt %u\n", flen, need, avail, R(RDH), R(RDT));
    if (avail < need) return 0;
    uint64_t base = ring_base(RDBAL, RDBAH);
    for (uint32_t off = 0; off < flen; off += bs) {
        uint32_t h = R(RDH) % n, len = flen - off < bs ? flen - off : bs;
        uint8_t d[16];
        pc_pci_dma_read(c, base + 16u * h, d, 16);
        uint64_t buf; memcpy(&buf, d, 8);
        pc_pci_dma_write(c, buf, f + off, len);
        uint8_t wb[8] = { (uint8_t)len, (uint8_t)(len >> 8), 0, 0, (uint8_t)(0x01 | 0x04 | (off + len == flen ? 0x02 : 0)), 0, 0, 0 };
        pc_pci_dma_write(c, base + 16u * h + 8, wb, 8);  /* length, checksum, status (DD, IXSM, EOP), errors, special */
        R(RDH) = (h + 1) % n;
    }
    e.rx_packets++; e.rx_bytes += flen;
    R(GPRC)++; R(TPR)++;
    uint32_t left = (R(RDT) % n + n - R(RDH) % n) % n;
    R(ICR) |= ICR_RXT0 | (left <= (n >> (((R(RCTL) >> 8) & 3) + 1)) ? ICR_RXDMT0 : 0);
    update_irq();
    return 1;
}

/* What the network has sent, while the ring has room for it */
static void rx_drain(void) {
    const uint8_t *f; uint32_t len;
    while ((f = pc_net_rx_peek(&len)) && rx_frame(f, len)) pc_net_rx_pop();
}

static uint32_t reg_read(uint32_t off) {
    off &= 0x1FFFC;
    if (off == EECD) return eecd_read();
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
    case EECD: eecd_write(v); return;
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
    case TDH: case RDH: R(off) = v & 0xFFFF; return;
    case RDT: R(RDT) = v & 0xFFFF; rx_drain(); return;
    case RCTL: R(RCTL) = v; rx_drain(); return;
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

/* BAR0: the same registers, memory-mapped (Linux's way in). A narrower
 * access reads its lanes of the register and writes by merging. */
static uint32_t mmio_rd(pc_pci_dev *d, int bar, uint32_t off, int size) {
    (void)d;
    if (bar != 0) return 0xFFFFFFFFu;
    uint32_t v = reg_read(off & ~3u);
    return size == 4 ? v : (v >> (8 * (off & 3))) & (size == 2 ? 0xFFFFu : 0xFFu);
}
static void mmio_wr(pc_pci_dev *d, int bar, uint32_t off, int size, uint32_t val) {
    (void)d;
    if (bar != 0) return;
    if (size != 4) {
        uint32_t sh = 8 * (off & 3), m = (size == 2 ? 0xFFFFu : 0xFFu) << sh;
        val = (R(off & 0x1FFFC) & ~m) | ((val << sh) & m);
    }
    reg_write(off & ~3u, val);
}

static void reset(pc_pci_dev *d) { (void)d; e.ioaddr = 0; memset(&ee, 0, sizeof ee); mac_reset(); }
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
    d->bar_size[0] = 0x20000;                            /* the registers, memory-mapped */
    d->bar_size[2] = 32; d->bar_io[2] = 1;               /* ... and through IOADDR/IODATA */
    d->irq = 11;
    d->reset = reset;
    d->written = written;
    d->io_read = io_read;
    d->io_write = io_write;
    d->mmio_read = mmio_rd;
    d->mmio_write = mmio_wr;
    eeprom_init();
    pc_pci_add(3, d);
}

/* From pc_poll: the network's sockets, and what it has for the card */
void pc_e1000_poll(void) {
    if (!e.enabled || !pc_net_present()) return;
    pc_net_poll(0);
    rx_drain();
}
