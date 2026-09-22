/* dos_int21.c — INT 21h (and 20h/27h/29h/2Fh) services.
 *
 * Conventions: `err(c, code)` sets CF and AX like DOS; `ok(c)` clears
 * CF. Console output goes through the BIOS teletype so the cell buffer
 * and the cursor stay right for programs that mix DOS writes and direct
 * video access.
 */
#include "dos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <stdarg.h>

#define AH x86_get_r8(c, R_AH)
#define AL x86_get_r8(c, R_AL)
#define BX x86_get_r16(c, R_BX)
#define CX x86_get_r16(c, R_CX)
#define DX x86_get_r16(c, R_DX)
#define SI x86_get_r16(c, R_SI)
#define DI x86_get_r16(c, R_DI)
#define DS (c->seg[S_DS].sel)
#define ES (c->seg[S_ES].sel)
#define SET_AX(v) x86_set_r16(c, R_AX, (uint16_t)(v))
#define SET_AL(v) x86_set_r8(c, R_AL, (uint8_t)(v))
#define SET_AH(v) x86_set_r8(c, R_AH, (uint8_t)(v))
#define SET_BX(v) x86_set_r16(c, R_BX, (uint16_t)(v))
#define SET_CX(v) x86_set_r16(c, R_CX, (uint16_t)(v))
#define SET_DX(v) x86_set_r16(c, R_DX, (uint16_t)(v))
#define SET_SI(v) x86_set_r16(c, R_SI, (uint16_t)(v))

static void ok(x86_cpu *c) { c->eflags &= ~X86_CF; }
static void err(x86_cpu *c, int code) { dos.last_error = code; SET_AX(code); c->eflags |= X86_CF; }

