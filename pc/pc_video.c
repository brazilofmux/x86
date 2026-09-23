/* pc_video.c — 80x25 colour text: BIOS INT 10h services over the B800
 * cell buffer, the BDA cursor, and two host views.
 *
 *   stdio mode  — teletype output (INT 10h/0E, DOS console writes) is
 *                 echoed to stdout as it happens; direct video writes
 *                 stay invisible. For bring-up, scripts and -V runs.
 *   tty mode    — a diffing ANSI painter: every flush compares the
 *                 4000-byte cell buffer with the last painted copy and
 *                 emits the minimum. Direct writes by the guest (the
 *                 JIT stores straight into guest memory, nothing traps)
 *                 cost the painter nothing until the next flush.
 *
 * Lifted in spirit from ~/z80/kaypro/kaypro_video.c + kaypro_render_tty.c.
 */
#include "pc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define BDA PC_BDA_SEG
#define PAGE_BYTES 0x1000

static uint8_t  shadow[PC_ROWS * PC_COLS * 2];
static int      shadow_valid;
static int      active;
static uint64_t last_paint_ns;
static int      term_rows = 25, term_cols = 80;
static char     hud_text[PC_COLS + 1], hud_painted[PC_COLS + 1];
static int      hud_dirty;

/* CP437 → Unicode for the painter. */
static const uint16_t cp437[256] = {
    0x0020,0x263A,0x263B,0x2665,0x2666,0x2663,0x2660,0x2022,0x25D8,0x25CB,0x25D9,0x2642,0x2640,0x266A,0x266B,0x263C,
    0x25BA,0x25C4,0x2195,0x203C,0x00B6,0x00A7,0x25AC,0x21A8,0x2191,0x2193,0x2192,0x2190,0x221F,0x2194,0x25B2,0x25BC,
    0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,
    0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
    0x0040,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,
    0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,0x0058,0x0059,0x005A,0x005B,0x005C,0x005D,0x005E,0x005F,
    0x0060,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
    0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,0x2302,
    0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
    0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
    0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
    0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
    0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0,
};

static void put_utf8(FILE *f, unsigned cp) {
    if (cp < 0x80) fputc((int)cp, f);
    else if (cp < 0x800) { fputc((int)(0xC0 | (cp >> 6)), f); fputc((int)(0x80 | (cp & 0x3F)), f); }
    else { fputc((int)(0xE0 | (cp >> 12)), f); fputc((int)(0x80 | ((cp >> 6) & 0x3F)), f); fputc((int)(0x80 | (cp & 0x3F)), f); }
}

/* ---- BDA accessors ----------------------------------------------------- */
static int cols(x86_cpu *c) { int n = pc_rd16(c, BDA, 0x4A); return n ? n : 80; }
static int rows(x86_cpu *c) { return pc_rd8(c, BDA, 0x84) + 1; }
static int page(x86_cpu *c) { return pc_rd8(c, BDA, 0x62) & 7; }
static uint16_t page_off(int pg) { return (uint16_t)(pg * PAGE_BYTES); }

static void get_cursor(x86_cpu *c, int pg, int *row, int *col) {
    uint16_t v = pc_rd16(c, BDA, (uint16_t)(0x50 + pg * 2));
    *row = v >> 8; *col = v & 0xFF;
}
static void set_cursor(x86_cpu *c, int pg, int row, int col) {
    pc_wr16(c, BDA, (uint16_t)(0x50 + pg * 2), (uint16_t)((row << 8) | col));
    /* the displayed page's cursor is also the CRTC's, in words from the
     * start of video memory, as a real BIOS programs it */
    if (pg == (pc_rd8(c, BDA, 0x62) & 7))
        pc_vga_set_cursor_pos((uint16_t)(page_off(pg) / 2 + row * cols(c) + col));
}

