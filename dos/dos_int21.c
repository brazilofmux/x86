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

/* A pointer the client handed us, resolved through the segment's cached
 * base: sel<<4 in real mode, the descriptor's base in protected mode. */
static uint32_t cp(x86_cpu *c, int segreg, uint32_t off) { return c->seg[segreg].base + off; }
#define P_DS(off) cp(c, S_DS, (off))
#define P_ES(off) cp(c, S_ES, (off))
#define LRD8(lin)      x86_phys_rd8(c, (lin))
#define LWR8(lin, v)   x86_phys_wr8(c, (lin), (uint8_t)(v))
#define LRD16(lin)     ((uint16_t)x86_rd(c, (lin), 0, 0xFFFFFFFFu, 2))
#define LWR16(lin, v)  x86_wr(c, (lin), 0, 0xFFFFFFFFu, 2, (uint16_t)(v))

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
    pc_kbd_wait(c, 0);                        /* the shim's console waits in the host, as it always has */
    if (!pc_kbd_get(c, &key)) return 0x1A;
    uint8_t a = key & 0xFF;
    if (a == 0 || a == 0xE0) { pending_ext = (uint8_t)(key >> 8); a = 0; }
    if (echo && a) con_out(c, a);
    return a;
}

/* AH=0A: buffered line input into DS:DX (max, count, chars, CR). */
static void buffered_input(x86_cpu *c) {
    uint16_t buf = DX;
    int max = LRD8(P_DS(buf));
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
        LWR8(P_DS(buf + 2 + n), ch);
        n++;
        con_out(c, ch);
    }
    LWR8(P_DS(buf + 1), n);
    LWR8(P_DS(buf + 2 + n), 0x0D);
}

/* ---- Handles ------------------------------------------------------------- */
/* Handles are what DOS makes them: an index into the current PSP's job
 * file table (20 bytes at PSP:18h by default; count at :32h, far pointer
 * at :34h, both a program may change — INT 21h 67h, or by hand), each
 * byte naming a system file table entry (dos.handles[]) or FFh. Programs
 * do rearrange the table themselves: Micro Focus's run-time moves the
 * file it just opened up to slot 19 to keep the low handles free, and
 * reads through 19 from then on. Children inherit a copy, so an entry
 * counts its references and closes on the last. */
static uint16_t jft_base(x86_cpu *c, uint16_t psp, uint16_t *seg, uint16_t *off) {
    *off = pc_rd16(c, psp, 0x34); *seg = pc_rd16(c, psp, 0x36);
    return pc_rd16(c, psp, 0x32);
}
static int jft_get(x86_cpu *c, uint16_t psp, int h) {
    uint16_t seg, off, n = jft_base(c, psp, &seg, &off);
    if (h < 0 || h >= (int)n) return -1;
    return pc_rd8(c, seg, (uint16_t)(off + h));
}
static void jft_set(x86_cpu *c, uint16_t psp, int h, int sft) {
    uint16_t seg, off; jft_base(c, psp, &seg, &off);
    pc_wr8(c, seg, (uint16_t)(off + h), (uint8_t)sft);
}
static int sft_of(x86_cpu *c, int h) {
    int s = jft_get(c, dos.psp, h);
    if (s < 0 || s == 0xFF || s >= DOS_MAX_HANDLES || dos.handles[s].fd == -1) return -1;
    return s;
}
static dos_handle *handle(x86_cpu *c, int h) {
    int s = sft_of(c, h);
    return s < 0 ? NULL : &dos.handles[s];
}

/* A free JFT slot of the current PSP paired with a free SFT entry; the
 * slot points at the entry, whose refs start at 1. Returns the handle. */
static int new_handle(x86_cpu *c) {
    uint16_t seg, off, n = jft_base(c, dos.psp, &seg, &off);
    int h = -1, s = -1;
    for (int i = 0; i < (int)n; i++) if (pc_rd8(c, seg, (uint16_t)(off + i)) == 0xFF) { h = i; break; }
    for (int i = 5; i < DOS_MAX_HANDLES; i++) if (dos.handles[i].fd == -1) { s = i; break; }
    if (h < 0 || s < 0) return -1;
    pc_wr8(c, seg, (uint16_t)(off + h), (uint8_t)s);
    dos.handles[s].refs = 1;
    return h;
}

