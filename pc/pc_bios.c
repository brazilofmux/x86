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
    if (!(pc.irq_pending & (1 << 9)) && !pc.irq9_busy && pc_kbd_raw_pending()) {
        uint8_t code;
        pc_kbd_raw_next(&code);
        pc.last_scancode = code;
        pc.irq_pending |= 1 << 9;
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
    if (!pc.irq_pending || !(c->eflags & X86_IF) || c->int_inhibit) return 0;
    if (pc.irq_pending & (1 << 8)) {
        pc.irq_pending &= ~(1 << 8);
        pc.ticks_delivered++;
        deliver(c, 8);
    } else if (pc.irq_pending & (1 << 9)) {
        pc.irq_pending &= ~(1 << 9);
        deliver(c, 9);
    }
    return 1;
}

/* ---- Ports ------------------------------------------------------------- */
static uint32_t port_read(x86_cpu *c, uint16_t port, int size) {
    (void)c; (void)size;
    switch (port) {
    case 0x60: return pc.last_scancode;
    case 0x61: return 0x00;
    case 0x3DA: {                                /* CGA status: toggle retrace bits */
        static uint8_t t; t ^= 0x09; return t;
    }
    default: return 0xFF;
    }
}
static void port_write(x86_cpu *c, uint16_t port, uint32_t val, int size) {
    (void)c; (void)port; (void)val; (void)size;
}

/* ---- Boot -------------------------------------------------------------- */
void pc_init(x86_cpu *cpu) {
    memset(&pc, 0, sizeof pc);
    pc.cpu = cpu;
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

    pc_video_init(cpu);
    pc_kbd_init();
    cpu->eflags |= X86_IF;
}
