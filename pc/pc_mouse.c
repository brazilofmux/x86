/* pc_mouse.c — a Microsoft-compatible mouse driver (INT 33h) in the host
 *
 * Installed only when there is a mouse to take input from — the window,
 * the -t terminal's SGR mouse reports, or a script's (X86_MOUSE=1): a
 * headless run keeps "no mouse", as it always had. The pointer arrives
 * in a 640x480 frame (the window's logical size; terminal cells mapped
 * onto it); here that becomes the driver's virtual screen — 640 wide in every text and CGA-era mode, 8
 * units a character cell (16 in 40 columns), rows x 8 high in text — and
 * button presses, releases and motion counters (mickeys) accumulate as
 * the real driver's do. Programs poll (AX=03h, 05h, 06h, 0Bh) or install
 * an event handler (AX=0Ch/14h), which is called from pc_mouse_poll the
 * way the driver's own IRQ handler calls it: with the event mask, buttons,
 * position and raw mickeys in registers, returning with a far RET.
 *
 * The window's pointer is absolute and many programs' are not: WordPerfect
 * never asks where the mouse is, only how far it moved (AX=0Bh), and
 * keeps a pointer of its own that starts wherever it likes and doubles
 * any move over 50 mickeys. So the motion counters are not the host's
 * deltas but a walk: first one long move down and left, which pins any
 * clamping program's pointer to the bottom-left corner, then steps of at
 * most MICKEY_STEP toward the host position counted from that corner, at
 * the default 8 mickeys per 8 pixels across and 16 down. A program that
 * counts the way the driver's defaults do then has its pointer where the
 * host's is, with no capture and no acceleration. Clamping loses motion
 * at the program's edges, so
 * the walk starts over from the corner before every click that begins
 * with no button down; a drag is left alone. Events wait in a queue
 * and each is handed over once the walk has reached where it happened,
 * one handler call apiece — WordPerfect finds clicks by comparing the
 * buttons in BX with the last call's, so a press and release in one call
 * would be no click at all.
 *
 * The text-mode cursor is the software cursor: the cell under the pointer
 * drawn as (cell AND screen mask) XOR cursor mask — by default its colours
 * inverted. The renderer draws it (pc_mouse_text_cursor) instead of the
 * driver writing video memory, so a program reading the screen never
 * finds it there, and hiding and showing costs nothing.
 */
#include "pc.h"
#include <stdio.h>
#include <string.h>

#define TRAP_HANDLER_RET 0xF6        /* F000:00F6: an event handler RETFed here */
#define MICKEY_STEP 32               /* per report: under WordPerfect's 50-mickey doubling */
#define MICKEY_PIN  1000             /* the first report, down and left: past any screen */
#define QUEUE 32

static struct {
    int installed;
    int x, y;                        /* virtual screen position */
    int minx, maxx, miny, maxy;
    int buttons;                     /* bit 0 left, 1 right, 2 middle */
    int visible;                     /* 0 shown; negative hidden (show/hide nest) */
    int press_n[3], rel_n[3];
    int press_x[3], press_y[3], rel_x[3], rel_y[3];
    int mick_x, mick_y;              /* since the last AX=0Bh */
    int raw_mick_x, raw_mick_y;      /* running, for the event handler's SI/DI */
    int ratio_x, ratio_y;            /* mickeys per 8 pixels */
    uint16_t screen_mask, cursor_mask;
    uint16_t ev_mask, ev_seg, ev_off;/* event handler */
    struct { int mask, buttons, x, y, pin; } q[QUEUE];   /* events not yet handed to the handler */
    int qn;
    int pinned;                      /* the corner move has been reported */
    int sent_x, sent_y;              /* where the reports put the pointer, in mickeys */
    int shown_buttons;               /* the buttons as of the last handler call */
    int in_handler;
    uint32_t saved[8], saved_eflags; /* the interrupted state while a handler runs */
    uint16_t saved_seg[4];
    uint16_t saved_ip, saved_cs;
    int sens_x, sens_y, sens_thr;
} m;

static int virt_w(x86_cpu *c) { (void)c; return 640; }
static int virt_h(x86_cpu *c) {
    int mode = pc_rd8(c, PC_BDA_SEG, 0x49) & 0x7F;
    if (mode <= 3 || mode == 7) return (pc_rd8(c, PC_BDA_SEG, 0x84) + 1) * 8;
    if (mode == 0x11 || mode == 0x12) return 480;
    if (mode == 0x0F || mode == 0x10) return 350;
    return 200;
}

static void clamp(void) {
    if (m.x < m.minx) m.x = m.minx;
    if (m.x > m.maxx) m.x = m.maxx;
    if (m.y < m.miny) m.y = m.miny;
    if (m.y > m.maxy) m.y = m.maxy;
}

