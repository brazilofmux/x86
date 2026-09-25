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
#define PC_STUB_SEG     0xF100     /* native BIOS stubs: in the ROM area, but not the trap segment's base */
#define PC_STUB_INT1C   0x0000     /* INT 1Ch; EOI; IRET — INT 8's tail */
#define PC_STUB_INT9    0x0010     /* a booted machine's INT 9: IN 60h, translate (PC_TRAP_KBD), EOI */
#define PC_STUB_INT75   0x0020     /* IRQ 13, the coprocessor's: clear FERR# (port F0h), EOI both PICs, INT 2 */
#define PC_TRAP_KBD     0xF5       /* F000:00F5: the host's scancode translation, code in AL */
#define PC_STUB_VGAVARS 0x00F0     /* INT 10h AH=00's native half: its ROM variables (table, DAC, DAC length) */
#define PC_STUB_VGAPROG 0x0100     /*   the code (tools/vgabios.asm) */
#define PC_STUB_VGATAB  0x0400     /*   the modes' parameter tables, 64 bytes apart */
#define PC_STUB_DAC64   0x0800     /*   the EGA 64-colour DAC set */
#define PC_STUB_DAC256  0x0900     /*   mode 13h's 256 */
/* The ROM fonts, in the BIOS segment where INT 10h AX=1130h points (data:
 * only execution there traps). 8x8's first half also at the AT's FA6E. */
#define PC_FONT16_OFF   0xC000
#define PC_FONT14_OFF   0xD000
#define PC_FONT8_OFF    0xE000
#define PC_FONT8_AT_OFF 0xFA6E
#define PC_SYSCONF_OFF  0xE820     /* INT 15h AH=C0h's system configuration table */
extern const uint8_t pc_font8[256 * 8], pc_font14[256 * 14], pc_font16[256 * 16];
#define PC_HLE_DUMMY_IRET 0xFF53   /* where every unserved vector points, as on an AT (traps as vector 53h) */
#define PC_HLE_DPMI_ENTRY 0x00FD   /* offset in the HLE segment; the trap vector is eip & FFh */
#define PC_HLE_DPMI_RMRET 0x00FC   /* a real-mode excursion (INT 31h 0300-0302) has returned here */
#define PC_HLE_DPMI_CBRET 0x00FA   /* a real-mode callback's client procedure IRETed to here */
#define PC_HLE_DPMI_CB    0x00FB   /* real-mode callback n lives at PC_HLE_SEG:(n << 8 | FBh) */
#define PC_HLE_DPMI_EXCRET 0x00F9  /* a DPMI exception handler far-returned to here */
#define PC_HLE_DPMI_RAW   0x00F8   /* raw mode switch (INT 31h 0306), both directions */
#define PC_HLE_DPMI_SAVE  0x00F7   /* state save/restore (INT 31h 0305): nothing to save */

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
    uint8_t  kbc_cmdbyte;            /* 8042 command byte: bit 0 IRQ 1 enable, 2 system flag, 6 translate */
    uint8_t  kbd_disabled;           /* 8042 ADh (keyboard interface off), or the keyboard's F5 (scanning off) */
    uint8_t  kbd_cmd;                /* keyboard command awaiting its data byte (EDh LEDs, F3h rate, F0h set) */
    int      kbd_raw;                /* host terminal is in raw mode */
    uint64_t kbd_reads;              /* guest keyboard reads/polls; paces scripted input */
    uint64_t next_slow_ns;           /* pc_poll: when its periodic work is next due */
    uint64_t now_ns;                 /* clock read by the last pc_poll, for reuse */
    uint64_t blocked_ns;             /* host time spent deliberately idle (waiting on stdin, honouring a guest delay) */
    uint64_t blocked_calls;
    int booted;                      /* started from a disk image, not the HLE DOS */
    int boot_drive;                  /* ... this one (00h, 80h) */
    int reboot;                      /* a booted machine reset itself: POST and boot again */
    int      eof_seen;

    /* Timer */
    uint64_t t0_ns;                  /* wall clock at boot */
    int      vclock;                 /* the clock is the instruction counter (X86_GOLDEN: repeatable runs) */
    uint64_t ticks_delivered;        /* INT 8s raised so far */
    uint64_t next_tick_ns;           /* when IRQ 0 is next due, at PIT channel 0's rate */
    int      irq_pending;            /* bitmask: 1<<8 timer, 1<<9 keyboard */
    int      irq_in_service;         /* 8259 ISR: bits set from delivery until EOI */
    uint64_t irq_service_ns;         /* when the in-service IRQ was delivered (stuck-handler guard) */
    uint8_t  aux_full, aux_out;      /* 8042: a byte from the auxiliary device (the PS/2 mouse) in the output buffer */
    uint64_t rtc_next_ns;
    uint64_t ide_due;                /* the IDE drive is busy until this instruction count (UINT64_MAX: idle) */            /* when the RTC next has an interrupt to raise (pc_rtc_poll); ~0 for never */

    /* Services by vector; NULL = plain IRET stub. */
    pc_service_fn service[256];
    /* Protected-mode exceptions get first refusal here: a DPMI client's own
     * handler wants the frame DPMI specifies, not the one the CPU pushed.
     * Returns nonzero if it took the exception. */
    int (*pm_exception)(x86_cpu *c, int vector);
    uint8_t       ret_mode[256];
    uint8_t       ivt_service[256];  /* the vector points at its own stub (pc_set_service) */

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
void pc_request_reset(x86_cpu *c, const char *how);   /* CPU reset: a booted machine reboots */
void pc_reboot(x86_cpu *c);              /* after the run stopped for pc.reboot: POST, boot sector */
void pc_empty_upper_memory(x86_cpu *c);  /* booted machines: C0000-EFFFF reads as an empty bus */
void pc_irq_raise(int irq);          /* IRQ 8-15: a request to the slave 8259 */
void pc_irq_line(int irq, int level); /* IRQ 3-7: a device's line into the master 8259 (it takes the rising edge) */
void pc_irq_unmask(int irq);         /* as the BIOS opens a line it has a handler for */

