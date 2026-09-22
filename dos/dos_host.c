/* dos_host.c — the host directory as drive C:.
 *
 * Path resolution walks the DOS path one component at a time and finds
 * each on the host case-insensitively, so C:\WP51\MACROS\FOO.WPM opens
 * wp51/macros/Foo.wpm. Names that don't fit 8.3 are invisible to the
 * guest (skipped in searches, unresolvable), which keeps the illusion
 * of a DOS volume without inventing ~1 names.
 */
#include "dos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

int dos_errno(void) {
    switch (errno) {
    case ENOENT: return DE_FILE_NOT_FOUND;
    case ENOTDIR: return DE_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: return DE_ACCESS_DENIED;
    case EMFILE: case ENFILE: return DE_TOO_MANY_OPEN;
    case EEXIST: return DE_FILE_EXISTS;
    case EBADF: return DE_INVALID_HANDLE;
    case ENOTEMPTY: return DE_ACCESS_DENIED;
    default: return DE_ACCESS_DENIED;
    }
}

/* Upper-case 8.3 form of a host name, "" when it doesn't fit. */
void dos_shortname(const char *name, char *out) {
    out[0] = 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) { strcpy(out, name); return; }
    const char *dot = strrchr(name, '.');
    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext_len = dot ? strlen(dot + 1) : 0;
    if (base_len == 0 || base_len > 8 || ext_len > 3) return;
    if (dot && strchr(name, '.') != dot) return;          /* two dots */
    size_t n = 0;
    for (const char *p = name; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch <= ' ' || ch >= 0x7F || strchr("\"*+,/:;<=>?[\\]|", ch)) return;
        out[n++] = (char)toupper(ch);
    }
    out[n] = 0;
}

/* Find `comp` (case-insensitive) in host dir `dir`; writes the host
 * spelling into `found`. */
static int find_component(const char *dir, const char *comp, char *found, size_t n) {
    DIR *d = opendir(dir[0] ? dir : ".");
    if (!d) return 0;
    struct dirent *e;
    int ok = 0;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, comp)) { snprintf(found, n, "%s", e->d_name); ok = 1; break; }
    }
    closedir(d);
    if (!ok) {
        /* DOS name with a trailing dot or no extension: "FOO." == "FOO" */
        size_t l = strlen(comp);
        if (l && comp[l - 1] == '.') {
            char t[64]; snprintf(t, sizeof t, "%.*s", (int)(l - 1), comp);
            return find_component(dir, t, found, n);
        }
    }
    return ok;
}

int dos_path_drive(const char *p) {
    while (*p == ' ') p++;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') return toupper((unsigned char)p[0]) - 'A';
    return dos.cur_drive;
}

/* Normalise a DOS path (drive, relative/absolute, . and ..) into an
 * absolute DOS path "\A\B\C" without drive. Returns 0 on a bad drive. */
static int canon(const char *in, char *out, size_t n, int *drive) {
    char comps[32][64]; int nc = 0;
    const char *p = in;
    while (*p == ' ') p++;
    *drive = dos_path_drive(p);
    if (*drive < 0 || *drive >= 26 || !dos.drives[*drive].root[0]) return 0;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') p += 2;
    if (*p != '\\' && *p != '/') {
        /* relative: start from that drive's cwd */
        const char *c = dos.drives[*drive].cwd;
        while (*c) {
            while (*c == '\\') c++;
            if (!*c) break;
            size_t l = strcspn(c, "\\");
            snprintf(comps[nc++], 64, "%.*s", (int)l, c);
            c += l;
        }
    }
    while (*p) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        size_t l = strcspn(p, "\\/");
        char comp[64]; snprintf(comp, sizeof comp, "%.*s", (int)(l < 63 ? l : 63), p);
        p += l;
        /* trim trailing spaces */
        size_t k = strlen(comp); while (k && comp[k - 1] == ' ') comp[--k] = 0;
        if (!strcmp(comp, ".") || !*comp) continue;
        if (!strcmp(comp, "..")) { if (nc) nc--; continue; }
        if (nc < 32) strcpy(comps[nc++], comp);
    }
    size_t o = 0; out[0] = 0;
    for (int i = 0; i < nc; i++) o += (size_t)snprintf(out + o, n - o, "\\%s", comps[i]);
    if (!nc) snprintf(out, n, "\\");
    return 1;
}

