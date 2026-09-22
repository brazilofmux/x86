/* pc_bios.c — IVT, BIOS data area, the HLE stub segment, BIOS services
 * (INT 11h/12h/15h/1Ah), the timer tick, and IRQ delivery.
 */
#include "pc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

pc_state pc;


uint64_t pc_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* An internal trap: an address in the HLE segment the host hands out
 * itself (the DPMI entry, its return trampolines). The IVT is not touched. */
void pc_set_trap(int offset, pc_service_fn fn, int ret_mode) {
    pc.service[offset & 0xFF] = fn;
    pc.ret_mode[offset & 0xFF] = (uint8_t)ret_mode;
}

/* A real interrupt service: the vector gets its own stub, F000:vector. */
void pc_set_service(int vector, pc_service_fn fn, int ret_mode) {
    pc_set_trap(vector, fn, ret_mode);
    pc_wr16(pc.cpu, 0, (uint16_t)((vector & 0xFF) * 4), (uint16_t)(vector & 0xFF));
    pc_wr16(pc.cpu, 0, (uint16_t)((vector & 0xFF) * 4 + 2), PC_HLE_SEG);
}

/* Pop the INT frame. FLAGS mode is MS-DOS's RETF 2: the caller gets the
 * handler's flags (CF/ZF results) but keeps its own IF/TF. */
void pc_hle_return(x86_cpu *c, int mode) {
    /* The frame is as wide as the way in: a real-mode INT pushes words, a
     * protected-mode gate pushes words or dwords as the gate is 16- or
     * 32-bit, and the trap segment it leads to has the matching D bit — so
     * does a 16-bit handler's PUSHF / CALL FAR chain to the vector it read
     * back. The stack is addressed at SS's own width, wrapping like the CPU. */
    int w = c->pmode && X86_AR_DB(c->seg[S_CS].attr) ? 4 : 2;
    uint32_t m = c->pmode && X86_AR_DB(c->seg[S_SS].attr) ? 0xFFFFFFFFu : 0xFFFFu;
    uint32_t base = c->seg[S_SS].base, sp = c->r[R_SP] & m;
    uint32_t ip = x86_rd(c, base, sp, m, w);
    uint16_t cs = (uint16_t)x86_rd(c, base, (sp + (uint32_t)w) & m, m, w);
    uint32_t fl = x86_rd(c, base, (sp + 2u * (uint32_t)w) & m, m, w);
    c->r[R_SP] = (c->r[R_SP] & ~m) | ((sp + 3u * (uint32_t)w) & m);
    pc.returned = 1;
    x86_load_seg(c, S_CS, cs);
    c->eip = ip;
    if (mode == HLE_RET_IRET)
        c->eflags = x86_flags_fixup(c, w == 4 ? fl : (c->eflags & 0xFFFF0000u) | fl);
    else
        c->eflags = x86_flags_fixup(c, (c->eflags & ~(uint32_t)(X86_IF | X86_TF)) | (fl & (X86_IF | X86_TF)));
}

/* X86_SVCPROF=1: host time and call count per (vector, AH), dumped at
 * exit. Two clock reads per service call, so it stays behind the env
 * check; the point is to find which service the guest actually lives in. */
static struct { uint64_t ns, calls; } svc_prof[256][256];
static int svc_prof_on = -1;

void pc_svcprof_dump(FILE *out) {
    if (svc_prof_on <= 0) return;
    fprintf(out, "  service profile (host time by INT/AH):\n");
    for (int n = 0; n < 12; n++) {
        int bv = -1, ba = -1; uint64_t best = 0;
        for (int v = 0; v < 256; v++)
            for (int a = 0; a < 256; a++)
                if (svc_prof[v][a].ns > best) { best = svc_prof[v][a].ns; bv = v; ba = a; }
        if (bv < 0) break;
        fprintf(out, "    INT %02Xh AH=%02X  %8.1f ms  %9llu calls  %6.0f ns/call\n",
                bv, ba, (double)best / 1e6, (unsigned long long)svc_prof[bv][ba].calls,
                (double)best / (double)svc_prof[bv][ba].calls);
        svc_prof[bv][ba].ns = 0;
    }
}

