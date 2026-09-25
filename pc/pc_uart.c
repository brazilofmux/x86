/* pc_uart.c — a 16550A serial port: COM1 at 3F8h-3FFh, IRQ 4
 *
 * Present only when asked for (-com1): the machine the DOS and Windows
 * suites boot has no serial port, and keeps not having one. Its far end is
 * the host: "stdio" makes the terminal the serial console (the terminal's
 * keys arrive as received characters, what the guest sends is written to
 * stdout; the keyboard then takes its keys from the window only), and a
 * file name makes it a transmit-only log.
 *
 * The chip, as QEMU and VirtualBox model it — the machines the software
 * that uses it (Xinu's console, Linux's ttyS0) was written against:
 * - the divisor latch behind LCR bit 7 (DLAB), the scratch register;
 * - transmission completes at once: THRE and TEMT are always set, and a
 *   THR write raises the THRE interrupt again straight away, as does any
 *   IER write that enables it (bit 1) — how a driver "kicks" its output;
 * - a 16-byte receive FIFO when FCR enables it (IIR bits 7:6 = 11): at or
 *   above the trigger level the received-data interrupt (IIR 04h), below
 *   it, but not empty, the character timeout (0Ch) — at once, rather than
 *   four character times later;
 * - loopback (MCR bit 4): what is sent is received, and MSR reflects MCR
 *   (DTR→DSR, RTS→CTS, OUT1→RI, OUT2→DCD); otherwise the modem lines say
 *   a terminal is there (CTS, DSR, DCD);
 * - the interrupt goes to the 8259 whether or not MCR's OUT2 is set. On a
 *   PC's serial card OUT2 gates it; QEMU and VirtualBox do not, and Xinu,
 *   developed on the latter, never sets it.
 * The line into the PIC is level-shaped (pc_irq_line) and the 8259 takes
 * its rising edge, as it does from any ISA card; a change of cause while it
 * is up is a fresh edge (update).
 */
#include "pc.h"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

enum { IER_RDA = 1, IER_THRE = 2, IER_RLS = 4, IER_MS = 8 };
enum { LSR_DR = 0x01, LSR_THRE = 0x20, LSR_TEMT = 0x40 };
enum { MCR_DTR = 1, MCR_RTS = 2, MCR_OUT1 = 4, MCR_OUT2 = 8, MCR_LOOP = 0x10 };

#define RXQ 16

static struct {
    int present;
    int out_fd;                     /* where transmitted bytes go */
    int in_stdin;                   /* received bytes come from stdin */
    uint8_t ier, lcr, mcr, scr, fcr, dll, dlm;
    uint8_t rx[RXQ]; int rx_head, rx_n;
    int thr_ipending;               /* THRE interrupt raised and not yet taken */
    int line;                       /* the level last put on IRQ 4 */
    uint8_t last_id;                /* the cause it was for */
} u = { .out_fd = -1 };

static struct termios saved_tio;
static int raw_active;

int pc_uart_open(const char *spec) {
    if (!strcmp(spec, "stdio")) {
        u.out_fd = STDOUT_FILENO;
        u.in_stdin = 1;
        if (isatty(STDIN_FILENO)) {
            tcgetattr(STDIN_FILENO, &saved_tio);
            struct termios t = saved_tio;
            t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);   /* the guest echoes; Ctrl-C is the guest's */
            t.c_iflag &= ~(IXON | ICRNL | INLCR);
            t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
            raw_active = 1;
        }
    } else {
        u.out_fd = open(spec, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (u.out_fd < 0) { perror(spec); return -1; }
    }
    u.present = 1;
    return 0;
}

void pc_uart_shutdown(void) {
    if (raw_active) { tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio); raw_active = 0; }
}

int pc_uart_owns_stdin(void) { return u.in_stdin; }

static int trigger(void) {
    static const int lv[4] = { 1, 4, 8, 14 };
    return (u.fcr & 1) ? lv[u.fcr >> 6] : 1;
}

/* the interrupt the chip has to offer now, in its priority order; 1 = none */
static uint8_t iir_id(void) {
    if ((u.ier & IER_RDA) && u.rx_n) return (u.fcr & 1) && u.rx_n < trigger() ? 0x0C : 0x04;
    if ((u.ier & IER_THRE) && u.thr_ipending) return 0x02;
    return 0x01;
}

/* The line into the 8259, which takes rising edges. When the cause the
 * chip reports changes while the line stays up (received data read, the
 * transmitter's turn next), it drops and rises again: a handler that
 * takes one cause per interrupt and returns (Xinu's) would otherwise
 * leave the next one with no edge to be seen by, and the output it just
 * queued would never go. */
static void update(void) {
    uint8_t id = iir_id();
    int level = id != 0x01;
    if (level && u.line && id != u.last_id) { pc_irq_line(4, 0); pc_irq_line(4, 1); }
    else if (level != u.line) { u.line = level; pc_irq_line(4, level); }
    u.last_id = id;
}

