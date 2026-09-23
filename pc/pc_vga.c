/* pc_vga.c — the VGA as far as mode 13h and its unchained form need it
 *
 * Mode 13h is 320x200 with one byte per pixel, and in its normal "chain-4"
 * form the A000 window is simply the framebuffer: guest memory holds it and
 * nothing here has to intervene. Turn chain-4 off (sequencer register 4,
 * bit 3) — Mode X/Y, which DOOM uses — and the window becomes a view onto
 * four 64 KB bit planes: a store goes to every plane the map mask enables,
 * shaped by the graphics controller's write mode, set/reset, rotate, logical
 * function and bit mask; a read returns one plane (or a colour compare) and
 * loads the four latches.
 *
 * The planes live here. While the planar form is on, every byte of the
 * window carries X86_BM_DEVICE in the code bitmap, so a store — from the
 * interpreter or from translated code — lands in guest memory and then
 * reaches vga_store, which moves it into the planes and puts back the byte
 * the window should show: the read plane's. Guest memory is therefore
 * always a correct view for read mode 0, which is what translated code
 * reads directly. The interpreter reads through vga_read, which is exact:
 * it also loads the latches and honours read mode 1. Translated code does
 * not load latches — the one thing that differs, and -V would say so.
 *
 * The DAC (256 six-bit RGB entries, loaded through 3C8h/3C9h) and the CRTC
 * start address and pitch are kept for whoever draws the screen: today the
 * -G PNG writer. */
#include "pc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define WIN   0xA0000u
#define WLEN  0x10000u

static struct {
    uint8_t plane[4][WLEN];
    uint8_t seq[8], seq_idx;
    uint8_t gc[9], gc_idx;
    uint8_t crtc[0x19], crtc_idx;
    uint8_t latch[4];
    int     planar;                  /* mode 13h with chain-4 off */
    uint8_t dac[256][3];
    uint8_t dac_widx, dac_wcomp, dac_ridx, dac_rcomp;
} vga;

static int mode13(x86_cpu *c) { return (pc_rd8(c, PC_BDA_SEG, 0x49) & 0x7F) == 0x13; }
static int read_plane(void) { return vga.gc[4] & 3; }

/* ---- planar memory ------------------------------------------------------ */

static void refresh_view(x86_cpu *c) {
    int rp = read_plane();
    memcpy(c->mem + WIN, vga.plane[rp], WLEN);
}

uint64_t pc_vga_stores;

static void vga_store(x86_cpu *c, uint32_t p) {
    uint32_t off = p - WIN;
    pc_vga_stores++;
    uint8_t v = c->mem[p];
    /* The common case, and DOOM's every store: write mode 0 with no
     * rotate, logical function, set/reset or bit mask — the byte goes to
     * each plane the map mask enables, and the window keeps showing it
     * unless the read plane was not among them. */
    if ((vga.gc[5] & 3) == 0 && vga.gc[3] == 0 && vga.gc[1] == 0 && vga.gc[8] == 0xFF) {
        uint8_t mm = vga.seq[2];
        for (int i = 0; i < 4; i++) if (mm & (1 << i)) vga.plane[i][off] = v;
        if (!(mm & (1 << read_plane()))) c->mem[p] = vga.plane[read_plane()][off];
        return;
    }
    uint8_t mode = vga.gc[5] & 3, mask = vga.gc[8], func = (vga.gc[3] >> 3) & 3;
    uint8_t rot = vga.gc[3] & 7;
    uint8_t rv = (uint8_t)((v >> rot) | (v << ((8 - rot) & 7)));
    uint8_t sr = vga.gc[0], esr = vga.gc[1];
    if (mode == 3) { mask &= rv; }
    for (int i = 0; i < 4; i++) {
        if (!(vga.seq[2] & (1 << i))) continue;          /* map mask */
        uint8_t val, l = vga.latch[i];
        switch (mode) {
        case 1: vga.plane[i][off] = l; continue;          /* copy the latches */
        case 2: val = (v & (1 << i)) ? 0xFF : 0x00; break;
        case 3: val = (sr & (1 << i)) ? 0xFF : 0x00; break;
        default: val = (esr & (1 << i)) ? ((sr & (1 << i)) ? 0xFF : 0x00) : rv; break;
        }
        if (mode != 3) {
            switch (func) {
            case 1: val &= l; break;
            case 2: val |= l; break;
            case 3: val ^= l; break;
            }
        }
        vga.plane[i][off] = (uint8_t)((val & mask) | (l & ~mask));
    }
    c->mem[p] = vga.plane[read_plane()][off];            /* the window shows the read plane */
}