/* A CPU exception that reaches the HLE segment in protected mode has no
 * handler: the client never installed one through INT 31h 0203, and
 * IRETing would just re-run the faulting instruction forever. Say what it
 * was and stop, which is what a DPMI host does to a client that faults.
 * Only a real fault, mind: an interrupt with the same vector — the timer
 * is INT 8, its user hook INT 1Ch — is not a fault and just IRETs. */
static void unhandled_exception(x86_cpu *c, int vector) {
    static const char *name[] = { "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
                                  "#DF", "---", "#TS", "#NP", "#SS", "#GP", "#PF", "---",
                                  "#MF", "#AC", "#MC", "#XM" };
    uint32_t sp = c->r[R_SP], base = c->seg[S_SS].base;
    uint32_t ip = x86_rd(c, base, sp, 0xFFFFFFFFu, 4);
    uint16_t cs = (uint16_t)x86_rd(c, base, sp + 4, 0xFFFFFFFFu, 4);
    fprintf(stderr, "dpmi: unhandled %s at %04X:%08X, bytes",
            vector < 20 ? name[vector] : "exception", cs, ip);
    x86_cpu probe = *c;                          /* load CS without disturbing the real one */
    x86_load_seg(&probe, S_CS, cs);
    for (int i = 0; i < 8; i++)
        fprintf(stderr, " %02X", (unsigned)x86_rd(&probe, probe.seg[S_CS].base, ip + i, 0xFFFFFFFFu, 1));
    fprintf(stderr, "\n");
    c->halted = 1;
}

static void hle_dispatch(x86_cpu *c, int vector) {
    pc_service_fn fn = pc.service[vector];
    if (c->pmode && c->exc_delivered && vector < 0x20) {
        if (pc.pm_exception && pc.pm_exception(c, vector)) return;
        if (!fn) { unhandled_exception(c, vector); return; }
    }
    int mode = fn ? pc.ret_mode[vector] : HLE_RET_IRET;
    pc.returned = 0;
    if (svc_prof_on < 0) svc_prof_on = getenv("X86_SVCPROF") != NULL;
    if (svc_prof_on) {
        int ah = x86_get_r8(c, R_AH);
        uint64_t t0 = pc_now_ns();
        if (fn) fn(c, vector);
        svc_prof[vector & 0xFF][ah].ns += pc_now_ns() - t0;
        svc_prof[vector & 0xFF][ah].calls++;
    } else
    if (fn) fn(c, vector);
    /* A service that transferred control itself (program exit, exec,
     * INT 8 chaining to 1Ch) has already done its own return. */
    if (c->halted || pc.returned) return;
    pc_hle_return(c, mode);
}

/* ---- Timer ------------------------------------------------------------- */
static void bios_int8(x86_cpu *c, int vector) {
    (void)vector;
    pc.irq_in_service &= ~1;                     /* the BIOS handler's EOI */
    uint32_t t = pc_rd16(c, PC_BDA_SEG, 0x6C) | ((uint32_t)pc_rd16(c, PC_BDA_SEG, 0x6E) << 16);
    t++;
    if (t >= 0x1800B0) { t = 0; pc_wr8(c, PC_BDA_SEG, 0x70, 1); }
    pc_wr16(c, PC_BDA_SEG, 0x6C, (uint16_t)t);
    pc_wr16(c, PC_BDA_SEG, 0x6E, (uint16_t)(t >> 16));
    /* Return from INT 8, then invoke the user tick hook INT 1Ch. */
    pc_hle_return(c, HLE_RET_IRET);
    x86_interrupt(c, 0x1C, 0);
}

