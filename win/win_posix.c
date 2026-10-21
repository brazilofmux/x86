/* win_posix.c — the POSIX subset in win/posix.h, over Win32.
 *
 * The terminal is the Windows console with virtual-terminal processing
 * on both sides: the tty renderer's escape sequences go out as they do
 * on a Unix terminal, and keys (arrows and function keys included) come
 * in as the same VT sequences pc_kbd.c already parses. */
#include "posix.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdarg.h>

/* The UCRT's default for a bad argument (close(-1), a stale fd) is to end
 * the process; POSIX code expects -1 and errno. */
static void quiet_invalid_parameter(const wchar_t *e, const wchar_t *f, const wchar_t *file, unsigned line, uintptr_t r) {
    (void)e; (void)f; (void)file; (void)line; (void)r;
}

/* A crash the process does not handle itself: what and where, and the
 * stack, symbolized from the .pdb beside the .exe — Windows has no core
 * file to hand to a debugger, and no debugger may be installed. Frames
 * in translated code have no unwind data, so the walk can stop there. */
static LONG WINAPI crash_report(EXCEPTION_POINTERS *ep) {
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    fflush(stdout);
    fprintf(stderr, "\ndos-monster: exception %08lX at %p", (unsigned long)er->ExceptionCode, er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, " (%s %p)", er->ExceptionInformation[0] == 1 ? "writing" : er->ExceptionInformation[0] == 8 ? "executing" : "reading",
                (void *)er->ExceptionInformation[1]);
    fprintf(stderr, "\n");
    HANDLE proc = GetCurrentProcess(), thr = GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, NULL, TRUE);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 f = { 0 };
    f.AddrPC.Offset = ctx.Rip;    f.AddrPC.Mode = AddrModeFlat;
    f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 40; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thr, &f, &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
        if (!f.AddrPC.Offset) break;
        union { SYMBOL_INFO s; char buf[sizeof(SYMBOL_INFO) + 256]; } u;
        memset(&u, 0, sizeof u);
        u.s.SizeOfStruct = sizeof(SYMBOL_INFO); u.s.MaxNameLen = 255;
        DWORD64 disp = 0; DWORD ldisp = 0;
        IMAGEHLP_LINE64 ln = { sizeof ln };
        if (SymFromAddr(proc, f.AddrPC.Offset, &disp, &u.s)) {
            if (SymGetLineFromAddr64(proc, f.AddrPC.Offset, &ldisp, &ln))
                fprintf(stderr, "  #%-2d %s+0x%llx  %s:%lu\n", i, u.s.Name, (unsigned long long)disp, ln.FileName, (unsigned long)ln.LineNumber);
            else fprintf(stderr, "  #%-2d %s+0x%llx\n", i, u.s.Name, (unsigned long long)disp);
        } else fprintf(stderr, "  #%-2d %p\n", i, (void *)f.AddrPC.Offset);
    }
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- startup: binary std streams, a VT console, UTF-8 ---- */
static void win_posix_init(void) {
    _set_invalid_parameter_handler(quiet_invalid_parameter);
    SetUnhandledExceptionFilter(crash_report);
    _setmode(0, _O_BINARY); _setmode(1, _O_BINARY); _setmode(2, _O_BINARY);
    DWORD m;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(out, &m))
        SetConsoleMode(out, m | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (GetConsoleMode(err, &m))
        SetConsoleMode(err, m | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) static void (*win_posix_init_p)(void) = win_posix_init;

static int map_err(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: return ENOENT;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return EACCES;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return EEXIST;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_NOT_SAME_DEVICE: return EXDEV;
    case ERROR_INVALID_HANDLE: return EBADF;
    default: return EINVAL;
    }
}

int posix_isatty(int fd) {
    DWORD m;
    return GetConsoleMode((HANDLE)_get_osfhandle(fd), &m) ? 1 : 0;
}

/* ---- fastmem's fault: a vectored handler, first in line ---- */
static int (*fault_fn)(uint64_t pc, const uint8_t *addr, uint64_t efl, uint64_t *rip);