static uint8_t vga_read(x86_cpu *c, uint32_t p) {
    (void)c;
    uint32_t off = p - WIN;
    for (int i = 0; i < 4; i++) vga.latch[i] = vga.plane[i][off];
    if (!(vga.gc[5] & 0x08)) return vga.latch[read_plane()];
    /* Read mode 1: a bit is set where every plane the colour don't-care
     * register selects matches the colour compare register. */
    uint8_t r = 0xFF;
    for (int i = 0; i < 4; i++) {
        if (!(vga.gc[7] & (1 << i))) continue;
        uint8_t want = (vga.gc[2] & (1 << i)) ? 0xFF : 0x00;
        r &= (uint8_t)~(vga.latch[i] ^ want);
    }
    return r;
}

/* Chain-4 stores byte A at plane A&3, offset A; the CRTC's doubleword mode
 * then fetches it back as a linear line. Switching forms carries the picture
 * across the same way the hardware would. */
static void set_planar(x86_cpu *c, int on) {
    if (on == vga.planar) return;
    uint8_t *bm = c->code_bitmap;
    if (on) {
        for (uint32_t a = 0; a < WLEN; a++) vga.plane[a & 3][a] = c->mem[WIN + a];
        for (uint32_t a = 0; a < WLEN; a++) bm[WIN + a] |= X86_BM_DEVICE;
        c->device_store = vga_store;
        c->device_read = vga_read;
        vga.planar = 1;
        refresh_view(c);
    } else {
        for (uint32_t a = 0; a < WLEN; a++) bm[WIN + a] &= (uint8_t)~X86_BM_DEVICE;
        c->device_store = NULL;
        c->device_read = NULL;
        vga.planar = 0;
        for (uint32_t a = 0; a < WLEN; a++) c->mem[WIN + a] = vga.plane[a & 3][a];
    }
}

/* ---- registers ------------------------------------------------------------ */

static void dac_default(void) {
    /* The 16 EGA colours; the rest black until a program loads its own. */
    static const uint8_t ega[16][3] = {
        {0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},
        {21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63}};
    memset(vga.dac, 0, sizeof vga.dac);
    memcpy(vga.dac, ega, sizeof ega);
}

/* INT 10h AH=00 lands here after the BIOS data area is set up. */
void pc_vga_set_mode(x86_cpu *c, int mode) {
    set_planar(c, 0);
    memset(vga.seq, 0, sizeof vga.seq);
    memset(vga.gc, 0, sizeof vga.gc);
    memset(vga.crtc, 0, sizeof vga.crtc);
    vga.seq[2] = 0x0F;                                   /* all planes */
    vga.gc[8] = 0xFF;                                    /* bit mask: all bits from the CPU */
    if ((mode & 0x7F) == 0x13) {
        vga.seq[4] = 0x0E;                               /* chain-4, extended memory, odd/even off */
        vga.gc[5] = 0x40;                                /* 256-colour shift */
        vga.gc[6] = 0x05;                                /* graphics, A000 64K */
        vga.crtc[0x13] = 0x28;                           /* 40 words a line */
        vga.crtc[0x14] = 0x40;                           /* doubleword mode */
        vga.crtc[0x17] = 0xA3;
        dac_default();
        if (!(mode & 0x80)) {
            memset(c->mem + WIN, 0, 320 * 200);
            memset(vga.plane, 0, sizeof vga.plane);
        }
    }
}

int pc_vga_port_read(uint16_t port, uint32_t *val) {
    switch (port) {
    case 0x3C4: *val = vga.seq_idx; return 1;
    case 0x3C5: *val = vga.seq[vga.seq_idx & 7]; return 1;
    case 0x3CE: *val = vga.gc_idx; return 1;
    case 0x3CF: *val = vga.gc_idx < 9 ? vga.gc[vga.gc_idx] : 0xFF; return 1;
    case 0x3D4: *val = vga.crtc_idx; return 1;
    case 0x3D5: *val = vga.crtc_idx < sizeof vga.crtc ? vga.crtc[vga.crtc_idx] : 0xFF; return 1;
    case 0x3C7: *val = 0; return 1;                      /* DAC state: nobody looks */
    case 0x3C8: *val = vga.dac_widx; return 1;
    case 0x3C9:
        *val = vga.dac[vga.dac_ridx][vga.dac_rcomp];
        if (++vga.dac_rcomp == 3) { vga.dac_rcomp = 0; vga.dac_ridx++; }
        return 1;
    }
    return 0;
}

