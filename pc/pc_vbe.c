/* pc_vbe.c — VESA BIOS Extensions 3.0: linear framebuffer modes
 *
 * -vbe: INT 10h AH=4Fh answers, as a VBE 3.0 BIOS with 8 MB of video
 * memory and a linear framebuffer — what Haiku requires of a video card,
 * and what lets Windows or Linux draw above 640x480 through a VESA
 * driver. Without -vbe, AH=4Fh fails as on a plain VGA (the machine the
 * DOS, Windows and NT suites boot stays one).
 *
 * The framebuffer is the top 8 MB of guest RAM, reported to the system
 * as not RAM (pc_cmos.c: mem_top, and a reserved E820 range): the guest's
 * pixels are ordinary memory to translated code — its stores take the
 * fast path — and the display reads them straight from there.
 *
 * Modes (numbers the VBE 1.2 table gave these resolutions, at 8 bpp; the
 * 16- and 32-bit ones as most VBE BIOSes of the 2000s had them): 640x480,
 * 800x600, 1024x768 and 1280x1024, each at 8 (a DAC palette), 16 (5:6:5)
 * and 32 (x:8:8:8) bits per pixel. Linear framebuffer only: no banked
 * window at A0000h (ModeAttributes says so; 4F05h fails).
 *
 * On the PCI machine the display is also a PCI function, 00:02.0, class
 * 03.00 (a VGA-compatible controller, vendor 1234h device 1112h: no
 * real card's, so no OS binds a card's driver to it), BAR 0 the
 * framebuffer (8 MB, prefetchable, at the RAM it is). Windows-family
 * systems install their VESA path on a PCI display device: ReactOS's
 * display.inf puts vgapnp.sys (its VBE miniport) and framebuf.dll on
 * PCI\CC_0300; its Root\VgaSave fallback is 640x480x16 for good. That
 * miniport calls here through the kernel's V86 monitor from CSRSS's
 * address space, where the buffer's page may be paged out between calls
 * (pc_hle_touch), and being a VBE 3.0 reader it takes the colour masks
 * from the linear-mode fields of the mode information block.
 *
 * Functions: 00h controller information (the mode list and OEM strings
 * inside the caller's buffer), 01h mode information, 02h set mode (a
 * VGA mode number sets that mode as AH=00h does), 03h current mode, 06h
 * logical scan line length (get; set only to what the mode has), 07h
 * display start (panning, page flips), 08h DAC width (6 bits, fixed),
 * 09h palette data. Everything else is AX=014Fh, "not supported".
 */
#include "pc.h"
#include <stdio.h>
#include <string.h>

#define VRAM (8u << 20)

static const struct vbe_mode { uint16_t num, w, h; uint8_t bpp; } modes[] = {
    { 0x101,  640, 480,  8 }, { 0x111,  640, 480, 16 }, { 0x112,  640, 480, 32 },
    { 0x103,  800, 600,  8 }, { 0x114,  800, 600, 16 }, { 0x115,  800, 600, 32 },
    { 0x105, 1024, 768,  8 }, { 0x117, 1024, 768, 16 }, { 0x118, 1024, 768, 32 },
    { 0x107, 1280, 1024, 8 }, { 0x11A, 1280, 1024, 16 }, { 0x11B, 1280, 1024, 32 },
};
#define NMODES (sizeof modes / sizeof modes[0])

static struct {
    int enabled;
    const struct vbe_mode *m;          /* the VBE mode set, or NULL: the VGA's */
    int lfb;                           /* set with bit 14 */
    uint32_t start;                    /* display start, bytes into the framebuffer */
} v;

void pc_vbe_enable(void) { v.enabled = 1; }
int  pc_vbe_enabled(void) { return v.enabled; }
uint32_t pc_vbe_reserved(void) { return v.enabled ? VRAM : 0; }
static uint32_t lfb_base(const x86_cpu *c) { return c->mem_size - VRAM; }
void pc_vbe_vga_mode(void) { v.m = NULL; v.start = 0; }   /* INT 10h AH=00h: the VGA again */
int  pc_vbe_active(void) { return v.m != NULL; }