static LONG WINAPI fault_veh(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || !fault_fn)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *cx = ep->ContextRecord;
    uint64_t rip;
    if (!fault_fn(cx->Rip, (const uint8_t *)er->ExceptionInformation[1], cx->EFlags, &rip)) return EXCEPTION_CONTINUE_SEARCH;
    cx->Rip = rip;
    return EXCEPTION_CONTINUE_EXECUTION;
}

int win_fault_handler(int (*fn)(uint64_t pc, const uint8_t *addr, uint64_t efl, uint64_t *rip)) {
    fault_fn = fn;
    return AddVectoredExceptionHandler(1, fault_veh) ? 0 : -1;
}

/* ---- the environment, cached ---- */
#undef getenv
static SRWLOCK env_lock = SRWLOCK_INIT;
static struct { char *name, *value; } env_cache[256];
static unsigned env_n;

char *win_getenv(const char *name) {
    AcquireSRWLockShared(&env_lock);
    for (unsigned i = 0; i < env_n; i++)
        if (!strcmp(env_cache[i].name, name)) { char *v = env_cache[i].value; ReleaseSRWLockShared(&env_lock); return v; }
    ReleaseSRWLockShared(&env_lock);
    AcquireSRWLockExclusive(&env_lock);
    const char *v = getenv(name);
    char *copy = v ? _strdup(v) : NULL;
    if (env_n < sizeof env_cache / sizeof env_cache[0]) {
        env_cache[env_n].name = _strdup(name);
        env_cache[env_n].value = copy;
        env_n++;
    }
    ReleaseSRWLockExclusive(&env_lock);
    return copy;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && win_getenv(name)) return 0;
    if (_putenv_s(name, value)) return -1;
    AcquireSRWLockExclusive(&env_lock);
    for (unsigned i = 0; i < env_n; i++)
        if (!strcmp(env_cache[i].name, name)) { env_cache[i].value = _strdup(value); break; }   /* (the old copy may still be held: kept) */
    ReleaseSRWLockExclusive(&env_lock);
    return 0;
}

/* ---- X86_SAMPLE: a PC sampler ---- */
static struct {
    HANDLE target, thread;
    volatile LONG stop;
    long us;
    double after_s;
    uintptr_t *buf;
    uint32_t max;
    volatile uint32_t *n;
} smp;

static DWORD WINAPI sampler(void *arg) {
    (void)arg;
    if (smp.after_s > 0) Sleep((DWORD)(smp.after_s * 1000));
    while (!smp.stop) {
        usleep((useconds_t)smp.us);
        if (SuspendThread(smp.target) == (DWORD)-1) break;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(smp.target, &ctx)) {
            uint32_t k = *smp.n;
            if (k < smp.max) { smp.buf[k] = (uintptr_t)ctx.Rip; *smp.n = k + 1; }
        }
        ResumeThread(smp.target);
    }
    return 0;
}

int win_sampler_start(long us, double after_s, uintptr_t *buf, uint32_t max, volatile uint32_t *n) {
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &smp.target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) return -1;
    smp.us = us; smp.after_s = after_s; smp.buf = buf; smp.max = max; smp.n = n; smp.stop = 0;
    smp.thread = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    if (smp.thread) SetThreadPriority(smp.thread, THREAD_PRIORITY_TIME_CRITICAL);
    return smp.thread ? 0 : -1;
}

void win_sampler_stop(void) {
    if (!smp.thread) return;
    InterlockedExchange(&smp.stop, 1);
    WaitForSingleObject(smp.thread, INFINITE);
    CloseHandle(smp.thread); smp.thread = NULL;
}