/* INT 10h AH=01: the cursor's start/end scan lines in CH/CL, bit 5 of CH
 * to hide it. Like a VGA BIOS, a shape given in the CGA's 8-line terms
 * (both lines under 8) is scaled to the loaded font's height, so the
 * usual 0607h becomes 13-14 of 16 and 0007h a block. */
static void cursor_shape(x86_cpu *c, uint16_t cx) {
    int s = (cx >> 8) & 0x1F, e = cx & 0x1F, hide = (cx >> 8) & 0x20;
    int h = pc_rd8(c, BDA, 0x85); if (!h) h = 16;
    if (h > 8 && s < 8 && e < 8) {
        if (e == 7) { e = h - 2; s = s >= 6 ? h - 3 : s * h / 8; }
        else { s = s * h / 8; e = e * h / 8; }
    }
    pc_vga_set_cursor_shape((uint8_t)(s | hide), (uint8_t)e);
}

static uint16_t cell_off(x86_cpu *c, int pg, int row, int col) {
    return (uint16_t)(page_off(pg) + (row * cols(c) + col) * 2);
}

/* Scroll the window (r0,c0)-(r1,c1) by n lines (0 = clear), filling with attr. */
static void scroll(x86_cpu *c, int pg, int up, int n, int r0, int c0, int r1, int c1, uint8_t attr) {
    int nr = rows(c), nc = cols(c);
    if (r1 >= nr) r1 = nr - 1;
    if (c1 >= nc) c1 = nc - 1;
    if (r0 > r1 || c0 > c1) return;
    int h = r1 - r0 + 1;
    if (n <= 0 || n >= h) n = h;
    for (int i = 0; i < h; i++) {
        int dst = up ? r0 + i : r1 - i;
        int src = up ? dst + n : dst - n;
        for (int col = c0; col <= c1; col++) {
            uint16_t v = (src >= r0 && src <= r1) ? pc_rd16(c, PC_VIDEO_SEG, cell_off(c, pg, src, col))
                                                   : (uint16_t)((attr << 8) | ' ');
            pc_wr16(c, PC_VIDEO_SEG, cell_off(c, pg, dst, col), v);
        }
    }
}

/* Stdio echo of one teletype character (CP437 → UTF-8, CRLF → LF, a
 * lone CR stays a CR so progress lines overwrite themselves). */
static void echo(uint8_t ch) {
    static int pending_cr;
    if (pc.tty_mode) return;
    if (pending_cr && ch != 0x0A) fputc('\r', stdout);
    pending_cr = ch == 0x0D;
    switch (ch) {
    case 0x0D: break;
    case 0x0A: fputc('\n', stdout); break;
    case 0x08: fputc('\b', stdout); break;
    case 0x07: fputc('\a', stdout); break;
    case 0x09: fputc('\t', stdout); break;
    default:   put_utf8(stdout, cp437[ch]); break;
    }
}

void pc_video_teletype(x86_cpu *c, uint8_t ch) {
    int pg = page(c), row, col;
    get_cursor(c, pg, &row, &col);
    int nr = rows(c), nc = cols(c);
    switch (ch) {
    case 0x07: break;
    case 0x08: if (col > 0) col--; break;
    case 0x0A: row++; break;
    case 0x0D: col = 0; break;
    default:
        pc_wr8(c, PC_VIDEO_SEG, cell_off(c, pg, row, col), ch);
        col++;
        if (col >= nc) { col = 0; row++; }
        break;
    }
    if (row >= nr) {
        scroll(c, pg, 1, 1, 0, 0, nr - 1, nc - 1, 0x07);
        row = nr - 1;
    }
    set_cursor(c, pg, row, col);
    echo(ch);
}

