/* pc_kbd.c — host keyboard → BIOS keyboard buffer (40:1E) + INT 16h.
 *
 * Host bytes arrive from stdin (raw mode when it is a terminal, plain
 * reads when it is a pipe or file). Escape sequences become the
 * extended keys (arrows, Home/End, PgUp/PgDn, Ins/Del, F1–F12); every
 * key becomes an (ascii, scancode) pair with the scancode of a US
 * layout so programs that inspect AH see what they expect. The pair
 * goes into the BIOS ring buffer directly; INT 9 is raised for guests
 * that hook it, with the scancode readable at port 60h.
 *
 * Lifted in spirit from ~/z80/kaypro/kaypro_kbd.c.
 */
#include "pc.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>

#define BDA PC_BDA_SEG

static struct termios saved_tio;
static int raw_active;
static uint8_t pending[64];
static int npending;

/* Scancodes for ASCII 0x20..0x7E (US layout, unshifted key). */
static const uint8_t sc_ascii[95] = {
    0x39,0x02,0x28,0x04,0x05,0x06,0x08,0x28,0x0A,0x0B,0x09,0x0D,0x33,0x0C,0x34,0x35,   /* space ! " # $ % & ' ( ) * + , - . / */
    0x0B,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x27,0x27,0x33,0x0D,0x34,0x35,   /* 0-9 : ; < = > ? */
    0x03,0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,   /* @ A-O */
    0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C,0x1A,0x2B,0x1B,0x07,0x0C,   /* P-Z [ \ ] ^ _ */
    0x29,0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,   /* ` a-o */
    0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C,0x1A,0x2B,0x1B,0x29,        /* p-z { | } ~ */
};

static uint8_t scancode_for(uint8_t ascii) {
    if (ascii >= 0x20 && ascii < 0x7F) return sc_ascii[ascii - 0x20];
    switch (ascii) {
    case 0x0D: return 0x1C;
    case 0x08: case 0x7F: return 0x0E;
    case 0x09: return 0x0F;
    case 0x1B: return 0x01;
    case 0x0A: return 0x1C;
    default:
        if (ascii >= 1 && ascii <= 26) return sc_ascii['a' + ascii - 1 - 0x20];   /* Ctrl-letter */
        return 0;
    }
}

void pc_kbd_init(void) {
    if (isatty(STDIN_FILENO) && pc.tty_mode) {
        tcgetattr(STDIN_FILENO, &saved_tio);
        struct termios t = saved_tio;
        t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        t.c_iflag &= ~(IXON | ICRNL | INLCR);
        t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        raw_active = 1;
    }
}

void pc_kbd_shutdown(void) {
    if (raw_active) { tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio); raw_active = 0; }
}

/* ---- Raw scancode stream ------------------------------------------------
 * Programs that take the keyboard over (INT 9 + port 60h: Borland's
 * IDEs, games) see what a keyboard would send: shift make, key make,
 * key break, shift break. One code is latched per INT 9; pc_poll raises
 * the next after the guest's handler has returned. */
static uint8_t rawq[256];
static int rawq_head, rawq_tail;
static void raw_enqueue(uint8_t code) {
    int next = (rawq_tail + 1) & 255;
    if (next == rawq_head) return;
    rawq[rawq_tail] = code; rawq_tail = next;
}
static int raw_dequeue(uint8_t *code) {
    if (rawq_head == rawq_tail) return 0;
    *code = rawq[rawq_head]; rawq_head = (rawq_head + 1) & 255;
    return 1;
}
int pc_kbd_raw_pending(void) { return rawq_head != rawq_tail; }
int pc_kbd_raw_next(uint8_t *code) { return raw_dequeue(code); }

static int needs_shift(uint8_t ascii) {
    if (ascii >= 'A' && ascii <= 'Z') return 1;
    return ascii < 0x7F && strchr("!@#$%^&*()_+{}|:\"<>?~", ascii) != NULL;
}

/* ---- BIOS ring buffer ---------------------------------------------------- */
void pc_kbd_push(x86_cpu *c, uint8_t ascii, uint8_t scancode) {
    uint16_t head = pc_rd16(c, BDA, 0x1A), tail = pc_rd16(c, BDA, 0x1C);
    uint16_t start = pc_rd16(c, BDA, 0x80), end = pc_rd16(c, BDA, 0x82);
    uint16_t next = (uint16_t)(tail + 2 >= end ? start : tail + 2);
    if (next == head) return;                        /* full: drop */
    pc_wr16(c, BDA, tail, (uint16_t)((scancode << 8) | ascii));
    pc_wr16(c, BDA, 0x1C, next);
    if (!scancode) return;
    int shift = ascii && needs_shift(ascii), ctrl = ascii >= 1 && ascii <= 26 && ascii != 8 && ascii != 9 && ascii != 13;
    int ext = ascii == 0 && (scancode == 0x48 || scancode == 0x50 || scancode == 0x4B || scancode == 0x4D ||
                             scancode == 0x47 || scancode == 0x4F || scancode == 0x49 || scancode == 0x51 ||
                             scancode == 0x52 || scancode == 0x53);
    if (shift) raw_enqueue(0x2A);
    if (ctrl) raw_enqueue(0x1D);
    if (ext) raw_enqueue(0xE0);
    raw_enqueue(scancode);
    if (ext) raw_enqueue(0xE0);
    raw_enqueue((uint8_t)(scancode | 0x80));
    if (ctrl) raw_enqueue(0x9D);
    if (shift) raw_enqueue(0xAA);
}

