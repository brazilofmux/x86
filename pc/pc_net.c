/* pc_net.c — the far end of the network card's cable
 *
 * -nic e1000,user: libslirp, the user-mode network QEMU uses. The guest is
 * on 10.0.2.0/24 as 10.0.2.15 (slirp's DHCP server hands that out), the
 * gateway is 10.0.2.2 and the DNS server 10.0.2.3, both slirp's; TCP and
 * UDP out of it become the host's own sockets — NAT, with no privileges
 * and nothing to configure. ICMP echo goes out too where the host allows
 * unprivileged ping sockets (macOS does).
 *
 * Built only when pkg-config finds libslirp (as the window is only with
 * SDL2); without it, the card is there with its cable unplugged from
 * anything: what it sends is dropped.
 *
 * Frames to the guest can come at any time — from slirp_input itself (an
 * ARP reply, a DHCP answer) while the card is in the middle of sending —
 * so they wait in a queue here until the card has receive descriptors
 * for them (pc_e1000.c takes them with pc_net_rx_peek/pc_net_rx_pop).
 */
#include "pc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RXQ 256

static struct {
    int present;
    struct { uint8_t *p; uint32_t n; } q[RXQ];
    int head, n;
    uint64_t dropped;
} net;

int pc_net_present(void) { return net.present; }

static void rx_queue(const void *buf, size_t len) {
    if (net.n == RXQ || len > 16384) { net.dropped++; return; }   /* nobody is taking them: the wire drops */
    int t = (net.head + net.n) % RXQ;
    net.q[t].p = malloc(len);
    if (!net.q[t].p) return;
    memcpy(net.q[t].p, buf, len);
    net.q[t].n = (uint32_t)len;
    net.n++;
}

const uint8_t *pc_net_rx_peek(uint32_t *len) {
    if (!net.n) return NULL;
    *len = net.q[net.head].n;
    return net.q[net.head].p;
}
void pc_net_rx_pop(void) {
    if (!net.n) return;
    free(net.q[net.head].p);
    net.head = (net.head + 1) % RXQ;
    net.n--;
}

#ifdef HAVE_SLIRP
#include <libslirp.h>
#include <arpa/inet.h>
#include <poll.h>

static Slirp *slirp;

/* slirp's timers: a handful (TCP's fast and slow, the RA timer), checked
 * at each poll */
#define TIMERS 16
static struct tmr { SlirpTimerCb cb; void *opaque; int64_t expire_ms; int used; } timers[TIMERS];

static slirp_ssize_t send_packet(const void *buf, size_t len, void *opaque) {
    (void)opaque;
    rx_queue(buf, len);
    return (slirp_ssize_t)len;
}
static void guest_error(const char *msg, void *opaque) { (void)opaque; if (pc.debug) fprintf(stderr, "[net] %s\n", msg); }
static int64_t clock_get_ns(void *opaque) { (void)opaque; return (int64_t)pc_wall_ns(); }
static void *timer_new(SlirpTimerCb cb, void *cb_opaque, void *opaque) {
    (void)opaque;
    for (int i = 0; i < TIMERS; i++)
        if (!timers[i].used) { timers[i] = (struct tmr){ cb, cb_opaque, -1, 1 }; return &timers[i]; }
    fprintf(stderr, "[net] out of timers\n");
    abort();
}
static void timer_free(void *t, void *opaque) { (void)opaque; ((struct tmr *)t)->used = 0; }
static void timer_mod(void *t, int64_t expire_ms, void *opaque) { (void)opaque; ((struct tmr *)t)->expire_ms = expire_ms; }
#if !SLIRP_CHECK_VERSION(4, 8, 0)                        /* before 4.8: descriptors are ints */
typedef int slirp_os_socket;
#endif
static void no_socket(slirp_os_socket s, void *opaque) { (void)s; (void)opaque; }
static void notify(void *opaque) { (void)opaque; }

#define FDS 256
static struct pollfd fds[FDS];
static int nfds;

