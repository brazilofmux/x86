/* pc_vga.c — the VGA as far as mode 13h, its unchained form and the
 * 16-colour planar modes (12h: 640x480, Windows' VGA driver) need it
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
 * Mode 12h (and the EGA modes 0Dh, 0Eh, 10h) is planar from the start:
 * each byte of a plane holds eight pixels, one bit of each pixel's
 * four-bit colour, through the attribute controller's palette (and its
 * colour plane enable) into the DAC.
 *
 * The DAC (256 six-bit RGB entries, loaded through 3C8h/3C9h) and the CRTC
 * start address and pitch are kept for whoever draws the screen: the
 * window and the -G PNG writer.
 *
 * Text modes (pc_vga_text_frame): the cell buffer at B800 (B000 for mode
 * 7) from the CRTC's start address, drawn as the VGA does — 9-dot cells,
 * the ninth column repeating the eighth for the line-drawing characters
 * C0-DF, the 8x16/8x14/8x8 font the BIOS data area says is loaded — with
 * colours through the attribute controller's palette into the DAC,
 * attribute bit 7 as blink or bright background per the AC mode control
 * register, and the CRTC's cursor (location, start/end scan lines,
 * disable bit). The BIOS keeps those registers as a real one does. */
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
    int     planar;                  /* mode 13h with chain-4 off, or a 16-colour mode */
    uint8_t dac[256][3];
    uint8_t dac_widx, dac_wcomp, dac_ridx, dac_rcomp;
    uint8_t ac[0x15], ac_idx, ac_flip;   /* attribute controller: 3C0h index/data flip-flop */
    uint8_t misc;                    /* miscellaneous output: written at 3C2h, read at 3CCh */
} vga;

/* The standard VGA BIOS parameter tables: what a mode set loads into the
 * miscellaneous output, sequencer 1-4, CRTC 0-18h, graphics controller
 * 0-8 and attribute controller 0-14h. Software that saves and restores
 * the VGA (Windows' VDD, in 386 enhanced mode) reads them back and works
 * out the screen from them — a CRTC left at zero is a one-line display. */
