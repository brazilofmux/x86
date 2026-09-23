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
static uint8_t pending[1024];                    /* big enough that a scripted burst of escape sequences is not split mid-sequence */
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
 * Every host key becomes what a keyboard would send: shift make, key
 * make, key break, shift break. pc_poll latches one code at port 60h
 * and raises INT 9; the next code waits until the guest's handler has
 * read the port. Whoever owns INT 9 translates: a program's own handler
 * (WP, games) does its thing, and the BIOS default (bios_int9 below)
 * pushes the (ascii, scancode) pair we remembered for the make code
 * into the ring buffer at 40:1E. */
typedef struct { uint8_t code, ascii; } rawkey;
static rawkey rawq[256];
static int rawq_head, rawq_tail;
static uint8_t latched_ascii;
static void raw_enqueue(uint8_t code, uint8_t ascii) {
    if (pc.debug > 1) fprintf(stderr, "[kbd] raw %02X (ascii %02X)\n", code, ascii);
    int next = (rawq_tail + 1) & 255;
    if (next == rawq_head) return;
    rawq[rawq_tail] = (rawkey){ code, ascii }; rawq_tail = next;
}
int pc_kbd_raw_pending(void) { return rawq_head != rawq_tail; }
void pc_kbd_raw_key(uint8_t code, uint8_t ascii) { raw_enqueue(code, ascii); }
int pc_kbd_raw_next(uint8_t *code) {
    if (rawq_head == rawq_tail) return 0;
    *code = rawq[rawq_head].code; latched_ascii = rawq[rawq_head].ascii;
    rawq_head = (rawq_head + 1) & 255;
    return 1;
}

static int needs_shift(uint8_t ascii) {
    if (ascii >= 'A' && ascii <= 'Z') return 1;
    return ascii < 0x7F && strchr("!@#$%^&*()_+{}|:\"<>?~", ascii) != NULL;
}

/* Queue a host key as scancodes. */
static void key_to_raw(uint8_t ascii, uint8_t scancode) {
    if (!scancode) return;
    int shift = ascii && needs_shift(ascii), ctrl = ascii >= 1 && ascii <= 26 && ascii != 8 && ascii != 9 && ascii != 13;
    int ext = ascii == 0 && (scancode == 0x48 || scancode == 0x50 || scancode == 0x4B || scancode == 0x4D ||
                             scancode == 0x47 || scancode == 0x4F || scancode == 0x49 || scancode == 0x51 ||
                             scancode == 0x52 || scancode == 0x53);
    /* ascii 0 on an ordinary key = Alt+key (ESC-letter in the script) */
    int alt = ascii == 0 && !ext && scancode < 0x3B && scancode != 0x01;
    if (shift) raw_enqueue(0x2A, 0);
    if (ctrl) raw_enqueue(0x1D, 0);
    if (alt) raw_enqueue(0x38, 0);
    if (ext) raw_enqueue(0xE0, 0);
    raw_enqueue(scancode, ascii);
    if (ext) raw_enqueue(0xE0, 0);
    raw_enqueue((uint8_t)(scancode | 0x80), 0);
    if (alt) raw_enqueue(0xB8, 0);
    if (ctrl) raw_enqueue(0x9D, 0);
    if (shift) raw_enqueue(0xAA, 0);
}

/* ---- BIOS ring buffer ---------------------------------------------------- */
void pc_kbd_push(x86_cpu *c, uint8_t ascii, uint8_t scancode) {
    uint16_t head = pc_rd16(c, BDA, 0x1A), tail = pc_rd16(c, BDA, 0x1C);
    uint16_t start = pc_rd16(c, BDA, 0x80), end = pc_rd16(c, BDA, 0x82);
    uint16_t next = (uint16_t)(tail + 2 >= end ? start : tail + 2);
    if (next == head) return;                        /* full: drop */
    pc_wr16(c, BDA, tail, (uint16_t)((scancode << 8) | ascii));
    pc_wr16(c, BDA, 0x1C, next);
}

/* Default INT 9: the code latched at port 60h was ours, so the make
 * code's (ascii, scancode) goes into the buffer. Shift/ctrl makes and
 * all breaks are dropped; E0-prefixed keys keep ascii 0. */
