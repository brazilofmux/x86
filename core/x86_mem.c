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
 * it guards — and it sits X86_BM_DELTA from the memory in the same
 * reservation, so the check is also `[host_addr + X86_BM_DELTA]`.
 *
 * Both regions carry X86_MEM_SLACK readable bytes past the end so a
 * decode at the top never faults. On POSIX hosts the bitmap is below the
 * memory, and the memory's slack is read-only and FFh — what the
 * interpreter reads past mem_size — with nothing mapped after it up to
 * X86_FLAT_SPAN (x86.h), so a flat access past the memory faults.
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

static uint32_t next_size = X86_MEM_SIZE;
void x86_set_mem_size(uint32_t bytes) {
    if (bytes < X86_MEM_SIZE) bytes = X86_MEM_SIZE;
    if (bytes > X86_MEM_MAX) bytes = X86_MEM_MAX;
#if defined(_WIN32)
    next_size = bytes & ~0xFFFFu;             /* view granularity */
#else
    next_size = bytes & ~0xFFFu;
#endif
}

#if defined(_WIN32)
/* Windows: the backing object is a pagefile-backed section, the
 * reservation one placeholder (VirtualAlloc2, Windows 10 1803 and later)
 * carved into the pieces map_region lays out, each replaced by a view of
 * the section (MapViewOfFile3) or by committed slack. The section handle
 * rides in mem_fd: 64-bit Windows handles fit in 32 bits, and code that
 * copies an x86_cpu around already carries mem_fd across. Views are 64 KB
 * granular — every boundary below is a multiple of it (x86_set_mem_size
 * rounds the size so). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER  0x00040000
#define MEM_REPLACE_PLACEHOLDER  0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
typedef PVOID (WINAPI *valloc2_fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef PVOID (WINAPI *mapview3_fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef BOOL  (WINAPI *unmapview2_fn)(HANDLE, PVOID, ULONG);
static valloc2_fn    p_VirtualAlloc2;
static mapview3_fn   p_MapViewOfFile3;
static unmapview2_fn p_UnmapViewOfFile2;
static uint8_t *resv_end;                 /* the reservation's end: the last piece needs no split */

#define SECTION(fd) ((HANDLE)(intptr_t)(fd))

static int win_api(void) {
    HMODULE k = GetModuleHandleA("kernelbase.dll");
    if (!k) return -1;
    p_VirtualAlloc2    = (valloc2_fn)(void *)GetProcAddress(k, "VirtualAlloc2");
    p_MapViewOfFile3   = (mapview3_fn)(void *)GetProcAddress(k, "MapViewOfFile3");
    p_UnmapViewOfFile2 = (unmapview2_fn)(void *)GetProcAddress(k, "UnmapViewOfFile2");
    return p_VirtualAlloc2 && p_MapViewOfFile3 && p_UnmapViewOfFile2 ? 0 : -1;
}