/* pc_ps2.c: the PS/2 mouse on the 8042's auxiliary port */
void pc_ps2_post(x86_cpu *c);
void pc_ps2_write(uint8_t v);            /* 8042 D4h: a byte to the device */
int  pc_ps2_pending(void);               /* bytes from the device waiting for the output buffer */
uint8_t pc_ps2_take(void);
void pc_ps2_inject(uint8_t v);           /* 8042 D3h: a byte as if the device sent it */
void pc_ps2_motion(int fx, int fy);      /* the host's pointer, 640x480 frame */
void pc_ps2_button(int button, int down);
void pc_ps2_poll(uint64_t now);
int  pc_int15_ps2(x86_cpu *c);           /* INT 15h AH=C2h; 0 if not that */
void pc_native_irq_vectors(x86_cpu *c);  /* booted machines: INT 9 through the native stub */
void pc_kbd_trap(x86_cpu *c, int vector);   /* PC_TRAP_KBD: translate the scancode in AL */
void pc_set_trap(int offset, pc_service_fn fn, int ret_mode);
void pc_hle_return(x86_cpu *c, int mode); /* pop the INT frame per mode */
int  pc_poll(x86_cpu *c);                /* between blocks: keys, timer, IRQ delivery; 1 if cpu state changed */
uint64_t pc_now_ns(void);
uint64_t pc_wall_ns(void);            /* the host's clock, whatever pc_now_ns is */