static void rx_put(uint8_t b) {
    if (u.rx_n == ((u.fcr & 1) ? RXQ : 1)) return;      /* overrun: the byte is lost (no LSR OE) */
    u.rx[(u.rx_head + u.rx_n) % RXQ] = b;
    u.rx_n++;
}

static void transmit(uint8_t b) {
    if (u.mcr & MCR_LOOP) rx_put(b);
    else if (u.out_fd >= 0) { ssize_t w = write(u.out_fd, &b, 1); (void)w; }
    u.thr_ipending = 1;                                  /* sent at once: the holding register is empty again */
}

/* POST: the registers at reset; the BIOS data area's COM1 address */
void pc_uart_post(x86_cpu *c) {
    if (!u.present) return;
    u.ier = 0; u.lcr = 0; u.mcr = 0; u.scr = 0; u.fcr = 0; u.dll = 0x0C; u.dlm = 0;   /* 9600 */
    u.rx_head = u.rx_n = 0; u.thr_ipending = 0;
    if (u.line) { u.line = 0; pc_irq_line(4, 0); }
    pc_wr16(c, PC_BDA_SEG, 0x00, 0x3F8);
}
int pc_uart_present(void) { return u.present; }

int pc_uart_port_read(uint16_t port, int size, uint32_t *val) {
    (void)size;
    if (!u.present || port < 0x3F8 || port > 0x3FF) return 0;
    uint8_t v = 0xFF;
    switch (port - 0x3F8) {
    case 0:
        if (u.lcr & 0x80) { v = u.dll; break; }
        v = 0;
        if (u.rx_n) { v = u.rx[u.rx_head]; u.rx_head = (u.rx_head + 1) % RXQ; u.rx_n--; }
        break;
    case 1: v = (u.lcr & 0x80) ? u.dlm : u.ier; break;
    case 2:
        v = (uint8_t)(iir_id() | ((u.fcr & 1) ? 0xC0 : 0));
        if ((v & 0x0F) == 0x02) u.thr_ipending = 0;      /* reading the THRE identification takes it */
        break;
    case 3: v = u.lcr; break;
    case 4: v = u.mcr; break;
    case 5: v = (uint8_t)(LSR_THRE | LSR_TEMT | (u.rx_n ? LSR_DR : 0)); break;
    case 6:
        if (u.mcr & MCR_LOOP)
            v = (uint8_t)(((u.mcr & MCR_DTR) ? 0x20 : 0) | ((u.mcr & MCR_RTS) ? 0x10 : 0)
                        | ((u.mcr & MCR_OUT1) ? 0x40 : 0) | ((u.mcr & MCR_OUT2) ? 0x80 : 0));
        else v = 0xB0;                                   /* DCD, DSR, CTS: a terminal is there */
        break;
    case 7: v = u.scr; break;
    }
    *val = v;
    update();
    return 1;
}

int pc_uart_port_write(uint16_t port, uint32_t val, int size) {
    (void)size;
    if (!u.present || port < 0x3F8 || port > 0x3FF) return 0;
    uint8_t v = (uint8_t)val;
    switch (port - 0x3F8) {
    case 0:
        if (u.lcr & 0x80) { u.dll = v; break; }
        u.thr_ipending = 0;
        transmit(v);
        break;
    case 1:
        if (u.lcr & 0x80) { u.dlm = v; break; }
        /* THRE enabled while the holding register is empty — newly or
         * again — is an interrupt now: drivers start their output this way
         * (Xinu's ttykickout rewrites IER with the bit already set, and
         * QEMU's and VirtualBox's 16550s answer it) */
        if (v & IER_THRE) u.thr_ipending = 1;
        u.ier = v & 0x0F;
        break;
    case 2:
        if ((v ^ u.fcr) & 1) { u.rx_head = u.rx_n = 0; }  /* enabling or disabling the FIFO empties it */
        if (v & 2) u.rx_head = u.rx_n = 0;                /* clear the receive FIFO */
        u.fcr = v & 0xC9;
        break;
    case 3: u.lcr = v; break;
    case 4: u.mcr = v & 0x1F; break;
    case 7: u.scr = v; break;
    default: break;                                      /* LSR, MSR: read-only */
    }
    update();
    return 1;
}

/* From pc_poll: what the far end has sent, into the receive FIFO. */
void pc_uart_poll(void) {
    if (!u.present || !u.in_stdin || (u.mcr & MCR_LOOP)) return;
    int room = ((u.fcr & 1) ? RXQ : 1) - u.rx_n;
    if (room <= 0) return;
    struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return;
    uint8_t b[RXQ];
    ssize_t n = read(STDIN_FILENO, b, (size_t)room);
    for (ssize_t i = 0; i < n; i++) rx_put(b[i] == '\n' && !isatty(STDIN_FILENO) ? '\r' : b[i]);   /* a script's lines end as a terminal's Enter does */
    update();
}