static void set_mode(x86_cpu *c, int mode) {
    int m = mode & 0x7F;
    int nc = (m == 0 || m == 1 || m == 0x13) ? 40 : 80;
    pc_wr8(c, BDA, 0x49, (uint8_t)(mode & 0x7F));
    pc_wr16(c, BDA, 0x4A, (uint16_t)nc);
    pc_wr16(c, BDA, 0x4C, PAGE_BYTES);
    pc_wr16(c, BDA, 0x4E, 0);
    for (int i = 0; i < 8; i++) set_cursor(c, i, 0, 0);
    pc_wr16(c, BDA, 0x60, 0x0607);
    pc_wr8(c, BDA, 0x62, 0);
    pc_wr16(c, BDA, 0x63, mode == 7 ? 0x3B4 : 0x3D4);
    pc_wr8(c, BDA, 0x84, PC_ROWS - 1);
    pc_wr16(c, BDA, 0x85, 16);
    pc_wr8(c, BDA, 0x87, 0x60);
    pc_wr8(c, BDA, 0x88, 0x09);
    pc_wr8(c, BDA, 0x89, 0x51);
    pc_wr8(c, BDA, 0x8A, 0x08);
    pc_vga_set_mode(c, mode);                    /* graphics state follows the mode, text or 13h */
    pc_vga_set_cursor_pos(0);
    if (m != 0x13 && !(mode & 0x80))
        for (int i = 0; i < PC_ROWS * PC_COLS; i++)
            pc_wr16(c, PC_VIDEO_SEG, (uint16_t)(i * 2), 0x0720);
    shadow_valid = 0;
}

void pc_video_init(x86_cpu *c) {
    set_mode(c, 3);
    memset(hud_text, 0, sizeof hud_text);
    hud_painted[0] = 0;
}