/* Resolve a DOS path to a host path. The final component need not
 * exist (creation); *exists / *is_dir describe what was found. Returns
 * a DOS error code, 0 on success (even when the leaf is missing). */
int dos_resolve(const char *dos_path, char *host, size_t n, int *exists, int *is_dir) {
    char abs[DOS_MAX_PATH]; int drive;
    if (!canon(dos_path, abs, sizeof abs, &drive)) return DE_INVALID_DRIVE;
    snprintf(host, n, "%s", dos.drives[drive].root);
    *exists = 1; *is_dir = 1;
    const char *p = abs;
    while (*p) {
        while (*p == '\\') p++;
        if (!*p) break;
        size_t l = strcspn(p, "\\");
        char comp[64]; snprintf(comp, sizeof comp, "%.*s", (int)l, p);
        p += l;
        int last = *p == 0;
        char found[256];
        if (find_component(host, comp, found, sizeof found)) {
            size_t hl = strlen(host);
            snprintf(host + hl, n - hl, "/%s", found);
            struct stat st;
            if (stat(host, &st) != 0) return DE_PATH_NOT_FOUND;
            *is_dir = S_ISDIR(st.st_mode);
            if (!last && !*is_dir) return DE_PATH_NOT_FOUND;
        } else {
            if (!last) return DE_PATH_NOT_FOUND;
            size_t hl = strlen(host);
            snprintf(host + hl, n - hl, "/%s", comp);
            *exists = 0; *is_dir = 0;
        }
    }
    return DE_OK;
}

/* 8.3 wildcard match: pattern and name are upper-case, name may be
 * "FOO" or "FOO.BAR". '*' expands to '?'s within its field. */
static void split83(const char *s, char base[9], char ext[4]) {
    const char *dot = strchr(s, '.');
    size_t bl = dot ? (size_t)(dot - s) : strlen(s);
    if (bl > 8) bl = 8;
    memcpy(base, s, bl); base[bl] = 0;
    ext[0] = 0;
    if (dot) snprintf(ext, 4, "%.3s", dot + 1);
}
static int match_field(const char *pat, const char *s, int width) {
    for (int i = 0; i < width; i++) {
        char p = *pat, ch = *s;
        if (p == '*') return 1;
        if (p == '?') { if (ch) s++; if (*pat) pat++; continue; }
        if (!p && !ch) return 1;
        if (toupper((unsigned char)p) != toupper((unsigned char)ch)) return 0;
        pat++; s++;
    }
    return 1;
}
int dos_match(const char *pattern, const char *name) {
    char pb[9], pe[4], nb[9], ne[4];
    split83(pattern, pb, pe);
    split83(name, nb, ne);
    if (!strcmp(pattern, "*.*") || !strcmp(pattern, "*")) return 1;
    return match_field(pb, nb, 8) && match_field(pe, ne, 3);
}