const char *win_symbol_name(uintptr_t pc) {
    static int sym_ready;
    static struct { DWORD64 addr; char *name; } *names;
    static unsigned nnames, capnames;
    HANDLE proc = GetCurrentProcess();
    if (!sym_ready) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(proc, NULL, TRUE);
        sym_ready = 1;
    }
    union { SYMBOL_INFO s; char buf[sizeof(SYMBOL_INFO) + 256]; } u;
    memset(&u, 0, sizeof u);
    u.s.SizeOfStruct = sizeof(SYMBOL_INFO); u.s.MaxNameLen = 255;
    DWORD64 disp = 0;
    if (!SymFromAddr(proc, pc, &disp, &u.s)) return "(unknown)";
    for (unsigned i = 0; i < nnames; i++) if (names[i].addr == u.s.Address) return names[i].name;
    if (nnames == capnames) {
        unsigned cap = capnames ? capnames * 2 : 256;
        void *p = realloc(names, cap * sizeof *names);
        if (!p) return "(unknown)";
        names = p; capnames = cap;
    }
    names[nnames].addr = u.s.Address;
    names[nnames].name = _strdup(u.s.Name);
    return names[nnames++].name;
}

/* ---- files ---- */
#undef open
static uint8_t fd_acc[4096];              /* each fd's O_ACCMODE, for fcntl(F_GETFL) */

/* Through CreateFile for FILE_SHARE_DELETE: POSIX (and DOS programs)
 * delete or rename a file that is still open, which _open's handles
 * refuse. */
int posix_open(const char *path, int flags, ...) {
    int mode = 0666;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    int acc = flags & O_ACCMODE;
    DWORD access = acc == O_RDONLY ? GENERIC_READ : acc == O_WRONLY ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE;
    DWORD disp = (flags & O_CREAT) ? ((flags & O_EXCL) ? CREATE_NEW : (flags & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS)
                                   : (flags & O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;
    DWORD attr = (mode & 0200) ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_READONLY;
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, disp, attr, NULL);
    if (h == INVALID_HANDLE_VALUE) { errno = map_err(GetLastError()); return -1; }
    int fd = _open_osfhandle((intptr_t)h, (flags & _O_APPEND) | (acc == O_RDONLY ? _O_RDONLY : 0));
    if (fd < 0) { CloseHandle(h); errno = EMFILE; return -1; }
    if (fd < (int)sizeof fd_acc) fd_acc[fd] = (uint8_t)acc;
    return fd;
}

int fcntl(int fd, int cmd, ...) {
    if (cmd != F_GETFL || fd < 0) { errno = EINVAL; return -1; }
    return fd < (int)sizeof fd_acc ? fd_acc[fd] : O_RDWR;
}

int posix_rename(const char *from, const char *to) {
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return 0;
    errno = map_err(GetLastError());
    return -1;
}

static ssize_t pio(int fd, void *buf, size_t n, int64_t off, int wr) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    OVERLAPPED ov = { 0 };
    ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    DWORD done = 0;
    BOOL ok = wr ? WriteFile(h, buf, (DWORD)n, &done, &ov) : ReadFile(h, buf, (DWORD)n, &done, &ov);
    if (!ok) {
        DWORD e = GetLastError();
        if (!wr && e == ERROR_HANDLE_EOF) return 0;
        errno = map_err(e);
        return -1;
    }
    return (ssize_t)done;
}
ssize_t pread(int fd, void *buf, size_t n, int64_t off) { return pio(fd, buf, n, off, 0); }
ssize_t pwrite(int fd, const void *buf, size_t n, int64_t off) { return pio(fd, (void *)buf, n, off, 1); }

int ftruncate(int fd, int64_t len) {
    errno_t e = _chsize_s(fd, len);
    if (e) { errno = e; return -1; }
    return 0;
}

/* An absolute path with forward slashes; the path must exist. */
char *realpath(const char *path, char *resolved) {
    char buf[PATH_MAX];
    if (!_fullpath(buf, path, sizeof buf)) { errno = ENAMETOOLONG; return NULL; }
    if (GetFileAttributesA(buf) == INVALID_FILE_ATTRIBUTES) { errno = map_err(GetLastError()); return NULL; }
    for (char *p = buf; *p; p++) if (*p == '\\') *p = '/';
    size_t n = strlen(buf);
    if (n > 3 && buf[n - 1] == '/') buf[--n] = 0;
    if (!resolved) return _strdup(buf);
    memcpy(resolved, buf, n + 1);
    return resolved;
}