int pc_kbd_buffer_empty(x86_cpu *c) {
    pc.kbd_reads++;
    return pc_rd16(c, BDA, 0x1A) == pc_rd16(c, BDA, 0x1C);
}

int pc_kbd_peek(x86_cpu *c, uint16_t *key) {
    pc.kbd_reads++;
    uint16_t head = pc_rd16(c, BDA, 0x1A);
    if (head == pc_rd16(c, BDA, 0x1C)) return 0;
    *key = pc_rd16(c, BDA, head);
    return 1;
}

int pc_kbd_get(x86_cpu *c, uint16_t *key) {
    pc.kbd_reads++;
    uint16_t head = pc_rd16(c, BDA, 0x1A);
    if (head == pc_rd16(c, BDA, 0x1C)) return 0;
    *key = pc_rd16(c, BDA, head);
    uint16_t start = pc_rd16(c, BDA, 0x80), end = pc_rd16(c, BDA, 0x82);
    pc_wr16(c, BDA, 0x1A, (uint16_t)(head + 2 >= end ? start : head + 2));
    return 1;
}

/* ---- Host input ---------------------------------------------------------- */
static int host_readable(int timeout_ms) {
    struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
    return poll(&p, 1, timeout_ms) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL));   /* macOS: /dev/null polls as POLLNVAL */
}

static void fill_pending(void) {
    if (npending || pc.eof_seen) return;
    ssize_t n = read(STDIN_FILENO, pending, sizeof pending);
    if (n <= 0) { pc.eof_seen = 1; return; }
    npending = (int)n;
}

/* Translate one key from the pending bytes; returns 0 if none. */
static int next_key(x86_cpu *c, int blocking) {
    if (!npending) { if (blocking || host_readable(0)) fill_pending(); }
    if (!npending) return 0;
    uint8_t b = pending[0];
    int used = 1;
    uint8_t ascii = b, sc = scancode_for(b);
    if (b == 0x1B && npending >= 3 && (pending[1] == '[' || pending[1] == 'O')) {
        /* CSI / SS3 sequences: arrows, home/end, function keys */
        uint8_t k = pending[2];
        int num = 0, i = 2;
        while (i < npending && pending[i] >= '0' && pending[i] <= '9') num = num * 10 + (pending[i++] - '0');
        if (i < npending) { k = pending[i]; used = i + 1; } else { k = 0; used = npending; }
        ascii = 0; sc = 0;
        if (pending[1] == 'O' || num == 0) {
            switch (k) {
            case 'A': sc = 0x48; break; case 'B': sc = 0x50; break;
            case 'C': sc = 0x4D; break; case 'D': sc = 0x4B; break;
            case 'H': sc = 0x47; break; case 'F': sc = 0x4F; break;
            case 'P': sc = 0x3B; break; case 'Q': sc = 0x3C; break;
            case 'R': sc = 0x3D; break; case 'S': sc = 0x3E; break;
            }
        } else if (k == '~') {
            switch (num) {
            case 1: case 7: sc = 0x47; break; case 4: case 8: sc = 0x4F; break;
            case 2: sc = 0x52; break; case 3: sc = 0x53; break;
            case 5: sc = 0x49; break; case 6: sc = 0x51; break;
            case 11: sc = 0x3B; break; case 12: sc = 0x3C; break; case 13: sc = 0x3D; break;
            case 14: sc = 0x3E; break; case 15: sc = 0x3F; break; case 17: sc = 0x40; break;
            case 18: sc = 0x41; break; case 19: sc = 0x42; break; case 20: sc = 0x43; break;
            case 21: sc = 0x44; break; case 23: sc = 0x85; break; case 24: sc = 0x86; break;
            }
        }
        if (!sc) { ascii = 0x1B; sc = 0x01; used = 1; }
    } else if (b == 0x1B && npending >= 2 && ((pending[1] >= 'a' && pending[1] <= 'z') || (pending[1] >= '0' && pending[1] <= '9'))) {
        /* ESC letter/digit: Alt+key, as xterm sends it (ascii 0, key scancode) */
        ascii = 0; sc = scancode_for(pending[1]); used = 2;
    } else if (b == 0x0A && !raw_active) {
        ascii = 0x0D; sc = 0x1C;                       /* piped newline is Enter */
    } else if (b == 0x7F) {
        ascii = 0x08; sc = 0x0E;
    }
    memmove(pending, pending + used, (size_t)(npending - used));
    npending -= used;
    pc_kbd_push(c, ascii, sc);
    return 1;
}

