/* pc_ps2.c — the PS/2 mouse on the 8042's auxiliary port (IRQ 12), and
 * the BIOS's pointing-device services (INT 15h AH=C2h) and INT 74h
 *
 * The device answers the PS/2 command set as a plain two-button-and-
 * middle mouse (ID 00h): reset (FA AA 00), defaults, reporting on and
 * off, sample rate, resolution, scaling, stream and remote modes, read
 * data, the status request, identify, wrap (echo) mode, resend. What it
 * sends — ACKs, replies, and 3-byte movement packets in stream mode with
 * reporting on — queues here and reaches the guest one byte at a time
 * through the 8042's output buffer, marked as the auxiliary device's
 * (status bit 5) and raising IRQ 12 when the command byte allows (bit 1).
 * Keyboard and mouse share that one buffer; pc_bios.c arbitrates.
 *
 * The host's pointer is absolute (the window's 640x480 frame, or a
 * terminal's cells); packets carry its motion as counts, one per frame
 * pixel, Y up, at most one packet per sample period and only once the
 * last one has been taken, so a busy guest gets fresh ones rather than a
 * backlog.
 *
 * INT 15h AH=C2h is how DOS mouse drivers (FreeDOS's CTMOUSE among them)
 * and Windows' PS/2 driver reach it: enable/disable, reset, rate,
 * resolution, type, initialise, status and scaling, and the far routine
 * INT 74h calls with each packet — status, X, Y and a zero word pushed,
 * as the PS/2 BIOS does. INT 74h itself is real instructions (the port
 * 60h read and the EOIs a V86 monitor must see) around a host trap that
 * assembles the packet.
 */
#include "pc.h"
#include <stdio.h>
#include <string.h>

#define Q 256

static struct {
    /* the device */
    int reporting, remote, wrap, scale21;
    int rate, res;                   /* samples a second; resolution code 0-3 */
    uint8_t want;                    /* a command awaiting its data byte (F3h rate, E8h resolution) */
    uint8_t last;                    /* the last byte sent (FEh: resend) */
    int buttons;                     /* bit 0 left, 1 right, 2 middle */
    int dx, dy, moved;               /* counts not yet reported */
    int sent_buttons;
    int hx, hy, have_host;           /* the host's pointer, for deltas */
    uint64_t next_ns;                /* the next packet may go */
    /* its bytes on their way to the 8042 */
    uint8_t q[Q];
    int qh, qt;
    /* the BIOS's services */
    int packet;                      /* bytes collected by INT 74h */
    uint8_t pkt[3];
} ps;

static void put(uint8_t b) {
    if (((ps.qt + 1) % Q) == ps.qh) return;     /* full: the device drops it */
    ps.q[ps.qt] = b;
    ps.qt = (ps.qt + 1) % Q;
    ps.last = b;
}
int pc_ps2_pending(void) { return ps.qh != ps.qt; }
void pc_ps2_inject(uint8_t v) { put(v); }
uint8_t pc_ps2_take(void) {
    uint8_t b = ps.q[ps.qh];
    ps.qh = (ps.qh + 1) % Q;
    return b;
}

static void defaults(void) {
    ps.reporting = 0; ps.remote = 0; ps.wrap = 0; ps.scale21 = 0;
    ps.rate = 100; ps.res = 2; ps.want = 0;
    ps.dx = ps.dy = ps.moved = 0;
}

static void packet(void) {
    int dx = ps.dx, dy = ps.dy;
    /* status bits 0 left, 1 right, 2 middle (the PS/2 order), 3 always */
    uint8_t st = (uint8_t)(0x08 | ((ps.buttons & 1) ? 1 : 0) | ((ps.buttons & 2) ? 2 : 0) | ((ps.buttons & 4) ? 4 : 0));
    if (dx > 255) { dx = 255; st |= 0x40; } else if (dx < -256) { dx = -256; st |= 0x40; }
    if (dy > 255) { dy = 255; st |= 0x80; } else if (dy < -256) { dy = -256; st |= 0x80; }
    if (dx < 0) st |= 0x10;
    if (dy < 0) st |= 0x20;
    if (pc.debug) fprintf(stderr, "[ps2] packet %02X %02X %02X\n", st, (uint8_t)dx, (uint8_t)dy);
    put(st); put((uint8_t)dx); put((uint8_t)dy);
    ps.dx -= dx; ps.dy -= dy;
    if (!ps.dx && !ps.dy) ps.moved = 0;
    ps.sent_buttons = ps.buttons;
}