void pc_kbd_int9(x86_cpu *c, int vector) {
    (void)vector;
    pc.irq9_busy = 0;
    pc.irq_in_service &= ~2;                     /* the BIOS handler's EOI */
    uint8_t code = pc.last_scancode;
    /* shift state → BDA 40:17 (bit 0 rshift, 1 lshift, 2 ctrl, 3 alt) */
    uint8_t flags = pc_rd8(c, BDA, 0x17);
    int down = !(code & 0x80);
    switch (code & 0x7F) {
    case 0x2A: flags = (uint8_t)(down ? flags | 2 : flags & ~2); pc_wr8(c, BDA, 0x17, flags); return;
    case 0x36: flags = (uint8_t)(down ? flags | 1 : flags & ~1); pc_wr8(c, BDA, 0x17, flags); return;
    case 0x1D: flags = (uint8_t)(down ? flags | 4 : flags & ~4); pc_wr8(c, BDA, 0x17, flags); return;
    case 0x38: flags = (uint8_t)(down ? flags | 8 : flags & ~8); pc_wr8(c, BDA, 0x17, flags); return;
    }
    if (!down || code == 0xE0) return;
    int alt = (flags & 8) != 0, ctrl = (flags & 4) != 0, shift = (flags & 3) != 0;
    if (code >= 0x3B && code <= 0x44) {                               /* F1-F10 with modifiers */
        int m = alt ? 0x2D : ctrl ? 0x23 : shift ? 0x19 : 0;
        pc_kbd_push(c, 0, (uint8_t)(code + m)); return;
    }
    if (code == 0x57 || code == 0x58) code = (uint8_t)(0x85 + (code - 0x57));   /* F11/F12's make codes */
    if (code == 0x85 || code == 0x86) {                               /* F11/F12 */
        int m = alt ? 6 : ctrl ? 4 : shift ? 2 : 0;
        pc_kbd_push(c, 0, (uint8_t)(code + m)); return;
    }
    /* The enhanced keyboard's extended codes (what an AT BIOS's INT 9
     * stores; INT 16h hands them to the program as they are): Ctrl and
     * Alt on the navigation keys, Tab/Backspace/Enter with modifiers,
     * Alt on the number row. Word processors live on these. */
    if (code >= 0x47 && code <= 0x53 && code != 0x4A && code != 0x4C && code != 0x4E && (ctrl || alt)) {
        static const uint8_t ctl[13] = { 0x77, 0x8D, 0x84, 0, 0x73, 0x8F, 0x74, 0, 0x75, 0x91, 0x76, 0x92, 0x93 };
        static const uint8_t alc[13] = { 0x97, 0x98, 0x99, 0, 0x9B, 0,    0x9D, 0, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3 };
        uint8_t x = alt ? alc[code - 0x47] : ctl[code - 0x47];
        if (x) { pc_kbd_push(c, 0, x); return; }
    }
    if (code == 0x0F && (shift || ctrl || alt)) { pc_kbd_push(c, 0, alt ? 0xA5 : ctrl ? 0x94 : 0x0F); return; }   /* Tab */
    if (code == 0x0E && (ctrl || alt)) { pc_kbd_push(c, alt ? 0 : 0x7F, 0x0E); return; }                      /* Backspace */
    if (code == 0x1C && (ctrl || alt)) { pc_kbd_push(c, alt ? 0 : 0x0A, alt ? 0xA6 : 0x1C); return; }        /* Enter */
    if (alt && code >= 0x02 && code <= 0x0D) { pc_kbd_push(c, 0, (uint8_t)(0x78 + (code - 0x02))); return; } /* Alt+1..= */
    if (alt) { pc_kbd_push(c, 0, code); return; }                     /* Alt+key */
    if (!latched_ascii && !(code >= 0x3B && code <= 0x44) && !(code >= 0x47 && code <= 0x53) && code != 0x85 && code != 0x86 && code != 0x01) return;
    pc_kbd_push(c, latched_ascii, code);
}

static void drain_raw_here(x86_cpu *c);

int pc_kbd_buffer_empty(x86_cpu *c) {
    pc.kbd_reads++;
    return pc_rd16(c, BDA, 0x1A) == pc_rd16(c, BDA, 0x1C);
}