static const struct vbe_mode *find(uint16_t num) {
    for (size_t i = 0; i < NMODES; i++) if (modes[i].num == (num & 0x1FF)) return &modes[i];
    return NULL;
}
static uint32_t pitch(const struct vbe_mode *m) { return (uint32_t)m->w * (m->bpp / 8); }

static void wr8(x86_cpu *c, uint32_t a, uint8_t b) { x86_phys_wr8(c, a, b); }
static void wr16(x86_cpu *c, uint32_t a, uint16_t w) { wr8(c, a, (uint8_t)w); wr8(c, a + 1, (uint8_t)(w >> 8)); }
static void wr32(x86_cpu *c, uint32_t a, uint32_t d) { wr16(c, a, (uint16_t)d); wr16(c, a + 2, (uint16_t)(d >> 16)); }
static uint32_t es_di(x86_cpu *c) { return c->seg[S_ES].base + x86_get_r16(c, R_DI); }
static void ok(x86_cpu *c) { x86_set_r16(c, R_AX, 0x004F); }
static void fail(x86_cpu *c) { x86_set_r16(c, R_AX, 0x014F); }

/* 00h: the VbeInfoBlock at ES:DI — 256 bytes, or 512 when the caller
 * asks for VBE 2.0+ ("VBE2" there first). The mode list and the OEM
 * strings go in its reserved area, which both sizes have, so the far
 * pointers point into the caller's own buffer. */
static void info(x86_cpu *c) {
    uint32_t b = es_di(c);
    uint16_t seg = c->seg[S_ES].sel, off = x86_get_r16(c, R_DI);
    if (!pc_hle_touch(c, b, 256, 1)) return;
    int v2 = x86_phys_rd8(c, b) == 'V' && x86_phys_rd8(c, b + 1) == 'B' && x86_phys_rd8(c, b + 2) == 'E' && x86_phys_rd8(c, b + 3) == '2';
    if (pc.debug) fprintf(stderr, "[vbe] 4F00 at %04X:%04X (pmode %d vm %d), %s\n", seg, off, c->pmode, (c->eflags & X86_VM) != 0, v2 ? "VBE2" : "VBE1");
    if (v2 && !pc_hle_touch(c, b + 256, 256, 1)) return;
    for (uint32_t i = 0; i < (v2 ? 512u : 256u); i++) wr8(c, b + i, 0);
    wr8(c, b, 'V'); wr8(c, b + 1, 'E'); wr8(c, b + 2, 'S'); wr8(c, b + 3, 'A');
    wr16(c, b + 4, 0x0300);
    uint32_t list = 0x22, str = 0x22 + 2 * (NMODES + 1);
    static const char *const s[4] = { "dos-monster VBE", "dos-monster", "VBE framebuffer", "1.0" };
    uint32_t sp[4];
    for (int k = 0; k < 4; k++) {
        sp[k] = str;
        for (const char *p = s[k]; ; p++) { wr8(c, b + str++, (uint8_t)*p); if (!*p) break; }
    }
    wr16(c, b + 0x06, (uint16_t)(off + sp[0])); wr16(c, b + 0x08, seg);   /* OemStringPtr */
    wr32(c, b + 0x0A, 0);                                                    /* capabilities: 6-bit DAC, VGA-compatible registers */
    wr16(c, b + 0x0E, (uint16_t)(off + list)); wr16(c, b + 0x10, seg);    /* VideoModePtr */
    wr16(c, b + 0x12, (uint16_t)(VRAM >> 16));                               /* TotalMemory, 64 KB units */
    wr16(c, b + 0x14, 0x0100);                                               /* OemSoftwareRev */
    wr16(c, b + 0x16, (uint16_t)(off + sp[1])); wr16(c, b + 0x18, seg);   /* OemVendorNamePtr */
    wr16(c, b + 0x1A, (uint16_t)(off + sp[2])); wr16(c, b + 0x1C, seg);   /* OemProductNamePtr */
    wr16(c, b + 0x1E, (uint16_t)(off + sp[3])); wr16(c, b + 0x20, seg);   /* OemProductRevPtr */
    for (size_t i = 0; i < NMODES; i++) wr16(c, b + list + 2 * (uint32_t)i, modes[i].num);
    wr16(c, b + list + 2 * NMODES, 0xFFFF);
    ok(c);
}