static void bios_int1a(x86_cpu *c, int vector) {
    (void)vector;
    switch (x86_get_r8(c, R_AH)) {
    case 0x00: {
        c->r[R_CX] = (c->r[R_CX] & 0xFFFF0000u) | pc_rd16(c, PC_BDA_SEG, 0x6E);
        c->r[R_DX] = (c->r[R_DX] & 0xFFFF0000u) | pc_rd16(c, PC_BDA_SEG, 0x6C);
        x86_set_r8(c, R_AL, pc_rd8(c, PC_BDA_SEG, 0x70));
        pc_wr8(c, PC_BDA_SEG, 0x70, 0);
        break;
    }
    case 0x01:
        pc_wr16(c, PC_BDA_SEG, 0x6C, x86_get_r16(c, R_DX));
        pc_wr16(c, PC_BDA_SEG, 0x6E, x86_get_r16(c, R_CX));
        break;
    case 0x02: case 0x04: {
        time_t now = time(NULL);
        struct tm tm; localtime_r(&now, &tm);
        #define BCD(v) ((uint8_t)((((v) / 10) << 4) | ((v) % 10)))
        if (x86_get_r8(c, R_AH) == 0x02) {
            x86_set_r8(c, R_CH, BCD(tm.tm_hour)); x86_set_r8(c, R_CL, BCD(tm.tm_min));
            x86_set_r8(c, R_DH, BCD(tm.tm_sec)); x86_set_r8(c, R_DL, 0);
        } else {
            int y = tm.tm_year + 1900;
            x86_set_r8(c, R_CH, BCD(y / 100)); x86_set_r8(c, R_CL, BCD(y % 100));
            x86_set_r8(c, R_DH, BCD(tm.tm_mon + 1)); x86_set_r8(c, R_DL, BCD(tm.tm_mday));
        }
        c->eflags &= ~X86_CF;
        break;
    }
    default:
        c->eflags |= X86_CF;
        break;
    }
}

static void bios_int11(x86_cpu *c, int vector) {
    (void)vector;
    x86_set_r16(c, R_AX, pc_rd16(c, PC_BDA_SEG, 0x10));
}
static void bios_int12(x86_cpu *c, int vector) {
    (void)vector;
    x86_set_r16(c, R_AX, pc_rd16(c, PC_BDA_SEG, 0x13));
}

static void a20_set(x86_cpu *c, int on);

/* INT 13h AH=02, enough of it to let a boot sector load the rest of itself.
 * The image is addressed as a 1.44M floppy: 18 sectors per track, 2 heads. */
static void bios_int13(x86_cpu *c, int vector) {
    (void)vector;
    if (x86_get_r8(c, R_AH) != 0x02 || !pc.boot_img) {
        x86_set_r8(c, R_AH, 0x01);
        c->eflags |= X86_CF;
        return;
    }
    int count = x86_get_r8(c, R_AL);
    int sector = x86_get_r8(c, R_CL) & 0x3F;                 /* 1-based */
    long lba = ((long)x86_get_r8(c, R_CH) * 2 + x86_get_r8(c, R_DH)) * 18 + (sector - 1);
    uint32_t dst = ((uint32_t)c->seg[S_ES].sel << 4) + x86_get_r16(c, R_BX);
    for (long b = 0; b < (long)count * 512; b++) {
        long off = lba * 512 + b;
        x86_phys_wr8(c, dst + (uint32_t)b, off < (long)pc.boot_len ? pc.boot_img[off] : 0);
    }
    x86_set_r8(c, R_AH, 0);
    x86_set_r8(c, R_AL, (uint8_t)count);
    c->eflags &= ~X86_CF;
}

static void bios_int15(x86_cpu *c, int vector) {
    (void)vector;
    switch (x86_get_r8(c, R_AH)) {
    case 0x88:                                   /* extended memory size in KB */
        x86_set_r16(c, R_AX, 0);
        c->eflags &= ~X86_CF;
        break;
    case 0x4F:                                   /* keyboard intercept: keep the key */
        c->eflags |= X86_CF;
        break;
    case 0xC0:                                   /* system configuration: none */
        x86_set_r8(c, R_AH, 0x80);
        c->eflags |= X86_CF;
        break;
    case 0x24:                                   /* A20 gate */
        switch (x86_get_r8(c, R_AL)) {
        case 0x00: a20_set(c, 0); x86_set_r8(c, R_AH, 0); c->eflags &= ~X86_CF; break;
        case 0x01: a20_set(c, 1); x86_set_r8(c, R_AH, 0); c->eflags &= ~X86_CF; break;
        case 0x02: x86_set_r16(c, R_AX, (uint16_t)(c->a20_mask != 0xFFFFFu ? 1 : 0)); c->eflags &= ~X86_CF; break;
        case 0x03: x86_set_r16(c, R_AX, 0); x86_set_r16(c, R_BX, 3); c->eflags &= ~X86_CF; break;   /* keyboard controller and port 92h */
        default: x86_set_r8(c, R_AH, 0x86); c->eflags |= X86_CF; break;
        }
        break;
    case 0x86: {                                 /* wait CX:DX microseconds */
        uint32_t us = ((uint32_t)x86_get_r16(c, R_CX) << 16) | x86_get_r16(c, R_DX);
        if (us) { uint64_t w0 = pc_now_ns(); usleep(us); pc.blocked_ns += pc_now_ns() - w0; pc.blocked_calls++; }
        c->eflags &= ~X86_CF;
        break;
    }
    default:
        x86_set_r8(c, R_AH, 0x86);
        c->eflags |= X86_CF;
        break;
    }
}