static const struct vga_regs {
    uint8_t mode, misc, seq[4], crtc[25], gc[9], ac[21];
} vga_tables[] = {
    { 0x01, 0x67, { 0x08, 0x03, 0x00, 0x02 },
      { 0x2D,0x27,0x28,0x90,0x2B,0xA0,0xBF,0x1F,0x00,0x4F,0x0D,0x0E,0x00,0x00,0x00,0x00,0x9C,0x8E,0x8F,0x14,0x1F,0x96,0xB9,0xA3,0xFF },
      { 0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF },
      { 0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,0x0C,0x00,0x0F,0x08,0x00 } },
    { 0x03, 0x67, { 0x00, 0x03, 0x00, 0x02 },
      { 0x5F,0x4F,0x50,0x82,0x55,0x81,0xBF,0x1F,0x00,0x4F,0x0D,0x0E,0x00,0x00,0x00,0x00,0x9C,0x8E,0x8F,0x28,0x1F,0x96,0xB9,0xA3,0xFF },
      { 0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF },
      { 0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,0x0C,0x00,0x0F,0x08,0x00 } },
    { 0x04, 0x63, { 0x09, 0x03, 0x00, 0x02 },
      { 0x2D,0x27,0x28,0x90,0x2B,0x80,0xBF,0x1F,0x00,0xC1,0x00,0x00,0x00,0x00,0x00,0x00,0x9C,0x8E,0x8F,0x14,0x00,0x96,0xB9,0xA2,0xFF },
      { 0x00,0x00,0x00,0x00,0x00,0x30,0x0F,0x00,0xFF },
      { 0x00,0x13,0x15,0x17,0x02,0x04,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x01,0x00,0x03,0x00,0x00 } },
    { 0x06, 0x63, { 0x01, 0x01, 0x00, 0x06 },
      { 0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0xC1,0x00,0x00,0x00,0x00,0x00,0x00,0x9C,0x8E,0x8F,0x28,0x00,0x96,0xB9,0xC2,0xFF },
      { 0x00,0x00,0x00,0x00,0x00,0x00,0x0D,0x00,0xFF },
      { 0x00,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x01,0x00,0x01,0x00,0x00 } },
    { 0x07, 0xA6, { 0x00, 0x03, 0x00, 0x02 },
      { 0x5F,0x4F,0x50,0x82,0x55,0x81,0xBF,0x1F,0x00,0x4F,0x0D,0x0E,0x00,0x00,0x00,0x00,0x9C,0x8E,0x8F,0x28,0x0F,0x96,0xB9,0xA3,0xFF },
      { 0x00,0x00,0x00,0x00,0x00,0x10,0x0A,0x00,0xFF },
      { 0x00,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x10,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x0E,0x00,0x0F,0x08,0x00 } },
    { 0x0D, 0x63, { 0x09, 0x0F, 0x00, 0x06 },
      { 0x2D,0x27,0x28,0x90,0x2B,0x80,0xBF,0x1F,0x00,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x9C,0x8E,0x8F,0x14,0x00,0x96,0xB9,0xE3,0xFF },
      { 0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0F,0xFF },
      { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x01,0x00,0x0F,0x00,0x00 } },
    { 0x0E, 0x63, { 0x01, 0x0F, 0x00, 0x06 },
      { 0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x9C,0x8E,0x8F,0x28,0x00,0x96,0xB9,0xE3,0xFF },
      { 0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0F,0xFF },
      { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x01,0x00,0x0F,0x00,0x00 } },
    { 0x10, 0xA3, { 0x01, 0x0F, 0x00, 0x06 },
      { 0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x83,0x85,0x5D,0x28,0x0F,0x63,0xBA,0xE3,0xFF },
      { 0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0F,0xFF },
      { 0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,0x01,0x00,0x0F,0x00,0x00 } },
    { 0x12, 0xE3, { 0x01, 0x0F, 0x00, 0x06 },
      { 0x5F,0x4F,0x50,0x82,0x54,0x80,0x0B,0x3E,0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0xEA,0x8C,0xDF,0x28,0x00,0xE7,0x04,0xE3,0xFF },
      { 0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0F,0xFF },
      { 0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,0x01,0x00,0x0F,0x00,0x00 } },
    { 0x13, 0x63, { 0x01, 0x0F, 0x00, 0x0E },
      { 0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0x41,0x00,0x00,0x00,0x00,0x00,0x00,0x9C,0x8E,0x8F,0x28,0x40,0x96,0xB9,0xA3,0xFF },
      { 0x00,0x00,0x00,0x00,0x00,0x40,0x05,0x0F,0xFF },
      { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x41,0x00,0x0F,0x00,0x00 } },
};

/* tools/vgabios.asm, assembled: programs a mode's table through OUTs */
static const uint8_t vgaprog[] = {0x50, 0x53, 0x51, 0x52, 0x56, 0x1e, 0x0e, 0x1f, 0xfc, 0x8b, 0x36, 0xf0, 0x00, 0xba, 0xc4, 0x03, 0xb8, 0x00, 0x01, 0xef, 0xba, 0xc2, 0x03, 0xac, 0xee, 0xba, 0xc4, 0x03, 0xb3, 0x01, 0x88, 0xd8, 0x8a, 0x24, 0x46, 0xef, 0xfe, 0xc3, 0x80, 0xfb, 0x05, 0x72, 0xf3, 0xb8, 0x00, 0x03, 0xef, 0xba, 0xd4, 0x03, 0xb8, 0x11, 0x00, 0xef, 0x30, 0xdb, 0x88, 0xd8, 0x8a, 0x24, 0x46, 0xef, 0xfe, 0xc3, 0x80, 0xfb, 0x19, 0x72, 0xf3, 0xba, 0xce, 0x03, 0x30, 0xdb, 0x88, 0xd8, 0x8a, 0x24, 0x46, 0xef, 0xfe, 0xc3, 0x80, 0xfb, 0x09, 0x72, 0xf3, 0xba, 0xda, 0x03, 0xec, 0xba, 0xc0, 0x03, 0x30, 0xdb, 0x88, 0xd8, 0xee, 0xac, 0xee, 0xfe, 0xc3, 0x80, 0xfb, 0x15, 0x72, 0xf4, 0xb0, 0x20, 0xee, 0x8b, 0x36, 0xf2, 0x00, 0x8b, 0x0e, 0xf4, 0x00, 0xba, 0xc8, 0x03, 0x30, 0xc0, 0xee, 0x42, 0xac, 0xee, 0xe2, 0xfc, 0x1f, 0x5e, 0x5a, 0x59, 0x5b, 0x58, 0xcf};

