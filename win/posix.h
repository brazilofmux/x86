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

static __inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) ? -1 : 0;
}

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

#endif /* _WIN32 */
#endif