static void reset(x86_cpu *c) {
    int installed = m.installed;
    uint16_t ev_mask = 0;
    memset(&m, 0, sizeof m);
    m.installed = installed;
    m.ev_mask = ev_mask;
    m.maxx = virt_w(c) - 1; m.maxy = virt_h(c) - 1;
    m.x = virt_w(c) / 2; m.y = virt_h(c) / 2;
    m.visible = -1;                                  /* hidden until AX=01h */
    m.ratio_x = 8; m.ratio_y = 16;
    m.screen_mask = 0x77FF; m.cursor_mask = 0x7700;  /* keep the character, invert the colours */
    m.sens_x = m.sens_y = 50; m.sens_thr = 50;
}

/* ---- from the window -------------------------------------------------- */

static void enqueue(int mask) {
    if (m.qn && (m.q[m.qn - 1].mask == 0x01 || m.qn == QUEUE) && (mask == 0x01 || m.qn == QUEUE)) {
        m.q[m.qn - 1].mask |= mask;                  /* moves coalesce; so does anything once full */
    } else {
        m.q[m.qn].mask = mask; m.q[m.qn].pin = 0; m.qn++;
    }
    m.q[m.qn - 1].buttons = m.buttons; m.q[m.qn - 1].x = m.x; m.q[m.qn - 1].y = m.y;
}

/* The pointer at (fx, fy) of a 640x480 frame. */
void pc_mouse_motion(int fx, int fy) {
    x86_cpu *c = pc.cpu;
    pc_ps2_motion(fx, fy);                           /* the PS/2 mouse sees the same hand */
    if (!m.installed || !c) return;
    int nx = fx * virt_w(c) / 640, ny = fy * virt_h(c) / 480;
    if (nx == m.x && ny == m.y) return;
    m.x = nx; m.y = ny;
    clamp();
    enqueue(0x01);
}

void pc_mouse_button(int button, int down) {
    pc_ps2_button(button, down);
    if (!m.installed || button < 0 || button > 2) return;
    int bit = 1 << button;
    if (down) {
        m.buttons |= bit;
        m.press_n[button]++; m.press_x[button] = m.x; m.press_y[button] = m.y;
    } else {
        m.buttons &= ~bit;
        m.rel_n[button]++; m.rel_x[button] = m.x; m.rel_y[button] = m.y;
    }
    /* event bits: 1 moved, 2/4 left press/release, 8/16 right, 32/64 middle */
    static const int ev_down[3] = { 0x02, 0x08, 0x20 }, ev_up[3] = { 0x04, 0x10, 0x40 };
    int was = m.buttons & ~bit;
    enqueue(down ? ev_down[button] : ev_up[button]);
    if (down && !was) m.q[m.qn - 1].pin = 1;         /* a click: walk from the corner again */
}

/* One step of the walk toward (x, y); 0 when already there. */
static int walk(int x, int y) {
    if (!m.pinned) {
        m.pinned = 1;
        m.mick_x -= MICKEY_PIN; m.mick_y += MICKEY_PIN;
        m.raw_mick_x -= MICKEY_PIN; m.raw_mick_y += MICKEY_PIN;
        m.sent_x = m.minx * m.ratio_x / 8; m.sent_y = m.maxy * m.ratio_y / 8;
        return 1;
    }
    int dx = x * m.ratio_x / 8 - m.sent_x, dy = y * m.ratio_y / 8 - m.sent_y;
    if (dx > MICKEY_STEP) dx = MICKEY_STEP; else if (dx < -MICKEY_STEP) dx = -MICKEY_STEP;
    if (dy > MICKEY_STEP) dy = MICKEY_STEP; else if (dy < -MICKEY_STEP) dy = -MICKEY_STEP;
    if (!dx && !dy) return 0;
    m.mick_x += dx; m.mick_y += dy; m.raw_mick_x += dx; m.raw_mick_y += dy;
    m.sent_x += dx; m.sent_y += dy;
    return 1;
}

/* ---- the text cursor, for the renderer --------------------------------- */

/* 1 and the cell (row, col) with its masks when a text cursor shows. */
int pc_mouse_text_cursor(int *row, int *col, uint16_t *screen_mask, uint16_t *cursor_mask) {
    x86_cpu *c = pc.cpu;
    if (!m.installed || m.visible < 0 || !c) return 0;
    int cols = pc_rd16(c, PC_BDA_SEG, 0x4A); if (!cols) cols = 80;
    *col = m.x / (640 / cols);
    *row = m.y / 8;
    *screen_mask = m.screen_mask; *cursor_mask = m.cursor_mask;
    return 1;
}