static const struct vga_regs *vga_table(int mode);
static void dac_default(void);
static void text_default(x86_cpu *c);

/* POST: the native half of the mode set, its tables and DAC data, in the
 * stub segment (pc.h). */
void pc_vga_rom(x86_cpu *c) {
    for (size_t i = 0; i < sizeof vgaprog; i++) pc_wr8(c, PC_STUB_SEG, (uint16_t)(PC_STUB_VGAPROG + i), vgaprog[i]);
    for (size_t t = 0; t < sizeof vga_tables / sizeof vga_tables[0]; t++) {
        const struct vga_regs *r = &vga_tables[t];
        uint16_t at = (uint16_t)(PC_STUB_VGATAB + t * 64);
        pc_wr8(c, PC_STUB_SEG, at++, r->misc);
        for (int i = 0; i < 4; i++)  pc_wr8(c, PC_STUB_SEG, at++, r->seq[i]);
        for (int i = 0; i < 25; i++) pc_wr8(c, PC_STUB_SEG, at++, r->crtc[i]);
        for (int i = 0; i < 9; i++)  pc_wr8(c, PC_STUB_SEG, at++, r->gc[i]);
        for (int i = 0; i < 21; i++) pc_wr8(c, PC_STUB_SEG, at++, r->ac[i]);
    }
    /* the DAC sets the host's mode set loads too: same values either way */
    uint8_t save[256][3]; memcpy(save, vga.dac, sizeof save);
    uint8_t ac[0x15]; memcpy(ac, vga.ac, sizeof ac);
    uint8_t crtc[0x19]; memcpy(crtc, vga.crtc, sizeof crtc);
    text_default(c);
    for (int i = 0; i < 64 * 3; i++) pc_wr8(c, PC_STUB_SEG, (uint16_t)(PC_STUB_DAC64 + i), vga.dac[i / 3][i % 3]);
    dac_default();
    for (int i = 0; i < 256 * 3; i++) pc_wr8(c, PC_STUB_SEG, (uint16_t)(PC_STUB_DAC256 + i), vga.dac[i / 3][i % 3]);
    memcpy(vga.dac, save, sizeof save); memcpy(vga.ac, ac, sizeof ac); memcpy(vga.crtc, crtc, sizeof crtc);
}

void pc_vga_rom_mode(x86_cpu *c, int mode) {
    const struct vga_regs *t = vga_table(mode);
    uint16_t idx = (uint16_t)(t - vga_tables);
    int m13 = (mode & 0x7F) == 0x13;
    pc_wr16(c, PC_STUB_SEG, PC_STUB_VGAVARS, (uint16_t)(PC_STUB_VGATAB + idx * 64));
    pc_wr16(c, PC_STUB_SEG, PC_STUB_VGAVARS + 2, m13 ? PC_STUB_DAC256 : PC_STUB_DAC64);
    pc_wr16(c, PC_STUB_SEG, PC_STUB_VGAVARS + 4, m13 ? 256 * 3 : 64 * 3);
}

static const struct vga_regs *vga_table(int mode) {
    mode &= 0x7F;
    if (mode == 0x00) mode = 0x01;                       /* 40 columns, colour burst off: same timing */
    if (mode == 0x02) mode = 0x03;
    if (mode == 0x05) mode = 0x04;
    if (mode == 0x11) mode = 0x12;                       /* 640x480 mono: 12h's timing */
    for (size_t i = 0; i < sizeof vga_tables / sizeof vga_tables[0]; i++)
        if (vga_tables[i].mode == mode) return &vga_tables[i];
    return &vga_tables[1];                               /* anything else: mode 3's */
}

