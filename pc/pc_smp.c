/* pc_smp.c — the processors as host threads.
 *
 * The bootstrap processor runs on the main thread as it always has; each
 * application processor is a thread of its own with its own x86_cpu and
 * translator, over the one guest memory. They share the machine through
 * one lock (QEMU's big-lock model): port I/O, memory-mapped devices, the
 * BIOS's host services and the device half of pc_poll run under it, and
 * guest memory accesses do not. An application processor starts life
 * waiting for its STARTUP IPI; INIT sends it back there. A halted
 * processor sleeps on its own condition variable until something raises
 * a request for it — an IPI, a device the I/O APIC routes to it, its own
 * APIC timer, whose due time bounds the sleep. */
#include "pc.h"
#include <pthread.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t big;
static pthread_cond_t wake[PC_MAX_CPUS];
static int ready, exiting;
static struct { int sipi_vector, init_pending, running; } ap[PC_MAX_CPUS];

void pc_smp_setup(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&big, &a);
    for (int i = 0; i < PC_MAX_CPUS; i++) pthread_cond_init(&wake[i], NULL);
    memset(ap, 0, sizeof ap);
    ap[0].running = 1;
    ready = 1;
}
void pc_lock(void)   { if (ready) pthread_mutex_lock(&big); }
void pc_unlock(void) { if (ready) pthread_mutex_unlock(&big); }
int  pc_smp_exiting(void) { return exiting; }
int  pc_smp_running(int cpu) { return cpu >= 0 && cpu < PC_MAX_CPUS && ap[cpu].running; }

/* Something was raised for processor CPU (the caller holds the lock). */
void pc_smp_wake(int cpu) {
    if (ready && cpu >= 0 && cpu < PC_MAX_CPUS) pthread_cond_signal(&wake[cpu]);
}

/* Sleep, the lock held once, until woken or UNTIL_NS (pc_now_ns time;
 * UINT64_MAX: nothing due) — and no longer than 10 ms, so a stop or an
 * exit is seen. */
void pc_smp_wait(x86_cpu *c, uint64_t until_ns) {
    if (!ready) return;
    uint64_t now = pc_now_ns();
    uint64_t ns = until_ns > now ? until_ns - now : 0;
    if (until_ns != UINT64_MAX && ns == 0) return;
    if (ns > 10000000ull) ns = 10000000ull;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)(ns % 1000000000ull);
    ts.tv_sec += (time_t)(ns / 1000000000ull) + ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    uint64_t w0 = pc_wall_ns();
    pthread_cond_timedwait(&wake[c->cpu_id], &big, &ts);
    if (c->cpu_id == 0) { pc.blocked_ns += pc_wall_ns() - w0; pc.blocked_calls++; }
}

/* An application processor's thread, between runs: wait for a STARTUP
 * IPI; its vector, or -1 when the machine is shutting down. */
int pc_smp_wait_sipi(x86_cpu *c) {
    int i = c->cpu_id, v;
    pthread_mutex_lock(&big);
    ap[i].running = 0;
    while (!exiting && !ap[i].sipi_vector) pthread_cond_wait(&wake[i], &big);
    v = exiting ? -1 : ap[i].sipi_vector - 1;
    ap[i].sipi_vector = 0;
    ap[i].init_pending = 0;
    if (v >= 0) ap[i].running = 1;
    pthread_mutex_unlock(&big);
    return v;
}
/* From the ICR, the lock held: a STARTUP IPI starts a waiting processor
 * at VECTOR:0000; one that is running ignores it, as the silicon does. */
void pc_smp_sipi(int cpu, int vector) {
    if (cpu <= 0 || cpu >= pc.ncpus || ap[cpu].running) return;
    ap[cpu].sipi_vector = vector + 1;
    pthread_cond_signal(&wake[cpu]);
}
/* INIT: a running processor goes back to waiting for STARTUP, at its
 * next boundary (pc_smp_take_init, from its poll). */
void pc_smp_init_ipi(int cpu) {
    if (cpu <= 0 || cpu >= pc.ncpus || !ap[cpu].running) return;
    ap[cpu].init_pending = 1;
    pc.cpus[cpu]->jit_cur_hit = 1;
    pthread_cond_signal(&wake[cpu]);
}
int pc_smp_take_init(x86_cpu *c) {
    if (!ap[c->cpu_id].init_pending) return 0;
    ap[c->cpu_id].init_pending = 0;
    return 1;
}
/* Shutting down: every waiting or sleeping thread returns. */
void pc_smp_exit(void) {
    if (!ready) return;
    pthread_mutex_lock(&big);
    exiting = 1;
    for (int i = 0; i < PC_MAX_CPUS; i++) pthread_cond_broadcast(&wake[i]);
    pthread_mutex_unlock(&big);
}