/* ---- IRQ delivery ------------------------------------------------------ */
static void deliver(x86_cpu *c, int vector) {
    pc.irq_in_service |= 1 << (vector - 8);
    pc.irq_service_ns = pc_now_ns();
    if (pc.debug > 1) fprintf(stderr, "[irq] INT %02X → %04X:%04X @%llu\n", vector,
                              pc_rd16(c, 0, (uint16_t)(vector * 4 + 2)), pc_rd16(c, 0, (uint16_t)(vector * 4)), (unsigned long long)c->insn_count);
    x86_interrupt(c, vector, 0);
    c->halted = 0;
}

/* The run loop calls this between block runs AND after every interpreter
 * fallback — hundreds of thousands of times a second — so the common
 * case has to be nearly free. Everything periodic here is already
 * rate-limited well below 1 kHz (key feed 20 ms, repaint 16 ms, PIT
 * tick 55 ms, scancode latch 2 ms), so it runs off a deadline and costs
 * one clock read to skip. Only IRQ delivery is considered on every
 * call, and only when one could actually be taken — which is what makes
 * it affordable to poll at every interrupt boundary. */
#define PC_POLL_PERIOD_NS 1000000ull        /* 1 ms: finer than anything below needs */

static uint64_t irq0_period_ns(void);

int pc_poll(x86_cpu *c) {
    uint64_t now = pc_now_ns();
    pc.now_ns = now;
    int deliverable = pc.irq_pending && (c->eflags & X86_IF) && !c->int_inhibit;
    if (!deliverable && !c->halted && now < pc.next_slow_ns) return 0;

    if (c->halted || now >= pc.next_slow_ns) {
    pc.next_slow_ns = now + PC_POLL_PERIOD_NS;
    pc_kbd_poll(c);
    pc_kbd_idle_poll(c);
    pc_video_flush(0);
    pc_sdl_poll(c, now);
    if (c->halted && pc.exit_requested) return 1;

    /* Keyboard: latch the next raw code and raise INT 9 once the previous
     * one has been taken (the guest's handler has returned). */
    /* A keyboard delivers a scancode every couple of milliseconds at
     * best; handlers (WP's) rely on having finished with one before the
     * next arrives, beyond what the PIC's in-service gating guarantees. */
    static uint64_t last_code_ns;
    if (!(pc.irq_pending & (1 << 9)) && !pc.irq9_busy && pc_kbd_raw_pending() && now - last_code_ns >= 2000000ull) {
        uint8_t code;
        pc_kbd_raw_next(&code);
        pc.last_scancode = code;
        pc.irq_pending |= 1 << 9;
        pc.irq9_busy = 1;                        /* until the handler reads port 60h */
        last_code_ns = now;
    }

    /* IRQ 0 comes at whatever rate PIT channel 0 was programmed for — the
     * BIOS's 18.2 Hz until a program reloads it. DOOM's sound library sets
     * 140 Hz and counts game time in those interrupts; delivering 18.2 Hz
     * regardless ran its clock at an eighth of real time, and the game
     * spent its CPU waiting for tics. */
    uint64_t period = irq0_period_ns();
    if (!pc.next_tick_ns) pc.next_tick_ns = pc.t0_ns + period;
    if (now >= pc.next_tick_ns) {
        if (now - pc.next_tick_ns > 18 * period) pc.next_tick_ns = now;   /* don't storm after a stall */
        pc.irq_pending |= 1 << 8;
    }

    if (c->halted && (c->eflags & X86_IF) && !pc.irq_pending) {
        /* HLT with interrupts on: the guest is idling for the next tick. */
        uint64_t next = pc.next_tick_ns;
        uint64_t hnow = pc_now_ns();
        if (next > hnow) { usleep((useconds_t)((next - hnow) / 1000 + 1)); pc.blocked_ns += pc_now_ns() - hnow; pc.blocked_calls++; }
        pc.irq_pending |= 1 << 8;
    }
    }
    if (pc.debug > 2) { static int n; if ((n++ & 1023) == 0) fprintf(stderr, "[poll] pending %X isr %X IF %d inhibit %d @%llu\n", pc.irq_pending, pc.irq_in_service, (c->eflags & X86_IF) != 0, c->int_inhibit, (unsigned long long)c->insn_count); }
    if (!pc.irq_pending || !(c->eflags & X86_IF) || c->int_inhibit) return 0;
    /* 8259 priority: nothing while an equal-or-higher IRQ is in service
     * (until its EOI). A handler that never EOIs would hang a real PC;
     * we forgive it after 200 ms of wall clock. */
    if (pc.irq_in_service && now - pc.irq_service_ns > 200000000ull) pc.irq_in_service = 0;
    if ((pc.irq_pending & (1 << 8)) && !(pc.irq_in_service & 1)) {
        pc.irq_pending &= ~(1 << 8);
        pc.ticks_delivered++;
        pc.next_tick_ns += irq0_period_ns();
        deliver(c, 8);
        return 1;
    }
    if ((pc.irq_pending & (1 << 9)) && !(pc.irq_in_service & 3)) {
        pc.irq_pending &= ~(1 << 9);
        deliver(c, 9);
        return 1;
    }
    return 0;
}