/* Split [p, p+len) off the front of the placeholder that starts at p. */
static int carve(uint8_t *p, size_t len) {
    if (p + len >= resv_end) return 0;
    return VirtualFree(p, len, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ? 0 : -1;
}

/* Map [off, off+len) of the section at addr, over whatever view is there. */
static int map_fixed(int fd, uint8_t *addr, size_t len, uint64_t off) {
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery(addr, &mi, sizeof mi) && mi.Type == MEM_MAPPED &&
        !p_UnmapViewOfFile2(GetCurrentProcess(), addr, MEM_PRESERVE_PLACEHOLDER)) return -1;
    void *p = p_MapViewOfFile3(SECTION(fd), GetCurrentProcess(), addr, off, len,
                               MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    return p == (void *)addr ? 0 : -1;
}

static int map_region(x86_cpu *c, uint8_t *base, uint64_t obj_off, int a20_on) {
    size_t ext = c->mem_size - X86_LOW_SIZE;
    if (carve(base, HMA_OFF) < 0 || map_fixed(c->mem_fd, base, HMA_OFF, obj_off) < 0) return -1;
    if (carve(base + HMA_OFF, HMA_SIZE) < 0 ||
        map_fixed(c->mem_fd, base + HMA_OFF, HMA_SIZE, obj_off + (a20_on ? HMA_OFF : 0)) < 0) return -1;
    if (ext && (carve(base + X86_LOW_SIZE, ext) < 0 ||
                map_fixed(c->mem_fd, base + X86_LOW_SIZE, ext, obj_off + X86_LOW_SIZE) < 0)) return -1;
    uint8_t *slack = base + c->mem_size;
    if (carve(slack, X86_MEM_SLACK) < 0) return -1;
    void *s = p_VirtualAlloc2(GetCurrentProcess(), slack, X86_MEM_SLACK,
                              MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    return s == (void *)slack ? 0 : -1;
}

static int alloc_mirrored(x86_cpu *c) {
    if (win_api() < 0) return -1;
    uint64_t obj = (uint64_t)c->mem_size * 2;
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT,
                                  (DWORD)(obj >> 32), (DWORD)obj, NULL);
    if (!h) return -1;
    c->mem_fd = (int)(intptr_t)h;

    /* one reservation: memory at 0, the bitmap at X86_BM_DELTA */
    size_t region = (size_t)c->mem_size + X86_MEM_SLACK, total = X86_BM_DELTA + region;
    c->mem = p_VirtualAlloc2(GetCurrentProcess(), NULL, total, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                             PAGE_NOACCESS, NULL, 0);
    if (!c->mem) return -1;
    resv_end = c->mem + total;
    c->code_bitmap = c->mem + X86_BM_DELTA;
    if (map_region(c, c->mem, 0, 0) < 0) return -1;
    if (carve(c->mem + region, X86_BM_DELTA - region) < 0) return -1;     /* the gap stays a placeholder */
    if (map_region(c, c->code_bitmap, c->mem_size, 0) < 0) return -1;
    c->mem_mirrored = 1;
    return 0;
}

/* Every view, the slack and the placeholders left, then the section. */
static void free_mirrored(x86_cpu *c) {
    if (c->mem) {
        uint8_t *a = c->mem, *end = c->mem + X86_BM_DELTA + (size_t)c->mem_size + X86_MEM_SLACK;
        MEMORY_BASIC_INFORMATION mi;
        while (a < end && VirtualQuery(a, &mi, sizeof mi)) {
            if (mi.State == MEM_FREE) { a = (uint8_t *)mi.BaseAddress + mi.RegionSize; continue; }
            if (!(mi.Type == MEM_MAPPED ? UnmapViewOfFile(a) : VirtualFree(a, 0, MEM_RELEASE))) break;
        }
    }
    if (c->mem_fd >= 0) CloseHandle(SECTION(c->mem_fd));
}
#else
/* Map [off, off+len) of the backing object at addr (MAP_FIXED). */
static int map_fixed(int fd, uint8_t *addr, size_t len, off_t off) {
    void *p = mmap(addr, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, off);
    return p == (void *)addr ? 0 : -1;
}

static uint8_t *reserve(size_t len) {
    void *p = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
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

/* The memory's slack, read-only and FFh (open bus): a flat read that runs
 * past mem_size sees what the interpreter would, and a write faults. */
static int seal_slack(uint8_t *slack) {
    memset(slack, 0xFF, X86_MEM_SLACK);
    return mprotect(slack, X86_MEM_SLACK, PROT_READ);
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

    /* one reservation: the bitmap at its start, the memory X86_BM_DELTA
     * after it, and PROT_NONE on to the memory's X86_FLAT_SPAN */
    size_t below = (size_t)-X86_BM_DELTA;
    uint8_t *r = reserve(below + X86_FLAT_SPAN);
    if (!r) return -1;
    c->code_bitmap = r;
    c->mem = r + below;
    if (map_region(c, c->mem, 0, 0) < 0) return -1;
    if (map_region(c, c->code_bitmap, (off_t)c->mem_size, 0) < 0) return -1;
    if (seal_slack(c->mem + c->mem_size) < 0) return -1;
    c->mem_mirrored = 1;
    return 0;
}

static void free_mirrored(x86_cpu *c) {
    if (c->mem) munmap(c->mem + X86_BM_DELTA, (size_t)-X86_BM_DELTA + X86_FLAT_SPAN);   /* the bitmap lives inside the same reservation */
    if (c->mem_fd >= 0) close(c->mem_fd);
}
#endif

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
    if (c->mem_mirrored || c->mem_fd >= 0) {
        free_mirrored(c);
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
                      (uint64_t)c->mem_size + (on ? HMA_OFF : 0)) < 0) return -1;
    }
    c->a20_mask = mask;
    x86_tlb_flush(c);                         /* cached translations hold A20-masked addresses */
    if (c->a20_hook) c->a20_hook(c, on);
    return 0;
}