/* ---- File buffering ----
 * DOS kept file data in its BUFFERS, so a program that reads and writes
 * a record per call (the Micro Focus run-time: 106-byte sort work
 * records, a seek and a read per indexed access) paid a memory copy.
 * Here each call was a host system call. An open file now has a 32 KB
 * window: reads are served from it, writes land in it (contiguously,
 * the dirty part tracked), and it is written back when the window moves
 * or the file closes, before any path is looked at (open, create,
 * delete, rename, attributes, directory searches, EXEC: dos_flush_all),
 * before the file's size or date is asked for, and at exit. Two entries
 * naming the same host file (the file opened twice) are not buffered at
 * all, so neither can see stale bytes. */
#define FILE_WIN 32768u

static void win_flush(dos_handle *dh) {
    if (dh->buf && dh->dlo < dh->dhi) {
        if (pwrite(dh->fd, dh->buf + dh->dlo, dh->dhi - dh->dlo, dh->bstart + dh->dlo) < 0) { /* reported by the next call */ }
    }
    dh->dlo = dh->dhi = 0;
}
static void win_drop(dos_handle *dh) { win_flush(dh); dh->blen = 0; }

void dos_flush_all(void) {
    for (int i = 5; i < DOS_MAX_HANDLES; i++)
        if (dos.handles[i].fd >= 0 && !dos.handles[i].dev) win_flush(&dos.handles[i]);
}
void dos_flush_atexit(void) { dos_flush_all(); }

/* Mark every open entry naming dh's host file (dh included) as aliased,
 * their windows written back and dropped, if there is more than one. */
static void win_alias_check(dos_handle *dh) {
    int n = 0;
    for (int i = 5; i < DOS_MAX_HANDLES; i++) {
        dos_handle *o = &dos.handles[i];
        if (o != dh && o->fd >= 0 && !o->dev && !strcmp(o->path, dh->path)) {
            n++; win_drop(o); o->alias = 1;
        }
    }
    dh->alias = n > 0;
}

/* Read up to len bytes at dh->pos into out; returns the count or -1. */
static ssize_t file_read(dos_handle *dh, uint8_t *out, uint32_t len) {
    if (dh->alias) { ssize_t n = pread(dh->fd, out, len, dh->pos); if (n > 0) dh->pos += n; return n; }
    uint32_t done = 0;
    while (done < len) {
        if (dh->buf && dh->pos >= dh->bstart && dh->pos < dh->bstart + dh->blen) {
            uint32_t at = (uint32_t)(dh->pos - dh->bstart), n = dh->blen - at;
            if (n > len - done) n = len - done;
            memcpy(out + done, dh->buf + at, n);
            done += n; dh->pos += n;
            continue;
        }
        win_drop(dh);
        if (len - done >= FILE_WIN) {                       /* big reads go straight through */
            ssize_t n = pread(dh->fd, out + done, len - done, dh->pos);
            if (n < 0) return done ? (ssize_t)done : -1;
            done += (uint32_t)n; dh->pos += n;
            break;
        }
        if (!dh->buf && !(dh->buf = malloc(FILE_WIN))) return -1;
        ssize_t n = pread(dh->fd, dh->buf, FILE_WIN, dh->pos);
        if (n < 0) return done ? (ssize_t)done : -1;
        dh->bstart = dh->pos; dh->blen = (uint32_t)n;
        if (n == 0) break;                                  /* end of file */
    }
    return done;
}

/* Write len bytes at dh->pos; returns the count or -1. */
static ssize_t file_write(dos_handle *dh, const uint8_t *in, uint32_t len) {
    if (dh->hostacc == O_RDONLY) { errno = EBADF; return -1; }
    if (dh->alias || len >= FILE_WIN) {
        win_drop(dh);
        ssize_t n = pwrite(dh->fd, in, len, dh->pos);
        if (n > 0) dh->pos += n;
        return n;
    }
    int in_win = dh->buf && dh->pos >= dh->bstart && dh->pos <= dh->bstart + dh->blen
                 && dh->pos + len <= dh->bstart + FILE_WIN;
    if (!in_win) {
        win_drop(dh);
        if (!dh->buf && !(dh->buf = malloc(FILE_WIN))) return -1;
        dh->bstart = dh->pos; dh->blen = 0;
    }
    uint32_t at = (uint32_t)(dh->pos - dh->bstart);
    memcpy(dh->buf + at, in, len);
    if (dh->dlo >= dh->dhi) { dh->dlo = at; dh->dhi = at + len; }
    else { if (at < dh->dlo) dh->dlo = at; if (at + len > dh->dhi) dh->dhi = at + len; }
    if (at + len > dh->blen) dh->blen = at + len;
    dh->pos += len;
    return len;
}

