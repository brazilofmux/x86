/* win_posix.c — the POSIX subset in win/posix.h, over Win32.
 *
 * The terminal is the Windows console with virtual-terminal processing
 * on both sides: the tty renderer's escape sequences go out as they do
 * on a Unix terminal, and keys (arrows and function keys included) come
 * in as the same VT sequences pc_kbd.c already parses. */
#include "posix.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>

/* The UCRT's default for a bad argument (close(-1), a stale fd) is to end
 * the process; POSIX code expects -1 and errno. */
static void quiet_invalid_parameter(const wchar_t *e, const wchar_t *f, const wchar_t *file, unsigned line, uintptr_t r) {
    (void)e; (void)f; (void)file; (void)line; (void)r;
}

/* ---- startup: binary std streams, a VT console, UTF-8 ---- */
static void win_posix_init(void) {
    _set_invalid_parameter_handler(quiet_invalid_parameter);
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

/* Sleep() is 15.6 ms coarse; a high-resolution waitable timer is not. */
int usleep(useconds_t us) {
    static HANDLE timer;
    static int tried;
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