/* A byte from the host to the device (8042 command D4h). */
void pc_ps2_write(uint8_t v) {
    if (pc.debug) fprintf(stderr, "[ps2] to the mouse: %02X\n", v);
    if (ps.want) {                               /* a command's data byte */
        if (ps.want == 0xF3) ps.rate = v ? v : 100;
        else if (ps.want == 0xE8) ps.res = v & 3;
        ps.want = 0;
        put(0xFA);
        return;
    }
    if (ps.wrap && v != 0xEC && v != 0xFF) { put(v); return; }   /* wrap mode: echo */
    switch (v) {
    case 0xFF: defaults(); put(0xFA); put(0xAA); put(0x00); break;       /* reset: self test passed, ID 00 */
    case 0xFE: put(ps.last); break;                                      /* resend */
    case 0xF6: defaults(); put(0xFA); break;                             /* set defaults (reporting off) */
    case 0xF5: ps.reporting = 0; put(0xFA); break;
    case 0xF4: ps.reporting = 1; ps.dx = ps.dy = 0; ps.moved = 0; put(0xFA); break;
    case 0xF3: case 0xE8: ps.want = v; put(0xFA); break;                 /* a data byte follows */
    case 0xF2: put(0xFA); put(0x00); break;                              /* identify: a plain mouse */
    case 0xF0: ps.remote = 1; put(0xFA); break;
    case 0xEE: ps.wrap = 1; put(0xFA); break;
    case 0xEC: ps.wrap = 0; put(0xFA); break;
    case 0xEB: put(0xFA); packet(); break;                               /* read data (remote mode) */
    case 0xEA: ps.remote = 0; put(0xFA); break;
    case 0xE9: {                                                         /* status request */
        put(0xFA);
        put((uint8_t)((ps.remote ? 0x40 : 0) | (ps.reporting ? 0x20 : 0) | (ps.scale21 ? 0x10 : 0)
                      | ((ps.buttons & 1) ? 4 : 0) | ((ps.buttons & 4) ? 2 : 0) | ((ps.buttons & 2) ? 1 : 0)));
        put((uint8_t)ps.res);
        put((uint8_t)ps.rate);
        break;
    }
    case 0xE7: ps.scale21 = 1; put(0xFA); break;
    case 0xE6: ps.scale21 = 0; put(0xFA); break;
    default: put(0xFE); break;                                           /* not a command: resend */
    }
}

/* ---- from the host ------------------------------------------------------ */

void pc_ps2_motion(int fx, int fy) {
    if (pc.debug > 1) fprintf(stderr, "[ps2] host pointer %d,%d (reporting %d)\n", fx, fy, ps.reporting);
    if (ps.have_host && ps.reporting) {
        ps.dx += fx - ps.hx;
        ps.dy -= fy - ps.hy;                     /* PS/2 counts Y up */
        if (fx != ps.hx || fy != ps.hy) ps.moved = 1;
    }
    ps.hx = fx; ps.hy = fy; ps.have_host = 1;
}
void pc_ps2_button(int button, int down) {
    if (pc.debug > 1) fprintf(stderr, "[ps2] host button %d %s\n", button, down ? "down" : "up");
    if (button < 0 || button > 2) return;
    if (down) ps.buttons |= 1 << button; else ps.buttons &= ~(1 << button);
}

/* From pc_poll: a packet when there is news, the device is streaming, the
 * last packet has been taken and the sample period has passed. */