int pc_vga_port_write(uint16_t port, uint32_t val, int size) {
    x86_cpu *c = pc.cpu;
    /* A word OUT to an index port writes the index, then the data port. */
    if (size == 2 && (port == 0x3C4 || port == 0x3CE || port == 0x3D4)) {
        pc_vga_port_write(port, val & 0xFF, 1);
        return pc_vga_port_write((uint16_t)(port + 1), (val >> 8) & 0xFF, 1);
    }
    uint8_t v = (uint8_t)val;
    switch (port) {
    case 0x3C4: vga.seq_idx = v; return 1;
    case 0x3C5:
        vga.seq[vga.seq_idx & 7] = v;
        if ((vga.seq_idx & 7) == 4) set_planar(c, mode13(c) && !(v & 0x08));
        return 1;
    case 0x3CE: vga.gc_idx = v; return 1;
    case 0x3CF:
        if (vga.gc_idx < 9) {
            int was = read_plane();
            vga.gc[vga.gc_idx] = v;
            if (vga.planar && read_plane() != was) refresh_view(c);
        }
        return 1;
    case 0x3D4: vga.crtc_idx = v; return 1;
    case 0x3D5: if (vga.crtc_idx < sizeof vga.crtc) vga.crtc[vga.crtc_idx] = v; return 1;
    case 0x3C7: vga.dac_ridx = v; vga.dac_rcomp = 0; return 1;
    case 0x3C8: vga.dac_widx = v; vga.dac_wcomp = 0; return 1;
    case 0x3C9:
        vga.dac[vga.dac_widx][vga.dac_wcomp] = (uint8_t)(v & 0x3F);
        if (++vga.dac_wcomp == 3) { vga.dac_wcomp = 0; vga.dac_widx++; }
        return 1;
    }
    return 0;
}

/* ---- the picture ------------------------------------------------------------ */

/* The pixel the CRT would show at (x, y) in mode 13h: linear when chained;
 * when not, plane x&3 at the CRTC start address plus y lines of the CRTC's
 * pitch (register 13h, in words) plus x/4. */
static uint8_t pixel(x86_cpu *c, int x, int y) {
    if (!vga.planar) return x86_phys_rd8(c, WIN + (uint32_t)(y * 320 + x));
    uint32_t start = ((uint32_t)vga.crtc[0x0C] << 8) | vga.crtc[0x0D];
    uint32_t pitch = (uint32_t)vga.crtc[0x13] * 2;
    return vga.plane[x & 3][(start + (uint32_t)y * pitch + (uint32_t)(x >> 2)) & 0xFFFF];
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t be[4] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len };
    fwrite(be, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, data, len);
    uint8_t cb[4] = { (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc };
    fwrite(cb, 1, 4, f);
}

/* The mode 13h screen as an 8-bit RGB PNG: 0, or -1 if the screen is not in
 * mode 13h or the file cannot be written. */
/* The current mode 13h picture as 320x200 RGB24 rows: 0, or -1 if the
 * screen is not in mode 13h. The window and the PNG writer both use it. */
int pc_vga_frame(x86_cpu *c, uint8_t *rgb) {
    if (!mode13(c)) return -1;
    uint8_t lut[256][3];
    for (int i = 0; i < 256; i++)
        for (int k = 0; k < 3; k++) {
            uint8_t v = vga.dac[i][k];
            lut[i][k] = (uint8_t)((v << 2) | (v >> 4));  /* 6 bits to 8 */
        }
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 320; x++)
            memcpy(rgb + (y * 320 + x) * 3, lut[pixel(c, x, y)], 3);
    return 0;
}

int pc_video_png(x86_cpu *c, const char *path) {
    enum { W = 320, H = 200 };
    static uint8_t rgb[W * H * 3], raw[H * (1 + W * 3)];
    if (pc_vga_frame(c, rgb) < 0) return -1;
    for (int y = 0; y < H; y++) {
        raw[y * (1 + W * 3)] = 0;                        /* filter: none */
        memcpy(raw + y * (1 + W * 3) + 1, rgb + y * W * 3, W * 3);
    }
    uLongf zlen = compressBound(sizeof raw);
    uint8_t *z = malloc(zlen);
    if (!z || compress2(z, &zlen, raw, sizeof raw, 6) != Z_OK) { free(z); return -1; }
    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return -1; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    uint8_t ihdr[13] = { 0, 0, 1, 0x40, 0, 0, 0, 200, 8, 2, 0, 0, 0 };   /* 320x200, 8-bit RGB */
    fwrite(sig, 1, 8, f);
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zlen);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(z);
    return 0;
}