int pc_kbd_peek(x86_cpu *c, uint16_t *key) {
    pc.kbd_reads++;
    drain_raw_here(c);
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
        /* CSI num [; mod] final — xterm: mod 2 shift, 3 alt, 5 ctrl */
        uint8_t k = pending[2];
        int num = 0, mod = 1, i = 2;
        while (i < npending && pending[i] >= '0' && pending[i] <= '9') num = num * 10 + (pending[i++] - '0');
        if (i < npending && pending[i] == ';') { i++; mod = 0; while (i < npending && pending[i] >= '0' && pending[i] <= '9') mod = mod * 10 + (pending[i++] - '0'); }
        if (i < npending) { k = pending[i]; used = i + 1; } else { k = 0; used = npending; }
        ascii = 0; sc = 0;
        int fkey = 0;                                   /* 1..12 */
        if (pending[1] == 'O' || (num == 0 || num == 1) ) {
            switch (k) {
            case 'A': sc = 0x48; break; case 'B': sc = 0x50; break;
            case 'C': sc = 0x4D; break; case 'D': sc = 0x4B; break;
            case 'H': sc = 0x47; break; case 'F': sc = 0x4F; break;
            case 'P': fkey = 1; break; case 'Q': fkey = 2; break;
            case 'R': fkey = 3; break; case 'S': fkey = 4; break;
            }
        }
        if (!sc && !fkey && k == '~') {
            switch (num) {
            case 1: case 7: sc = 0x47; break; case 4: case 8: sc = 0x4F; break;
            case 2: sc = 0x52; break; case 3: sc = 0x53; break;
            case 5: sc = 0x49; break; case 6: sc = 0x51; break;
            case 11: fkey = 1; break; case 12: fkey = 2; break; case 13: fkey = 3; break;
            case 14: fkey = 4; break; case 15: fkey = 5; break; case 17: fkey = 6; break;
            case 18: fkey = 7; break; case 19: fkey = 8; break; case 20: fkey = 9; break;
            case 21: fkey = 10; break; case 23: fkey = 11; break; case 24: fkey = 12; break;
            }
        }
        if (fkey) sc = fkey <= 10 ? 0x3B + fkey - 1 : 0x57 + fkey - 11;   /* make codes (F11/F12: 57h/58h); the modifier travels separately */
        if (sc && mod != 1) {
            /* Shift/Ctrl/Alt + key as a keyboard sends it: the modifier's
             * make, the key, the breaks. Whoever owns INT 9 translates. */
            uint8_t modsc = mod == 2 ? 0x2A : mod == 5 ? 0x1D : 0x38;
            memmove(pending, pending + used, (size_t)(npending - used)); npending -= used;
            raw_enqueue(modsc, 0); raw_enqueue(sc, 0); raw_enqueue((uint8_t)(sc | 0x80), 0); raw_enqueue((uint8_t)(modsc | 0x80), 0);
            return 1;
        }
        if (!sc) { ascii = 0x1B; sc = 0x01; used = 1; }
    } else if (b == 0x1B && npending >= 2 && pending[1] == '+') {
        /* ESC + : change diskettes (next directory of a removable drive) */
        memmove(pending, pending + 2, (size_t)(npending - 2)); npending -= 2;
        if (pc.swap_disk) pc.swap_disk();
        return 1;
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
    (void)c;
    key_to_raw(ascii, sc);
    return 1;
}

/* Scripted keys (stdin not a terminal) arrive the way a person types
 * them: the next one only after the guest has asked for keyboard input
 * since the last, and at most one per 20 ms. A terminal's keys are
 * taken as they come. */