void pc_ps2_poll(uint64_t now) {
    if (!ps.reporting || ps.remote || ps.wrap) return;
    if (!ps.moved && ps.buttons == ps.sent_buttons) return;
    if (pc_ps2_pending() || now < ps.next_ns) return;
    packet();
    ps.next_ns = now + 1000000000ull / (uint64_t)(ps.rate ? ps.rate : 100);
}

/* ---- the BIOS ----------------------------------------------------------- */

/* INT 74h's trap, with the byte from port 60h in AL: assemble packets
 * (a first byte without bit 3 is out of step: dropped); with one whole
 * and a routine installed, AL=1 and the packet waits at PC_PS2_VARS for
 * the stub to push. */
static void int74_trap(x86_cpu *c, int vector) {
    (void)vector;
    uint8_t b = x86_get_r8(c, R_AL);
    x86_set_r8(c, R_AL, 0);
    if (!ps.packet && !(b & 0x08)) return;
    ps.pkt[ps.packet++] = b;
    if (ps.packet < 3) return;
    ps.packet = 0;
    if (!pc_rd16(c, PC_STUB_SEG, PC_PS2_VARS) && !pc_rd16(c, PC_STUB_SEG, PC_PS2_VARS + 2)) return;
    pc_wr16(c, PC_STUB_SEG, PC_PS2_VARS + 4, ps.pkt[0]);
    pc_wr16(c, PC_STUB_SEG, PC_PS2_VARS + 6, ps.pkt[1]);
    pc_wr16(c, PC_STUB_SEG, PC_PS2_VARS + 8, ps.pkt[2]);
    x86_set_r8(c, R_AL, 1);
}

/* INT 15h AH=C2h, the pointing device. Returns 0 if AH is not C2h. */
int pc_int15_ps2(x86_cpu *c) {
    if (x86_get_r8(c, R_AH) != 0xC2) return 0;
    uint8_t al = x86_get_r8(c, R_AL), bh = x86_get_r8(c, R_BH);
    int err = 0;
    if (pc.debug) fprintf(stderr, "[ps2] INT 15h AX=C2%02X BX=%04X ES=%04X\n", al, x86_get_r16(c, R_BX), c->seg[S_ES].sel);
    switch (al) {
    case 0x00:                                   /* disable (BH=0) / enable (BH=1) */
        if (bh > 1) { err = 1; break; }
        if (bh && !pc_rd16(c, PC_STUB_SEG, PC_PS2_VARS) && !pc_rd16(c, PC_STUB_SEG, PC_PS2_VARS + 2)) { err = 5; break; }
        ps.reporting = bh; ps.dx = ps.dy = 0; ps.moved = 0; ps.packet = 0;
        if (bh) {
            pc.kbc_cmdbyte = (uint8_t)((pc.kbc_cmdbyte | 0x02) & ~0x20);   /* IRQ 12 on, aux clock on */
            pc_irq_unmask(12);
        }
        break;
    case 0x01:                                   /* reset: BH = ID, BL = AAh */
        defaults(); ps.packet = 0;
        x86_set_r8(c, R_BH, 0x00); x86_set_r8(c, R_BL, 0xAA);
        break;
    case 0x02: {                                 /* sample rate */
        static const uint8_t r[7] = { 10, 20, 40, 60, 80, 100, 200 };
        if (bh > 6) { err = 2; break; }
        ps.rate = r[bh];
        break;
    }
    case 0x03: if (bh > 3) { err = 2; break; } ps.res = bh; break;       /* resolution */
    case 0x04: x86_set_r8(c, R_BH, 0x00); break;                         /* type: a mouse */
    case 0x05:                                   /* initialise, BH = packet size */
        if (bh < 1 || bh > 8) { err = 2; break; }
        defaults(); ps.packet = 0;
        break;
    case 0x06:
        if (bh == 0) {                           /* status: BL status, CL resolution, DL rate */
            x86_set_r8(c, R_BL, (uint8_t)((ps.remote ? 0x40 : 0) | (ps.reporting ? 0x20 : 0) | (ps.scale21 ? 0x10 : 0)
                                          | ((ps.buttons & 1) ? 4 : 0) | ((ps.buttons & 4) ? 2 : 0) | ((ps.buttons & 2) ? 1 : 0)));
            x86_set_r8(c, R_CL, (uint8_t)ps.res);
            x86_set_r8(c, R_DL, (uint8_t)ps.rate);
        } else if (bh == 1 || bh == 2) ps.scale21 = bh == 2;
        else err = 1;
        break;
    case 0x07:                                   /* the routine INT 74h calls, ES:BX (0:0 none) */
        pc_wr16(c, PC_STUB_SEG, PC_PS2_VARS, x86_get_r16(c, R_BX));
        pc_wr16(c, PC_STUB_SEG, PC_PS2_VARS + 2, c->seg[S_ES].sel);
        break;
    default: err = 1; break;
    }
    x86_set_r8(c, R_AH, (uint8_t)err);
    if (err) c->eflags |= X86_CF; else c->eflags &= ~X86_CF;
    return 1;
}