/* 01h: the ModeInfoBlock for mode CX at ES:DI */
static void mode_info(x86_cpu *c) {
    const struct vbe_mode *m = find(x86_get_r16(c, R_CX));
    if (!m) { fail(c); return; }
    uint32_t b = es_di(c), p = pitch(m);
    if (!pc_hle_touch(c, b, 256, 1)) return;
    for (uint32_t i = 0; i < 256; i++) wr8(c, b + i, 0);
    wr16(c, b + 0x00, 0x00FB);         /* supported, extended info, colour, graphics, not VGA, no window, linear */
    wr16(c, b + 0x10, (uint16_t)p);    /* BytesPerScanLine */
    wr16(c, b + 0x12, m->w); wr16(c, b + 0x14, m->h);
    wr8(c, b + 0x16, 8); wr8(c, b + 0x17, 16);   /* character cell */
    wr8(c, b + 0x18, 1);               /* planes */
    wr8(c, b + 0x19, m->bpp);
    wr8(c, b + 0x1A, 1);               /* banks */
    wr8(c, b + 0x1B, m->bpp == 8 ? 4 : 6);   /* memory model: packed pixel, or direct colour */
    uint32_t pages = VRAM / (p * m->h);
    wr8(c, b + 0x1D, (uint8_t)(pages ? pages - 1 : 0));
    wr8(c, b + 0x1E, 1);               /* reserved: 1 */
    /* colour masks: sizes and positions (red, green, blue, reserved) */
    static const uint8_t m16[8] = { 5, 11, 6, 5, 5, 0, 0, 0 }, m32[8] = { 8, 16, 8, 8, 8, 0, 8, 24 };
    const uint8_t *mk = m->bpp == 16 ? m16 : m->bpp == 32 ? m32 : NULL;
    if (mk) {
        for (int k = 0; k < 8; k++) { wr8(c, b + 0x1F + (uint32_t)k, mk[k]); wr8(c, b + 0x36 + (uint32_t)k, mk[k]); }   /* ...and the linear-mode copies at 36h (VBE 3.0 readers, ReactOS's vbemp, take these) */
        wr8(c, b + 0x27, m->bpp == 32 ? 0x02 : 0x00);   /* DirectColorModeInfo: reserved field usable (32) */
    }
    wr32(c, b + 0x28, lfb_base(c));    /* PhysBasePtr */
    wr16(c, b + 0x32, (uint16_t)p);    /* LinBytesPerScanLine */
    wr8(c, b + 0x34, (uint8_t)(pages ? pages - 1 : 0));   /* BnkNumberOfImagePages */
    wr8(c, b + 0x35, (uint8_t)(pages ? pages - 1 : 0));   /* LinNumberOfImagePages */
    ok(c);
}

void pc_video_int10(x86_cpu *c, int vector);

