/* pc.h — the PC personality: BIOS services, text video, keyboard, timer.
 *
 * The guest sees an IBM PC/AT-ish machine: IVT at 0, BIOS data area at
 * 0040:0000, 640 KB conventional memory, 80x25 colour text at B800:0000,
 * BIOS services INT 10h/11h/12h/15h/16h/1Ah, and a timer tick (INT 8 →
 * INT 1Ch) at 18.2 Hz. Every service is high-level emulated: the IVT
 * entries point into the stub segment PC_HLE_SEG (one IRET per vector)
 * and executing there traps to pc_hle_dispatch (see x86_cpu.hle).
 *
 * Video memory is guest memory. The text model in pc_video.c keeps the
 * cursor in the BDA like a real BIOS and offers two views: stdio mode
 * (teletype output echoed to stdout, for bring-up and scripting) and
 * tty mode (a diffing ANSI painter of the B800 cell buffer, for real
 * full-screen applications).
 */
#ifndef PC_H
#define PC_H

#include "../core/x86.h"

#define PC_HLE_SEG      0xF000
#define PC_BDA_SEG      0x0040
#define PC_VIDEO_SEG    0xB800
#define PC_CONV_KB      640
#define PC_ROWS         25
#define PC_COLS         80

/* Return convention of a service after pc_hle_dispatch ran it. */
enum { HLE_RET_FLAGS, HLE_RET_IRET };

typedef void (*pc_service_fn)(x86_cpu *c, int vector);

typedef struct pc_state {
    x86_cpu *cpu;

    /* Video */
    int tty_mode;                    /* 1: full-screen ANSI painter; 0: stdio echo */
    int cursor_row, cursor_col;      /* mirror of BDA 40:50 for the painter */

    /* Keyboard: host keys become (ascii, scancode) pairs in the BIOS
     * ring buffer at 40:1E; the last scancode is readable at port 60h. */
    uint8_t  last_scancode;
    uint8_t  irq9_busy;              /* reserved: INT 9 in progress */
    uint8_t  kbc_cmd;                /* 8042: command awaiting its data byte on port 60h (0 = none) */
    uint8_t  kbc_out;                /* 8042: pending response for port 60h (with kbc_out_full) */
    uint8_t  kbc_out_full;
    int      kbd_raw;                /* host terminal is in raw mode */
    uint64_t kbd_reads;              /* guest keyboard reads/polls; paces scripted input */
    uint64_t next_slow_ns;           /* pc_poll: when its periodic work is next due */
    uint64_t now_ns;                 /* clock read by the last pc_poll, for reuse */
    int      eof_seen;

    /* Timer */
    uint64_t t0_ns;                  /* wall clock at boot */
    uint64_t ticks_delivered;        /* INT 8s raised so far */
    int      irq_pending;            /* bitmask: 1<<8 timer, 1<<9 keyboard */
    int      irq_in_service;         /* 8259 ISR: bits set from delivery until EOI */
    uint64_t irq_service_ns;         /* when the in-service IRQ was delivered (stuck-handler guard) */

    /* Services by vector; NULL = plain IRET stub. */
    pc_service_fn service[256];
    uint8_t       ret_mode[256];

    int returned;                    /* the running service popped its own frame */
    int (*swap_disk)(void);          /* ESC-+ : next diskette (dos layer) */
    int exit_requested;              /* DOS asked to terminate */
    int exit_code;
    int debug;
} pc_state;

extern pc_state pc;

/* pc_bios.c */
void pc_init(x86_cpu *cpu, int tty_mode); /* IVT, BDA, stub segment, services */
void pc_set_service(int vector, pc_service_fn fn, int ret_mode);
void pc_hle_return(x86_cpu *c, int mode); /* pop the INT frame per mode */
int  pc_poll(x86_cpu *c);                /* between blocks: keys, timer, IRQ delivery; 1 if cpu state changed */
uint64_t pc_now_ns(void);

/* Segment:offset helpers on guest memory (real mode, A20 respected). */
static inline uint32_t pc_lin(uint16_t seg, uint16_t off) { return ((uint32_t)seg << 4) + off; }
static inline uint8_t  pc_rd8 (x86_cpu *c, uint16_t seg, uint16_t off) { return x86_phys_rd8(c, pc_lin(seg, off)); }
static inline uint16_t pc_rd16(x86_cpu *c, uint16_t seg, uint16_t off) { return (uint16_t)x86_rd(c, (uint32_t)seg << 4, off, 0xFFFF, 2); }
static inline void     pc_wr8 (x86_cpu *c, uint16_t seg, uint16_t off, uint8_t v) { x86_phys_wr8(c, pc_lin(seg, off), v); }
static inline void     pc_wr16(x86_cpu *c, uint16_t seg, uint16_t off, uint16_t v) { x86_wr(c, (uint32_t)seg << 4, off, 0xFFFF, 2, v); }

/* pc_video.c */
void pc_video_init(x86_cpu *c);
void pc_video_teletype(x86_cpu *c, uint8_t ch);          /* INT 10h/0E semantics, page 0 */
void pc_video_int10(x86_cpu *c, int vector);
void pc_video_flush(int force);                          /* tty mode painter */
void pc_video_shutdown(void);
void pc_video_set_hud(const char *text);
void pc_video_dump(x86_cpu *c, FILE *f);                 /* the text buffer as 25 lines of UTF-8 */

/* pc_kbd.c */
void pc_kbd_init(void);
void pc_kbd_shutdown(void);
void pc_kbd_poll(x86_cpu *c);                            /* host keys → BIOS buffer */
int  pc_kbd_buffer_empty(x86_cpu *c);
int  pc_kbd_peek(x86_cpu *c, uint16_t *key);             /* ascii | scancode<<8 */
int  pc_kbd_get(x86_cpu *c, uint16_t *key);
void pc_kbd_wait(x86_cpu *c);                            /* block until a key is in the buffer */
void pc_kbd_idle_poll(x86_cpu *c);                       /* DOS-level "is a key ready" polls */
void pc_kbd_int16(x86_cpu *c, int vector);
void pc_kbd_int9(x86_cpu *c, int vector);                /* default INT 9: latched code → BIOS buffer */
void pc_kbd_push(x86_cpu *c, uint8_t ascii, uint8_t scancode);
int  pc_kbd_raw_pending(void);
int  pc_kbd_raw_next(uint8_t *code);                     /* next raw make/break code for port 60h */

#endif /* PC_H */