/* ---- time ---- */
int clock_gettime(int clk, struct timespec *ts) {
    if (clk == CLOCK_REALTIME) return timespec_get(ts, TIME_UTC) ? 0 : -1;
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    uint64_t f = (uint64_t)freq.QuadPart, t = (uint64_t)now.QuadPart;
    ts->tv_sec = (time_t)(t / f);
    ts->tv_nsec = (long)((t % f) * 1000000000ull / f);
    return 0;
}

/* Sleep() is 15.6 ms coarse; a high-resolution waitable timer is not.
 * One per thread: guest processors and the sampler sleep at once, and a
 * shared timer re-armed by one cuts or stretches the other's wait. */
int usleep(useconds_t us) {
    static _Thread_local HANDLE timer;
    static _Thread_local int tried;
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)us * 10;              /* relative, 100 ns units */
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return 0;
        }
    }
    Sleep((us + 999) / 1000);
    return 0;
}

struct tm *localtime_r(const time_t *t, struct tm *out) { return localtime_s(out, t) ? NULL : out; }
struct tm *gmtime_r(const time_t *t, struct tm *out) { return gmtime_s(out, t) ? NULL : out; }
time_t timegm(struct tm *tm) { return _mkgmtime(tm); }

/* ---- threads: the subset pc/pc_smp.c and main.c call ---- */
int pthread_mutexattr_init(pthread_mutexattr_t *a) { a->type = PTHREAD_MUTEX_NORMAL; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type) { a->type = type; return 0; }
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    (void)a;                                          /* a critical section reenters */
    CRITICAL_SECTION *cs = malloc(sizeof *cs);
    if (!cs) { errno = ENOMEM; return -1; }
    InitializeCriticalSection(cs);
    m->cs = cs;
    return 0;
}
int pthread_mutex_lock(pthread_mutex_t *m) { EnterCriticalSection(m->cs); return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m) { LeaveCriticalSection(m->cs); return 0; }

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    (void)a;
    CONDITION_VARIABLE *cv = malloc(sizeof *cv);
    if (!cv) { errno = ENOMEM; return -1; }
    InitializeConditionVariable(cv);
    c->cv = cv;
    return 0;
}
int pthread_cond_signal(pthread_cond_t *c) { WakeConditionVariable(c->cv); return 0; }
int pthread_cond_broadcast(pthread_cond_t *c) { WakeAllConditionVariable(c->cv); return 0; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    return SleepConditionVariableCS(c->cv, m->cs, INFINITE) ? 0 : -1;
}
/* abs is CLOCK_REALTIME, as pc_smp_wait computes it. The timeout is in
 * milliseconds; round up so a short wait does not return early. */
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abs) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return -1;
    int64_t ns = ((int64_t)abs->tv_sec - (int64_t)now.tv_sec) * 1000000000LL
               + ((int64_t)abs->tv_nsec - (int64_t)now.tv_nsec);
    if (ns <= 0) return ETIMEDOUT;
    uint64_t ms = ((uint64_t)ns + 999999ull) / 1000000ull;
    if (ms > 0xFFFFFFFEull) ms = 0xFFFFFFFEull;       /* INFINITE is the untimed wait */
    if (SleepConditionVariableCS(c->cv, m->cs, (DWORD)ms)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : -1;
}

struct win_thr { void *(*fn)(void *); void *arg; };
static unsigned __stdcall win_thr_main(void *p) {
    struct win_thr *t = p;
    void *(*fn)(void *) = t->fn;
    void *arg = t->arg;
    free(t);
    fn(arg);
    return 0;
}
int pthread_attr_init(pthread_attr_t *a) { a->stacksize = 0; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t stack) {
    a->stacksize = stack > 0xFFFFFFFFu ? 0 : (unsigned)stack;
    return 0;
}
int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg) {
    struct win_thr *s = malloc(sizeof *s);
    if (!s) { errno = ENOMEM; return -1; }
    s->fn = fn; s->arg = arg;
    uintptr_t h = _beginthreadex(NULL, a ? a->stacksize : 0, win_thr_main, s, 0, NULL);
    if (!h) { int e = errno ? errno : EAGAIN; free(s); errno = e; return -1; }
    *t = (pthread_t)h;
    return 0;
}
int pthread_join(pthread_t t, void **ret) {
    if (WaitForSingleObject((HANDLE)t, INFINITE) != WAIT_OBJECT_0) { errno = EINVAL; return -1; }
    CloseHandle((HANDLE)t);
    if (ret) *ret = NULL;
    return 0;
}