extern const uint8_t pc_font8[256 * 8], pc_font14[256 * 14], pc_font16[256 * 16];

/* What the display shows is what the registers say, as a monitor sees it
 * — not the BIOS data area's mode byte, which under WIN386 is each VM's
 * own and changes with the VM that happens to be running. Graphics or
 * text from the graphics controller (06h bit 0), 256 colours from its
 * shift register mode (05h bit 6), the size from the CRTC: display end
 * in character clocks and scan lines, over the scan lines a row takes. */
static int is_graphics(void) { return vga.gc[6] & 1; }
static int is_256(void) { return is_graphics() && (vga.gc[5] & 0x40); }
static int is_16(void) { return is_graphics() && !(vga.gc[5] & 0x40); }
/* the planes are the memory: 16 colours, or 256 without chain-4 */
static int wants_planar(void) { return is_16() || (is_256() && !(vga.seq[4] & 0x08)); }
static int crtc_lines(void) {
    return (vga.crtc[0x12] | (vga.crtc[0x07] & 0x02) << 7 | (vga.crtc[0x07] & 0x40) << 3) + 1;
}
static int crtc_scan(void) { return ((vga.crtc[0x09] & 0x1F) + 1) * (vga.crtc[0x09] & 0x80 ? 2 : 1); }
static void gfx_size(int *w, int *h) {
    int chars = vga.crtc[0x01] + 1;
    *w = is_256() ? chars * 4 : chars * 8;
    *h = crtc_lines() / crtc_scan();
    if (*w > 640) *w = 640;
    if (*h > 480) *h = 480;
    if (*w < 8) *w = 8;
    if (*h < 1) *h = 1;
}
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
    c->dev_wplane = c->dev_rplane = NULL;                /* update_fast re-derives them after the register write */
    uint8_t *bm = c->code_bitmap;
    if (on) {
        for (uint32_t a = 0; a < WLEN; a++) vga.plane[a & 3][a] = c->mem[WIN + a];
        for (uint32_t a = 0; a < WLEN; a++) bm[WIN + a] |= X86_BM_DEVICE;
        c->device_store = vga_store;
        c->device_read = vga_read;
        vga.planar = 1;
        refresh_view(c);
        if (c->dev_hook) c->dev_hook(c);                 /* translated reads must now avoid the window */
    } else {
        for (uint32_t a = 0; a < WLEN; a++) bm[WIN + a] &= (uint8_t)~X86_BM_DEVICE;
        c->device_store = NULL;
        c->device_read = NULL;
        vga.planar = 0;
        for (uint32_t a = 0; a < WLEN; a++) c->mem[WIN + a] = vga.plane[a & 3][a];
        if (c->dev_hook) c->dev_hook(c);
    }
}

/* Keep the translator's fast store path (c->dev_wplane) in step with the
 * registers: set only while a store is nothing but "this byte into the
 * one plane the map mask enables" (vga_store's fast case with a single
 * plane), with the window showing the read plane. */