void pc_kbd_poll(x86_cpu *c) {
    static uint64_t next_ok, next_probe, reads_at_last;
    uint64_t now = pc_now_ns();
    /* next_key() probes stdin with poll(2) whenever its own buffer is
     * empty, and INT 16h status calls land here on every pass of a
     * program's idle loop. Rate-limit the probe itself, not just a
     * successful feed: WordPerfect polls the keyboard two million times
     * in a 17 s run, and one syscall apiece was 11 s of it. */
    if (!npending) {
        if (now < next_probe) return;
        next_probe = now + 1000000ull;                    /* 1 kHz is plenty for a keyboard */
    }
    if (raw_active) { while (next_key(c, 0)) { } return; }
    /* "Idle-polling" = several keyboard queries since the last key; a
     * single poll is often a flush, and a key fed then is lost. */
    /* A program that takes its keys from IRQ 1 and port 60h (DOOM) never
     * polls the BIOS at all; after a second of that silence, feed anyway. */
    static uint64_t last_fed;
    int polled = pc.kbd_reads >= reads_at_last + 8;
    int silent = pc.kbd_reads == reads_at_last && now - last_fed >= 1000000000ull;
    if (now < next_ok || !(polled || silent)) return;
    if (next_key(c, 0)) { next_ok = now + 20000000ull; reads_at_last = pc.kbd_reads; last_fed = now; }
}

/* Scripted stdin ran dry: hand the guest one Ctrl-Z, and if it is still
 * running two seconds later without having consumed anything more, end
 * the run — a headless test would otherwise spin forever in whatever
 * idle loop the program has. Called from pc_poll. */
void pc_kbd_idle_poll(x86_cpu *c) {
    static uint64_t deadline, reads_at_eof; static int fed_eof;
    if (!pc.eof_seen || isatty(STDIN_FILENO)) return;
    if (!fed_eof) {
        fed_eof = 1; key_to_raw(0x1A, 0x2C);
        deadline = pc_now_ns() + 2000000000ull; reads_at_eof = pc.kbd_reads;
        return;
    }
    if (pc_now_ns() < deadline) return;
    /* Only a program that is still asking for keys is stuck waiting for
     * them; one that never polls the BIOS (DOOM) is just running. */
    if (pc.kbd_reads == reads_at_eof) return;
    fprintf(stderr, "dos-monster: input exhausted, program still running\n");
    pc.exit_requested = 1; pc.exit_code = 1;
    c->halted = 1;
}

/* Inside a blocking BIOS/DOS read no INT 9 can run: move queued codes
 * straight through the default translation when INT 9 is ours. (A
 * program with its own INT 9 handler never blocks in INT 16h/00 with
 * an empty buffer unless it wants to sleep until its handler fills it,
 * which we cannot do from here — it gets the keys when it returns.) */
static void drain_raw_here(x86_cpu *c) {
    if (pc_rd16(c, 0, 9 * 4 + 2) != PC_HLE_SEG) return;
    /* pc.last_scancode is a one-code latch shared with the IRQ path. A
     * code already latched for an INT 9 that has not run yet has to be
     * translated before we overwrite it, or it is simply lost — which
     * shows up as a dropped keystroke, and only ever a visible one when
     * the same key is pressed twice in a row ("hello" -> "helo"). */
    if (pc.irq_pending & (1 << 9)) {
        pc.irq_pending &= ~(1 << 9);
        pc_kbd_int9(c, 9);
    }
    uint8_t code;
    while (pc_kbd_raw_next(&code)) { pc.last_scancode = code; pc_kbd_int9(c, 9); }
}

void pc_kbd_wait(x86_cpu *c) {
    while (pc_kbd_buffer_empty(c)) {
        pc_video_flush(1);
        drain_raw_here(c);
        if (!pc_kbd_buffer_empty(c)) break;
        if (pc.eof_seen) {
            /* Scripted input ran out: hand the program a Ctrl-Z once,
             * then treat further waits as "nothing more will happen". */
            static int fed_eof;
            if (!fed_eof) { fed_eof = 1; key_to_raw(0x1A, 0x2C); return; }
            fprintf(stderr, "dos-monster: input exhausted while waiting for a key\n");
            pc.exit_requested = 1; pc.exit_code = 1;
            c->halted = 1;
            return;
        }
        pc.kbd_reads++;
        /* Waiting on the host, not emulating: charged separately so it
          * is not mistaken for the cost of running the service. */
        uint64_t w0 = pc_now_ns();
        int ready = host_readable(50);
        pc.blocked_ns += pc_now_ns() - w0;
        pc.blocked_calls++;
        if (ready) pc_kbd_poll(c);
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