/* Scripted keys (stdin not a terminal) arrive the way a person types
 * them: the next one only after the guest has asked for keyboard input
 * since the last, and at most one per 20 ms. A terminal's keys are
 * taken as they come. */
void pc_kbd_poll(x86_cpu *c) {
    static uint64_t next_ok, reads_at_last;
    if (raw_active) { while (next_key(c, 0)) { } return; }
    uint64_t now = pc_now_ns();
    /* "Idle-polling" = several keyboard queries since the last key; a
     * single poll is often a flush, and a key fed then is lost. */
    if (now < next_ok || pc.kbd_reads < reads_at_last + 8) return;
    if (next_key(c, 0)) { next_ok = now + 20000000ull; reads_at_last = pc.kbd_reads; }
}

/* Scripted stdin ran dry: hand the guest one Ctrl-Z, and if it is still
 * running two seconds later without having consumed anything more, end
 * the run — a headless test would otherwise spin forever in whatever
 * idle loop the program has. Called from pc_poll. */
void pc_kbd_idle_poll(x86_cpu *c) {
    static uint64_t deadline; static int fed_eof;
    if (!pc.eof_seen || isatty(STDIN_FILENO)) return;
    if (!fed_eof) { fed_eof = 1; pc_kbd_push(c, 0x1A, 0x2C); deadline = pc_now_ns() + 2000000000ull; return; }
    if (pc_now_ns() < deadline) return;
    fprintf(stderr, "dos-monster: input exhausted, program still running\n");
    pc.exit_requested = 1; pc.exit_code = 1;
    c->halted = 1;
}

void pc_kbd_wait(x86_cpu *c) {
    while (pc_kbd_buffer_empty(c)) {
        pc_video_flush(1);
        if (pc.eof_seen) {
            /* Scripted input ran out: hand the program a Ctrl-Z once,
             * then treat further waits as "nothing more will happen". */
            static int fed_eof;
            if (!fed_eof) { fed_eof = 1; pc_kbd_push(c, 0x1A, 0x2C); return; }
            fprintf(stderr, "dos-monster: input exhausted while waiting for a key\n");
            pc.exit_requested = 1; pc.exit_code = 1;
            c->halted = 1;
            return;
        }
        pc.kbd_reads++;
        if (host_readable(50)) pc_kbd_poll(c);
    }
}

/* ---- INT 16h --------------------------------------------------------------- */
void pc_kbd_int16(x86_cpu *c, int vector) {
    (void)vector;
    uint16_t key;
    if (pc.debug > 1) fprintf(stderr, "[bios] INT 16h AH=%02X from %04X:%04X @%llu stack: %04X %04X %04X %04X\n", x86_get_r8(c, R_AH),
                              pc_rd16(c, c->seg[S_SS].sel, (uint16_t)(c->r[R_SP] + 2)), pc_rd16(c, c->seg[S_SS].sel, (uint16_t)c->r[R_SP]),
                              (unsigned long long)c->insn_count,
                              pc_rd16(c, c->seg[S_SS].sel, (uint16_t)(c->r[R_SP] + 6)), pc_rd16(c, c->seg[S_SS].sel, (uint16_t)(c->r[R_SP] + 8)),
                              pc_rd16(c, c->seg[S_SS].sel, (uint16_t)(c->r[R_SP] + 10)), pc_rd16(c, c->seg[S_SS].sel, (uint16_t)(c->r[R_SP] + 12)));
    switch (x86_get_r8(c, R_AH)) {
    case 0x00: case 0x10:
        if (pc.debug > 2) pc_video_dump(c, stderr);
        pc_kbd_wait(c);
        if (pc_kbd_get(c, &key)) x86_set_r16(c, R_AX, key);
        if (pc.debug > 1) fprintf(stderr, "[bios] INT 16h read → %04X (IVT 9 = %04X:%04X, IVT 16 = %04X:%04X, shift %02X)\n", key,
                                  pc_rd16(c, 0, 9 * 4 + 2), pc_rd16(c, 0, 9 * 4), pc_rd16(c, 0, 0x16 * 4 + 2), pc_rd16(c, 0, 0x16 * 4), pc_rd8(c, BDA, 0x17));
        break;
    case 0x01: case 0x11:
        pc_kbd_poll(c);
        if (pc_kbd_peek(c, &key)) { x86_set_r16(c, R_AX, key); c->eflags &= ~X86_ZF; }
        else c->eflags |= X86_ZF;
        break;
    case 0x02: case 0x12:
        x86_set_r8(c, R_AL, pc_rd8(c, BDA, 0x17));
        if (x86_get_r8(c, R_AH) == 0x12) x86_set_r8(c, R_AH, pc_rd8(c, BDA, 0x18));
        break;
    case 0x03:                                          /* typematic: accepted */
        break;
    case 0x05:
        pc_kbd_push(c, x86_get_r8(c, R_CL), x86_get_r8(c, R_CH));
        x86_set_r8(c, R_AL, 0);
        break;
    default:
        break;
    }
}