/* ---- the event handler --------------------------------------------------
 * pc_poll calls this between runs; with a handler installed for a
 * pending event and interrupts on, the interrupted CS:IP and registers
 * are kept and the handler entered with the driver's calling convention,
 * its far RET landing on the TRAP_HANDLER_RET trap, which puts everything
 * back. One call at a time, as the driver's own IRQ handler is not
 * reentrant. Real mode only: a DPMI client's handler is DOS/4GW's
 * business (it installs a real-mode callback, which is just as well). */
static int handler_walks(void) { return m.ev_seg && (m.ev_mask & 0x01); }

int pc_mouse_poll(x86_cpu *c) {
    if (!m.installed || !m.qn || m.in_handler) return 0;
    if (!m.ev_seg || !m.ev_mask) { m.qn = 0; return 0; }      /* nobody to tell */
    if (c->pmode || !(c->eflags & X86_IF) || c->int_inhibit) return 0;
    int ev, bx, x, y;
    if (m.q[0].pin) { m.q[0].pin = 0; m.pinned = 0; }
    if (handler_walks() && walk(m.q[0].x, m.q[0].y)) {
        /* on the way to the next event: the mickeys walk, the position
         * a program reads is already where the pointer is */
        ev = 0x01; bx = m.shown_buttons; x = m.q[0].x; y = m.q[0].y;
    } else {
        ev = m.q[0].mask & m.ev_mask; bx = m.q[0].buttons; x = m.q[0].x; y = m.q[0].y;
        memmove(m.q, m.q + 1, (size_t)(m.qn - 1) * sizeof m.q[0]); m.qn--;
        m.shown_buttons = bx;
        if (handler_walks()) ev &= ~0x01;            /* the walk's calls were the move */
        if (!ev) return 0;
    }
    c->halted = 0;                                   /* the driver's IRQ ends a HLT, as any interrupt does */
    m.in_handler = 1;
    memcpy(m.saved, c->r, sizeof m.saved);
    m.saved_eflags = c->eflags;
    m.saved_seg[0] = c->seg[S_DS].sel; m.saved_seg[1] = c->seg[S_ES].sel;
    m.saved_cs = c->seg[S_CS].sel; m.saved_ip = (uint16_t)c->eip;
    /* the handler's return address: our trap */
    uint16_t sp = (uint16_t)(c->r[R_SP] - 4);
    pc_wr16(c, c->seg[S_SS].sel, sp, TRAP_HANDLER_RET);
    pc_wr16(c, c->seg[S_SS].sel, (uint16_t)(sp + 2), PC_HLE_SEG);
    x86_set_r16(c, R_SP, sp);
    x86_set_r16(c, R_AX, (uint16_t)ev);
    x86_set_r16(c, R_BX, (uint16_t)bx);
    x86_set_r16(c, R_CX, (uint16_t)x);
    x86_set_r16(c, R_DX, (uint16_t)y);
    x86_set_r16(c, R_SI, (uint16_t)m.raw_mick_x);
    x86_set_r16(c, R_DI, (uint16_t)m.raw_mick_y);
    c->eflags &= ~(uint32_t)X86_IF;                  /* as inside the driver's IRQ handler */
    x86_load_seg(c, S_CS, m.ev_seg);
    c->eip = m.ev_off;
    return 1;
}

static void handler_returned(x86_cpu *c, int vector) {
    (void)vector;
    memcpy(c->r, m.saved, sizeof m.saved);
    c->eflags = m.saved_eflags;
    x86_load_seg(c, S_DS, m.saved_seg[0]);
    x86_load_seg(c, S_ES, m.saved_seg[1]);
    x86_load_seg(c, S_CS, m.saved_cs);
    c->eip = m.saved_ip;
    m.in_handler = 0;
    pc.returned = 1;                                 /* we placed CS:IP ourselves */
}

/* ---- INT 33h ------------------------------------------------------------- */

