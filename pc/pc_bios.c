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


/* Monotonic nanoseconds. Every caller takes differences only. The run
 * loop asks after every interpreter step (a DOS call, most of the time),
 * which through clock_gettime was 6% of a COBOL program's run; on
 * AArch64 the virtual counter is a register read, scaled here by a
 * 32.32 fixed-point factor (24 MHz on Apple Silicon: 41.7 ns ticks). */
#if defined(__aarch64__)
uint64_t pc_now_ns(void) {
    static uint64_t mult;
    uint64_t v;
    if (!mult) {
        uint64_t f;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
        mult = f ? (1000000000ull << 32) / f : 1ull << 32;
    }
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return (uint64_t)(((unsigned __int128)v * mult) >> 32);
}
#else
uint64_t pc_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

/* An internal trap: an address in the HLE segment the host hands out
 * itself (the DPMI entry, its return trampolines). The IVT is not touched. */
void pc_set_trap(int offset, pc_service_fn fn, int ret_mode) {
    pc.service[offset & 0xFF] = fn;
    pc.ret_mode[offset & 0xFF] = (uint8_t)ret_mode;
}

/* A real interrupt service: the vector gets its own stub, F000:vector. */
void pc_set_service(int vector, pc_service_fn fn, int ret_mode) {
    pc_set_trap(vector, fn, ret_mode);
    pc.ivt_service[vector & 0xFF] = 1;
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
    if (mode == HLE_RET_IRET) {
        uint32_t nf = w == 4 ? fl : (c->eflags & 0xFFFF0000u) | fl;
        if (c->eflags & X86_VM) nf = (nf & ~(uint32_t)X86_IOPL) | (c->eflags & X86_IOPL);   /* V86 cannot change IOPL */
        c->eflags = x86_flags_fixup(c, nf);
    }
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
    uint32_t t = pc_rd16(c, PC_BDA_SEG, 0x6C) | ((uint32_t)pc_rd16(c, PC_BDA_SEG, 0x6E) << 16);
    t++;
    if (t >= 0x1800B0) { t = 0; pc_wr8(c, PC_BDA_SEG, 0x70, 1); }
    pc_wr16(c, PC_BDA_SEG, 0x6C, (uint16_t)t);
    pc_wr16(c, PC_BDA_SEG, 0x6E, (uint16_t)(t >> 16));
    /* Then the user tick hook, INT 1Ch, the way a BIOS calls it: by
     * executing INT 1Ch, from the native stub at PC_STUB_SEG:0 (INT 1Ch;
     * IRET) with our frame still on the stack. From V86 mode that INT is a
     * real instruction for the monitor to fault on, decode and reflect —
     * EMM386 reads the CD 1C at CS:IP; a delivery made by the host instead
     * went through its IDT and ended at 0000:0000. The HLE DPMI host's
     * protected mode keeps the direct call. */
    /* The EOI likewise: the stub's OUT 20h after INT 1Ch, as the AT BIOS
     * sends it. Done here in the host, WIN386's virtual PIC never saw it,
     * kept IRQ 0 in service and delivered nothing below it (the keyboard). */
    if (c->pmode && !(c->eflags & X86_VM)) {
        pc.irq_in_service &= ~1;                 /* the HLE DPMI host's: no stub */
        pc_hle_return(c, HLE_RET_IRET);
        x86_interrupt(c, 0x1C, 0);
        return;
    }
    x86_load_seg(c, S_CS, PC_STUB_SEG);
    c->eip = PC_STUB_INT1C;
    pc.returned = 1;
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

static void bios_int15(x86_cpu *c, int vector) {
    (void)vector;
    if (pc_int15_memory(c)) return;              /* 87h, 88h, E801h, E820h: pc_cmos.c */
    switch (x86_get_r8(c, R_AH)) {
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
/* The master 8259: the mask (IRQs 0 and 1 are the ones we raise), the
 * vector base from ICW2, and what a read of port 20h returns (OCW3: the
 * request register, or the in-service register after 0Bh). DOS/4GW under
 * a VCPI server tells IRQ 0 from a double fault, both vector 8, by
 * reading the ISR; a port that read 0 made every tick a double fault. */
static struct { uint8_t mask, base, icw_step, need_icw4, single, read_isr; } pic = { 0xB8, 8, 0, 0, 0, 0 };
static void deliver(x86_cpu *c, int irq) {
    int vector = pic.base + irq;
    pc.irq_in_service |= 1 << irq;
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
    if (pc_mouse_poll(c)) return 1;                  /* a program's mouse event handler, called */

    /* Keyboard: latch the next raw code and raise INT 9 once the previous
     * one has been taken (the guest's handler has returned). */
    /* A keyboard delivers a scancode every couple of milliseconds at
     * best; handlers (WP's) rely on having finished with one before the
     * next arrives, beyond what the PIC's in-service gating guarantees. */
    /* And not while IRQ 1 is still in service: a V86 monitor (JEMM) reads
     * port 60h itself before it reflects the interrupt to DOS, which let
     * the next code overwrite the latch before the BIOS's INT 9 had taken
     * the first — one key lost, the next one twice ("HHlo"). The EOI, or
     * our INT 9, says the code has been used. */
    static uint64_t last_code_ns;
    if (!(pc.irq_pending & (1 << 9)) && !pc.irq9_busy && !(pc.irq_in_service & 2) && !pc.kbd_disabled
        && pc_kbd_raw_pending() && now - last_code_ns >= 2000000ull) {
        uint8_t code;
        pc_kbd_raw_next(&code);
        pc.last_scancode = code;
        if (pc.kbc_cmdbyte & 1) pc.irq_pending |= 1 << 9;   /* IRQ 1, if the command byte enables it */
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
        /* Behind by more than one period (a stall, or a host too slow for
         * the guest's rate — -V is thirty times slower): the missed ticks
         * are dropped, as the 8259 would drop them, since it latches one
         * IRQ 0 at most. Delivering the backlog instead came out
         * back-to-back at consecutive polls with no guest instruction in
         * between, and DOOM, which waits for its tick count to EQUAL
         * start + 30, could see it jump past and spin forever. */
        if (now - pc.next_tick_ns > period) pc.next_tick_ns = now;
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
    if ((pc.irq_pending & (1 << 8)) && !(pc.irq_in_service & 1) && !(pic.mask & 1)) {
        pc.irq_pending &= ~(1 << 8);
        pc.ticks_delivered++;
        pc.next_tick_ns += irq0_period_ns();
        deliver(c, 0);
        return 1;
    }
    if ((pc.irq_pending & (1 << 9)) && !(pc.irq_in_service & 3) && !(pic.mask & 2)) {
        pc.irq_pending &= ~(1 << 9);
        deliver(c, 1);
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
static uint8_t pit_speaker;

/* Channel 0's period: reload 0 means 65536, the BIOS's 54.9 ms.
 * X86_PIT_SCALE=N (measurement aid) makes the timer tick N times faster
 * than wall clock, so code that spins on the tick count — DOOM's screen
 * melt inside a timedemo, a third of its wall time — stops hiding what
 * the translator does. Everything the guest times from IRQ 0 is off by
 * the same factor, so only host seconds mean anything under it. */
static uint64_t irq0_period_ns(void) {
    static double scale;
    if (scale == 0) { const char *s = getenv("X86_PIT_SCALE"); scale = s ? atof(s) : 1.0; if (scale <= 0) scale = 1.0; }
    uint64_t reload = pit[0].reload ? pit[0].reload : 65536;
    return (uint64_t)((double)reload * 1000000000.0 / (double)PIT_HZ / scale);
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
    if (pc_cmos_port_read(port, &vv)) return vv;
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
    case 0x20: return pic.read_isr ? (uint32_t)(pc.irq_in_service & 0xFF) : (uint32_t)((pc.irq_pending >> 8) & 3);
    case 0x21: return pic.mask;
    case 0x60:
        if (pc.kbc_out_full) { pc.kbc_out_full = 0; return pc.kbc_out; }
        pc.irq9_busy = 0; return pc.last_scancode;
    case 0x61:
        /* bit 4 toggles with the DRAM refresh, every 15.085 us — the
         * AT's own timing reference (IO.SYS counts its toggles while it
         * waits for the keyboard; a bit that never moved hung it) */
        return (uint32_t)((pit_speaker & 0x0F) | ((pc_now_ns() / 15085u) & 1u) << 4);
    /* 8042 status: not busy, system flag; bit 0 = output buffer full — a
     * controller reply, or a scancode latched for IRQ 1 and not yet read
     * (WIN386's keyboard VxD looks here before it reads port 60h) */
    case 0x64: return (uint32_t)(0x14 | (pc.kbc_out_full || pc.irq9_busy ? 1 : 0));
    case 0x92: return (uint32_t)(c->a20_mask != 0xFFFFFu ? 2 : 0);
    case 0x3DA: {                                /* CGA status: toggle retrace bits */
        static uint8_t t; t ^= 0x09;
        pc_vga_status_read();
        return t;
    }
    default: return 0xFF;
    }
}
static void port_write(x86_cpu *c, uint16_t port, uint32_t val, int size) {
    (void)size;
    if (pc_vga_port_write(port, val, size)) return;
    if (pc_cmos_port_write(port, val)) return;
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
    case 0x21:
        if (pic.icw_step == 2) {                 /* ICW2: the vector base; ICW3 next if cascaded */
            pic.base = (uint8_t)(val & 0xF8);
            pic.icw_step = !pic.single ? 3 : pic.need_icw4 ? 4 : 0;
        } else if (pic.icw_step == 3) pic.icw_step = pic.need_icw4 ? 4 : 0;   /* ICW3 */
        else if (pic.icw_step == 4) pic.icw_step = 0;                          /* ICW4 */
        else pic.mask = (uint8_t)val;
        break;
    case 0x20:                                   /* EOI: non-specific clears the highest in service */
        if (val & 0x10) {                        /* ICW1: mask cleared, IRR selected, ICW2.. follow */
            pic.icw_step = 2; pic.need_icw4 = val & 1; pic.single = (val >> 1) & 1;
            pic.mask = 0; pic.read_isr = 0;
        }
        else if ((val & 0x18) == 0x08) { if (val & 2) pic.read_isr = val & 1; }   /* OCW3 */
        else if ((val & 0xE0) == 0x60) pc.irq_in_service &= ~(1 << (val & 7));
        else if (val == 0x20) for (int i = 0; i < 8; i++) if (pc.irq_in_service & (1 << i)) { pc.irq_in_service &= ~(1 << i); break; }
        break;
    case 0x61: pit_speaker = (uint8_t)(val & 0x0F); break;
    case 0x64:
        /* The 8042's commands. Only 60h and D1h-D4h take a data byte
         * (kbc_cmd); a command that took one by mistake swallowed the
         * next byte meant for the keyboard — WIN386's F3h never got its
         * ACK, and its keyboard driver stalled. */
        switch (val & 0xFF) {
        case 0x20: pc.kbc_out = pc.kbc_cmdbyte; pc.kbc_out_full = 1; break;    /* read command byte */
        case 0x60: case 0xD1: case 0xD2: case 0xD3: case 0xD4:
            pc.kbc_cmd = (uint8_t)val; break;                                  /* data follows on 60h */
        case 0xA7: case 0xA8: break;                                           /* auxiliary port off/on: none */
        case 0xA9: pc.kbc_out = 0x00; pc.kbc_out_full = 1; break;              /* aux interface test: ok */
        case 0xAA: pc.kbc_out = 0x55; pc.kbc_out_full = 1; break;              /* self test: passed */
        case 0xAB: pc.kbc_out = 0x00; pc.kbc_out_full = 1; break;              /* keyboard interface test: ok */
        case 0xAD: pc.kbd_disabled |= 1; break;                                /* keyboard interface off */
        case 0xAE: pc.kbd_disabled &= (uint8_t)~1; break;                      /* ... on */
        case 0xC0: pc.kbc_out = 0xBF; pc.kbc_out_full = 1; break;              /* input port: not inhibited, colour */
        case 0xD0: pc.kbc_out = a20_out_port(c); pc.kbc_out_full = 1; break;   /* read output port */
        case 0xDD: a20_set(c, 0); break;
        case 0xDF: a20_set(c, 1); break;
        case 0xE0: pc.kbc_out = 0x00; pc.kbc_out_full = 1; break;              /* test inputs */
        case 0xFE: pc_request_reset(c, "8042 CPU reset"); break;
        default: break;
        }
        break;
    case 0x60: {
        uint8_t v = (uint8_t)val;
        switch (pc.kbc_cmd) {
        case 0x60: pc.kbc_cmdbyte = v; break;                                  /* write command byte */
        case 0xD1:
            if (!(v & 1)) pc_request_reset(c, "8042 output port reset bit");
            a20_set(c, (v >> 1) & 1);
            break;
        case 0xD2: pc_kbd_raw_reply(v); break;                                 /* as if the keyboard sent it */
        case 0xD3: case 0xD4: break;                                           /* to the auxiliary port: none */
        default:
            /* a byte for the keyboard itself. Replies go in front of any
             * queued keys, last first (pc_kbd_raw_reply inserts at the head). */
            if (pc.kbd_cmd) { pc.kbd_cmd = 0; pc_kbd_raw_reply(0xFA); break; }   /* the command's data byte */
            switch (v) {
            case 0xFF: pc_kbd_raw_reply(0xAA); pc_kbd_raw_reply(0xFA); pc.kbd_disabled &= (uint8_t)~2; break;   /* reset: ACK, self test passed */
            case 0xF2: pc_kbd_raw_reply(0x41); pc_kbd_raw_reply(0xAB); pc_kbd_raw_reply(0xFA); break;           /* identify: MF2, translated */
            case 0xEE: pc_kbd_raw_reply(0xEE); break;                          /* echo */
            case 0xED: case 0xF3: case 0xF0: case 0xFB: case 0xFC: case 0xFD:
                pc.kbd_cmd = v; pc_kbd_raw_reply(0xFA); break;                 /* a data byte follows */
            case 0xF4: pc.kbd_disabled &= (uint8_t)~2; pc_kbd_raw_reply(0xFA); break;   /* enable scanning */
            case 0xF5: pc.kbd_disabled |= 2; pc_kbd_raw_reply(0xFA); break;             /* default, scanning off */
            default: pc_kbd_raw_reply(0xFA); break;
            }
        }
        pc.kbc_cmd = 0;
        break;
    }
    case 0xE9:                                   /* the Bochs/QEMU debug console: a boot
                                                  * image's transcript (tools/pmoracle) */
        if (pc.booted) { fputc((int)(val & 0xFF), stdout); if ((val & 0xFF) == '\n') fflush(stdout); }
        break;
    case 0xF4:                                   /* isa-debug-exit, as QEMU offers it:
                                                  * a boot image can say it is done */
        if (pc.booted) { pc.exit_requested = 1; pc.exit_code = (int)((val << 1) | 1); c->halted = 1; }
        break;
    case 0x92:
        a20_set(c, (val >> 1) & 1);
        if (val & 1) pc_request_reset(c, "port 92h fast reset");
        break;
    default: break;
    }
}

/* ---- Boot -------------------------------------------------------------- */

/* What POST leaves behind: the ROM's stubs and signature, an IVT with the
 * served vectors on their own stubs and the rest on the shared dummy
 * IRET, the reset vector, the BIOS data area. Run at power-on and again
 * at every reboot, so it only writes. */
static void post(x86_cpu *cpu) {
    pc.kbc_cmdbyte = 0x45; pc.kbd_disabled = 0; pc.kbd_cmd = 0;   /* 8042: IRQ 1 on, system flag, translate */
    /* Stub segment: one IRET per vector, plus the ROM signature bytes.
     * Like a real BIOS, every vector nobody serves points at one shared
     * dummy IRET — the AT BIOS's own is at F000:FF53, and software knows
     * it: DOS/4GW finds free vectors by scanning the IVT for two adjacent
     * identical entries, and with a distinct stub per vector it scanned
     * forever. pc_set_service gives a served vector its own stub. */
    for (int v = 0; v < 256; v++) {
        pc_wr8(cpu, PC_HLE_SEG, (uint16_t)v, 0xCF);
        pc_wr16(cpu, 0, (uint16_t)(v * 4), pc.ivt_service[v] ? (uint16_t)v : PC_HLE_DUMMY_IRET);
        pc_wr16(cpu, 0, (uint16_t)(v * 4 + 2), PC_HLE_SEG);
    }
    pc_wr8(cpu, PC_HLE_SEG, PC_HLE_DUMMY_IRET, 0xCF);
    /* The ROM fonts, where the video BIOS hands them out (AX=1130h) and a
     * VxD copies them from; INT 1Fh names the 8x8 set's upper half, INT
     * 43h the graphics font (the mode set changes it). */
    for (int i = 0; i < 256 * 16; i++) pc_wr8(cpu, PC_HLE_SEG, (uint16_t)(PC_FONT16_OFF + i), pc_font16[i]);
    for (int i = 0; i < 256 * 14; i++) pc_wr8(cpu, PC_HLE_SEG, (uint16_t)(PC_FONT14_OFF + i), pc_font14[i]);
    for (int i = 0; i < 256 * 8; i++)  pc_wr8(cpu, PC_HLE_SEG, (uint16_t)(PC_FONT8_OFF + i), pc_font8[i]);
    for (int i = 0; i < 128 * 8; i++)  pc_wr8(cpu, PC_HLE_SEG, (uint16_t)(PC_FONT8_AT_OFF + i), pc_font8[i]);
    pc_wr16(cpu, 0, 0x1F * 4, (uint16_t)(PC_FONT8_OFF + 128 * 8)); pc_wr16(cpu, 0, 0x1F * 4 + 2, PC_HLE_SEG);
    pc_wr16(cpu, 0, 0x43 * 4, PC_FONT8_AT_OFF);                    pc_wr16(cpu, 0, 0x43 * 4 + 2, PC_HLE_SEG);
    /* Native BIOS code (outside the trap segment's base, so it runs):
     * INT 8's tail, INT 1Ch then IRET. */
    /* INT 1Ch; push ax; mov al,20h; out 20h,al (EOI); pop ax; iret */
    static const uint8_t int1c_tail[] = { 0xCD, 0x1C, 0x50, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF };
    for (size_t i = 0; i < sizeof int1c_tail; i++) pc_wr8(cpu, PC_STUB_SEG, (uint16_t)(PC_STUB_INT1C + i), int1c_tail[i]);
    /* push ax; in al,60h; pushf; call far F000:PC_TRAP_KBD; cli;
     * mov al,20h; out 20h,al; pop ax; iret */
    static const uint8_t int9[] = { 0x50, 0xE4, 0x60, 0x9C, 0x9A, PC_TRAP_KBD, 0x00, 0x00, 0xF0,
                                    0xFA, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF };
    for (size_t i = 0; i < sizeof int9; i++) pc_wr8(cpu, PC_STUB_SEG, (uint16_t)(PC_STUB_INT9 + i), int9[i]);
    pc_vga_rom(cpu);                             /* INT 10h's mode set, programmed by OUTs */
    /* FFFF:0000, the reset vector: JMP F000:FFF0, which traps (TRAP_RESET) */
    static const uint8_t jmp[5] = { 0xEA, 0xF0, 0xFF, 0x00, 0xF0 };
    for (int i = 0; i < 5; i++) pc_wr8(cpu, 0xFFFF, (uint16_t)i, jmp[i]);
    static const char date[] = "01/01/92";
    for (int i = 0; i < 8; i++) pc_wr8(cpu, PC_HLE_SEG, (uint16_t)(0xFFF5 + i), (uint8_t)date[i]);
    pc_wr8(cpu, PC_HLE_SEG, 0xFFFE, 0xFC);       /* model: AT */

    /* BIOS data area */
    for (int i = 0; i < 0x100; i++) pc_wr8(cpu, PC_BDA_SEG, (uint16_t)i, 0);
    pc_wr16(cpu, PC_BDA_SEG, 0x10, 0x0021);      /* equipment: 80x25 colour, 1 floppy */
    pc_wr16(cpu, PC_BDA_SEG, 0x13, PC_CONV_KB);
    pc_wr8 (cpu, PC_BDA_SEG, 0x17, 0x00);        /* shift flags */
    pc_wr16(cpu, PC_BDA_SEG, 0x1A, 0x1E);        /* kbd buffer head */
    pc_wr16(cpu, PC_BDA_SEG, 0x1C, 0x1E);        /* tail */
    pc_wr16(cpu, PC_BDA_SEG, 0x80, 0x1E);        /* buffer start */
    pc_wr16(cpu, PC_BDA_SEG, 0x82, 0x3E);        /* buffer end */
    pc_wr8 (cpu, PC_BDA_SEG, 0x96, 0x10);        /* enhanced keyboard */
}

/* The upper memory area where adapters would sit (C0000-EFFFF): no option
 * ROM and no adapter RAM here, so it behaves as an empty bus does: reads
 * FFh, and a store does not stick. EMM386's scan takes that for free
 * space; writable zeros looked like adapter RAM, and it found no room for
 * its page frame. A booted machine
 * only: under the HLE DOS nothing scans for it. */
/* A booted machine takes IRQ 1 through the native INT 9 (PC_STUB_INT9),
 * whose port 60h read and EOI a V86 monitor sees. The HLE shim keeps the
 * host's handler, which drains keys straight through (drain_raw_here). */
void pc_native_irq_vectors(x86_cpu *c) {
    pc_wr16(c, 0, 9 * 4, PC_STUB_INT9);
    pc_wr16(c, 0, 9 * 4 + 2, PC_STUB_SEG);
}

void pc_empty_upper_memory(x86_cpu *c) {
    memset(c->mem + 0xC0000, 0xFF, 0x30000);
    for (uint32_t p = 0xC0000; p < 0xF0000; p++) c->code_bitmap[p] |= X86_BM_EMPTY;   /* stores put the FFh back */
}

/* A CPU reset — the 8042's pulse, port 92h, a jump to FFFF:0000. A booted
 * machine stops the run and main() reboots it (pc_reboot); under the HLE
 * DOS there is nothing to boot, so it is reported and ignored as always. */
void pc_request_reset(x86_cpu *c, const char *how) {
    if (!pc.booted) { fprintf(stderr, "pc: %s requested; ignored\n", how); return; }
    if (pc.debug) fprintf(stderr, "pc: %s: rebooting\n", how);
    pc.reboot = 1;
    pc.exit_requested = 1;
    c->halted = 1;
}

#define TRAP_RESET 0xF0                  /* F000:FFF0 */
static void reset_trap(x86_cpu *c, int vector) {
    (void)vector;
    pc_request_reset(c, "jump to the reset vector");
    pc.returned = 1;
}

/* Warm boot, as the ROM does it after a reset: memory cleared, POST, the
 * drives' tables, mode 3, the boot drive's sector one at 0000:7C00. The
 * caller has stopped the run and flushes the translator's cache. */
void pc_reboot(x86_cpu *c) {
    memset(c->mem, 0, c->mem_size);
    x86_reset(c);
    pc.reboot = 0; pc.exit_requested = 0; pc.exit_code = 0;
    pc.irq_pending = 0; pc.irq_in_service = 0; pc.irq9_busy = 0;
    pc.kbc_cmd = 0; pc.kbc_out_full = 0;
    memset(pit, 0, sizeof pit);
    pic.mask = 0xB8; pic.base = 8; pic.icw_step = 0; pic.read_isr = 0;
    post(c);
    pc_empty_upper_memory(c);
    pc_native_irq_vectors(c);
    pc_disk_install(c);
    pc_cmos_init(c);
    pc_mouse_reboot(c);
    pc_video_init(c);
    pc_disk_boot(c, pc.boot_drive);
    for (int i = 0; i < 6; i++) x86_load_seg(c, i, 0);
    c->eip = 0x7C00;
    c->r[R_SP] = 0x7C00;
    c->r[R_DX] = (uint32_t)pc.boot_drive;
    x86_set_a20(c, 1);
    c->eflags |= X86_IF;
}

void pc_init(x86_cpu *cpu, int tty_mode) {
    memset(&pc, 0, sizeof pc);
    pc.cpu = cpu;
    pc.tty_mode = tty_mode;
    pc.t0_ns = pc_now_ns();

    cpu->hle_seg = PC_HLE_SEG;
    cpu->hle = hle_dispatch;
    cpu->io_read = port_read;
    cpu->io_write = port_write;
    post(cpu);
    pc_set_trap(TRAP_RESET, reset_trap, HLE_RET_IRET);

    pc_set_service(0x08, bios_int8, HLE_RET_IRET);
    pc_set_service(0x11, bios_int11, HLE_RET_FLAGS);
    pc_set_service(0x12, bios_int12, HLE_RET_FLAGS);
    pc_set_service(0x15, bios_int15, HLE_RET_FLAGS);
    pc_set_service(0x1A, bios_int1a, HLE_RET_FLAGS);
    pc_set_service(0x10, pc_video_int10, HLE_RET_FLAGS);
    pc_set_service(0x16, pc_kbd_int16, HLE_RET_FLAGS);
    pc_set_service(0x09, pc_kbd_int9, HLE_RET_IRET);
    pc_set_trap(PC_TRAP_KBD, pc_kbd_trap, HLE_RET_IRET);

    pc_video_init(cpu);
    pc_kbd_init();
    cpu->eflags |= X86_IF;
}
