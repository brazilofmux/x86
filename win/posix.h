/* win/posix.h — the POSIX subset dos-monster uses, over Win32 and the
 * UCRT, for an MSVC build. Force-included into every translation unit
 * (/FI); the stub headers beside it (unistd.h, sys/mman.h, ...) exist so
 * the sources' #includes resolve and only point back here.
 *
 * Every file is binary (link with binmode.obj): DOS files, disk images
 * and the terminal stream are bytes, never text. */
#ifndef WIN_POSIX_H
#define WIN_POSIX_H
#ifdef _WIN32

#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_NONSTDC_NO_DEPRECATE 1
#define _CRT_DECLARE_NONSTDC_NAMES 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <process.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <setjmp.h>
#include <intrin.h>
#undef signbit                    /* C99's, via intrin.h; the interpreter has its own */

/* ---- types ---- */
typedef intptr_t ssize_t;
typedef unsigned int useconds_t;
typedef int mode_t;

/* ---- the compiler ---- */
#define __attribute__(x)
#define __builtin_expect(e, v) (e)
static __inline int __builtin_ctz(unsigned x) { unsigned long i; _BitScanForward(&i, x); return (int)i; }
static __inline int __builtin_clz(unsigned x) { unsigned long i; _BitScanReverse(&i, x); return 31 - (int)i; }
static __inline int __builtin_popcount(unsigned x) { return (int)__popcnt(x); }
/* __builtin_sqrt: MSVC has it, as an intrinsic */

/* gcc's __atomic_* , which the SMP paths use. On x64 a plain load is an
 * acquire and a plain store a release; sequential consistency adds a
 * fence, and a read-modify-write is an Interlocked op. */