/* ---- INT 10h ------------------------------------------------------------ */
void pc_video_int10(x86_cpu *c, int vector) {
    (void)vector;
    int ah = x86_get_r8(c, R_AH), al = x86_get_r8(c, R_AL);
    if (pc.debug > 1 && ah != 0x0E) fprintf(stderr, "[bios] INT 10h AH=%02X AL=%02X BX=%04X CX=%04X DX=%04X @%llu\n", ah, al,
                              x86_get_r16(c, R_BX), x86_get_r16(c, R_CX), x86_get_r16(c, R_DX), (unsigned long long)c->insn_count);
    int bh = x86_get_r8(c, R_BH), bl = x86_get_r8(c, R_BL);
    int pg = page(c);
    switch (ah) {
    case 0x00:
        set_mode(c, al);
        break;
    case 0x01:
        pc_wr16(c, BDA, 0x60, x86_get_r16(c, R_CX));
        cursor_shape(c, x86_get_r16(c, R_CX));
        break;
    case 0x02:
        set_cursor(c, bh & 7, x86_get_r8(c, R_DH), x86_get_r8(c, R_DL));
        break;
    case 0x03: {
        int row, col; get_cursor(c, bh & 7, &row, &col);
        x86_set_r16(c, R_DX, (uint16_t)((row << 8) | col));
        x86_set_r16(c, R_CX, pc_rd16(c, BDA, 0x60));
        break;
    }
    case 0x05: {
        pc_wr8(c, BDA, 0x62, (uint8_t)(al & 7));
        pc_wr16(c, BDA, 0x4E, page_off(al & 7));
        pc_vga_set_start((uint16_t)(page_off(al & 7) / 2));
        int row, col; get_cursor(c, al & 7, &row, &col);
        set_cursor(c, al & 7, row, col);              /* the new page's cursor into the CRTC */
        break;
    }
    case 0x06: case 0x07:
        scroll(c, pg, ah == 0x06, al, x86_get_r8(c, R_CH), x86_get_r8(c, R_CL),
               x86_get_r8(c, R_DH), x86_get_r8(c, R_DL), (uint8_t)bh);
        break;
    case 0x08: {
        int row, col; get_cursor(c, bh & 7, &row, &col);
        x86_set_r16(c, R_AX, pc_rd16(c, PC_VIDEO_SEG, cell_off(c, bh & 7, row, col)));
        break;
    }
    case 0x09: case 0x0A: {
        int row, col; get_cursor(c, bh & 7, &row, &col);
        int n = x86_get_r16(c, R_CX), nc = cols(c);
        for (int i = 0; i < n && row < rows(c); i++) {
            uint16_t off = cell_off(c, bh & 7, row, col);
            pc_wr8(c, PC_VIDEO_SEG, off, (uint8_t)al);
            if (ah == 0x09) pc_wr8(c, PC_VIDEO_SEG, (uint16_t)(off + 1), (uint8_t)bl);
            if (++col >= nc) { col = 0; row++; }
        }
        break;
    }
    case 0x0E:
        pc_video_teletype(c, (uint8_t)al);
        break;
    case 0x0F:
        x86_set_r8(c, R_AL, pc_rd8(c, BDA, 0x49));
        x86_set_r8(c, R_AH, (uint8_t)cols(c));
        x86_set_r8(c, R_BH, (uint8_t)pg);
        break;
    case 0x10: {                                   /* palette: the attribute controller and the DAC */
        uint16_t es = c->seg[S_ES].sel, dx = x86_get_r16(c, R_DX), bx = x86_get_r16(c, R_BX);
        uint8_t rgb[3];
        switch (al) {
        case 0x00: if (bl < 16) pc_vga_set_ac(bl, (uint8_t)bh); break;
        case 0x01: pc_vga_set_ac(0x11, (uint8_t)bh); break;
        case 0x02:
            for (int i = 0; i < 16; i++) pc_vga_set_ac(i, pc_rd8(c, es, (uint16_t)(dx + i)));
            pc_vga_set_ac(0x11, pc_rd8(c, es, (uint16_t)(dx + 16)));
            break;
        case 0x03: {                                  /* BL 0: bright backgrounds, 1: blink */
            uint8_t m = pc_vga_get_ac(0x10);
            pc_vga_set_ac(0x10, (uint8_t)(bl ? m | 0x08 : m & ~0x08));
            break;
        }
        case 0x07: x86_set_r8(c, R_BH, pc_vga_get_ac(bl & 0x1F)); break;
        case 0x08: x86_set_r8(c, R_BH, pc_vga_get_ac(0x11)); break;
        case 0x09:
            for (int i = 0; i < 16; i++) pc_wr8(c, es, (uint16_t)(dx + i), pc_vga_get_ac(i));
            pc_wr8(c, es, (uint16_t)(dx + 16), pc_vga_get_ac(0x11));
            break;
        case 0x10:
            rgb[0] = x86_get_r8(c, R_DH); rgb[1] = x86_get_r8(c, R_CH); rgb[2] = x86_get_r8(c, R_CL);
            pc_vga_set_dac(bx, rgb);
            break;
        case 0x12:
            for (int i = 0; i < (int)x86_get_r16(c, R_CX); i++) {
                for (int k = 0; k < 3; k++) rgb[k] = pc_rd8(c, es, (uint16_t)(dx + i * 3 + k));
                pc_vga_set_dac(bx + i, rgb);
            }
            break;
        case 0x15:
            pc_vga_get_dac(bx, rgb);
            x86_set_r8(c, R_DH, rgb[0]); x86_set_r8(c, R_CH, rgb[1]); x86_set_r8(c, R_CL, rgb[2]);
            break;
        case 0x17:
            for (int i = 0; i < (int)x86_get_r16(c, R_CX); i++) {
                pc_vga_get_dac(bx + i, rgb);
                for (int k = 0; k < 3; k++) pc_wr8(c, es, (uint16_t)(dx + i * 3 + k), rgb[k]);
            }
            break;
        case 0x1A:
            x86_set_r8(c, R_BL, (uint8_t)(pc_vga_get_ac(0x10) >> 7));
            x86_set_r8(c, R_BH, pc_vga_get_ac(0x14));
            break;
        }
        break;
    }
    case 0x11: {                                   /* character generator */
        int h = 0;
        switch (al) {
        case 0x01: case 0x11: h = 14; break;       /* the ROM 8x14 set: 28 rows */
        case 0x02: case 0x12: h = 8; break;        /* 8x8: 50 rows */
        case 0x04: case 0x14: h = 16; break;       /* 8x16: 25 rows */
        case 0x30: {
            int ch = pc_rd8(c, BDA, 0x85);
            x86_set_r16(c, R_CX, (uint16_t)(ch ? ch : 16));
            x86_set_r8(c, R_DL, (uint8_t)(rows(c) - 1));
            break;
        }
        }
        if (h) {
            pc_wr8(c, BDA, 0x85, (uint8_t)h);
            pc_wr8(c, BDA, 0x84, (uint8_t)(400 / h - 1));
            pc_vga_set_char_height(h);
            cursor_shape(c, 0x0607);
        }
        break;
    }
    case 0x12:
        if (bl == 0x10) { x86_set_r8(c, R_BH, 0); x86_set_r8(c, R_BL, 3); x86_set_r16(c, R_CX, 0); }
        else if (bl == 0x30) x86_set_r8(c, R_AL, 0x12);
        else if (bl >= 0x31 && bl <= 0x36) x86_set_r8(c, R_AL, 0x12);
        break;
    case 0x13: {   /* write string ES:BP, CX chars, at DH,DL; AL bits: 0 move cursor, 1 attrs inline */
        int row = x86_get_r8(c, R_DH), col = x86_get_r8(c, R_DL);
        int n = x86_get_r16(c, R_CX);
        uint16_t p = x86_get_r16(c, R_BP);
        int saved_row, saved_col; get_cursor(c, bh & 7, &saved_row, &saved_col);
        set_cursor(c, bh & 7, row, col);
        for (int i = 0; i < n; i++) {
            uint8_t ch = pc_rd8(c, c->seg[S_ES].sel, p++);
            uint8_t at = (al & 2) ? pc_rd8(c, c->seg[S_ES].sel, p++) : (uint8_t)bl;
            if (ch == 0x0D || ch == 0x0A || ch == 0x08 || ch == 0x07) { pc_video_teletype(c, ch); continue; }
            int r, cl; get_cursor(c, bh & 7, &r, &cl);
            uint16_t off = cell_off(c, bh & 7, r, cl);
            pc_wr8(c, PC_VIDEO_SEG, off, ch);
            pc_wr8(c, PC_VIDEO_SEG, (uint16_t)(off + 1), at);
            pc_video_teletype(c, ch);                   /* advances/scrolls; rewrites ch, keeps attr */
        }
        if (!(al & 1)) set_cursor(c, bh & 7, saved_row, saved_col);
        break;
    }
    case 0x1A:
        if (al == 0) { x86_set_r8(c, R_AL, 0x1A); x86_set_r16(c, R_BX, 0x0008); }
        break;
    case 0x1B:
        x86_set_r8(c, R_AL, 0);      /* not supported */
        break;
    case 0xFE:                        /* TopView: get video buffer — unchanged */
        break;
    default:
        break;
    }
}