/* ---- Ports -------------------------------------------------------------
 * 8253 PIT channel 0 (WP 5.1 and games calibrate delays against it):
 * the counter runs at 1193182 Hz from wall clock, counting down from
 * the reload value; OUT 43h with a latch command captures it for the
 * next two reads of port 40h. Channel 2 (speaker) behaves the same so
 * timing loops on it work; no sound. PIC mask register at 21h is just
 * stored. */
#define PIT_HZ 1193182ull
static struct { uint16_t reload; uint16_t latch; int latched, rw_phase, mode_rw; } pit[3];
static uint8_t pic_mask = 0xB8, pit_speaker;

/* Channel 0's period: reload 0 means 65536, the BIOS's 54.9 ms. */
static uint64_t irq0_period_ns(void) {
    uint64_t reload = pit[0].reload ? pit[0].reload : 65536;
    return reload * 1000000000ull / PIT_HZ;
}

static uint16_t pit_now(int ch) {
    uint64_t ticks = (pc_now_ns() - pc.t0_ns) * PIT_HZ / 1000000000ull;
    uint32_t reload = pit[ch].reload ? pit[ch].reload : 65536;
    return (uint16_t)(reload - (ticks % reload));
}

/* ---- A20 --------------------------------------------------------------
 * Three ways to reach the gate, all landing in x86_set_a20 (which
 * remaps the HMA window and tells the DBT to drop its cache): the 8042
 * output port (64h D1 / 60h data, or the DD/DF shortcuts), the PS/2
 * system control port 92h, and INT 15h AH=24h. */
static void a20_set(x86_cpu *c, int on) {
    int was = c->a20_mask != 0xFFFFFu;
    if (pc.debug && was != on) fprintf(stderr, "[pc] A20 %s @%llu\n", on ? "on" : "off", (unsigned long long)c->insn_count);
    x86_set_a20(c, on);
}
static uint8_t a20_out_port(const x86_cpu *c) { return (uint8_t)(0xCD | (c->a20_mask != 0xFFFFFu ? 2 : 0)); }   /* 8042 output port: A20 in bit 1, bit 0 = no reset */