#define __ATOMIC_RELAXED 0
#define __ATOMIC_CONSUME 1
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_ACQ_REL 4
#define __ATOMIC_SEQ_CST 5
static __inline void win_fence(int order) {
    _ReadWriteBarrier();
    if (order == __ATOMIC_SEQ_CST) _mm_mfence();
}
static __inline uint8_t  win_load8 (const volatile uint8_t  *p, int o) { uint8_t  v = *p; if (o != __ATOMIC_RELAXED) win_fence(o); return v; }
static __inline uint16_t win_load16(const volatile uint16_t *p, int o) { uint16_t v = *p; if (o != __ATOMIC_RELAXED) win_fence(o); return v; }
static __inline uint32_t win_load32(const volatile uint32_t *p, int o) { uint32_t v = *p; if (o != __ATOMIC_RELAXED) win_fence(o); return v; }
static __inline uint64_t win_load64(const volatile uint64_t *p, int o) { uint64_t v = *p; if (o != __ATOMIC_RELAXED) win_fence(o); return v; }
static __inline void win_store8 (volatile uint8_t  *p, uint8_t  v, int o) { if (o != __ATOMIC_RELAXED) win_fence(o == __ATOMIC_SEQ_CST ? __ATOMIC_RELEASE : o); *p = v; if (o == __ATOMIC_SEQ_CST) _mm_mfence(); }
static __inline void win_store16(volatile uint16_t *p, uint16_t v, int o) { if (o != __ATOMIC_RELAXED) win_fence(o == __ATOMIC_SEQ_CST ? __ATOMIC_RELEASE : o); *p = v; if (o == __ATOMIC_SEQ_CST) _mm_mfence(); }
static __inline void win_store32(volatile uint32_t *p, uint32_t v, int o) { if (o != __ATOMIC_RELAXED) win_fence(o == __ATOMIC_SEQ_CST ? __ATOMIC_RELEASE : o); *p = v; if (o == __ATOMIC_SEQ_CST) _mm_mfence(); }
static __inline void win_store64(volatile uint64_t *p, uint64_t v, int o) { if (o != __ATOMIC_RELAXED) win_fence(o == __ATOMIC_SEQ_CST ? __ATOMIC_RELEASE : o); *p = v; if (o == __ATOMIC_SEQ_CST) _mm_mfence(); }
static __inline uint8_t  win_xchg8 (volatile uint8_t  *p, uint8_t  v) { return (uint8_t)_InterlockedExchange8((char volatile *)p, (char)v); }
static __inline uint16_t win_xchg16(volatile uint16_t *p, uint16_t v) { return (uint16_t)_InterlockedExchange16((short volatile *)p, (short)v); }
static __inline uint32_t win_xchg32(volatile uint32_t *p, uint32_t v) { return (uint32_t)_InterlockedExchange((long volatile *)p, (long)v); }
static __inline uint64_t win_xchg64(volatile uint64_t *p, uint64_t v) { return (uint64_t)_InterlockedExchange64((long long volatile *)p, (long long)v); }
static __inline int win_cas8 (volatile uint8_t  *p, uint8_t  *e, uint8_t  d) { uint8_t  o = (uint8_t)_InterlockedCompareExchange8((char volatile *)p, (char)d, (char)*e); if (o == *e) return 1; *e = o; return 0; }
static __inline int win_cas16(volatile uint16_t *p, uint16_t *e, uint16_t d) { uint16_t o = (uint16_t)_InterlockedCompareExchange16((short volatile *)p, (short)d, (short)*e); if (o == *e) return 1; *e = o; return 0; }
static __inline int win_cas32(volatile uint32_t *p, uint32_t *e, uint32_t d) { uint32_t o = (uint32_t)_InterlockedCompareExchange((long volatile *)p, (long)d, (long)*e); if (o == *e) return 1; *e = o; return 0; }
static __inline int win_cas64(volatile uint64_t *p, uint64_t *e, uint64_t d) { uint64_t o = (uint64_t)_InterlockedCompareExchange64((long long volatile *)p, (long long)d, (long long)*e); if (o == *e) return 1; *e = o; return 0; }
static __inline uint8_t  win_or8 (volatile uint8_t  *p, uint8_t  v) { return (uint8_t)_InterlockedOr8((char volatile *)p, (char)v); }
static __inline uint16_t win_or16(volatile uint16_t *p, uint16_t v) { return (uint16_t)_InterlockedOr16((short volatile *)p, (short)v); }
static __inline uint32_t win_or32(volatile uint32_t *p, uint32_t v) { return (uint32_t)_InterlockedOr((long volatile *)p, (long)v); }
static __inline uint64_t win_or64(volatile uint64_t *p, uint64_t v) { return (uint64_t)_InterlockedOr64((long long volatile *)p, (long long)v); }
static __inline uint8_t  win_and8 (volatile uint8_t  *p, uint8_t  v) { return (uint8_t)_InterlockedAnd8((char volatile *)p, (char)v); }
static __inline uint16_t win_and16(volatile uint16_t *p, uint16_t v) { return (uint16_t)_InterlockedAnd16((short volatile *)p, (short)v); }
static __inline uint32_t win_and32(volatile uint32_t *p, uint32_t v) { return (uint32_t)_InterlockedAnd((long volatile *)p, (long)v); }
static __inline uint64_t win_and64(volatile uint64_t *p, uint64_t v) { return (uint64_t)_InterlockedAnd64((long long volatile *)p, (long long)v); }
static __inline uint32_t win_add32(volatile uint32_t *p, uint32_t v) { return (uint32_t)_InterlockedExchangeAdd((long volatile *)p, (long)v); }
static __inline uint64_t win_add64(volatile uint64_t *p, uint64_t v) { return (uint64_t)_InterlockedExchangeAdd64((long long volatile *)p, (long long)v); }
#define __atomic_load_n(p, o) ( \
    sizeof(*(p)) == 1 ? win_load8 ((const volatile uint8_t  *)(const void *)(p), (o)) : \
    sizeof(*(p)) == 2 ? win_load16((const volatile uint16_t *)(const void *)(p), (o)) : \
    sizeof(*(p)) == 4 ? win_load32((const volatile uint32_t *)(const void *)(p), (o)) : \
                        win_load64((const volatile uint64_t *)(const void *)(p), (o)))