static void trace(x86_cpu *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void trace(x86_cpu *c, const char *fmt, ...) {
    if (!pc.debug) return;
    (void)c;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[dos] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ---- Console ------------------------------------------------------------ */
static void con_out(x86_cpu *c, uint8_t ch) { pc_video_teletype(c, ch); }

static uint8_t con_in(x86_cpu *c, int echo) {
    static uint8_t pending_ext;                 /* second byte of an extended key */
    if (pending_ext) { uint8_t v = pending_ext; pending_ext = 0; return v; }
    uint16_t key;
    pc_kbd_wait(c);
    if (!pc_kbd_get(c, &key)) return 0x1A;
    uint8_t a = key & 0xFF;
    if (a == 0 || a == 0xE0) { pending_ext = (uint8_t)(key >> 8); a = 0; }
    if (echo && a) con_out(c, a);
    return a;
}

/* AH=0A: buffered line input into DS:DX (max, count, chars, CR). */
static void buffered_input(x86_cpu *c) {
    uint16_t buf = DX;
    int max = pc_rd8(c, DS, buf);
    if (max == 0) return;
    int n = 0;
    for (;;) {
        uint8_t ch = con_in(c, 0);
        if (ch == 0x0D) { con_out(c, 0x0D); break; }
        if (ch == 0x08 || ch == 0x7F) {
            if (n > 0) { n--; con_out(c, 8); con_out(c, ' '); con_out(c, 8); }
            continue;
        }
        if (ch == 0x1A && pc.eof_seen) break;
        if (ch == 0 || n >= max - 1) { con_out(c, 7); continue; }
        pc_wr8(c, DS, (uint16_t)(buf + 2 + n), ch);
        n++;
        con_out(c, ch);
    }
    pc_wr8(c, DS, (uint16_t)(buf + 1), (uint8_t)n);
    pc_wr8(c, DS, (uint16_t)(buf + 2 + n), 0x0D);
}

/* ---- Handles ------------------------------------------------------------- */
static dos_handle *handle(x86_cpu *c, int h) {
    (void)c;
    if (h < 0 || h >= DOS_MAX_HANDLES || dos.handles[h].fd == -1) return NULL;
    return &dos.handles[h];
}

static int new_handle(void) {
    for (int i = 5; i < DOS_MAX_HANDLES; i++) if (dos.handles[i].fd == -1) return i;
    return -1;
}

static int is_device_name(const char *dos_path, uint8_t *dev) {
    const char *b = strrchr(dos_path, '\\'); b = b ? b + 1 : dos_path;
    const char *s = strrchr(b, '/'); b = s ? s + 1 : b;
    if (b[0] && b[1] == ':') b += 2;
    char name[16]; snprintf(name, sizeof name, "%s", b);
    char *dot = strchr(name, '.'); if (dot) *dot = 0;
    for (char *p = name; *p; p++) *p = (char)toupper((unsigned char)*p);
    if (!strcmp(name, "CON")) { *dev = 1; return 1; }
    if (!strcmp(name, "NUL") || !strcmp(name, "AUX") || !strcmp(name, "COM1") || !strcmp(name, "COM2")) { *dev = 2; return 1; }
    if (!strcmp(name, "PRN") || !strcmp(name, "LPT1") || !strcmp(name, "LPT2")) { *dev = 3; return 1; }
    return 0;
}

/* Open/create. mode: DOS access byte; create: 0 open, 1 create/truncate,
 * 2 create new (fail if exists), 3 create temp. */
static void do_open(x86_cpu *c, const char *dos_path, int mode, int create, int attr) {
    (void)attr;
    int h = new_handle();
    if (h < 0) { err(c, DE_TOO_MANY_OPEN); return; }
    dos_handle *dh = &dos.handles[h];
    uint8_t dev;
    if (is_device_name(dos_path, &dev)) {
        dh->fd = dev == 1 ? 0 : -2;
        dh->dev = dev; dh->binary = 0; dh->mode = (uint8_t)mode; dh->owner_psp = dos.psp;
        snprintf(dh->path, sizeof dh->path, "%s", dos_path);
        trace(c, "open device %s → %d", dos_path, h);
        SET_AX(h); ok(c);
        return;
    }
    char host[DOS_MAX_PATH]; int exists, is_dir;
    int e = dos_resolve(dos_path, host, sizeof host, &exists, &is_dir);
    if (e) { err(c, e); return; }
    if (is_dir && exists) { err(c, DE_ACCESS_DENIED); return; }
    int flags;
    switch (mode & 7) {
    case 0: flags = O_RDONLY; break;
    case 1: flags = O_WRONLY; break;
    default: flags = O_RDWR; break;
    }
    if (create == 1) flags = O_RDWR | O_CREAT | O_TRUNC;
    else if (create == 2) { if (exists) { err(c, DE_FILE_EXISTS); return; } flags = O_RDWR | O_CREAT | O_EXCL; }
    else if (!exists) { err(c, DE_FILE_NOT_FOUND); return; }
    int fd = open(host, flags, 0644);
    if (fd < 0 && (flags & O_ACCMODE) != O_RDONLY && errno == EACCES) fd = open(host, O_RDONLY);
    if (fd < 0) { err(c, dos_errno()); return; }
    dh->fd = fd; dh->dev = 0; dh->binary = 1; dh->mode = (uint8_t)mode; dh->owner_psp = dos.psp;
    snprintf(dh->path, sizeof dh->path, "%s", host);
    if (pc.debug) trace(c, "open %s (%s) mode %02X create %d → %d", dos_path, host, mode, create, h);
    SET_AX(h); ok(c);
}

static void do_read(x86_cpu *c, int h, uint16_t seg, uint16_t off, uint16_t len) {
    dos_handle *dh = handle(c, h);
    if (!dh) { err(c, DE_INVALID_HANDLE); return; }
    if (dh->dev == 1) {
        /* Console: cooked line input unless binary mode. */
        uint16_t n = 0;
        if (dh->binary) {
            while (n < len) { uint8_t ch = con_in(c, 0); pc_wr8(c, seg, (uint16_t)(off + n), ch); n++; if (pc_kbd_buffer_empty(c)) break; }
        } else if (len) {
            static uint8_t line[258]; static int line_len, line_pos;
            if (line_pos >= line_len) {
                line_len = 0; line_pos = 0;
                for (;;) {
                    uint8_t ch = con_in(c, 0);
                    if (ch == 0x0D) { con_out(c, 0x0D); con_out(c, 0x0A); line[line_len++] = 0x0D; line[line_len++] = 0x0A; break; }
                    if (ch == 0x08 || ch == 0x7F) { if (line_len) { line_len--; con_out(c, 8); con_out(c, ' '); con_out(c, 8); } continue; }
                    if (ch == 0x1A) { line[line_len++] = 0x1A; break; }
                    if (ch && line_len < 254) { line[line_len++] = ch; con_out(c, ch); }
                }
            }
            while (n < len && line_pos < line_len) pc_wr8(c, seg, (uint16_t)(off + n++), line[line_pos++]);
            if (n && line[line_pos - 1] == 0x1A) n--;   /* Ctrl-Z terminates */
        }
        SET_AX(n); ok(c);
        return;
    }
    if (dh->dev) { SET_AX(0); ok(c); return; }
    uint8_t *buf = malloc(len ? len : 1);
    ssize_t n = read(dh->fd, buf, len);
    if (n < 0) { free(buf); err(c, dos_errno()); return; }
    for (ssize_t i = 0; i < n; i++) x86_phys_wr8(c, (((uint32_t)seg << 4) + ((off + (uint32_t)i) & 0xFFFF)), buf[i]);
    free(buf);
    SET_AX(n); ok(c);
}

static void do_write(x86_cpu *c, int h, uint16_t seg, uint16_t off, uint16_t len) {
    dos_handle *dh = handle(c, h);
    if (!dh) { err(c, DE_INVALID_HANDLE); return; }
    if (dh->dev == 1) {
        for (uint16_t i = 0; i < len; i++) con_out(c, pc_rd8(c, seg, (uint16_t)(off + i)));
        SET_AX(len); ok(c);
        return;
    }
    if (dh->dev) { SET_AX(len); ok(c); return; }
    if (len == 0) {                                     /* truncate at current position */
        off_t pos = lseek(dh->fd, 0, SEEK_CUR);
        if (ftruncate(dh->fd, pos) < 0) { err(c, dos_errno()); return; }
        SET_AX(0); ok(c); return;
    }
    uint8_t *buf = malloc(len);
    for (uint16_t i = 0; i < len; i++) buf[i] = pc_rd8(c, seg, (uint16_t)(off + i));
    ssize_t n = write(dh->fd, buf, len);
    free(buf);
    if (n < 0) { err(c, dos_errno()); return; }
    SET_AX(n); ok(c);
}

static void fill_dta(x86_cpu *c, uint16_t id, const char *name, int attr, uint32_t size, time_t mtime, int search_attr) {
    uint16_t seg = dos.dta_seg, off = dos.dta_off;
    uint16_t date, tm = dos_ftime(mtime, &date);
    pc_wr16(c, seg, off, id);
    pc_wr8(c, seg, (uint16_t)(off + 2), (uint8_t)search_attr);
    pc_wr8(c, seg, (uint16_t)(off + 3), 0xC4);              /* our signature: a live search */
    pc_wr8(c, seg, (uint16_t)(off + 0x15), (uint8_t)attr);
    pc_wr16(c, seg, (uint16_t)(off + 0x16), tm);
    pc_wr16(c, seg, (uint16_t)(off + 0x18), date);
    pc_wr16(c, seg, (uint16_t)(off + 0x1A), (uint16_t)size);
    pc_wr16(c, seg, (uint16_t)(off + 0x1C), (uint16_t)(size >> 16));
    for (int i = 0; i < 13; i++) pc_wr8(c, seg, (uint16_t)(off + 0x1E + i), (uint8_t)(i < (int)strlen(name) ? name[i] : 0));
}

static void find_next_into_dta(x86_cpu *c, uint16_t id, int search_attr) {
    char name[16]; int attr; uint32_t size; time_t mtime;
    int e = dos_search_next(id, name, &attr, &size, &mtime);
    if (e) { err(c, e); return; }
    fill_dta(c, id, name, attr, size, mtime, search_attr);
    ok(c);
}

static void get_path(x86_cpu *c, uint16_t seg, uint16_t off, char *out, size_t n) {
    dos_read_str(c, seg, off, out, n);
}

static uint16_t bcd(int v) { return (uint16_t)v; }

/* ---- The dispatcher ----------------------------------------------------- */
void dos_int21(x86_cpu *c, int vector) {
    (void)vector;
    char path[DOS_MAX_PATH], host[DOS_MAX_PATH];
    int exists, is_dir, e;
    pc_wr8(c, DOS_SEG, 0, 1);                              /* InDOS */
    /* Like DOS: remember the caller's stack in its PSP; a child's
     * termination resumes the parent on the stack of its last call. */
    pc_wr16(c, dos.psp, 0x2E, (uint16_t)c->r[R_SP]);
    pc_wr16(c, dos.psp, 0x30, c->seg[S_SS].sel);
    if (pc.debug > 1) fprintf(stderr, "[dos] INT 21h AH=%02X AL=%02X BX=%04X CX=%04X DX=%04X from %04X:%04X @%llu\n", AH, AL, BX, CX, DX,
                              pc_rd16(c, c->seg[S_SS].sel, (uint16_t)(c->r[R_SP] + 2)), pc_rd16(c, c->seg[S_SS].sel, (uint16_t)c->r[R_SP]),
                              (unsigned long long)c->insn_count);
    switch (AH) {
    case 0x00: dos_terminate(c, 0, 0); break;
    case 0x01: SET_AL(con_in(c, 1)); break;
    case 0x02: con_out(c, x86_get_r8(c, R_DL)); break;
    case 0x03: SET_AL(0); break;
    case 0x04: case 0x05: break;
    case 0x06: {
        uint8_t dl = x86_get_r8(c, R_DL);
        if (dl != 0xFF) { con_out(c, dl); break; }
        uint16_t key;
        pc_kbd_poll(c);
        static uint8_t ext;
        if (ext) { SET_AL(ext); ext = 0; c->eflags &= ~X86_ZF; break; }
        if (pc_kbd_get(c, &key)) {
            uint8_t a = key & 0xFF;
            if (a == 0 || a == 0xE0) { ext = (uint8_t)(key >> 8); a = 0; }
            SET_AL(a); c->eflags &= ~X86_ZF;
        } else { SET_AL(0); c->eflags |= X86_ZF; }
        break;
    }
    case 0x07: case 0x08: SET_AL(con_in(c, 0)); break;
    case 0x09: {
        uint16_t p = DX;
        for (int i = 0; i < 65536; i++) { uint8_t ch = pc_rd8(c, DS, (uint16_t)(p + i)); if (ch == '$') break; con_out(c, ch); }
        SET_AL('$');
        break;
    }
    case 0x0A: buffered_input(c); break;
    case 0x0B: pc_kbd_poll(c); SET_AL(pc_kbd_buffer_empty(c) ? 0 : 0xFF); break;
    case 0x0C: {
        uint16_t key; while (pc_kbd_get(c, &key)) { }
        int fn = AL;
        if (fn == 1 || fn == 6 || fn == 7 || fn == 8 || fn == 0x0A) { SET_AH(fn); dos_int21(c, vector); return; }
        SET_AL(0);
        break;
    }
    case 0x0D: break;
    case 0x0E: SET_AL(5); break;                            /* select disk: LASTDRIVE = E */
    case 0x19: SET_AL(dos.cur_drive); break;
    case 0x1A: dos.dta_seg = DS; dos.dta_off = DX; break;
    case 0x1B: case 0x1C:
        SET_AL(8); SET_CX(512); SET_DX(0xFFF0);
        pc_wr8(c, DOS_SEG, 0x10, 0xF8);
        x86_load_seg(c, S_DS, DOS_SEG); SET_BX(0x10);
        break;
    case 0x25:
        pc_wr16(c, 0, (uint16_t)(AL * 4), DX);
        pc_wr16(c, 0, (uint16_t)(AL * 4 + 2), DS);
        trace(c, "set vector %02X → %04X:%04X", AL, DS, DX);
        break;
    case 0x26:                                              /* create PSP at DX */
        dos_make_child_psp(c, DX, pc_rd16(c, dos.psp, 2));
        break;
    case 0x55:                                              /* create child PSP at DX, memory top SI; it becomes current */
        dos_make_child_psp(c, DX, SI);
        dos.psp = DX;
        ok(c);
        break;
    case 0x2A: {
        time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
        SET_CX(tm.tm_year + 1900); x86_set_r8(c, R_DH, (uint8_t)(tm.tm_mon + 1)); x86_set_r8(c, R_DL, (uint8_t)tm.tm_mday);
        SET_AL(tm.tm_wday);
        break;
    }
    case 0x2B: SET_AL(0); break;
    case 0x2C: {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm; localtime_r(&ts.tv_sec, &tm);
        x86_set_r8(c, R_CH, (uint8_t)tm.tm_hour); x86_set_r8(c, R_CL, (uint8_t)tm.tm_min);
        x86_set_r8(c, R_DH, (uint8_t)tm.tm_sec); x86_set_r8(c, R_DL, (uint8_t)(ts.tv_nsec / 10000000));
        break;
    }
    case 0x2D: SET_AL(0); break;
    case 0x2E: dos.verify = AL & 1; break;
    case 0x2F: x86_load_seg(c, S_ES, dos.dta_seg); SET_BX(dos.dta_off); break;
    case 0x30: SET_AX(DOS_VERSION_MAJOR | (DOS_VERSION_MINOR << 8)); SET_BX(0); SET_CX(0); break;
    case 0x31: dos_terminate(c, AL, DX); break;
    case 0x33:
        switch (AL) {
        case 0: x86_set_r8(c, R_DL, dos.break_flag); break;
        case 1: dos.break_flag = x86_get_r8(c, R_DL) & 1; break;
        case 2: { uint8_t o = dos.break_flag; dos.break_flag = x86_get_r8(c, R_DL) & 1; x86_set_r8(c, R_DL, o); break; }
        case 5: x86_set_r8(c, R_DL, 3); break;
        case 6: SET_BX(DOS_VERSION_MAJOR | (DOS_VERSION_MINOR << 8)); SET_DX(0); break;
        default: SET_AL(0xFF); break;
        }
        break;
    case 0x34: x86_load_seg(c, S_ES, DOS_SEG); SET_BX(0); break;
    case 0x35:
        SET_BX(pc_rd16(c, 0, (uint16_t)(AL * 4)));
        x86_load_seg(c, S_ES, pc_rd16(c, 0, (uint16_t)(AL * 4 + 2)));
        break;
    case 0x36: {
        struct statvfs vs;
        if (statvfs(dos.root, &vs) == 0) {
            uint64_t total = (uint64_t)vs.f_blocks * vs.f_frsize, avail = (uint64_t)vs.f_bavail * vs.f_frsize;
            uint64_t cl = 32768;
            uint32_t tc = (uint32_t)(total / cl), fc = (uint32_t)(avail / cl);
            if (tc > 0xFFFF) tc = 0xFFFF;
            if (fc > 0xFFFF) fc = 0xFFFF;
            SET_AX(64); SET_CX(512); SET_DX(tc); SET_BX(fc);
        } else SET_AX(0xFFFF);
        break;
    }
    case 0x37: if (AL == 0) { SET_AL(0); x86_set_r8(c, R_DL, '/'); } else SET_AL(0); break;
    case 0x38: {                                            /* country info → DS:DX */
        if (DX == 0xFFFF) { ok(c); break; }
        static const uint8_t us[34] = { 1,0, '$',0,0,0,0, ',',0, '.',0, '-',0, ':',0, 0, 2, 0, 0,0,0,0, ':',0 };
        for (int i = 0; i < 34; i++) pc_wr8(c, DS, (uint16_t)(DX + i), us[i]);
        SET_BX(1); ok(c);
        break;
    }
    case 0x39: case 0x3A: {
        get_path(c, DS, DX, path, sizeof path);
        e = dos_resolve(path, host, sizeof host, &exists, &is_dir);
        if (e) { err(c, e); break; }
        if (AH == 0x39) { if (exists) { err(c, DE_ACCESS_DENIED); break; } if (mkdir(host, 0755) < 0) { err(c, dos_errno()); break; } }
        else { if (!exists || !is_dir) { err(c, DE_PATH_NOT_FOUND); break; } if (rmdir(host) < 0) { err(c, dos_errno()); break; } }
        ok(c);
        break;
    }
    case 0x3B: {
        get_path(c, DS, DX, path, sizeof path);
        e = dos_resolve(path, host, sizeof host, &exists, &is_dir);
        if (e || !exists || !is_dir) { err(c, DE_PATH_NOT_FOUND); break; }
        /* store the canonical DOS form: host path relative to root, uppercased */
        const char *rel = host + strlen(dos.root);
        char cwd[DOS_MAX_PATH]; size_t n = 0; cwd[0] = 0;
        for (const char *p = rel; *p && n + 1 < sizeof cwd; p++) cwd[n++] = (char)(*p == '/' ? '\\' : toupper((unsigned char)*p));
        cwd[n] = 0;
        snprintf(dos.cwd, sizeof dos.cwd, "%s", cwd[0] ? cwd : "\\");
        trace(c, "chdir %s → %s", path, dos.cwd);
        ok(c);
        break;
    }
    case 0x3C: get_path(c, DS, DX, path, sizeof path); do_open(c, path, 2, 1, CX); break;
    case 0x3D: get_path(c, DS, DX, path, sizeof path); do_open(c, path, AL, 0, 0); break;
    case 0x3E: {
        dos_handle *dh = handle(c, BX);
        if (!dh) { err(c, DE_INVALID_HANDLE); break; }
        if (BX >= 5) { if (dh->fd >= 0) close(dh->fd); dh->fd = -1; }
        ok(c);
        break;
    }
    case 0x3F: do_read(c, BX, DS, DX, CX); break;
    case 0x40: do_write(c, BX, DS, DX, CX); break;
    case 0x41: {
        get_path(c, DS, DX, path, sizeof path);
        e = dos_resolve(path, host, sizeof host, &exists, &is_dir);
        if (e) { err(c, e); break; }
        if (!exists) { err(c, DE_FILE_NOT_FOUND); break; }
        if (unlink(host) < 0) { err(c, dos_errno()); break; }
        ok(c);
        break;
    }
    case 0x42: {
        dos_handle *dh = handle(c, BX);
        if (!dh) { err(c, DE_INVALID_HANDLE); break; }
        if (dh->dev) { SET_AX(0); SET_DX(0); ok(c); break; }
        off_t off = (off_t)(int32_t)(((uint32_t)CX << 16) | DX);
        off_t r = lseek(dh->fd, off, AL == 0 ? SEEK_SET : AL == 1 ? SEEK_CUR : SEEK_END);
        if (r < 0) { err(c, DE_INVALID_FN); break; }
        SET_AX((uint32_t)r); SET_DX((uint32_t)r >> 16); ok(c);
        break;
    }
    case 0x43: {
        get_path(c, DS, DX, path, sizeof path);
        e = dos_resolve(path, host, sizeof host, &exists, &is_dir);
        if (e || !exists) { err(c, e ? e : DE_FILE_NOT_FOUND); break; }
        if (AL == 0) {
            struct stat st; stat(host, &st);
            int a = S_ISDIR(st.st_mode) ? DA_DIR : DA_ARCHIVE;
            if (!(st.st_mode & S_IWUSR)) a |= DA_RDONLY;
            SET_CX(a);
        }
        ok(c);
        break;
    }
    case 0x44: {
        dos_handle *dh = (AL <= 1 || AL == 6 || AL == 7 || AL == 0x0A) ? handle(c, BX) : NULL;
        switch (AL) {
        case 0x00:
            if (!dh) { err(c, DE_INVALID_HANDLE); break; }
            if (dh->dev) SET_DX(0x80C0 | (dh->dev == 1 ? 0x03 : 0) | (dh->binary ? 0x20 : 0));
            else SET_DX(0x0002);                                /* drive C:, not written */
            SET_AX(DX); ok(c);
            break;
        case 0x01:
            if (!dh) { err(c, DE_INVALID_HANDLE); break; }
            if (dh->dev) dh->binary = (x86_get_r8(c, R_DL) & 0x20) != 0;
            ok(c);
            break;
        case 0x06:
            if (!dh) { err(c, DE_INVALID_HANDLE); break; }
            if (dh->dev == 1) { pc_kbd_poll(c); SET_AL(pc_kbd_buffer_empty(c) ? 0 : 0xFF); }
            else SET_AL(0xFF);
            ok(c);
            break;
        case 0x07: SET_AL(0xFF); ok(c); break;
        case 0x08: SET_AX(1); ok(c); break;                    /* fixed disk */
        case 0x09: SET_DX(0); ok(c); break;                    /* local */
        case 0x0A: SET_DX(0); ok(c); break;
        case 0x0E: case 0x0F: SET_AL(0); ok(c); break;
        default: err(c, DE_INVALID_FN); break;
        }
        break;
    }
    case 0x45: {
        dos_handle *dh = handle(c, BX);
        if (!dh) { err(c, DE_INVALID_HANDLE); break; }
        int h = new_handle();   /* 45h dup */
        if (h < 0) { err(c, DE_TOO_MANY_OPEN); break; }
        dos.handles[h] = *dh;
        if (!dh->dev) { dos.handles[h].fd = dup(dh->fd); }
        SET_AX(h); ok(c);
        break;
    }
    case 0x46: {
        dos_handle *dh = handle(c, BX);
        int t = CX;
        if (!dh || t < 0 || t >= DOS_MAX_HANDLES) { err(c, DE_INVALID_HANDLE); break; }
        if (t != (int)BX) {
            if (dos.handles[t].fd >= 0 && t >= 5) close(dos.handles[t].fd);
            dos.handles[t] = *dh;
            if (!dh->dev) dos.handles[t].fd = dup(dh->fd);
        }
        ok(c);
        break;
    }
    case 0x47: {
        int drive = x86_get_r8(c, R_DL);
        if (drive != 0 && drive != 3) { err(c, DE_INVALID_DRIVE); break; }
        dos_write_str(c, DS, SI, dos.cwd[0] == '\\' ? dos.cwd + 1 : dos.cwd);
        SET_AX(0x100); ok(c);
        break;
    }
    case 0x48: {
        uint16_t largest;
        uint16_t seg = dos_mem_alloc(BX, dos.psp, &largest);
        if (!seg) { SET_BX(largest); err(c, DE_NO_MEMORY); break; }
        trace(c, "alloc %04X paras → %04X", BX, seg);
        SET_AX(seg); ok(c);
        break;
    }
    case 0x49: e = dos_mem_free(ES); if (e) err(c, e); else ok(c); break;
    case 0x4A: {
        uint16_t largest = 0;
        e = dos_mem_resize(ES, BX, &largest);
        trace(c, "resize %04X to %04X paras → %d (max %04X)", ES, BX, e, largest);
        if (e) { SET_BX(largest); err(c, e); } else ok(c);
        break;
    }
    case 0x4B:
        get_path(c, DS, DX, path, sizeof path);
        fprintf(stderr, "dos: EXEC %s not supported yet\n", path);
        err(c, DE_INVALID_FN);
        break;
    case 0x4C: dos_terminate(c, AL, 0); break;
    case 0x4D: SET_AX(dos.return_code); ok(c); break;
    case 0x4E: {
        get_path(c, DS, DX, path, sizeof path);
        uint16_t id;
        e = dos_search_first(path, CX, &id);
        trace(c, "findfirst %s attr %02X → %d", path, CX, e);
        if (e) { err(c, e); break; }
        find_next_into_dta(c, id, CX);
        break;
    }
    case 0x4F: {
        if (pc_rd8(c, dos.dta_seg, (uint16_t)(dos.dta_off + 3)) != 0xC4) { err(c, DE_NO_MORE_FILES); break; }
        find_next_into_dta(c, pc_rd16(c, dos.dta_seg, dos.dta_off), pc_rd8(c, dos.dta_seg, (uint16_t)(dos.dta_off + 2)));
        break;
    }
    case 0x50: dos.psp = BX; ok(c); break;
    case 0x51: case 0x62: SET_BX(dos.psp); ok(c); break;
    case 0x52: x86_load_seg(c, S_ES, DOS_SEG); SET_BX(0x40); break;   /* list of lists: unpopulated */
    case 0x54: SET_AL(dos.verify); break;
    case 0x56: {
        char dst[DOS_MAX_PATH], hdst[DOS_MAX_PATH];
        get_path(c, DS, DX, path, sizeof path);
        get_path(c, ES, DI, dst, sizeof dst);
        e = dos_resolve(path, host, sizeof host, &exists, &is_dir);
        if (e || !exists) { err(c, e ? e : DE_FILE_NOT_FOUND); break; }
        int e2, d2;
        e = dos_resolve(dst, hdst, sizeof hdst, &e2, &d2);
        if (e) { err(c, e); break; }
        if (e2) { err(c, DE_FILE_EXISTS); break; }
        if (rename(host, hdst) < 0) { err(c, dos_errno()); break; }
        ok(c);
        break;
    }
    case 0x57: {
        dos_handle *dh = handle(c, BX);
        if (!dh) { err(c, DE_INVALID_HANDLE); break; }
        if (AL == 0) {
            struct stat st; time_t t = time(NULL);
            if (!dh->dev && fstat(dh->fd, &st) == 0) t = st.st_mtime;
            uint16_t date, tm = dos_ftime(t, &date);
            SET_CX(tm); SET_DX(date);
        }
        ok(c);
        break;
    }
    case 0x58:
        switch (AL) {
        case 0: SET_AX(dos.alloc_strategy); ok(c); break;
        case 1: dos.alloc_strategy = (uint8_t)(BX & 3); ok(c); break;
        case 2: SET_AL(0); ok(c); break;
        case 3: ok(c); break;
        default: err(c, DE_INVALID_FN); break;
        }
        break;
    case 0x59: SET_AX(dos.last_error); x86_set_r8(c, R_BH, 1); x86_set_r8(c, R_BL, 1); x86_set_r8(c, R_CH, 1); break;
    case 0x5A: {
        get_path(c, DS, DX, path, sizeof path);
        char full[DOS_MAX_PATH];
        for (int i = 0; i < 1000; i++) {
            snprintf(full, sizeof full, "%s%sTMP%05d.$$$", path, (path[0] && path[strlen(path) - 1] != '\\') ? "\\" : "", i);
            e = dos_resolve(full, host, sizeof host, &exists, &is_dir);
            if (e) { err(c, e); goto done; }
            if (!exists) break;
        }
        dos_write_str(c, DS, DX, full);
        do_open(c, full, 2, 2, CX);
        break;
    }
    case 0x5B: get_path(c, DS, DX, path, sizeof path); do_open(c, path, 2, 2, CX); break;
    case 0x5C: ok(c); break;
    case 0x5D: if (AL == 6) { x86_load_seg(c, S_DS, DOS_SEG); SET_SI(0x100); SET_CX(0x80); SET_DX(0x1A); ok(c); } else err(c, DE_INVALID_FN); break;
    case 0x5E: case 0x5F: err(c, DE_INVALID_FN); break;
    case 0x60: {                                            /* truename */
        get_path(c, DS, SI, path, sizeof path);
        e = dos_resolve(path, host, sizeof host, &exists, &is_dir);
        if (e) { err(c, e); break; }
        const char *rel = host + strlen(dos.root);
        char out[DOS_MAX_PATH]; size_t n = (size_t)snprintf(out, sizeof out, "C:");
        for (const char *p = rel; *p && n + 1 < sizeof out; p++) out[n++] = (char)(*p == '/' ? '\\' : toupper((unsigned char)*p));
        if (n == 2) out[n++] = '\\';
        out[n] = 0;
        dos_write_str(c, ES, DI, out);
        ok(c);
        break;
    }
    case 0x63: err(c, DE_INVALID_FN); break;
    case 0x65:
        if (AL == 1 && CX >= 5) {
            pc_wr8(c, ES, DI, 1); pc_wr16(c, ES, (uint16_t)(DI + 1), 38);
            pc_wr16(c, ES, (uint16_t)(DI + 3), 1); pc_wr16(c, ES, (uint16_t)(DI + 5), 437);
            ok(c);
        } else err(c, DE_INVALID_FN);
        break;
    case 0x66: if (AL == 1) { SET_BX(437); SET_DX(437); ok(c); } else ok(c); break;
    case 0x67: ok(c); break;
    case 0x68: ok(c); break;
    case 0x6C: {
        get_path(c, DS, SI, path, sizeof path);
        int action = DX, mode = BX & 0x7F;
        e = dos_resolve(path, host, sizeof host, &exists, &is_dir);
        if (e) { err(c, e); break; }
        if (exists) {
            if (action & 1) { do_open(c, path, mode, 0, CX); if (!(c->eflags & X86_CF)) SET_CX(1); }
            else if (action & 2) { do_open(c, path, mode, 1, CX); if (!(c->eflags & X86_CF)) SET_CX(3); }
            else err(c, DE_FILE_EXISTS);
        } else {
            if (action & 0x10) { do_open(c, path, mode, 1, CX); if (!(c->eflags & X86_CF)) SET_CX(2); }
            else err(c, DE_FILE_NOT_FOUND);
        }
        break;
    }
    default:
        trace(c, "unsupported INT 21h AH=%02X", AH);
        err(c, DE_INVALID_FN);
        break;
    }
done:
    pc_wr8(c, DOS_SEG, 0, 0);
    (void)bcd;
}

void dos_int20(x86_cpu *c, int vector) {
    dos_terminate(c, 0, vector == 0x27 ? 1 : 0);
}

void dos_int29(x86_cpu *c, int vector) {
    (void)vector;
    con_out(c, AL);
}

void dos_int2f(x86_cpu *c, int vector) {
    (void)vector;
    uint16_t ax = x86_get_r16(c, R_AX);
    switch (ax >> 8) {
    case 0x16:                                              /* Windows: not running */
        if ((ax & 0xFF) == 0x00 || (ax & 0xFF) == 0x0A) SET_AL(0);
        break;
    case 0x43:                                              /* XMS: not installed */
        break;
    case 0x15:                                              /* MSCDEX: none */
        if ((ax & 0xFF) == 0) SET_BX(0);
        break;
    case 0x12: SET_AL(0xFF); break;                         /* DOS internal: installed */
    default: break;
    }
}