/* 02h: set mode BX (bit 14 linear, bit 15 keep the memory) */
static void set_mode(x86_cpu *c) {
    uint16_t bx = x86_get_r16(c, R_BX);
    if ((bx & 0x1FF) < 0x100) {                /* a VGA mode, as AH=00h sets it */
        x86_set_r16(c, R_AX, (uint16_t)(bx & 0x7F) | (bx & 0x8000 ? 0x80 : 0));
        pc_video_int10(c, 0x10);               /* (may continue in the native VGA programming stub, which keeps AX) */
        ok(c);
        return;
    }
    const struct vbe_mode *m = find(bx);
    if (!m) { fail(c); return; }
    v.m = m;
    v.lfb = (bx & 0x4000) != 0;
    v.start = 0;
    if (!(bx & 0x8000)) memset(c->mem + lfb_base(c), 0, VRAM);
    if (pc.debug) fprintf(stderr, "[vbe] mode %03X: %ux%u, %u bpp, framebuffer at %08X\n", m->num, m->w, m->h, m->bpp, lfb_base(c));
    ok(c);
}

void pc_vbe_int10(x86_cpu *c) {
    int al = x86_get_r8(c, R_AL), bl = x86_get_r8(c, R_BL);
    if (!v.enabled) { fail(c); return; }
    switch (al) {
    case 0x00: info(c); return;
    case 0x01: mode_info(c); return;
    case 0x02: set_mode(c); return;
    case 0x03:
        x86_set_r16(c, R_BX, v.m ? (uint16_t)(v.m->num | (v.lfb ? 0x4000 : 0)) : x86_phys_rd8(c, 0x449));
        ok(c);
        return;
    case 0x06:                                 /* scan line length: get (1), or set to what it is (0, 2) */
        if (!v.m || (bl != 0 && bl != 1 && bl != 2 && bl != 3)) { fail(c); return; }
        if (bl == 0 && x86_get_r16(c, R_CX) != v.m->w) { x86_set_r16(c, R_AX, 0x024F); return; }
        if (bl == 2 && x86_get_r16(c, R_CX) != pitch(v.m)) { x86_set_r16(c, R_AX, 0x024F); return; }
        x86_set_r16(c, R_BX, (uint16_t)pitch(v.m));
        x86_set_r16(c, R_CX, v.m->w);
        x86_set_r16(c, R_DX, (uint16_t)(VRAM / pitch(v.m)));
        ok(c);
        return;
    case 0x07:                                 /* display start: set (00h, 80h: at the retrace), get (01h) */
        if (!v.m) { fail(c); return; }
        if (bl == 0x00 || bl == 0x80) {
            uint32_t x = x86_get_r16(c, R_CX), y = x86_get_r16(c, R_DX);
            uint32_t s = y * pitch(v.m) + x * (v.m->bpp / 8);
            if (s + pitch(v.m) * v.m->h > VRAM) { x86_set_r16(c, R_AX, 0x024F); return; }
            v.start = s;
        } else if (bl == 0x01) {
            x86_set_r16(c, R_BX, 0);
            x86_set_r16(c, R_CX, (uint16_t)((v.start % pitch(v.m)) / (v.m->bpp / 8)));
            x86_set_r16(c, R_DX, (uint16_t)(v.start / pitch(v.m)));
        } else { fail(c); return; }
        ok(c);
        return;
    case 0x08:                                 /* DAC width: 6 bits, whatever is asked */
        if (bl > 1) { fail(c); return; }
        x86_set_r8(c, R_BH, 6);
        ok(c);
        return;
    case 0x09: {                               /* palette data: set (00h, 80h) or get (01h), CX entries from DX, at ES:DI (B, G, R, 0) */
        uint32_t n = x86_get_r16(c, R_CX), first = x86_get_r16(c, R_DX), a = es_di(c);
        if (first + n > 256 || (bl != 0 && bl != 1 && bl != 0x80)) { fail(c); return; }
        if (!pc_hle_touch(c, a, n * 4, bl == 1)) return;
        for (uint32_t i = 0; i < n; i++, a += 4) {
            uint8_t rgb[3];
            if (bl == 1) {
                pc_vga_get_dac((int)(first + i), rgb);
                wr8(c, a, rgb[2]); wr8(c, a + 1, rgb[1]); wr8(c, a + 2, rgb[0]); wr8(c, a + 3, 0);
            } else {
                rgb[0] = x86_phys_rd8(c, a + 2); rgb[1] = x86_phys_rd8(c, a + 1); rgb[2] = x86_phys_rd8(c, a);
                pc_vga_set_dac((int)(first + i), rgb);
            }
        }
        ok(c);
        return;
    }
    default:
        fail(c);
        return;
    }
}