/* Drop one JFT reference; the entry closes when nothing names it. */
static void sft_release(int s) {
    if (s < 5 || s >= DOS_MAX_HANDLES || dos.handles[s].fd == -1) return;
    if (--dos.handles[s].refs > 0) return;
    dos_handle *dh = &dos.handles[s];
    if (dh->fd >= 0 && !dh->dev) {
        win_flush(dh);
        close(dh->fd);
    }
    free(dh->buf); dh->buf = NULL; dh->blen = 0; dh->dlo = dh->dhi = 0;
    dh->fd = -1;
}

void dos_jft_inherit(x86_cpu *c, uint16_t psp) {
    uint16_t seg, off, n = jft_base(c, psp, &seg, &off);
    for (int i = 0; i < (int)n; i++) {
        int s = pc_rd8(c, seg, (uint16_t)(off + i));
        if (s >= 5 && s < DOS_MAX_HANDLES && dos.handles[s].fd != -1) dos.handles[s].refs++;
    }
}
void dos_jft_release(x86_cpu *c, uint16_t psp) {
    uint16_t seg, off, n = jft_base(c, psp, &seg, &off);
    for (int i = 0; i < (int)n; i++) {
        int s = pc_rd8(c, seg, (uint16_t)(off + i));
        if (s != 0xFF) { sft_release(s); pc_wr8(c, seg, (uint16_t)(off + i), 0xFF); }
    }
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
    dos_flush_all();
    trace(c, "open \"%s\" mode %d%s", dos_path, mode, create ? " create" : "");
    int h = new_handle(c);
    if (h < 0) { err(c, DE_TOO_MANY_OPEN); return; }
    dos_handle *dh = &dos.handles[jft_get(c, dos.psp, h)];
#define OPEN_FAIL(e) do { jft_set(c, dos.psp, h, 0xFF); err(c, (e)); return; } while (0)
    uint8_t dev;
    if (is_device_name(dos_path, &dev)) {
        dh->fd = -2;                                  /* devices own no host fd (never close(0)!) */
        dh->dev = dev; dh->binary = 0; dh->mode = (uint8_t)mode; dh->owner_psp = dos.psp;
        snprintf(dh->path, sizeof dh->path, "%s", dos_path);
        trace(c, "open device %s → %d", dos_path, h);
        SET_AX(h); ok(c);
        return;
    }
    char host[DOS_MAX_PATH]; int exists, is_dir;
    int e = dos_resolve(dos_path, host, sizeof host, &exists, &is_dir);
    if (e) OPEN_FAIL(e);
    if (is_dir && exists) OPEN_FAIL(DE_ACCESS_DENIED);
    int flags;
    switch (mode & 7) {
    case 0: flags = O_RDONLY; break;
    case 1: flags = O_WRONLY; break;
    default: flags = O_RDWR; break;
    }
    if (create == 1) flags = O_RDWR | O_CREAT | O_TRUNC;
    else if (create == 2) { if (exists) OPEN_FAIL(DE_FILE_EXISTS); flags = O_RDWR | O_CREAT | O_EXCL; }
    else if (!exists) OPEN_FAIL(DE_FILE_NOT_FOUND);
    int fd = open(host, flags, 0644);
    if (fd < 0 && (flags & O_ACCMODE) != O_RDONLY && errno == EACCES) fd = open(host, O_RDONLY);
    if (fd < 0) OPEN_FAIL(dos_errno());
    dh->fd = fd; dh->dev = 0; dh->binary = 1; dh->mode = (uint8_t)mode; dh->owner_psp = dos.psp;
    dh->drive = (uint8_t)dos_path_drive(dos_path);
    snprintf(dh->path, sizeof dh->path, "%s", host);
    dh->pos = 0; dh->hostacc = fcntl(fd, F_GETFL) & O_ACCMODE;
    dh->buf = NULL; dh->blen = 0; dh->dlo = dh->dhi = 0; dh->bstart = 0;
    win_alias_check(dh);
    if (pc.debug) trace(c, "open %s (%s) mode %02X create %d → %d", dos_path, host, mode, create, h);
    SET_AX(h); ok(c);
}
#undef OPEN_FAIL