/* pc_cmos.c: the real-time clock */
void pc_rtc_poll(uint64_t now);           /* run it to NOW: its flags, and IRQ 8 */
void pc_rtc_get(int *h, int *m, int *sec, int *year, int *mon, int *mday);
void pc_rtc_set_time(int h, int m, int sec);
void pc_rtc_set_date(int year, int mon, int mday);
void pc_rtc_pie(int on);                  /* the periodic interrupt (INT 15h AH=83h) */
void pc_rtc_alarm(int on, uint8_t h, uint8_t m, uint8_t sec);   /* INT 1Ah AH=06h/07h, BCD */
int  pc_rtc_alarm_on(void);
#define PC_TRAP_PS2     0xF3       /* F000:00F3: INT 74h's packet assembly, the byte from port 60h in AL */
#define PC_STUB_INT74   0x0050     /* IRQ 12: IN 60h, the host's assembly, the program's routine with the packet, EOIs */
#define PC_STUB_DISKBIOS 0x1000    /* INT 13h for the fixed disks, in real instructions (tools/diskbios.asm) */
#define PC_STUB_PCIBIOS 0x2000     /* F3000h: the BIOS32 directory and the 32-bit PCI BIOS (tools/pcibios.asm), with -pci */
#define PC_STUB_INT76   0x0098     /* IRQ 14: the hard-disk interrupt flag at 40:8Eh, EOIs */
#define PC_PS2_VARS     0x00C0     /*   its data: the routine (offset, segment), then status, X, Y as words */
#define PC_TRAP_RTC     0xF4       /* F000:00F4: INT 70h's bookkeeping (INT 15h AH=83h's wait), register C in AL */
#define PC_STUB_INT70   0x0030     /* IRQ 8: read register C, the host's bookkeeping, EOIs, INT 4Ah on an alarm */

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
void pc_kbd_busy(void);
void pc_kbd_raw_reply(uint8_t code);                     /* the keyboard answers a command byte */                                  /* the guest did work: not idle-polling */
void pc_video_flush(int force);                          /* tty mode painter */
void pc_video_shutdown(void);
void pc_video_set_hud(const char *text);
void pc_video_dump(x86_cpu *c, FILE *f);                 /* the text buffer as 25 lines of UTF-8 */
int  pc_video_png(x86_cpu *c, const char *path);         /* the screen: text, 13h or 16-colour; -1 if none */
int  pc_vga_port_read(uint16_t port, uint32_t *val);     /* 1 if the port is the VGA's */
int  pc_vga_port_write(uint16_t port, uint32_t val, int size);
void pc_vga_set_mode(x86_cpu *c, int mode);             /* INT 10h AH=00, after the BDA */