static uint32_t port_read(x86_cpu *c, uint16_t port, int size) {
    (void)size;
    uint32_t vv;
    if (pc_vga_port_read(port, &vv)) return vv;
    if (pc.debug > 1 && (port == 0x60 || port == 0x64 || port == 0x61))
        fprintf(stderr, "[port] in %02X → %02X @%llu\n", port, port == 0x60 ? pc.last_scancode : port == 0x61 ? pit_speaker : 0x14, (unsigned long long)c->insn_count);
    switch (port) {
    case 0x40: case 0x41: case 0x42: {
        int ch = port - 0x40;
        uint16_t v = pit[ch].latched ? pit[ch].latch : pit_now(ch);
        int rw = pit[ch].mode_rw ? pit[ch].mode_rw : 3;
        uint8_t b;
        if (rw == 1) b = (uint8_t)v;
        else if (rw == 2) b = (uint8_t)(v >> 8);
        else { b = pit[ch].rw_phase ? (uint8_t)(v >> 8) : (uint8_t)v; pit[ch].rw_phase ^= 1; }
        if (!pit[ch].rw_phase || rw != 3) pit[ch].latched = 0;
        return b;
    }
    case 0x43: return 0xFF;
    case 0x20: return 0;
    case 0x21: return pic_mask;
    case 0x60:
        if (pc.kbc_out_full) { pc.kbc_out_full = 0; return pc.kbc_out; }
        pc.irq9_busy = 0; return pc.last_scancode;
    case 0x61: return pit_speaker;
    case 0x64: return (uint32_t)(0x14 | (pc.kbc_out_full ? 1 : 0));   /* 8042 status: not busy; bit 0 = response ready */
    case 0x92: return (uint32_t)(c->a20_mask != 0xFFFFFu ? 2 : 0);
    case 0x3DA: {                                /* CGA status: toggle retrace bits */
        static uint8_t t; t ^= 0x09; return t;
    }
    default: return 0xFF;
    }
}
static void port_write(x86_cpu *c, uint16_t port, uint32_t val, int size) {
    (void)size;
    if (pc_vga_port_write(port, val, size)) return;
    if (pc.debug > 1 && (port == 0x60 || port == 0x64 || port == 0x61 || port == 0x20))
        fprintf(stderr, "[port] out %02X ← %02X @%llu\n", port, val & 0xFF, (unsigned long long)c->insn_count);
    switch (port) {
    case 0x43: {
        int ch = (val >> 6) & 3;
        if (ch == 3) break;                      /* read-back: unsupported */
        int rw = (val >> 4) & 3;
        if (rw == 0) { pit[ch].latch = pit_now(ch); pit[ch].latched = 1; pit[ch].rw_phase = 0; }
        else { pit[ch].mode_rw = rw; pit[ch].rw_phase = 0; }
        break;
    }
    case 0x40: case 0x41: case 0x42: {
        int ch = port - 0x40;
        int rw = pit[ch].mode_rw ? pit[ch].mode_rw : 3;
        if (rw == 1) pit[ch].reload = (uint16_t)((pit[ch].reload & 0xFF00) | (val & 0xFF));
        else if (rw == 2) pit[ch].reload = (uint16_t)((pit[ch].reload & 0x00FF) | ((val & 0xFF) << 8));
        else if (pit[ch].rw_phase == 0) { pit[ch].reload = (uint16_t)((pit[ch].reload & 0xFF00) | (val & 0xFF)); pit[ch].rw_phase = 1; }
        else { pit[ch].reload = (uint16_t)((pit[ch].reload & 0x00FF) | ((val & 0xFF) << 8)); pit[ch].rw_phase = 0; }
        break;
    }
    case 0x21: pic_mask = (uint8_t)val; break;
    case 0x20:                                   /* EOI: non-specific clears the highest in service */
        if ((val & 0xE0) == 0x60) pc.irq_in_service &= ~(1 << (val & 7));
        else if (val == 0x20) for (int i = 0; i < 8; i++) if (pc.irq_in_service & (1 << i)) { pc.irq_in_service &= ~(1 << i); break; }
        break;
    case 0x61: pit_speaker = (uint8_t)(val & 0x0F); break;
    case 0x64:
        switch (val & 0xFF) {
        case 0xD0: pc.kbc_out = a20_out_port(c); pc.kbc_out_full = 1; break;   /* read output port */
        case 0xD1: pc.kbc_cmd = 0xD1; break;                                   /* write output port: data follows */
        case 0xDD: a20_set(c, 0); break;
        case 0xDF: a20_set(c, 1); break;
        case 0xAD: case 0xAE: break;                                           /* keyboard disable/enable: no-op */
        case 0xFE: fprintf(stderr, "pc: 8042 CPU reset requested; ignored\n"); break;
        default: pc.kbc_cmd = (uint8_t)val; break;                             /* others: swallow any data byte */
        }
        break;
    case 0x60:
        if (pc.kbc_cmd == 0xD1) {
            if (!(val & 1)) fprintf(stderr, "pc: 8042 output port reset bit cleared; ignored\n");
            a20_set(c, (val >> 1) & 1);
        }
        pc.kbc_cmd = 0;                          /* keyboard commands (LEDs, typematic): swallowed */
        break;
    case 0xF4:                                   /* isa-debug-exit, as QEMU offers it:
                                                  * a boot image can say it is done */
        if (pc.boot_img) { pc.exit_requested = 1; pc.exit_code = (int)((val << 1) | 1); c->halted = 1; }
        break;
    case 0x92:
        a20_set(c, (val >> 1) & 1);
        if (val & 1) fprintf(stderr, "pc: port 92h fast reset requested; ignored\n");
        break;
    default: break;
    }
}