static int to_poll(int ev) {
    return (ev & SLIRP_POLL_IN ? POLLIN : 0) | (ev & SLIRP_POLL_OUT ? POLLOUT : 0) | (ev & SLIRP_POLL_PRI ? POLLPRI : 0);
}
static int add_poll(slirp_os_socket fd, int events, void *opaque) {
    (void)opaque;
    if (nfds == FDS) return -1;
    fds[nfds] = (struct pollfd){ fd, (short)to_poll(events), 0 };
    return nfds++;
}
static int get_revents(int idx, void *opaque) {
    (void)opaque;
    if (idx < 0 || idx >= nfds) return 0;
    int r = fds[idx].revents;
    return (r & POLLIN ? SLIRP_POLL_IN : 0) | (r & POLLOUT ? SLIRP_POLL_OUT : 0) | (r & POLLPRI ? SLIRP_POLL_PRI : 0)
         | (r & POLLERR ? SLIRP_POLL_ERR : 0) | (r & POLLHUP ? SLIRP_POLL_HUP : 0);
}

int pc_net_open(const char *spec) {
    if (strcmp(spec, "user")) { fprintf(stderr, "-nic: the network behind the card is 'user' (slirp)\n"); return -1; }
    static const SlirpCb cb = {
        .send_packet = send_packet, .guest_error = guest_error, .clock_get_ns = clock_get_ns,
        .timer_new = timer_new, .timer_free = timer_free, .timer_mod = timer_mod,
        .notify = notify,
        /* before config version 6, libslirp calls the old pair, and
         * unchecked: both are given (this layer polls everything anyway) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        .register_poll_fd = no_socket, .unregister_poll_fd = no_socket,
#pragma GCC diagnostic pop
#if SLIRP_CHECK_VERSION(4, 8, 0)
        .register_poll_socket = no_socket, .unregister_poll_socket = no_socket,
#endif
    };
    SlirpConfig cfg = { 0 };
    cfg.version = 1;
    cfg.in_enabled = true;
    inet_aton("10.0.2.0", &cfg.vnetwork);
    inet_aton("255.255.255.0", &cfg.vnetmask);
    inet_aton("10.0.2.2", &cfg.vhost);
    inet_aton("10.0.2.15", &cfg.vdhcp_start);
    inet_aton("10.0.2.3", &cfg.vnameserver);
    cfg.vhostname = "dos-monster";
    slirp = slirp_new(&cfg, &cb, NULL);
    if (!slirp) { fprintf(stderr, "-nic: slirp_new failed\n"); return -1; }
    net.present = 1;
    return 0;
}

void pc_net_send(const uint8_t *frame, uint32_t len) {
    if (slirp) slirp_input(slirp, frame, (int)len);
}

/* From pc_poll (and the HLT nap): the host's sockets and slirp's timers,
 * at most every half millisecond — each look is a poll() system call.
 * wait_ms > 0 sleeps in it, for a guest that is idle anyway. */
void pc_net_poll(int wait_ms) {
    if (!slirp) return;
    static uint64_t last;
    uint64_t now = pc_wall_ns();
    if (!wait_ms && now - last < 500000) return;
    last = now;
    uint32_t timeout = wait_ms > 0 ? (uint32_t)wait_ms : 0;
    nfds = 0;
#if SLIRP_CHECK_VERSION(4, 8, 0)
    slirp_pollfds_fill_socket(slirp, &timeout, add_poll, NULL);
#else
    slirp_pollfds_fill(slirp, &timeout, add_poll, NULL);
#endif
    int r = poll(fds, (nfds_t)nfds, wait_ms > 0 ? (int)timeout : 0);
    slirp_pollfds_poll(slirp, r < 0, get_revents, NULL);
    int64_t ms = (int64_t)(pc_wall_ns() / 1000000);
    for (int i = 0; i < TIMERS; i++)
        if (timers[i].used && timers[i].expire_ms >= 0 && timers[i].expire_ms <= ms) {
            timers[i].expire_ms = -1;
            timers[i].cb(timers[i].opaque);
        }
}

#else

int pc_net_open(const char *spec) {
    (void)spec;
    fprintf(stderr, "-nic: this build has no network behind the card (libslirp was not found)\n");
    return -1;
}
void pc_net_send(const uint8_t *frame, uint32_t len) { (void)frame; (void)len; (void)rx_queue; }
void pc_net_poll(int wait_ms) { (void)wait_ms; }

#endif
