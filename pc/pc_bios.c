/* pc_bios.c — IVT, BIOS data area, the HLE stub segment, BIOS services
 * (INT 11h/12h/15h/1Ah), the timer tick, and IRQ delivery.
 */
#include "pc.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

pc_state pc;

#define TICK_NS 54925493ull          /* 1193182 Hz / 65536 */

uint64_t pc_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void pc_set_service(int vector, pc_service_fn fn, int ret_mode) {
    pc.service[vector & 0xFF] = fn;
    pc.ret_mode[vector & 0xFF] = (uint8_t)ret_mode;
}

/* Pop the INT frame. FLAGS mode is MS-DOS's RETF 2: the caller gets the
 * handler's flags (CF/ZF results) but keeps its own IF/TF. */
void pc_hle_return(x86_cpu *c, int mode) {
    uint32_t sp = c->r[R_SP] & 0xFFFF;
    uint16_t ip = pc_rd16(c, c->seg[S_SS].sel, (uint16_t)sp);
    uint16_t cs = pc_rd16(c, c->seg[S_SS].sel, (uint16_t)(sp + 2));
    uint16_t fl = pc_rd16(c, c->seg[S_SS].sel, (uint16_t)(sp + 4));
    c->r[R_SP] = (c->r[R_SP] & 0xFFFF0000u) | ((sp + 6) & 0xFFFF);
    pc.returned = 1;
    x86_load_seg(c, S_CS, cs);
    c->eip = ip;
    if (mode == HLE_RET_IRET)
        c->eflags = x86_flags_fixup(c, (c->eflags & 0xFFFF0000u) | fl);
    else
        c->eflags = x86_flags_fixup(c, (c->eflags & ~(uint32_t)(X86_IF | X86_TF)) | (fl & (X86_IF | X86_TF)));
}

static void hle_dispatch(x86_cpu *c, int vector) {
    pc_service_fn fn = pc.service[vector];
    int mode = fn ? pc.ret_mode[vector] : HLE_RET_IRET;
    pc.returned = 0;
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
        if (us) usleep(us);
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

int pc_poll(x86_cpu *c) {
    pc_kbd_poll(c);
    pc_kbd_idle_poll(c);
    pc_video_flush(0);
    if (c->halted && pc.exit_requested) return 1;

    /* Keyboard: latch the next raw code and raise INT 9 once the previous
     * one has been taken (the guest's handler has returned). */
    /* A keyboard delivers a scancode every couple of milliseconds at
     * best; handlers (WP's) rely on having finished with one before the
     * next arrives, beyond what the PIC's in-service gating guarantees. */
    static uint64_t last_code_ns;
    uint64_t now = pc_now_ns();
    if (!(pc.irq_pending & (1 << 9)) && !pc.irq9_busy && pc_kbd_raw_pending() && now - last_code_ns >= 2000000ull) {
        uint8_t code;
        pc_kbd_raw_next(&code);
        pc.last_scancode = code;
        pc.irq_pending |= 1 << 9;
        pc.irq9_busy = 1;                        /* until the handler reads port 60h */
        last_code_ns = now;
    }

    uint64_t expected = (pc_now_ns() - pc.t0_ns) / TICK_NS;
    if (pc.ticks_delivered + 18 < expected) pc.ticks_delivered = expected - 1;   /* don't storm after a stall */
    if (pc.ticks_delivered < expected) pc.irq_pending |= 1 << 8;

    if (c->halted && (c->eflags & X86_IF) && !pc.irq_pending) {
        /* HLT with interrupts on: the guest is idling for the next tick. */
        uint64_t next = pc.t0_ns + (pc.ticks_delivered + 1) * TICK_NS;
        uint64_t now = pc_now_ns();
        if (next > now) usleep((useconds_t)((next - now) / 1000 + 1));
        pc.irq_pending |= 1 << 8;
    }
    if (pc.debug > 2) { static int n; if ((n++ & 1023) == 0) fprintf(stderr, "[poll] pending %X isr %X IF %d inhibit %d @%llu\n", pc.irq_pending, pc.irq_in_service, (c->eflags & X86_IF) != 0, c->int_inhibit, (unsigned long long)c->insn_count); }
    if (!pc.irq_pending || !(c->eflags & X86_IF) || c->int_inhibit) return 0;
    /* 8259 priority: nothing while an equal-or-higher IRQ is in service
     * (until its EOI). A handler that never EOIs would hang a real PC;
     * we forgive it after 200 ms of wall clock. */
    if (pc.irq_in_service && pc_now_ns() - pc.irq_service_ns > 200000000ull) pc.irq_in_service = 0;
    if ((pc.irq_pending & (1 << 8)) && !(pc.irq_in_service & 1)) {
        pc.irq_pending &= ~(1 << 8);
        pc.ticks_delivered++;
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

    /* Stub segment: one IRET per vector, plus the ROM signature bytes. */
    for (int v = 0; v < 256; v++) {
        pc_wr8(cpu, PC_HLE_SEG, (uint16_t)v, 0xCF);
        pc_wr16(cpu, 0, (uint16_t)(v * 4), (uint16_t)v);
        pc_wr16(cpu, 0, (uint16_t)(v * 4 + 2), PC_HLE_SEG);
    }
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
    pc_set_service(0x15, bios_int15, HLE_RET_FLAGS);
    pc_set_service(0x1A, bios_int1a, HLE_RET_FLAGS);
    pc_set_service(0x10, pc_video_int10, HLE_RET_FLAGS);
    pc_set_service(0x16, pc_kbd_int16, HLE_RET_FLAGS);
    pc_set_service(0x09, pc_kbd_int9, HLE_RET_IRET);

    pc_video_init(cpu);
    pc_kbd_init();
    cpu->eflags |= X86_IF;
}