/* pc_kbd.c */
void pc_kbd_init(void);
void pc_kbd_shutdown(void);
void pc_svcprof_dump(FILE *out);                         /* X86_SVCPROF: host time per service */
void pc_kbd_poll(x86_cpu *c);                            /* host keys → BIOS buffer */
int  pc_kbd_buffer_empty(x86_cpu *c);
int  pc_kbd_peek(x86_cpu *c, uint16_t *key);             /* ascii | scancode<<8 */
int  pc_kbd_get(x86_cpu *c, uint16_t *key);
int  pc_kbd_wait(x86_cpu *c, int can_return);            /* a key in the buffer (1), or return to the guest first (0; can_return) */
void pc_kbd_idle_poll(x86_cpu *c);                       /* DOS-level "is a key ready" polls */
void pc_kbd_int16(x86_cpu *c, int vector);
void pc_kbd_int9(x86_cpu *c, int vector);                /* default INT 9: latched code → BIOS buffer */
void pc_kbd_push(x86_cpu *c, uint8_t ascii, uint8_t scancode);
int  pc_kbd_raw_pending(void);
int  pc_kbd_raw_next(uint8_t *code);                     /* next raw make/break code for port 60h */
void pc_kbd_raw_key(uint8_t code, uint8_t ascii);         /* queue one make/break code (the window's keyboard) */
void pc_vga_rom(x86_cpu *c);                               /* POST: the native mode-set code and its tables */
void pc_vga_rom_mode(x86_cpu *c, int mode);                /* point its ROM variables at MODE's table */
int  pc_vga_frame(x86_cpu *c, uint8_t *rgb, int *w, int *h);   /* RGB24, at most 640x480; -1 if not graphics */
int  pc_vga_text_frame(x86_cpu *c, uint8_t *rgb, int maxw, int maxh, int *w, int *h, unsigned frame);   /* -1 if not text */
void pc_vga_set_cursor_pos(uint16_t words);               /* CRTC 0E/0F, as the BIOS keeps them */
void pc_vga_set_start(uint16_t words);                    /* CRTC 0C/0D: the displayed page */
void pc_vga_set_cursor_shape(uint8_t start, uint8_t end); /* CRTC 0A/0B */
void pc_vga_set_char_height(int h);                       /* CRTC 09 */
uint8_t pc_vga_get_ac(int i);
void pc_vga_set_ac(int i, uint8_t v);
void pc_vga_get_dac(int i, uint8_t rgb[3]);
void pc_vga_set_dac(int i, const uint8_t rgb[3]);
void pc_vga_status_read(void);                            /* port 3DAh read: resets the 3C0h flip-flop */
void pc_sdl_allow(int on, const char *prog);              /* may a window open for graphics modes */
void pc_sdl_text(int on);                                 /* -w: the window shows text modes too */
int  pc_sdl_window_allowed(void);                         /* a window may open (so there is a mouse to have) */
/* pc_mouse.c: the INT 33h driver, present when a window is */
void pc_mouse_install(x86_cpu *c);
int  pc_disk_attach(int drive, const char *path, int readonly);   /* 00h/01h diskettes, 80h/81h fixed */
int  pc_disk_present(int drive);
int  pc_disk_swap(void);                                          /* ESC-+: next image of a diskette sequence */
int  pc_disk_boot(x86_cpu *c, int drive);                          /* sector 0 to 0000:7C00 */
void pc_disk_install(x86_cpu *c);
void pc_mouse_reboot(x86_cpu *c);
uint8_t *pc_disk_hd(int unit, size_t *size, int *cyls, int *heads, int *spt);   /* pc_ide.c's view */
/* pc_ide.c: the primary IDE channel */
/* pc_pci.c: a PCI bus (a host bridge, the BIOS32 PCI BIOS), when -pci asks for one */
void pc_pci_enable(void);
int  pc_pci_present(void);
void pc_pci_post(x86_cpu *c);
int  pc_pci_port_read(uint16_t port, int size, uint32_t *val);
int  pc_pci_port_write(uint16_t port, uint32_t val, int size);
/* pc_uart.c: COM1, a 16550A, when -com1 asks for one */
int  pc_uart_open(const char *spec);   /* "stdio", or a file for what is sent */
void pc_uart_post(x86_cpu *c);
int  pc_uart_present(void);
int  pc_uart_owns_stdin(void);
int  pc_uart_port_read(uint16_t port, int size, uint32_t *val);
int  pc_uart_port_write(uint16_t port, uint32_t val, int size);
void pc_uart_poll(void);
void pc_uart_shutdown(void);
void pc_ide_post(x86_cpu *c);
int  pc_ide_port_read(uint16_t port, int size, uint32_t *val);
int  pc_ide_port_write(uint16_t port, uint32_t val, int size);
void pc_ide_poll(int now);               /* the drive's busy time over (NOW: at once, the CPU is waiting) */
int  pc_disk_floppy_type(int drive);                              /* CMOS type 1-5, 0 none */
/* pc_cmos.c */
void pc_cmos_init(x86_cpu *c);                                    /* after the disks: POST's configuration */
int  pc_cmos_port_read(uint16_t port, uint32_t *val);
int  pc_cmos_port_write(uint16_t port, uint32_t val);
uint32_t pc_ext_kb(const x86_cpu *c);                             /* extended memory reported, KB */
int  pc_int15_memory(x86_cpu *c);                                 /* INT 15h 87h/88h/E801h/E820h */
void pc_kbd_mouse_reporting(void);                        /* -t: the terminal reports mouse events */
void pc_mouse_motion(int fx, int fy);                     /* pointer in a 640x480 frame */
void pc_mouse_button(int button, int down);               /* 0 left, 1 right, 2 middle */
int  pc_mouse_poll(x86_cpu *c);                           /* call a pending event handler; 1 if it did */
int  pc_mouse_text_cursor(int *row, int *col, uint16_t *screen_mask, uint16_t *cursor_mask);
void pc_sdl_poll(x86_cpu *c, uint64_t now_ns);            /* frames at 70 Hz, window events */
void pc_sdl_shutdown(void);

#endif /* PC_H */
