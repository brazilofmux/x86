/* x86_mem.c — guest memory with an A20-aware mirror above 1 MB
 *
 * Guest physical memory is 1 MB plus the 64 KB HMA (X86_LOW_SIZE), and
 * then extended memory above that (cpu->mem_size in all: X86_MEM_SIZE
 * unless x86_set_mem_size asked for more). The
 * interpreter masks every address with a20_mask, so with A20 gated off
 * FFFF:0010 reads byte 0. Translated code cannot afford that mask on
 * every access, so the 64 KB window at 0x100000 is a second MAPPING of
 * the first 64 KB while A20 is off, and a mapping of the real HMA pages
 * once it is on: a host pointer `mem + seg_base + off` is then correct
 * in both states without arithmetic. Same trick as z80_mem_alloc, one
 * shared-memory object mapped twice.
 *
 * The code bitmap (one byte per guest byte, nonzero while a translated
 * block covers it) is laid out identically, so the JIT's post-store
 * check `bitmap[host_addr - mem]` sees the same aliasing as the memory
 * it guards — and it sits X86_BM_DELTA above the memory in the same
 * reservation, so the check is also `[host_addr + X86_BM_DELTA]`.
 *
 * Both regions carry X86_MEM_SLACK readable bytes past the end so a
 * decode or a straddling access at 0x10FFEF never faults.
 */
#include "x86.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define HMA_OFF   0x100000u
#define HMA_SIZE  0x10000u

/* Map [off, off+len) of the backing object at addr (MAP_FIXED). */
static int map_fixed(int fd, uint8_t *addr, size_t len, off_t off) {
    void *p = mmap(addr, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, off);
    return p == (void *)addr ? 0 : -1;
}

static uint8_t *reserve(size_t len) {
    void *p = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static uint32_t next_size = X86_MEM_SIZE;
void x86_set_mem_size(uint32_t bytes) {
    if (bytes < X86_MEM_SIZE) bytes = X86_MEM_SIZE;
    if (bytes > X86_MEM_MAX) bytes = X86_MEM_MAX;
    next_size = bytes & ~0xFFFu;
}

/* Region layout in the backing object: guest memory at 0, bitmap at
 * mem_size. Each mapped region is mem_size + X86_MEM_SLACK. */
static int map_region(x86_cpu *c, uint8_t *base, off_t obj_off, int a20_on) {
    if (map_fixed(c->mem_fd, base, HMA_OFF, obj_off) < 0) return -1;
    if (map_fixed(c->mem_fd, base + HMA_OFF, HMA_SIZE, obj_off + (a20_on ? HMA_OFF : 0)) < 0) return -1;
    /* Extended memory is never aliased — only the HMA window moves with A20. */
    if (map_fixed(c->mem_fd, base + X86_LOW_SIZE, c->mem_size - X86_LOW_SIZE, obj_off + X86_LOW_SIZE) < 0) return -1;
    void *s = mmap(base + c->mem_size, X86_MEM_SLACK, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return s == (void *)(base + c->mem_size) ? 0 : -1;
}

static int alloc_mirrored(x86_cpu *c) {
    int fd = -1;
#if defined(__linux__)
    fd = memfd_create("x86-guest-mem", MFD_CLOEXEC);
#else
    char name[64];
    snprintf(name, sizeof name, "/x86mem-%ld-%p", (long)getpid(), (void *)c);
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) shm_unlink(name);
#endif
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)c->mem_size * 2) != 0) { close(fd); return -1; }
    c->mem_fd = fd;

    /* one reservation: memory at 0, the bitmap at X86_BM_DELTA */
    size_t region = (size_t)c->mem_size + X86_MEM_SLACK;
    c->mem = reserve(X86_BM_DELTA + region);
    if (!c->mem) return -1;
    c->code_bitmap = c->mem + X86_BM_DELTA;
    if (map_region(c, c->mem, 0, 0) < 0) return -1;
    if (map_region(c, c->code_bitmap, (off_t)c->mem_size, 0) < 0) return -1;
    c->mem_mirrored = 1;
    return 0;
}

int x86_mem_alloc(x86_cpu *c) {
    c->mem_size = next_size;
    c->mem_fd = -1;
    c->a20_mask = 0xFFFFF;
    if (alloc_mirrored(c) == 0) return 0;

    /* Plain fallback: the interpreter still works (it masks), the JIT
     * refuses to start (dbt_jit_available checks mem_mirrored). */
    x86_mem_free(c);
    c->mem = calloc((size_t)c->mem_size + X86_MEM_SLACK, 1);
    c->code_bitmap = calloc((size_t)c->mem_size + X86_MEM_SLACK, 1);
    c->mem_mirrored = 0;
    return (c->mem && c->code_bitmap) ? 0 : -1;
}

void x86_mem_free(x86_cpu *c) {
    size_t region = (size_t)c->mem_size + X86_MEM_SLACK;
    if (c->mem_mirrored || c->mem_fd >= 0) {
        if (c->mem) munmap(c->mem, X86_BM_DELTA + region);   /* the bitmap lives inside the same reservation */
        if (c->mem_fd >= 0) close(c->mem_fd);
    } else {
        free(c->mem);
        free(c->code_bitmap);
    }
    c->mem = NULL;
    c->code_bitmap = NULL;
    c->mem_fd = -1;
    c->mem_mirrored = 0;
}

/* Gate A20. Remaps the HMA window, then tells the DBT (block keys are
 * A20-masked linear addresses and translated far transfers bake the
 * mask in, so its cache must go). */
int x86_set_a20(x86_cpu *c, int on) {
    uint32_t mask = on ? 0xFFFFFFFFu : 0xFFFFFu;
    if (mask == c->a20_mask) return 0;
    if (c->mem_mirrored) {
        if (map_fixed(c->mem_fd, c->mem + HMA_OFF, HMA_SIZE, on ? HMA_OFF : 0) < 0) return -1;
        if (map_fixed(c->mem_fd, c->code_bitmap + HMA_OFF, HMA_SIZE,
                      (off_t)c->mem_size + (on ? HMA_OFF : 0)) < 0) return -1;
    }
    c->a20_mask = mask;
    x86_tlb_flush(c);                         /* cached translations hold A20-masked addresses */
    if (c->a20_hook) c->a20_hook(c, on);
    return 0;
}