/* ---- memory mappings ---- */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off) {
    (void)addr;
    DWORD page = (prot & PROT_EXEC) ? ((prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
               : (prot & PROT_WRITE) ? PAGE_READWRITE : (prot & PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
    if (flags & MAP_ANONYMOUS) {
        void *p = VirtualAlloc(NULL, len, prot == PROT_NONE ? MEM_RESERVE : MEM_RESERVE | MEM_COMMIT, page);
        if (!p) { errno = ENOMEM; return MAP_FAILED; }
        return p;
    }
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return MAP_FAILED; }
    int cow = (flags & MAP_PRIVATE) && (prot & PROT_WRITE);
    DWORD mprot = cow ? PAGE_WRITECOPY : (prot & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
    DWORD access = cow ? FILE_MAP_COPY : (prot & PROT_WRITE) ? FILE_MAP_WRITE : FILE_MAP_READ;
    HANDLE m = CreateFileMappingA(h, NULL, mprot, 0, 0, NULL);
    if (!m) { errno = map_err(GetLastError()); return MAP_FAILED; }
    void *p = MapViewOfFile(m, access, (DWORD)((uint64_t)off >> 32), (DWORD)off, len);
    DWORD e = GetLastError();
    CloseHandle(m);                                     /* the view keeps the mapping alive */
    if (!p) { errno = map_err(e); return MAP_FAILED; }
    return p;
}

int munmap(void *addr, size_t len) {
    (void)len;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(addr, &mi, sizeof mi)) { errno = EINVAL; return -1; }
    BOOL ok = mi.Type == MEM_MAPPED ? UnmapViewOfFile(addr) : VirtualFree(mi.AllocationBase, 0, MEM_RELEASE);
    if (!ok) { errno = EINVAL; return -1; }
    return 0;
}

/* ---- directories ---- */
struct DIR { HANDLE h; WIN32_FIND_DATAA fd; int first; struct dirent ent; };

DIR *opendir(const char *path) {
    char pat[PATH_MAX];
    size_t n = strlen(path);
    if (n + 3 > sizeof pat) { errno = ENAMETOOLONG; return NULL; }
    memcpy(pat, path, n);
    if (n && pat[n - 1] != '/' && pat[n - 1] != '\\') pat[n++] = '/';
    pat[n++] = '*'; pat[n] = 0;
    DIR *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { errno = map_err(GetLastError()); free(d); return NULL; }
    d->first = 1;
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d->first && !FindNextFileA(d->h, &d->fd)) return NULL;
    d->first = 0;
    snprintf(d->ent.d_name, sizeof d->ent.d_name, "%s", d->fd.cFileName);
    return &d->ent;
}

int closedir(DIR *d) {
    FindClose(d->h);
    free(d);
    return 0;
}

int statvfs(const char *path, struct statvfs *vs) {
    ULARGE_INTEGER avail, total, free_;
    if (!GetDiskFreeSpaceExA(path, &avail, &total, &free_)) { errno = map_err(GetLastError()); return -1; }
    vs->f_bsize = vs->f_frsize = 4096;
    vs->f_blocks = total.QuadPart / 4096;
    vs->f_bfree = free_.QuadPart / 4096;
    vs->f_bavail = avail.QuadPart / 4096;
    return 0;
}

/* ---- the terminal ---- */
int tcgetattr(int fd, struct termios *t) {
    DWORD in, out;
    if (!GetConsoleMode((HANDLE)_get_osfhandle(fd), &in)) { errno = ENOTTY; return -1; }
    if (!GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &out)) out = 0;
    memset(t, 0, sizeof *t);
    t->in_mode = in; t->out_mode = out;
    if (in & ENABLE_LINE_INPUT) t->c_lflag |= ICANON | IEXTEN;
    if (in & ENABLE_ECHO_INPUT) t->c_lflag |= ECHO;
    if (in & ENABLE_PROCESSED_INPUT) t->c_lflag |= ISIG;
    t->c_iflag = IXON | ICRNL;
    return 0;
}