/* ---- Boot -------------------------------------------------------------- */
void pc_init(x86_cpu *cpu, int tty_mode) {
    memset(&pc, 0, sizeof pc);
    pc.cpu = cpu;
    pc.tty_mode = tty_mode;
    pc.t0_ns = pc_now_ns();

    cpu->hle_seg = PC_HLE_SEG;
    cpu->hle = hle_dispatch;
    cpu->io_read = port_read;
    cpu->io_write = port_write;

    /* Stub segment: one IRET per vector, plus the ROM signature bytes.
     * Like a real BIOS, every vector nobody serves points at one shared
     * dummy IRET — the AT BIOS's own is at F000:FF53, and software knows
     * it: DOS/4GW finds free vectors by scanning the IVT for two adjacent
     * identical entries, and with a distinct stub per vector it scanned
     * forever. pc_set_service gives a served vector its own stub. */
    for (int v = 0; v < 256; v++) {
        pc_wr8(cpu, PC_HLE_SEG, (uint16_t)v, 0xCF);
        pc_wr16(cpu, 0, (uint16_t)(v * 4), PC_HLE_DUMMY_IRET);
        pc_wr16(cpu, 0, (uint16_t)(v * 4 + 2), PC_HLE_SEG);
    }
    pc_wr8(cpu, PC_HLE_SEG, PC_HLE_DUMMY_IRET, 0xCF);
    static const char date[] = "01/01/92";
    for (int i = 0; i < 8; i++) pc_wr8(cpu, PC_HLE_SEG, (uint16_t)(0xFFF5 + i), (uint8_t)date[i]);
    pc_wr8(cpu, PC_HLE_SEG, 0xFFFE, 0xFC);       /* model: AT */

    /* BIOS data area */
    pc_wr16(cpu, PC_BDA_SEG, 0x10, 0x0021);      /* equipment: 80x25 colour, 1 floppy */
    pc_wr16(cpu, PC_BDA_SEG, 0x13, PC_CONV_KB);
    pc_wr8 (cpu, PC_BDA_SEG, 0x17, 0x00);        /* shift flags */
    pc_wr16(cpu, PC_BDA_SEG, 0x1A, 0x1E);        /* kbd buffer head */
    pc_wr16(cpu, PC_BDA_SEG, 0x1C, 0x1E);        /* tail */
    pc_wr16(cpu, PC_BDA_SEG, 0x80, 0x1E);        /* buffer start */
    pc_wr16(cpu, PC_BDA_SEG, 0x82, 0x3E);        /* buffer end */
    pc_wr8 (cpu, PC_BDA_SEG, 0x96, 0x10);        /* enhanced keyboard */
    pc_wr16(cpu, PC_BDA_SEG, 0x6C, 0);
    pc_wr16(cpu, PC_BDA_SEG, 0x6E, 0);

    pc_set_service(0x08, bios_int8, HLE_RET_IRET);
    pc_set_service(0x11, bios_int11, HLE_RET_FLAGS);
    pc_set_service(0x12, bios_int12, HLE_RET_FLAGS);
    pc_set_service(0x13, bios_int13, HLE_RET_FLAGS);
    pc_set_service(0x15, bios_int15, HLE_RET_FLAGS);
    pc_set_service(0x1A, bios_int1a, HLE_RET_FLAGS);
    pc_set_service(0x10, pc_video_int10, HLE_RET_FLAGS);
    pc_set_service(0x16, pc_kbd_int16, HLE_RET_FLAGS);
    pc_set_service(0x09, pc_kbd_int9, HLE_RET_IRET);

    pc_video_init(cpu);
    pc_kbd_init();
    cpu->eflags |= X86_IF;
}