#define __atomic_store_n(p, v, o) do { \
    if (sizeof(*(p)) == 1) win_store8 ((volatile uint8_t  *)(void *)(p), (uint8_t)(v), (o)); \
    else if (sizeof(*(p)) == 2) win_store16((volatile uint16_t *)(void *)(p), (uint16_t)(v), (o)); \
    else if (sizeof(*(p)) == 4) win_store32((volatile uint32_t *)(void *)(p), (uint32_t)(v), (o)); \
    else win_store64((volatile uint64_t *)(void *)(p), (uint64_t)(v), (o)); \
} while (0)
#define __atomic_exchange_n(p, v, o) ( (void)(o), \
    sizeof(*(p)) == 1 ? win_xchg8 ((volatile uint8_t  *)(void *)(p), (uint8_t)(v)) : \
    sizeof(*(p)) == 2 ? win_xchg16((volatile uint16_t *)(void *)(p), (uint16_t)(v)) : \
    sizeof(*(p)) == 4 ? win_xchg32((volatile uint32_t *)(void *)(p), (uint32_t)(v)) : \
                        win_xchg64((volatile uint64_t *)(void *)(p), (uint64_t)(v)))
#define __atomic_compare_exchange_n(p, e, d, weak, so, fo) ( (void)(weak), (void)(so), (void)(fo), \
    sizeof(*(p)) == 1 ? win_cas8 ((volatile uint8_t  *)(void *)(p), (uint8_t  *)(void *)(e), (uint8_t)(d)) : \
    sizeof(*(p)) == 2 ? win_cas16((volatile uint16_t *)(void *)(p), (uint16_t *)(void *)(e), (uint16_t)(d)) : \
    sizeof(*(p)) == 4 ? win_cas32((volatile uint32_t *)(void *)(p), (uint32_t *)(void *)(e), (uint32_t)(d)) : \
                        win_cas64((volatile uint64_t *)(void *)(p), (uint64_t *)(void *)(e), (uint64_t)(d)))
#define __atomic_fetch_or(p, v, o) ( (void)(o), \
    sizeof(*(p)) == 1 ? win_or8 ((volatile uint8_t  *)(void *)(p), (uint8_t)(v)) : \
    sizeof(*(p)) == 2 ? win_or16((volatile uint16_t *)(void *)(p), (uint16_t)(v)) : \
    sizeof(*(p)) == 4 ? win_or32((volatile uint32_t *)(void *)(p), (uint32_t)(v)) : \
                        win_or64((volatile uint64_t *)(void *)(p), (uint64_t)(v)))
#define __atomic_fetch_and(p, v, o) ( (void)(o), \
    sizeof(*(p)) == 1 ? win_and8 ((volatile uint8_t  *)(void *)(p), (uint8_t)(v)) : \
    sizeof(*(p)) == 2 ? win_and16((volatile uint16_t *)(void *)(p), (uint16_t)(v)) : \
    sizeof(*(p)) == 4 ? win_and32((volatile uint32_t *)(void *)(p), (uint32_t)(v)) : \
                        win_and64((volatile uint64_t *)(void *)(p), (uint64_t)(v)))
#define __atomic_fetch_add(p, v, o) ( (void)(o), \
    sizeof(*(p)) == 4 ? win_add32((volatile uint32_t *)(void *)(p), (uint32_t)(v)) : \
                        win_add64((volatile uint64_t *)(void *)(p), (uint64_t)(v)))
#define __atomic_thread_fence(o) do { if ((o) != __ATOMIC_RELAXED) win_fence(o); } while (0)
#define _longjmp longjmp
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp

/* ---- files: 64-bit sizes, POSIX rename ---- */
#define stat  _stat64                 /* both the struct and the function */
#define fstat _fstat64
#define lseek _lseeki64
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_IWUSR
#define S_IRUSR _S_IREAD
#define S_IWUSR _S_IWRITE
#endif
#define O_ACCMODE (_O_RDONLY | _O_WRONLY | _O_RDWR)
#define O_CLOEXEC 0
#define open posix_open                   /* the UCRT refuses modes like 0644 */
int posix_open(const char *path, int flags, ...);
#define F_GETFL 3
int fcntl(int fd, int cmd, ...);
#define mkdir(path, mode) _mkdir(path)
#define rename posix_rename
int posix_rename(const char *from, const char *to);
ssize_t pread(int fd, void *buf, size_t n, int64_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, int64_t off);
int ftruncate(int fd, int64_t len);
char *realpath(const char *path, char *resolved);

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define isatty posix_isatty               /* the UCRT's says yes to NUL */
int posix_isatty(int fd);