/* ---- tty painter -------------------------------------------------------- */
static void query_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) { term_rows = ws.ws_row; term_cols = ws.ws_col; }
}

static void emit_attr(FILE *f, uint8_t attr) {
    int fg = attr & 0x0F, bg = (attr >> 4) & 0x07, blink = attr & 0x80;
    static const int ansi[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    fprintf(f, "\033[0;%d;%dm", (fg & 8 ? 90 : 30) + ansi[fg & 7], 40 + ansi[bg]);
    if (blink) fputs("\033[5m", f);
}

void pc_video_set_hud(const char *text) {
    char next[PC_COLS + 1];
    if (text && *text) snprintf(next, sizeof next, "%-80.80s", text); else next[0] = 0;
    if (strcmp(next, hud_text) != 0) { memcpy(hud_text, next, sizeof hud_text); hud_dirty = 1; }
}

static void tty_init(void) {
    fputs("\033[?1049h\033[0m\033[2J\033[H\033[?25l", stdout);
    fflush(stdout);
    query_size();
    shadow_valid = 0;
    hud_dirty = 1;
    active = 1;
}

void pc_video_shutdown(void) {
    if (!active) return;
    fputs("\033[0m\033[?25h\033[?1049l", stdout);
    fflush(stdout);
    active = 0;
}

void pc_video_flush(int force) {
    if (!pc.tty_mode) { if (!force) fflush(stdout); return; }
    if (!active) tty_init();
    x86_cpu *c = pc.cpu;
    uint64_t now = pc_now_ns();
    if (!force && now - last_paint_ns < 16000000ull) return;   /* ~60 Hz cap */
    last_paint_ns = now;

    FILE *f = stdout;
    const uint8_t *vm = c->mem + ((uint32_t)PC_VIDEO_SEG << 4) + page_off(page(c));
    int nr = rows(c) < PC_ROWS ? rows(c) : PC_ROWS, nc = cols(c) < PC_COLS ? cols(c) : PC_COLS;
    int cur_attr = -1, painted = 0;
    for (int r = 0; r < nr && r < term_rows; r++) {
        int col = 0;
        while (col < nc) {
            const uint8_t *cell = vm + (r * cols(c) + col) * 2;
            uint8_t *sh = shadow + (r * PC_COLS + col) * 2;
            if (shadow_valid && cell[0] == sh[0] && cell[1] == sh[1]) { col++; continue; }
            fprintf(f, "\033[%d;%dH", r + 1, col + 1);
            while (col < nc) {
                cell = vm + (r * cols(c) + col) * 2;
                sh = shadow + (r * PC_COLS + col) * 2;
                if (shadow_valid && cell[0] == sh[0] && cell[1] == sh[1]) break;
                if (cell[1] != cur_attr) { emit_attr(f, cell[1]); cur_attr = cell[1]; }
                put_utf8(f, cp437[cell[0]]);
                sh[0] = cell[0]; sh[1] = cell[1];
                col++;
                painted++;
            }
        }
    }
    shadow_valid = 1;
    if (hud_dirty && term_rows > nr) {
        fprintf(f, "\033[%d;1H\033[0;7m%-*.*s\033[0m", nr + 1, term_cols < PC_COLS ? term_cols : PC_COLS,
                term_cols < PC_COLS ? term_cols : PC_COLS, hud_text);
        strcpy(hud_painted, hud_text);
        hud_dirty = 0;
        painted++;
    }
    int row, col; get_cursor(c, page(c), &row, &col);
    if (painted || row != pc.cursor_row || col != pc.cursor_col || force) {
        if (cur_attr != -1) fputs("\033[0m", f);
        if (row < nr) fprintf(f, "\033[%d;%dH\033[?25h", row + 1, col + 1);
        else fputs("\033[?25l", f);
        pc.cursor_row = row; pc.cursor_col = col;
        fflush(f);
    }
}

void pc_video_dump(x86_cpu *c, FILE *f) {
    int nr = rows(c) < PC_ROWS ? rows(c) : PC_ROWS, nc = cols(c) < PC_COLS ? cols(c) : PC_COLS;
    const uint8_t *vm = c->mem + ((uint32_t)PC_VIDEO_SEG << 4) + page_off(page(c));
    for (int r = 0; r < nr; r++) {
        int last = nc;
        while (last > 0 && (vm[(r * cols(c) + last - 1) * 2] == ' ' || vm[(r * cols(c) + last - 1) * 2] == 0)) last--;
        for (int col = 0; col < last; col++) {
            uint8_t ch = vm[(r * cols(c) + col) * 2];
            put_utf8(f, ch ? cp437[ch] : ' ');
        }
        fputc('\n', f);
    }
}