static void do_read(x86_cpu *c, int h, uint32_t lin, uint16_t len) {
    dos_handle *dh = handle(c, h);
    if (!dh) { err(c, DE_INVALID_HANDLE); return; }
    if (dh->dev == 1) {
        /* Console: cooked line input unless binary mode. */
        uint16_t n = 0;
        if (dh->binary) {
            while (n < len) { uint8_t ch = con_in(c, 0); x86_phys_wr8(c, lin + n, ch); n++; if (pc_kbd_buffer_empty(c)) break; }
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
            while (n < len && line_pos < line_len) x86_phys_wr8(c, lin + n++, line[line_pos++]);
            if (n && line[line_pos - 1] == 0x1A) n--;   /* Ctrl-Z terminates */
        }
        SET_AX(n); ok(c);
        return;
    }
    if (dh->dev) { SET_AX(0); ok(c); return; }
    /* Straight into guest memory when the range is plain memory (no A20
     * fold, inside the buffer), then the store hook for any byte the
     * code bitmap marks, as x86_phys_wr8 would per byte. */
    uint32_t p = lin & c->a20_mask;
    if (len && (lin & c->a20_mask) + len - 1 == ((lin + len - 1) & c->a20_mask) && p + len <= c->mem_size) {
        ssize_t n = file_read(dh, c->mem + p, len);
        if (n < 0) { err(c, dos_errno()); return; }
        for (ssize_t i = 0; i < n; i++) if (c->code_bitmap[p + (uint32_t)i]) x86_store_hook(c, p + (uint32_t)i);
        SET_AX(n); ok(c);
        return;
    }
    uint8_t buf[65536];
    ssize_t n = file_read(dh, buf, len);
    if (n < 0) { err(c, dos_errno()); return; }
    for (ssize_t i = 0; i < n; i++) x86_phys_wr8(c, lin + (uint32_t)i, buf[i]);
    SET_AX(n); ok(c);
}