static void update_fast(x86_cpu *c) {
    c->dev_wplane = c->dev_rplane = NULL;
    if (!vga.planar) return;
    uint8_t mm = vga.seq[2] & 0x0F;
    if ((vga.gc[5] & 3) == 0 && vga.gc[3] == 0 && vga.gc[1] == 0 && vga.gc[8] == 0xFF && mm && !(mm & (mm - 1))) {
        c->dev_wplane = vga.plane[__builtin_ctz(mm)];
        c->dev_rplane = vga.plane[read_plane()];
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

/* The text modes' power-on state: the EGA 64-colour set in DAC 0-63 (bit
 * 0/1/2 = blue/green/red at 2/3, bit 3/4/5 at 1/3 intensity), the
 * attribute palette that picks the CGA sixteen out of it (6 is brown,
 * 0x14; the bright eight are 0x38-0x3F), blink and line graphics on, the
 * cursor on scan lines 13-14 of 16. */
static void text_default(x86_cpu *c) {
    for (int i = 0; i < 64; i++) {
        vga.dac[i][0] = (uint8_t)((i & 4 ? 42 : 0) + (i & 32 ? 21 : 0));
        vga.dac[i][1] = (uint8_t)((i & 2 ? 42 : 0) + (i & 16 ? 21 : 0));
        vga.dac[i][2] = (uint8_t)((i & 1 ? 42 : 0) + (i & 8 ? 21 : 0));
    }
    static const uint8_t pal[16] = { 0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F };
    memset(vga.ac, 0, sizeof vga.ac);
    memcpy(vga.ac, pal, 16);
    vga.ac[0x10] = 0x0C;                                  /* mode control: blink, line graphics */
    vga.ac[0x12] = 0x0F;                                  /* colour plane enable */
    int h = pc_rd8(c, PC_BDA_SEG, 0x85); if (!h) h = 16;
    vga.crtc[0x09] = (uint8_t)(h - 1);                   /* maximum scan line */
    vga.crtc[0x0A] = (uint8_t)(h - 3);                   /* cursor start */
    vga.crtc[0x0B] = (uint8_t)(h - 2);                   /* cursor end */
}

/* ---- what the BIOS sets as it goes (INT 10h) ------------------------------ */
void pc_vga_set_cursor_pos(uint16_t words) { vga.crtc[0x0E] = (uint8_t)(words >> 8); vga.crtc[0x0F] = (uint8_t)words; }
void pc_vga_set_start(uint16_t words)      { vga.crtc[0x0C] = (uint8_t)(words >> 8); vga.crtc[0x0D] = (uint8_t)words; }
uint16_t pc_vga_start(void)               { return (uint16_t)(vga.crtc[0x0C] << 8 | vga.crtc[0x0D]); }
void pc_vga_set_cursor_shape(uint8_t start, uint8_t end) { vga.crtc[0x0A] = start; vga.crtc[0x0B] = end; }
void pc_vga_set_char_height(int h) { vga.crtc[0x09] = (uint8_t)((vga.crtc[0x09] & 0xE0) | ((h - 1) & 0x1F)); }
uint8_t pc_vga_get_ac(int i) { return i < 0x15 ? vga.ac[i] : 0; }
void pc_vga_set_ac(int i, uint8_t v) { if (i < 0x15) vga.ac[i] = v; }
void pc_vga_get_dac(int i, uint8_t rgb[3]) { memcpy(rgb, vga.dac[i & 0xFF], 3); }
void pc_vga_set_dac(int i, const uint8_t rgb[3]) { for (int k = 0; k < 3; k++) vga.dac[i & 0xFF][k] = rgb[k] & 0x3F; }
/* Port 3DAh (input status 1) read: resets the 3C0h flip-flop to "index". */
void pc_vga_status_read(void) { vga.ac_flip = 0; }

/* INT 10h AH=00 lands here after the BIOS data area is set up: the
 * mode's register table, then the palette and what the renderer keys on. */
void pc_vga_set_mode(x86_cpu *c, int mode) {
    set_planar(c, 0);
    const struct vga_regs *t = vga_table(mode);
    int m = mode & 0x7F;
    vga.misc = t->misc;
    vga.seq[0] = 0x03;
    memcpy(vga.seq + 1, t->seq, 4);
    memcpy(vga.crtc, t->crtc, sizeof vga.crtc);
    memcpy(vga.gc, t->gc, sizeof vga.gc);
    if (m == 0x13) {
        memcpy(vga.ac, t->ac, sizeof vga.ac);
        dac_default();
        if (!(mode & 0x80)) {
            memset(c->mem + WIN, 0, 320 * 200);
            memset(vga.plane, 0, sizeof vga.plane);
        }
    } else if (m == 0x0D || m == 0x0E || m == 0x10 || m == 0x12) {
        text_default(c);                                 /* the EGA DAC set */
        memcpy(vga.ac, t->ac, sizeof vga.ac);
        memcpy(vga.crtc, t->crtc, sizeof vga.crtc);      /* text_default's font height is the text modes' */
        set_planar(c, 1);
        if (!(mode & 0x80)) { memset(vga.plane, 0, sizeof vga.plane); refresh_view(c); }
    } else {
        text_default(c);
        memcpy(vga.ac, t->ac, 16 + 1);                   /* the palette and mode control; plane enable as text_default */
        vga.ac[0x10] = t->ac[0x10];
        vga.ac[0x13] = t->ac[0x13];
        vga.crtc[0x09] = (uint8_t)((t->crtc[0x09] & 0xE0) | (vga.crtc[0x09] & 0x1F));   /* the loaded font's height */
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
    case 0x3C0: *val = vga.ac_idx; return 1;
    case 0x3C1: *val = (vga.ac_idx & 0x1F) < 0x15 ? vga.ac[vga.ac_idx & 0x1F] : 0; return 1;
    case 0x3C7: *val = 0; return 1;                      /* DAC state: nobody looks */
    case 0x3CC: *val = vga.misc; return 1;
    case 0x3CA: *val = 0; return 1;                      /* feature control */
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
    case 0x3C0:                                          /* index, then data, alternately */
        if (!vga.ac_flip) vga.ac_idx = v;
        else if ((vga.ac_idx & 0x1F) < 0x15) vga.ac[vga.ac_idx & 0x1F] = v;
        vga.ac_flip ^= 1;
        return 1;
    case 0x3C4: vga.seq_idx = v; return 1;
    case 0x3C5:
        vga.seq[vga.seq_idx & 7] = v;
        if ((vga.seq_idx & 7) == 4) set_planar(c, wants_planar());
        update_fast(c);
        return 1;
    case 0x3CE: vga.gc_idx = v; return 1;
    case 0x3CF:
        if (vga.gc_idx < 9) {
            int was = read_plane();
            vga.gc[vga.gc_idx] = v;
            if (vga.gc_idx == 5 || vga.gc_idx == 6) set_planar(c, wants_planar());
            if (vga.planar && read_plane() != was) refresh_view(c);
            update_fast(c);
        }
        return 1;
    case 0x3D4: vga.crtc_idx = v; return 1;
    case 0x3D5: if (vga.crtc_idx < sizeof vga.crtc) vga.crtc[vga.crtc_idx] = v; return 1;
    case 0x3C7: vga.dac_ridx = v; vga.dac_rcomp = 0; return 1;
    case 0x3C2: vga.misc = v; return 1;
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
static uint8_t pixel(x86_cpu *c, int x, int y, int w) {
    if (!vga.planar) return x86_phys_rd8(c, WIN + (uint32_t)(y * w + x));
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

/* A 16-colour mode's picture: a pixel is one bit from each plane at the
 * CRTC start address plus y lines of the pitch plus x/8, the four bits
 * masked by the colour plane enable, then the attribute palette (with
 * the colour select register's top bits) into the DAC. */
static void frame16(uint8_t *rgb, int w, int h) {
    uint8_t lut[16][3];
    for (int i = 0; i < 16; i++) {
        int d = (vga.ac[i] & 0x3F) | ((vga.ac[0x14] & 0x0C) << 4);
        for (int k = 0; k < 3; k++) { uint8_t v = vga.dac[d][k]; lut[i][k] = (uint8_t)((v << 2) | (v >> 4)); }
    }
    uint32_t start = ((uint32_t)vga.crtc[0x0C] << 8) | vga.crtc[0x0D];
    uint32_t pitch = (uint32_t)vga.crtc[0x13] * 2;
    uint8_t en = vga.ac[0x12] & 0x0F;
    for (int y = 0; y < h; y++)
        for (int xb = 0; xb < w / 8; xb++) {
            uint32_t a = (start + (uint32_t)y * pitch + (uint32_t)xb) & 0xFFFF;
            uint8_t p0 = vga.plane[0][a], p1 = vga.plane[1][a], p2 = vga.plane[2][a], p3 = vga.plane[3][a];
            uint8_t *px = rgb + ((size_t)y * (size_t)w + (size_t)xb * 8) * 3;
            for (int b = 7; b >= 0; b--, px += 3) {
                int ci = (((p0 >> b) & 1) | (((p1 >> b) & 1) << 1) | (((p2 >> b) & 1) << 2) | (((p3 >> b) & 1) << 3)) & en;
                px[0] = lut[ci][0]; px[1] = lut[ci][1]; px[2] = lut[ci][2];
            }
        }
}

/* The current graphics picture as RGB24 rows, *w by *h (at most 640x480):
 * mode 13h at 320x200, or a 16-colour mode at its size. 0, or -1 if the
 * screen is in neither. The window and the PNG writer both use it. */
int pc_vga_frame(x86_cpu *c, uint8_t *rgb, int *w, int *h) {
    if (!is_graphics()) return -1;
    gfx_size(w, h);
    if (is_16()) { frame16(rgb, *w, *h); return 0; }
    uint8_t lut[256][3];
    for (int i = 0; i < 256; i++)
        for (int k = 0; k < 3; k++) {
            uint8_t v = vga.dac[i][k];
            lut[i][k] = (uint8_t)((v << 2) | (v >> 4));  /* 6 bits to 8 */
        }
    for (int y = 0; y < *h; y++)
        for (int x = 0; x < *w; x++)
            memcpy(rgb + (y * *w + x) * 3, lut[pixel(c, x, y, *w)], 3);
    return 0;
}

/* The screen as an 8-bit RGB PNG: mode 13h at 320x200, a 16-colour mode
 * at its own size, a text mode as
 * the VGA draws it (720x400 for 80x25), blink and cursor in their "on"
 * phase. 0, or -1 if neither or the file cannot be written. */
int pc_video_png(x86_cpu *c, const char *path) {
    enum { MW = 1188, MH = 480 };
    static uint8_t rgb[MW * MH * 3], raw[MH * (1 + MW * 3)];
    int W = 320, H = 200;
    if (pc_vga_frame(c, rgb, &W, &H) < 0 && pc_vga_text_frame(c, rgb, MW, MH, &W, &H, 0) < 0) return -1;
    for (int y = 0; y < H; y++) {
        raw[y * (1 + W * 3)] = 0;                        /* filter: none */
        memcpy(raw + y * (1 + W * 3) + 1, rgb + y * W * 3, (size_t)W * 3);
    }
    uLong rawlen = (uLong)H * (uLong)(1 + W * 3);
    uLongf zlen = compressBound(rawlen);
    uint8_t *z = malloc(zlen);
    if (!z || compress2(z, &zlen, raw, rawlen, 6) != Z_OK) { free(z); return -1; }
    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return -1; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    uint8_t ihdr[13] = { 0, 0, (uint8_t)(W >> 8), (uint8_t)W, 0, 0, (uint8_t)(H >> 8), (uint8_t)H, 8, 2, 0, 0, 0 };   /* W x H, 8-bit RGB */
    fwrite(sig, 1, 8, f);
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zlen);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(z);
    return 0;
}

/* ---- text modes ------------------------------------------------------------ */

static int text_mode(void) { return !is_graphics(); }

/* The text screen's shape as the CRTC has it: what the display shows,
 * whatever the BIOS data area says (a kernel that owns the machine need
 * not keep it). */
void pc_vga_text_geometry(int *rows, int *cols) {
    int ch = (vga.crtc[0x09] & 0x1F) + 1;
    *cols = vga.crtc[0x01] + 1; if (*cols < 1 || *cols > 132) *cols = 80;
    *rows = crtc_lines() / ch; if (*rows < 1 || *rows > 60) *rows = 25;
}

/* Draw the text screen into rgb (RGB24, width *w, height *h, at most
 * maxw x maxh): 0, or -1 when the display is not in a text mode. frame
 * counts the VGA's 70 Hz frames: the cursor blinks every 8, blinking
 * characters every 16, as the hardware's counters do. */
int pc_vga_text_frame(x86_cpu *c, uint8_t *rgb, int maxw, int maxh, int *w, int *h, unsigned frame) {
    if (!text_mode()) return -1;
    int mono = (vga.gc[6] & 0x0C) == 0x08;              /* memory at B000 */
    int ch = (vga.crtc[0x09] & 0x1F) + 1;
    int cols = vga.crtc[0x01] + 1; if (cols < 1 || cols > 132) cols = 80;
    int rows = crtc_lines() / ch; if (rows < 1 || rows > 60) rows = 25;
    const uint8_t *font = ch <= 8 ? pc_font8 : ch <= 14 ? pc_font14 : pc_font16;
    int fh = ch <= 8 ? 8 : ch <= 14 ? 14 : 16;
    if (cols * 9 > maxw) cols = maxw / 9;
    if (rows * fh > maxh) rows = maxh / fh;
    *w = cols * 9; *h = rows * fh;
    uint8_t lut[16][3];
    for (int i = 0; i < 16; i++) {
        int d = (vga.ac[i] & 0x3F) | ((vga.ac[0x14] & 0x0C) << 4);
        for (int k = 0; k < 3; k++) { uint8_t v = vga.dac[d][k]; lut[i][k] = (uint8_t)((v << 2) | (v >> 4)); }
    }
    int blink_attr = (vga.ac[0x10] & 0x08) != 0, line_gfx = (vga.ac[0x10] & 0x04) != 0;
    int blink_off = (frame >> 4) & 1, cursor_off = (frame >> 3) & 1;
    uint32_t base = mono ? 0xB0000u : 0xB8000u;
    uint32_t start = ((uint32_t)vga.crtc[0x0C] << 8) | vga.crtc[0x0D];
    uint32_t cur = ((uint32_t)vga.crtc[0x0E] << 8) | vga.crtc[0x0F];
    int cs = vga.crtc[0x0A] & 0x1F, ce = vga.crtc[0x0B] & 0x1F;
    int cursor_on = !(vga.crtc[0x0A] & 0x20) && cs <= ce && !cursor_off;
    int stride = *w * 3;
    int mrow = -1, mcol = -1; uint16_t msm = 0, mcm = 0;
    if (!pc_mouse_text_cursor(&mrow, &mcol, &msm, &mcm)) mrow = -1;
    for (int r = 0; r < rows; r++)
        for (int col = 0; col < cols; col++) {
            uint32_t cell = start + (uint32_t)(r * cols + col);
            uint32_t a = base + ((cell * 2) & 0x7FFF);
            uint8_t chr = c->mem[a], at = c->mem[a + 1];
            if (r == mrow && col == mcol) {           /* the mouse driver's software cursor */
                uint16_t v = (uint16_t)(((at << 8) | chr) & msm) ^ mcm;
                chr = (uint8_t)v; at = (uint8_t)(v >> 8);
            }
            int fg = at & 0x0F, bg = at >> 4, hide = 0;
            if (blink_attr) { if ((bg & 8) && blink_off) hide = 1; bg &= 7; }
            if (mono) {                                   /* MDA attributes: 07 normal, 0F bright, 70 reverse */
                int rev = (at & 0x77) == 0x70;
                fg = rev ? 0 : ((at & 0x07) ? ((at & 0x08) ? 15 : 7) : 0);
                bg = rev ? 7 : 0;
            }
            const uint8_t *glyph = font + chr * fh;
            int in_cur = cursor_on && cell == cur;
            for (int y = 0; y < fh; y++) {
                uint8_t bits = hide ? 0 : glyph[y];
                int cur_line = in_cur && y >= cs && y <= ce;      /* the cursor is drawn in the cell's foreground */
                int ninth = line_gfx && chr >= 0xC0 && chr <= 0xDF ? (bits & 1) : 0;
                if (cur_line) { bits = 0xFF; ninth = 1; }
                uint8_t *px = rgb + (r * fh + y) * stride + col * 27;
                for (int x = 0; x < 9; x++) {
                    int on = x < 8 ? (bits >> (7 - x)) & 1 : ninth;
                    const uint8_t *p = lut[on ? fg : bg];
                    px[x * 3] = p[0]; px[x * 3 + 1] = p[1]; px[x * 3 + 2] = p[2];
                }
            }
        }
    return 0;
}