static void int33(x86_cpu *c, int vector) {
    (void)vector;
    int ax = x86_get_r16(c, R_AX), bx = x86_get_r16(c, R_BX);
    int cx = x86_get_r16(c, R_CX), dx = x86_get_r16(c, R_DX);
    if (pc.debug > 1) fprintf(stderr, "[mouse] INT 33h AX=%04X BX=%04X CX=%04X DX=%04X ES=%04X from %04X:%04X\n", ax, bx, cx, dx, c->seg[S_ES].sel, pc_rd16(c, c->seg[S_SS].sel, (uint16_t)(c->r[R_SP] + 2)), pc_rd16(c, c->seg[S_SS].sel, (uint16_t)c->r[R_SP]));
    switch (ax) {
    case 0x00:                                       /* reset and status */
    case 0x21:                                       /* software reset */
        reset(c);
        x86_set_r16(c, R_AX, 0xFFFF);
        x86_set_r16(c, R_BX, 2);
        break;
    case 0x01: if (m.visible < 0) m.visible++; break;
    case 0x02: m.visible--; break;
    case 0x03:
        x86_set_r16(c, R_BX, (uint16_t)m.buttons);
        x86_set_r16(c, R_CX, (uint16_t)m.x);
        x86_set_r16(c, R_DX, (uint16_t)m.y);
        break;
    case 0x04: m.x = (int16_t)cx; m.y = (int16_t)dx; clamp(); break;
    case 0x05: case 0x06: {                          /* press / release data for button BX */
        int b = bx & 3; if (b > 2) b = 0;
        int press = ax == 0x05;
        x86_set_r16(c, R_AX, (uint16_t)m.buttons);
        x86_set_r16(c, R_BX, (uint16_t)(press ? m.press_n[b] : m.rel_n[b]));
        x86_set_r16(c, R_CX, (uint16_t)(press ? m.press_x[b] : m.rel_x[b]));
        x86_set_r16(c, R_DX, (uint16_t)(press ? m.press_y[b] : m.rel_y[b]));
        if (press) m.press_n[b] = 0; else m.rel_n[b] = 0;
        break;
    }
    case 0x07: m.minx = (int16_t)(cx < dx ? cx : dx); m.maxx = (int16_t)(cx < dx ? dx : cx); clamp(); break;
    case 0x08: m.miny = (int16_t)(cx < dx ? cx : dx); m.maxy = (int16_t)(cx < dx ? dx : cx); clamp(); break;
    case 0x09: break;                                /* graphics cursor shape: not drawn */
    case 0x0A: if (bx == 0) { m.screen_mask = (uint16_t)cx; m.cursor_mask = (uint16_t)dx; } break;
    case 0x0B:
        if (!handler_walks()) walk(m.x, m.y);
        x86_set_r16(c, R_CX, (uint16_t)m.mick_x);
        x86_set_r16(c, R_DX, (uint16_t)m.mick_y);
        m.mick_x = m.mick_y = 0;
        break;
    case 0x0C:
        m.ev_mask = (uint16_t)cx; m.ev_seg = c->seg[S_ES].sel; m.ev_off = (uint16_t)dx;
        break;
    case 0x0F: if (cx > 0) m.ratio_x = cx; if (dx > 0) m.ratio_y = dx; break;
    case 0x10: break;                                /* exclusion area: the overlay never collides */
    case 0x13: break;                                /* double-speed threshold */
    case 0x14: {                                     /* swap event handlers */
        uint16_t om = m.ev_mask, os = m.ev_seg, oo = m.ev_off;
        m.ev_mask = (uint16_t)cx; m.ev_seg = c->seg[S_ES].sel; m.ev_off = (uint16_t)dx;
        x86_set_r16(c, R_CX, om);
        x86_set_r16(c, R_DX, oo);
        x86_load_seg(c, S_ES, os);
        break;
    }
    case 0x15: x86_set_r16(c, R_BX, 16); break;      /* state buffer size */
    case 0x16: case 0x17: break;                     /* save / restore state: nothing a caller needs */
    case 0x1A: m.sens_x = bx; m.sens_y = cx; m.sens_thr = dx; break;
    case 0x1B:
        x86_set_r16(c, R_BX, (uint16_t)m.sens_x);
        x86_set_r16(c, R_CX, (uint16_t)m.sens_y);
        x86_set_r16(c, R_DX, (uint16_t)m.sens_thr);
        break;
    case 0x1D: case 0x1E: if (ax == 0x1E) x86_set_r16(c, R_BX, 0); break;   /* display page */
    case 0x24:                                       /* version 6.26, PS/2 mouse */
        x86_set_r16(c, R_BX, 0x0626);
        x86_set_r16(c, R_CX, 0x0400);
        break;
    default: break;
    }
}

/* A reboot forgets the program's handler and whatever it set. */
void pc_mouse_reboot(x86_cpu *c) { if (m.installed) reset(c); }

/* With a mouse: the driver answers INT 33h. Without one (main.c decides)
 * the vector keeps pointing at the BIOS's dummy IRET. */
void pc_mouse_install(x86_cpu *c) {
    m.installed = 1;
    reset(c);
    pc_set_service(0x33, int33, HLE_RET_IRET);
    pc_set_trap(TRAP_HANDLER_RET, handler_returned, HLE_RET_IRET);
}