static void do_write(x86_cpu *c, int h, uint32_t lin, uint16_t len) {
    dos_handle *dh = handle(c, h);
    if (!dh) { err(c, DE_INVALID_HANDLE); return; }
    if (dh->dev == 1) {
        for (uint16_t i = 0; i < len; i++) con_out(c, x86_phys_rd8(c, lin + i));
        SET_AX(len); ok(c);
        return;
    }
    if (dh->dev) { SET_AX(len); ok(c); return; }
    if (len == 0) {                                     /* truncate (or extend) at the current position */
        win_drop(dh);
        if (ftruncate(dh->fd, dh->pos) < 0) { err(c, dos_errno()); return; }
        SET_AX(0); ok(c); return;
    }
    /* Straight from guest memory unless the range folds at A20, leaves
     * the buffer, or reaches the device window (whose reads the device
     * answers). */
    uint32_t p = lin & c->a20_mask;
    ssize_t n;
    if ((lin & c->a20_mask) + len - 1 == ((lin + len - 1) & c->a20_mask) && p + len <= c->mem_size
        && (!c->device_read || p + len <= 0xA0000u || p >= 0xB0000u)) {
        n = file_write(dh, c->mem + p, len);
    } else {
        uint8_t buf[65536];
        for (uint16_t i = 0; i < len; i++) buf[i] = x86_phys_rd8(c, lin + i);
        n = file_write(dh, buf, len);
    }
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

static void get_path(x86_cpu *c, uint32_t lin, char *out, size_t n) {
    dos_read_str(c, lin, out, n);
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
    /* Anything that looks at a file by name, or at the disk, sees every
     * buffered write first (FCB calls, directories, delete, attributes,
     * EXEC, searches, rename, create, disk reset). */
    switch (AH) {
    case 0x0D: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x21: case 0x22: case 0x23: case 0x24: case 0x27: case 0x28:
    case 0x39: case 0x3A: case 0x3B: case 0x41: case 0x43: case 0x4B: case 0x4E: case 0x4F:
    case 0x56: case 0x5A: case 0x5B: case 0x6C:
        dos_flush_all();
        break;
    default: break;
    }
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
        for (uint32_t i = 0; i < 65536; i++) { uint8_t ch = LRD8(P_DS(p + i)); if (ch == '$') break; con_out(c, ch); }
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
    case 0x0E:                                              /* select disk; AL = LASTDRIVE */
        if (x86_get_r8(c, R_DL) < 26 && dos.drives[x86_get_r8(c, R_DL)].root[0]) dos.cur_drive = x86_get_r8(c, R_DL);
        SET_AL(5);
        break;
    case 0x19: SET_AL(dos.cur_drive); break;
    case 0x1A: dos.dta_seg = DS; dos.dta_off = DX; dos.dta_lin = P_DS(DX); break;
    case 0x1B: case 0x1C:
        if (AH == 0x1C) { int dr = x86_get_r8(c, R_DL) ? x86_get_r8(c, R_DL) - 1 : dos.cur_drive;
                          if (dr < 0 || dr >= 26 || !dos.drives[dr].root[0]) { SET_AL(0xFF); break; } }
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
        int dr = x86_get_r8(c, R_DL) ? x86_get_r8(c, R_DL) - 1 : dos.cur_drive;
        if (dr < 0 || dr >= 26 || !dos.drives[dr].root[0]) { SET_AX(0xFFFF); break; }
        if (statvfs(dos.drives[dr].root, &vs) == 0) {
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
        for (uint32_t i = 0; i < 34; i++) LWR8(P_DS(DX + i), us[i]);
        SET_BX(1); ok(c);
        break;
    }
    case 0x39: case 0x3A: {
        get_path(c, P_DS(DX), path, sizeof path);
        e = dos_resolve(path, host, sizeof host, &exists, &is_dir);
        if (e) { err(c, e); break; }
        if (AH == 0x39) { if (exists) { err(c, DE_ACCESS_DENIED); break; } if (mkdir(host, 0755) < 0) { err(c, dos_errno()); break; } }
        else { if (!exists || !is_dir) { err(c, DE_PATH_NOT_FOUND); break; } if (rmdir(host) < 0) { err(c, dos_errno()); break; } }
        ok(c);
        break;
    }
    case 0x3B: {
        get_path(c, P_DS(DX), path, sizeof path);
        e = dos_resolve(path, host, sizeof host, &exists, &is_dir);
        if (e || !exists || !is_dir) { err(c, DE_PATH_NOT_FOUND); break; }
        /* store the canonical DOS form: host path relative to root, uppercased */
        dos_drive *dv = &dos.drives[dos_path_drive(path)];
        const char *rel = host + strlen(dv->root);
        char cwd[DOS_MAX_PATH]; size_t n = 0; cwd[0] = 0;
        for (const char *p = rel; *p && n + 1 < sizeof cwd; p++) cwd[n++] = (char)(*p == '/' ? '\\' : toupper((unsigned char)*p));
        cwd[n] = 0;
        snprintf(dv->cwd, sizeof dv->cwd, "%s", cwd[0] ? cwd : "\\");
        trace(c, "chdir %s → %s", path, dv->cwd);
        ok(c);
        break;
    }
    case 0x3C: get_path(c, P_DS(DX), path, sizeof path); do_open(c, path, 2, 1, CX); break;
    case 0x3D: get_path(c, P_DS(DX), path, sizeof path); do_open(c, path, AL, 0, 0); break;
    case 0x3E: {
        int s = sft_of(c, BX);
        if (s < 0) { err(c, DE_INVALID_HANDLE); break; }
        jft_set(c, dos.psp, BX, 0xFF);
        sft_release(s);
        ok(c);
        break;
    }
    case 0x3F: do_read(c, BX, P_DS(DX), CX); break;
    case 0x40: do_write(c, BX, P_DS(DX), CX); break;
    case 0x41: {
        get_path(c, P_DS(DX), path, sizeof path);
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
        int64_t r;
        if (AL == 0) r = off;
        else if (AL == 1) r = dh->pos + off;
        else {                                          /* from the end: the size with our dirty bytes in it */
            struct stat st;
            win_flush(dh);
            if (fstat(dh->fd, &st) < 0) { err(c, dos_errno()); break; }
            r = (int64_t)st.st_size + off;
        }
        if (AL > 2 || r < 0) { err(c, DE_INVALID_FN); break; }
        dh->pos = r;
        SET_AX((uint32_t)r); SET_DX((uint32_t)r >> 16); ok(c);
        break;
    }
    case 0x43: {
        get_path(c, P_DS(DX), path, sizeof path);
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
            else SET_DX(dh->drive);                             /* drive number, not written */
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
        case 0x08: {                                          /* removable? BL = drive (0 = current) */
            int dr = x86_get_r8(c, R_BL) ? x86_get_r8(c, R_BL) - 1 : dos.cur_drive;
            if (dr < 0 || dr >= 26 || !dos.drives[dr].root[0]) { err(c, DE_INVALID_DRIVE); break; }
            SET_AX(dr < 2 ? 0 : 1); ok(c);
            break;
        }
        case 0x09: {                                          /* local/remote? BL = drive */
            int dr = x86_get_r8(c, R_BL) ? x86_get_r8(c, R_BL) - 1 : dos.cur_drive;
            if (dr < 0 || dr >= 26 || !dos.drives[dr].root[0]) { err(c, DE_INVALID_DRIVE); break; }
            SET_DX(0); ok(c);
            break;
        }
        case 0x0A: SET_DX(0); ok(c); break;
        case 0x0E: case 0x0F: SET_AL(0); ok(c); break;
        default: err(c, DE_INVALID_FN); break;
        }
        break;
    }
    case 0x45: {                                     /* dup: a new slot naming the same entry (shared position) */
        int s = sft_of(c, BX);
        if (s < 0) { err(c, DE_INVALID_HANDLE); break; }
        uint16_t seg, off, n = jft_base(c, dos.psp, &seg, &off);
        int h = -1;
        for (int i = 0; i < (int)n; i++) if (pc_rd8(c, seg, (uint16_t)(off + i)) == 0xFF) { h = i; break; }
        if (h < 0) { err(c, DE_TOO_MANY_OPEN); break; }
        jft_set(c, dos.psp, h, s);
        if (s >= 5) dos.handles[s].refs++;
        SET_AX(h); ok(c);
        break;
    }
    case 0x46: {                                     /* force dup: CX names the entry BX does */
        int s = sft_of(c, BX), t = CX;
        if (s < 0 || jft_get(c, dos.psp, t) < 0) { err(c, DE_INVALID_HANDLE); break; }
        if (t != (int)BX) {
            int old = jft_get(c, dos.psp, t);
            if (old != 0xFF) sft_release(old);
            jft_set(c, dos.psp, t, s);
            if (s >= 5) dos.handles[s].refs++;
        }
        ok(c);
        break;
    }
    case 0x47: {
        int drive = x86_get_r8(c, R_DL) ? x86_get_r8(c, R_DL) - 1 : dos.cur_drive;
        if (drive < 0 || drive >= 26 || !dos.drives[drive].root[0]) { err(c, DE_INVALID_DRIVE); break; }
        const char *cw = dos.drives[drive].cwd;
        dos_write_str(c, P_DS(SI), cw[0] == '\\' ? cw + 1 : cw);
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
        get_path(c, P_DS(DX), path, sizeof path);
        e = dos_exec(c, path, AL, ES, BX);
        if (e) { trace(c, "EXEC %s failed: %d", path, e); err(c, e); }
        else if (!pc.returned) ok(c);
        break;
    case 0x4C: dos_terminate(c, AL, 0); break;
    case 0x4D: SET_AX(dos.return_code); ok(c); break;
    case 0x4E: {
        get_path(c, P_DS(DX), path, sizeof path);
        uint16_t id;
        e = dos_search_first(path, CX, &id);
        trace(c, "findfirst %s attr %02X → %d", path, CX, e);
        if (e) { err(c, e); break; }
        find_next_into_dta(c, id, CX);
        break;
    }
    case 0x4F: {
        if (LRD8(dos.dta_lin + 3) != 0xC4) { err(c, DE_NO_MORE_FILES); break; }
        find_next_into_dta(c, LRD16(dos.dta_lin), LRD8(dos.dta_lin + 2));
        break;
    }
    case 0x50: dos.psp = BX; ok(c); break;
    case 0x51: case 0x62: SET_BX(dos.psp); ok(c); break;
    case 0x52: x86_load_seg(c, S_ES, DOS_SEG); SET_BX(0x40); break;   /* list of lists: unpopulated */
    case 0x54: SET_AL(dos.verify); break;
    case 0x56: {
        char dst[DOS_MAX_PATH], hdst[DOS_MAX_PATH];
        get_path(c, P_DS(DX), path, sizeof path);
        get_path(c, P_ES(DI), dst, sizeof dst);
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
            if (!dh->dev) win_flush(dh);
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
        get_path(c, P_DS(DX), path, sizeof path);
        char full[DOS_MAX_PATH];
        for (int i = 0; i < 1000; i++) {
            snprintf(full, sizeof full, "%s%sTMP%05d.$$$", path, (path[0] && path[strlen(path) - 1] != '\\') ? "\\" : "", i);
            e = dos_resolve(full, host, sizeof host, &exists, &is_dir);
            if (e) { err(c, e); goto done; }
            if (!exists) break;
        }
        dos_write_str(c, P_DS(DX), full);
        do_open(c, full, 2, 2, CX);
        break;
    }
    case 0x5B: get_path(c, P_DS(DX), path, sizeof path); do_open(c, path, 2, 2, CX); break;
    case 0x5C: ok(c); break;
    case 0x5D: if (AL == 6) { x86_load_seg(c, S_DS, DOS_SEG); SET_SI(0x100); SET_CX(0x80); SET_DX(0x1A); ok(c); } else err(c, DE_INVALID_FN); break;
    case 0x5E: case 0x5F: err(c, DE_INVALID_FN); break;
    case 0x60: {                                            /* truename */
        get_path(c, P_DS(SI), path, sizeof path);
        e = dos_resolve(path, host, sizeof host, &exists, &is_dir);
        if (e) { err(c, e); break; }
        int dr = dos_path_drive(path);
        const char *rel = host + strlen(dos.drives[dr].root);
        char out[DOS_MAX_PATH]; size_t n = (size_t)snprintf(out, sizeof out, "%c:", 'A' + dr);
        for (const char *p = rel; *p && n + 1 < sizeof out; p++) out[n++] = (char)(*p == '/' ? '\\' : toupper((unsigned char)*p));
        if (n == 2) out[n++] = '\\';
        out[n] = 0;
        dos_write_str(c, P_ES(DI), out);
        ok(c);
        break;
    }
    case 0x63: err(c, DE_INVALID_FN); break;
    case 0x65:
        if (AL == 1 && CX >= 5) {
            LWR8(P_ES(DI), 1); LWR16(P_ES(DI + 1), 38);
            pc_wr16(c, ES, (uint16_t)(DI + 3), 1); pc_wr16(c, ES, (uint16_t)(DI + 5), 437);
            ok(c);
        } else err(c, DE_INVALID_FN);
        break;
    case 0x66: if (AL == 1) { SET_BX(437); SET_DX(437); ok(c); } else ok(c); break;
    case 0x67: {                                     /* set handle count: a larger JFT in a block of its own */
        uint16_t seg, off, n = jft_base(c, dos.psp, &seg, &off);
        if (BX <= n) { ok(c); break; }
        uint16_t blk = dos_mem_alloc((uint16_t)((BX + 15) / 16), dos.psp, NULL);
        if (!blk) { err(c, DE_NO_MEMORY); break; }
        for (int i = 0; i < (int)BX; i++) pc_wr8(c, blk, (uint16_t)i, i < (int)n ? pc_rd8(c, seg, (uint16_t)(off + i)) : 0xFF);
        pc_wr16(c, dos.psp, 0x32, BX);
        pc_wr16(c, dos.psp, 0x34, 0); pc_wr16(c, dos.psp, 0x36, blk);
        ok(c);
        break;
    }
    case 0x68: {                                     /* commit */
        dos_handle *dh = handle(c, BX);
        if (!dh) { err(c, DE_INVALID_HANDLE); break; }
        if (!dh->dev) win_flush(dh);
        ok(c);
        break;
    }
    case 0x6C: {
        get_path(c, P_DS(SI), path, sizeof path);
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
    case 0x71:
        /* The long-filename API. Saying "no" here is not the same as saying
         * "invalid function": callers distinguish the two by whether AH is
         * still 71h on return, and a client told merely "invalid" concludes
         * that LFN works and this one call happened to fail. DJGPP then
         * opens every file through 716Ch and finds nothing. */
        trace(c, "no LFN support for INT 21h AX=%02X%02X", AH, AL);
        dos.last_error = DE_INVALID_FN;
        SET_AX(0x7100);
        c->eflags |= X86_CF;
        break;
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
    case 0x16:                                              /* Windows / DPMI */
        if ((ax & 0xFF) == 0x87) { dpmi_int2f_1687(c); break; }
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