/* getenv, cached: the UCRT's locks and walks the whole environment, and
 * the translator asks after its X86_* switches per instruction it plans
 * (30% of a Linux boot's host time). One lookup per name until setenv. */
#define getenv win_getenv
char *win_getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);

/* The x86-64 backend's fastmem (dbt_x64.c): FN is asked about every access
 * violation — the faulting PC, the address, RFLAGS — and returns 1 with
 * *rip set to resume there, or 0 to let the fault go on. */
int win_fault_handler(int (*fn)(uint64_t pc, const uint8_t *addr, uint64_t efl, uint64_t *rip));

/* X86_SAMPLE on Windows (dbt_common.c): a thread samples the calling
 * thread's RIP every US microseconds into buf; symbols come from DbgHelp,
 * one interned string per function, so equal names are equal pointers. */
int win_sampler_start(long us, double after_s, uintptr_t *buf, uint32_t max, volatile uint32_t *n);
void win_sampler_stop(void);
const char *win_symbol_name(uintptr_t pc);

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* ---- time ---- */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
int clock_gettime(int clk, struct timespec *ts);
int usleep(useconds_t us);
struct tm *localtime_r(const time_t *t, struct tm *out);
struct tm *gmtime_r(const time_t *t, struct tm *out);
time_t timegm(struct tm *tm);             /* -utc with -date: the weekday in UTC */

/* ---- memory mappings: anonymous and whole-file, no MAP_FIXED ---- */
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED    1
#define MAP_PRIVATE   2
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)
void *mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off);
int munmap(void *addr, size_t len);

/* ---- directories ---- */
struct dirent { char d_name[260]; };
typedef struct DIR DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);

struct statvfs { unsigned long long f_bsize, f_frsize, f_blocks, f_bfree, f_bavail; };
int statvfs(const char *path, struct statvfs *vs);

/* ---- the terminal: the console in VT mode ---- */
typedef unsigned int tcflag_t;
#define NCCS 32
struct termios { tcflag_t c_iflag, c_oflag, c_cflag, c_lflag; unsigned char c_cc[NCCS]; unsigned long in_mode, out_mode; };
#define ICANON 0x1
#define ECHO   0x2
#define ISIG   0x4
#define IEXTEN 0x8
#define IXON   0x1
#define ICRNL  0x2
#define INLCR  0x4
#define VMIN   0
#define VTIME  1
#define TCSANOW 0
int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int how, const struct termios *t);

#define pollfd win_pollfd                  /* Winsock has its own (pc_net.c takes that one) */
struct pollfd { int fd; short events, revents; };
#define POLLIN   0x1
#define POLLERR  0x8
#define POLLHUP  0x10
#define POLLNVAL 0x20
int poll(struct pollfd *p, unsigned n, int timeout_ms);

struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
#define TIOCGWINSZ 0x5413
int ioctl(int fd, unsigned long req, ...);

/* ---- cpuid ---- */
static __inline int __get_cpuid_count(unsigned leaf, unsigned sub, unsigned *a, unsigned *b, unsigned *c, unsigned *d) {
    int r[4];
    __cpuid(r, 0);
    if ((unsigned)r[0] < leaf) return 0;
    __cpuidex(r, (int)leaf, (int)sub);
    *a = (unsigned)r[0]; *b = (unsigned)r[1]; *c = (unsigned)r[2]; *d = (unsigned)r[3];
    return 1;
}

/* ---- the threads pc_smp.c and main.c use. A mutex is a critical
 * section, which reenters, so it is the recursive mutex SMP asks for. ---- */
typedef struct { void *cs; } pthread_mutex_t;
typedef struct { void *cv; } pthread_cond_t;
typedef uintptr_t pthread_t;
typedef struct { int type; } pthread_mutexattr_t;
typedef struct { int unused; } pthread_condattr_t;
typedef struct { unsigned stacksize; } pthread_attr_t;
#define PTHREAD_MUTEX_NORMAL    0
#define PTHREAD_MUTEX_RECURSIVE 1
int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abs);
int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t stack);
int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **ret);

#endif /* _WIN32 */
#endif