uint16_t dos_ftime(time_t t, uint16_t *date) {
    struct tm tm; localtime_r(&t, &tm);
    int y = tm.tm_year + 1900; if (y < 1980) y = 1980;
    *date = (uint16_t)(((y - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    return (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
}

/* ---- Directory searches ------------------------------------------------ */
typedef struct {
    int    in_use;
    char   host_dir[DOS_MAX_PATH];
    char   pattern[16];
    int    attr;
    int    pos;
    int    count;
    char (*names)[256];
} search_t;
static search_t searches[64];

int dos_search_first(const char *dos_spec, int attr, uint16_t *id) {
    char spec[DOS_MAX_PATH];
    snprintf(spec, sizeof spec, "%s", dos_spec);
    /* split directory and pattern */
    char *sep = strrchr(spec, '\\'); char *sep2 = strrchr(spec, '/');
    if (sep2 > sep) sep = sep2;
    char dir[DOS_MAX_PATH], pat[64];
    if (sep) { snprintf(pat, sizeof pat, "%s", sep + 1); *sep = 0; if (!*spec || spec[strlen(spec) - 1] == ':') strcat(spec, "\\"); snprintf(dir, sizeof dir, "%s", spec); }
    else { snprintf(pat, sizeof pat, "%s", spec); dir[0] = 0; }
    for (char *p = pat; *p; p++) *p = (char)toupper((unsigned char)*p);
    if (!*pat) strcpy(pat, "*.*");

    char host[DOS_MAX_PATH]; int exists, is_dir;
    int err = dos_resolve(dir[0] ? dir : ".", host, sizeof host, &exists, &is_dir);
    if (err) return err;
    if (!exists || !is_dir) return DE_PATH_NOT_FOUND;

    int i;
    for (i = 0; i < 64 && searches[i].in_use; i++) { }
    if (i == 64) { i = 0; free(searches[0].names); }   /* recycle the oldest */
    search_t *s = &searches[i];
    memset(s, 0, sizeof *s);
    s->in_use = 1;
    snprintf(s->host_dir, sizeof s->host_dir, "%s", host);
    snprintf(s->pattern, sizeof s->pattern, "%s", pat);
    s->attr = attr;
    DIR *d = opendir(host);
    if (!d) return DE_PATH_NOT_FOUND;
    struct dirent *e;
    int cap = 64;
    s->names = malloc((size_t)cap * 256);
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sn[16]; dos_shortname(e->d_name, sn);
        if (!sn[0] || !dos_match(pat, sn)) continue;
        if (s->count == cap) { cap *= 2; s->names = realloc(s->names, (size_t)cap * 256); }
        snprintf(s->names[s->count++], 256, "%s", e->d_name);
    }
    closedir(d);
    *id = (uint16_t)i;
    return DE_OK;
}

int dos_search_next(uint16_t id, char *name13, int *attr, uint32_t *size, time_t *mtime) {
    if (id >= 64 || !searches[id].in_use) return DE_NO_MORE_FILES;
    search_t *s = &searches[id];
    while (s->pos < s->count) {
        const char *hn = s->names[s->pos++];
        char path[DOS_MAX_PATH]; snprintf(path, sizeof path, "%s/%s", s->host_dir, hn);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        int a = S_ISDIR(st.st_mode) ? DA_DIR : DA_ARCHIVE;
        if (!(st.st_mode & S_IWUSR)) a |= DA_RDONLY;
        if (hn[0] == '.') a |= DA_HIDDEN;
        /* DOS semantics: directories/hidden/system entries appear only
         * if the search attribute includes them; plain files always. */
        if ((a & (DA_DIR | DA_HIDDEN | DA_SYSTEM)) & ~s->attr) continue;
        dos_shortname(hn, name13);
        *attr = a;
        *size = S_ISDIR(st.st_mode) ? 0 : (uint32_t)st.st_size;
        *mtime = st.st_mtime;
        return DE_OK;
    }
    dos_search_close(id);
    return DE_NO_MORE_FILES;
}

void dos_search_close(uint16_t id) {
    if (id >= 64) return;
    free(searches[id].names);
    searches[id].names = NULL;
    searches[id].in_use = 0;
}

void dos_read_str(x86_cpu *c, uint16_t seg, uint16_t off, char *out, size_t n) {
    size_t i = 0;
    for (; i + 1 < n; i++) {
        uint8_t ch = pc_rd8(c, seg, (uint16_t)(off + i));
        if (!ch) break;
        out[i] = (char)ch;
    }
    out[i] = 0;
}
void dos_write_str(x86_cpu *c, uint16_t seg, uint16_t off, const char *s) {
    for (size_t i = 0; ; i++) {
        pc_wr8(c, seg, (uint16_t)(off + i), (uint8_t)s[i]);
        if (!s[i]) break;
    }
}