/* Raw (no ICANON): VT input, no line editing, echo or Ctrl-C handling. */
int tcsetattr(int fd, int how, const struct termios *t) {
    (void)how;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    DWORD in = t->in_mode;
    if (!(t->c_lflag & ICANON)) {
        in &= ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT
                       | ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT | ENABLE_QUICK_EDIT_MODE);
        in |= ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS;
    }
    if (!SetConsoleMode(h, in)) { errno = EINVAL; return -1; }
    return 0;
}

/* Whether a read of the console would return bytes now: some key-down
 * record carries a character (with VT input on, arrows and function keys
 * arrive as characters of their escape sequences). Records that never
 * turn into bytes — key-ups, focus, resize — are drained so the handle
 * does not stay signaled for nothing. */
static int console_has_char(HANDLE h) {
    INPUT_RECORD r[64];
    DWORD n;
    for (;;) {
        if (!PeekConsoleInputW(h, r, 64, &n) || n == 0) return 0;
        DWORD skip = 0;
        for (DWORD i = 0; i < n; i++) {
            if (r[i].EventType == KEY_EVENT && r[i].Event.KeyEvent.bKeyDown && r[i].Event.KeyEvent.uChar.UnicodeChar)
                return 1;
            if (i == skip) skip++;
        }
        if (skip == 0) return 0;
        ReadConsoleInputW(h, r, skip, &n);
    }
}

int poll(struct pollfd *p, unsigned n, int timeout_ms) {
    if (n != 1) { errno = EINVAL; return -1; }
    HANDLE h = (HANDLE)_get_osfhandle(p->fd);
    p->revents = 0;
    if (h == INVALID_HANDLE_VALUE) { p->revents = POLLNVAL; return 1; }
    DWORD type = GetFileType(h), m;
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        if (type == FILE_TYPE_CHAR && GetConsoleMode(h, &m)) {
            if (console_has_char(h)) { p->revents = POLLIN; return 1; }
        } else if (type == FILE_TYPE_PIPE) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) { p->revents = POLLHUP; return 1; }
            if (avail) { p->revents = POLLIN; return 1; }
        } else {
            p->revents = POLLIN;                        /* a file, or NUL: a read returns at once */
            return 1;
        }
        ULONGLONG now = GetTickCount64();
        if (timeout_ms >= 0 && now >= deadline) return 0;
        DWORD wait = timeout_ms < 0 ? INFINITE : (DWORD)(deadline - now);
        if (type == FILE_TYPE_PIPE) Sleep(wait > 1 ? 1 : wait);
        else WaitForSingleObject(h, wait);
    }
}

int ioctl(int fd, unsigned long req, ...) {
    if (req != TIOCGWINSZ) { errno = EINVAL; return -1; }
    va_list ap;
    va_start(ap, req);
    struct winsize *ws = va_arg(ap, struct winsize *);
    va_end(ap);
    CONSOLE_SCREEN_BUFFER_INFO bi;
    if (!GetConsoleScreenBufferInfo((HANDLE)_get_osfhandle(fd), &bi)) { errno = ENOTTY; return -1; }
    ws->ws_row = (unsigned short)(bi.srWindow.Bottom - bi.srWindow.Top + 1);
    ws->ws_col = (unsigned short)(bi.srWindow.Right - bi.srWindow.Left + 1);
    ws->ws_xpixel = ws->ws_ypixel = 0;
    return 0;
}