/* The picture of a VBE mode as RGB24 rows, from the display start: 8 bpp
 * through the DAC, 16 as 5:6:5, 32 as x:8:8:8. 0, or -1 if no VBE mode is
 * set. RGB holds at least 1280x1024. */
int pc_vbe_frame(x86_cpu *c, uint8_t *rgb, int *w, int *h) {
    const struct vbe_mode *m = v.m;
    if (!m) return -1;
    *w = m->w; *h = m->h;
    const uint8_t *fb = c->mem + lfb_base(c) + v.start;
    uint32_t p = pitch(m);
    if (m->bpp == 8) {
        uint8_t lut[256][3];
        for (int i = 0; i < 256; i++) {
            uint8_t d[3]; pc_vga_get_dac(i, d);
            for (int k = 0; k < 3; k++) lut[i][k] = (uint8_t)((d[k] << 2) | (d[k] >> 4));
        }
        for (int y = 0; y < m->h; y++)
            for (int x = 0; x < m->w; x++) memcpy(rgb + ((size_t)y * m->w + (size_t)x) * 3, lut[fb[(size_t)y * p + (size_t)x]], 3);
    } else if (m->bpp == 16) {
        for (int y = 0; y < m->h; y++) {
            const uint8_t *row = fb + (size_t)y * p;
            uint8_t *o = rgb + (size_t)y * m->w * 3;
            for (int x = 0; x < m->w; x++, o += 3) {
                uint16_t px = (uint16_t)(row[2 * x] | row[2 * x + 1] << 8);
                uint8_t r = (uint8_t)(px >> 11), g = (uint8_t)((px >> 5) & 63), bl = (uint8_t)(px & 31);
                o[0] = (uint8_t)((r << 3) | (r >> 2)); o[1] = (uint8_t)((g << 2) | (g >> 4)); o[2] = (uint8_t)((bl << 3) | (bl >> 2));
            }
        }
    } else {
        for (int y = 0; y < m->h; y++) {
            const uint8_t *row = fb + (size_t)y * p;
            uint8_t *o = rgb + (size_t)y * m->w * 3;
            for (int x = 0; x < m->w; x++, o += 3) { o[0] = row[4 * x + 2]; o[1] = row[4 * x + 1]; o[2] = row[4 * x]; }
        }
    }
    return 0;
}

/* ---- the PCI function --------------------------------------------------- */

static pc_pci_dev vgapci;

void pc_vbe_pci_attach(x86_cpu *c) {
    if (!v.enabled) return;
    pc_pci_dev *d = &vgapci;
    memset(d, 0, sizeof *d);
    static const uint8_t id[16] = {
        0x34, 0x12, 0x12, 0x11,     /* vendor 1234h, device 1112h */
        0x03, 0x00, 0x80, 0x02,     /* command: I/O, memory; status: medium DEVSEL, fast back-to-back */
        0x00, 0x00, 0x00, 0x03,     /* revision 0; programming interface 0; class 03h (display), subclass 00h (VGA) */
        0x00, 0x00, 0x00, 0x00,     /* header type 0, single function */
    };
    memcpy(d->cfg, id, sizeof id);
    d->bar_size[0] = VRAM;
    d->bar_fixed[0] = lfb_base(c);
    d->cfg[0x10] = 0x08;                                 /* BAR 0: memory, prefetchable (the size's bits are written at POST) */
    pc_pci_add(2, d);
}