/* POST: a mouse on the port, reporting off; INT 74h's stub and its data. */
void pc_ps2_post(x86_cpu *c) {
    memset(&ps, 0, sizeof ps);
    defaults();
    for (int i = 0; i < 10; i++) pc_wr8(c, PC_STUB_SEG, (uint16_t)(PC_PS2_VARS + i), 0);
    /* push ax; in al,60h; pushf; call far F000:PC_TRAP_PS2; cli; test al,al; jz eoi;
     * push bx cx dx si di bp ds es; sti; push [cs:status]; push [cs:x];
     * push [cs:y]; xor ax,ax; push ax; call far [cs:routine]; add sp,8; cli;
     * pop es ds bp di si dx cx bx; eoi: mov al,20h; out A0h,al; out 20h,al;
     * pop ax; iret */
    static uint8_t code[96];
    int n = 0;
    const uint8_t head[] = { 0x50, 0xE4, 0x60, 0x9C, 0x9A, PC_TRAP_PS2, 0x00, 0x00, 0xF0, 0xFA, 0x84, 0xC0, 0x74, 0x00 };
    memcpy(code + n, head, sizeof head); n += (int)sizeof head;
    int jz = n - 1;
    const uint8_t call[] = {
        0x53, 0x51, 0x52, 0x56, 0x57, 0x55, 0x1E, 0x06, 0xFB,
        0x2E, 0xFF, 0x36, PC_PS2_VARS + 4, 0x00,
        0x2E, 0xFF, 0x36, PC_PS2_VARS + 6, 0x00,
        0x2E, 0xFF, 0x36, PC_PS2_VARS + 8, 0x00,
        0x31, 0xC0, 0x50,
        0x2E, 0xFF, 0x1E, PC_PS2_VARS, 0x00,
        0x83, 0xC4, 0x08, 0xFA,
        0x07, 0x1F, 0x5D, 0x5F, 0x5E, 0x5A, 0x59, 0x5B };
    memcpy(code + n, call, sizeof call); n += (int)sizeof call;
    code[jz] = (uint8_t)(n - (jz + 1));
    const uint8_t tail[] = { 0xB0, 0x20, 0xE6, 0xA0, 0xE6, 0x20, 0x58, 0xCF };
    memcpy(code + n, tail, sizeof tail); n += (int)sizeof tail;
    for (int i = 0; i < n; i++) pc_wr8(c, PC_STUB_SEG, (uint16_t)(PC_STUB_INT74 + i), code[i]);
    pc_wr16(c, 0, 0x74 * 4, PC_STUB_INT74);
    pc_wr16(c, 0, 0x74 * 4 + 2, PC_STUB_SEG);
    pc_set_trap(PC_TRAP_PS2, int74_trap, HLE_RET_IRET);
}
