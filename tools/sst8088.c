/* sst8088.c — run the SingleStepTests 8088 suite (MOO binary format)
 * against the interpreter.
 *
 *   sst8088 [-v] [-n max_fail] [-m metadata.json] file.MOO.gz ...
 *
 * For each test: load registers and RAM, execute exactly one x86_step,
 * compare all registers (flags under the per-opcode undefined mask from
 * metadata.json) and every RAM byte the test lists as final.
 */

#include "../core/x86.h"
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- flag masks from metadata.json ------------------------------------ */
/* mask[opcode][reg]; reg 8 = whole-opcode entry */
static uint16_t flag_mask[256][9];

static void load_metadata(const char *path) {
    for (int i = 0; i < 256; i++) for (int j = 0; j < 9; j++) flag_mask[i][j] = 0xFFFF;
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "warning: no %s, using full flag masks\n", path); return; }
    char line[512];
    int cur_op = -1, cur_reg = 8, depth = 0, in_opcodes = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "\"opcodes\"", 9)) { in_opcodes = 1; depth = 1; continue; }
        if (!in_opcodes) continue;
        /* "XX": {  at depth 0 → opcode; "N": { at depth 1 inside "reg" → reg field */
        if (p[0] == '"') {
            char key[16]; int n = 0;
            for (char *q = p + 1; *q && *q != '"' && n < 15; q++) key[n++] = *q;
            key[n] = 0;
            if (depth == 1 && n == 2 && strchr(p, '{')) { cur_op = (int)strtol(key, NULL, 16); cur_reg = 8; }
            else if (depth == 3 && n == 1 && strchr(p, '{')) cur_reg = key[0] - '0';
            else if (!strcmp(key, "flags-mask") && cur_op >= 0) {
                char *v = strchr(p, ':');
                if (v) flag_mask[cur_op][cur_reg] = (uint16_t)strtol(v + 1, NULL, 10);
            }
        }
        for (char *q = p; *q; q++) { if (*q == '{') depth++; else if (*q == '}') depth--; }
        if (depth < 0) break;
    }
    fclose(f);
}

/* ---- MOO parsing -------------------------------------------------------- */
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

typedef struct {
    uint16_t regs[14];
    uint16_t present;
    uint32_t nram;
    const uint8_t *ram;   /* 5-byte entries */
} state_t;

typedef struct {
    uint32_t idx;
    const char *name; uint32_t name_len;
    const uint8_t *bytes; uint32_t nbytes;
    state_t init, fin;
} test_t;

static void parse_state(const uint8_t *p, uint32_t len, state_t *s) {
    memset(s, 0, sizeof *s);
    uint32_t pos = 0;
    while (pos + 8 <= len) {
        const uint8_t *ck = p + pos;
        uint32_t clen = rd32(ck + 4);
        const uint8_t *d = ck + 8;
        if (!memcmp(ck, "REGS", 4)) {
            s->present = rd16(d);
            const uint8_t *q = d + 2;
            for (int i = 0; i < 14; i++) if (s->present & (1 << i)) { s->regs[i] = rd16(q); q += 2; }
        } else if (!memcmp(ck, "RAM ", 4)) {
            s->nram = rd32(d);
            s->ram = d + 4;
        }
        pos += 8 + clen;
    }
}

static int parse_test(const uint8_t *p, uint32_t len, test_t *t) {
    memset(t, 0, sizeof *t);
    t->idx = rd32(p);
    uint32_t pos = 4;
    while (pos + 8 <= len) {
        const uint8_t *ck = p + pos;
        uint32_t clen = rd32(ck + 4);
        const uint8_t *d = ck + 8;
        if (!memcmp(ck, "NAME", 4)) { t->name_len = rd32(d); t->name = (const char *)d + 4; }
        else if (!memcmp(ck, "BYTS", 4)) { t->nbytes = rd32(d); t->bytes = d + 4; }
        else if (!memcmp(ck, "INIT", 4)) parse_state(d, clen, &t->init);
        else if (!memcmp(ck, "FINA", 4)) parse_state(d, clen, &t->fin);
        pos += 8 + clen;
    }
    return 1;
}

static uint8_t *read_gz(const char *path, size_t *out_len) {
    gzFile g = gzopen(path, "rb");
    if (!g) return NULL;
    size_t cap = 1 << 24, len = 0;
    uint8_t *buf = malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; buf = realloc(buf, cap); }
        int n = gzread(g, buf + len, (unsigned)(cap - len));
        if (n <= 0) break;
        len += n;
    }
    gzclose(g);
    *out_len = len;
    return buf;
}

/* ---- running ------------------------------------------------------------ */
/* REGS order: ax bx cx dx cs ss ds es sp bp si di ip flags */
enum { T_AX, T_BX, T_CX, T_DX, T_CS, T_SS, T_DS, T_ES, T_SP, T_BP, T_SI, T_DI, T_IP, T_FL };
static const char *reg_names[14] = { "ax","bx","cx","dx","cs","ss","ds","es","sp","bp","si","di","ip","flags" };

static void apply_regs(x86_cpu *c, const uint16_t *r) {
    x86_set_r16(c, R_AX, r[T_AX]); x86_set_r16(c, R_BX, r[T_BX]);
    x86_set_r16(c, R_CX, r[T_CX]); x86_set_r16(c, R_DX, r[T_DX]);
    x86_set_r16(c, R_SP, r[T_SP]); x86_set_r16(c, R_BP, r[T_BP]);
    x86_set_r16(c, R_SI, r[T_SI]); x86_set_r16(c, R_DI, r[T_DI]);
    x86_load_seg(c, S_CS, r[T_CS]); x86_load_seg(c, S_SS, r[T_SS]);
    x86_load_seg(c, S_DS, r[T_DS]); x86_load_seg(c, S_ES, r[T_ES]);
    c->eip = r[T_IP];
    c->eflags = r[T_FL];
}

static void read_regs(x86_cpu *c, uint16_t *r) {
    r[T_AX] = x86_get_r16(c, R_AX); r[T_BX] = x86_get_r16(c, R_BX);
    r[T_CX] = x86_get_r16(c, R_CX); r[T_DX] = x86_get_r16(c, R_DX);
    r[T_SP] = x86_get_r16(c, R_SP); r[T_BP] = x86_get_r16(c, R_BP);
    r[T_SI] = x86_get_r16(c, R_SI); r[T_DI] = x86_get_r16(c, R_DI);
    r[T_CS] = c->seg[S_CS].sel; r[T_SS] = c->seg[S_SS].sel;
    r[T_DS] = c->seg[S_DS].sel; r[T_ES] = c->seg[S_ES].sel;
    r[T_IP] = (uint16_t)c->eip;
    r[T_FL] = (uint16_t)c->eflags;
}

static int verbose = 0, max_fail = 10;

static int run_test(x86_cpu *c, const test_t *t, uint16_t fmask, int *fail_count) {
    apply_regs(c, t->init.regs);
    c->halted = 0;
    for (uint32_t i = 0; i < t->init.nram; i++) {
        const uint8_t *e = t->init.ram + i * 5;
        c->mem[rd32(e) & 0xFFFFF] = e[4];
    }

    int rc = x86_step(c);

    uint16_t expect[14], got[14];
    memcpy(expect, t->init.regs, sizeof expect);
    for (int i = 0; i < 14; i++) if (t->fin.present & (1 << i)) expect[i] = t->fin.regs[i];
    read_regs(c, got);

    int bad = 0;
    char msg[1024]; int mp = 0;
    for (int i = 0; i < 14; i++) {
        uint16_t m = (i == T_FL) ? fmask : 0xFFFF;
        if ((expect[i] & m) != (got[i] & m)) {
            bad = 1;
            mp += snprintf(msg + mp, sizeof msg - mp, " %s: want %04X got %04X", reg_names[i], expect[i] & m, got[i] & m);
        }
    }
    for (uint32_t i = 0; i < t->fin.nram; i++) {
        const uint8_t *e = t->fin.ram + i * 5;
        uint32_t a = rd32(e) & 0xFFFFF;
        if (c->mem[a] != e[4]) {
            bad = 1;
            if (mp < 900) mp += snprintf(msg + mp, sizeof msg - mp, " [%05X]: want %02X got %02X", a, e[4], c->mem[a]);
        }
    }
    if (rc < 0) { bad = 1; mp += snprintf(msg + mp, sizeof msg - mp, " (step rc=%d)", rc); }

    if (bad && (*fail_count)++ < max_fail) {
        printf("  FAIL #%u %.*s  bytes:", t->idx, (int)t->name_len, t->name);
        for (uint32_t i = 0; i < t->nbytes; i++) printf(" %02X", t->bytes[i]);
        printf("\n       %s\n", msg);
        if (verbose) {
            printf("       init:");
            for (int i = 0; i < 14; i++) printf(" %s=%04X", reg_names[i], t->init.regs[i]);
            printf("\n       ram:");
            for (uint32_t i = 0; i < t->init.nram; i++) {
                const uint8_t *e = t->init.ram + i * 5;
                printf(" %05X=%02X", rd32(e), e[4]);
            }
            printf("\n");
        }
    }

    /* scrub the addresses we know about so tests don't bleed */
    for (uint32_t i = 0; i < t->init.nram; i++) c->mem[rd32(t->init.ram + i * 5) & 0xFFFFF] = 0;
    for (uint32_t i = 0; i < t->fin.nram; i++) c->mem[rd32(t->fin.ram + i * 5) & 0xFFFFF] = 0;
    return !bad;
}

static int run_file(x86_cpu *c, const char *path) {
    size_t len;
    uint8_t *buf = read_gz(path, &len);
    if (!buf) { fprintf(stderr, "cannot read %s\n", path); return -1; }
    if (len < 8 || memcmp(buf, "MOO ", 4)) { fprintf(stderr, "%s: not a MOO file\n", path); free(buf); return -1; }

    /* opcode and optional reg field from the filename: XX.MOO.gz or XX.N.MOO.gz */
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    int opc = (int)strtol(base, NULL, 16);
    int reg = 8;
    if (base[2] == '.' && base[3] >= '0' && base[3] <= '7' && base[4] == '.') reg = base[3] - '0';
    uint16_t fmask = flag_mask[opc][reg];
    if (reg == 8 && fmask == 0xFFFF) {
        /* whole-opcode file for a group opcode: masks per reg may differ; handled per test below */
    }

    uint32_t pos = 8 + rd32(buf + 4);
    int pass = 0, fail = 0, fail_shown = 0;
    while (pos + 8 <= len) {
        const uint8_t *ck = buf + pos;
        uint32_t clen = rd32(ck + 4);
        if (!memcmp(ck, "TEST", 4)) {
            test_t t;
            parse_test(ck + 8, clen, &t);
            uint16_t m = fmask;
            if (reg == 8 && t.nbytes) {
                /* find the modrm byte after prefixes for a per-reg mask */
                uint32_t i = 0;
                while (i < t.nbytes && (t.bytes[i] == 0x26 || t.bytes[i] == 0x2E || t.bytes[i] == 0x36 ||
                                        t.bytes[i] == 0x3E || t.bytes[i] == 0xF0 || t.bytes[i] == 0xF2 || t.bytes[i] == 0xF3)) i++;
                if (i + 1 < t.nbytes) {
                    uint16_t pm = flag_mask[t.bytes[i]][(t.bytes[i + 1] >> 3) & 7];
                    if (pm != 0xFFFF) m = pm;
                }
            }
            if (run_test(c, &t, m, &fail_shown)) pass++; else fail++;
        }
        pos += 8 + clen;
    }
    printf("%-14s %6d pass %6d fail%s\n", base, pass, fail, fail ? "  <---" : "");
    free(buf);
    return fail;
}

int main(int argc, char **argv) {
    const char *meta = "tests/sst8088/metadata.json";
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) max_fail = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) meta = argv[++i];
        else { fprintf(stderr, "usage: %s [-v] [-n maxfail] [-m metadata.json] file.MOO.gz...\n", argv[0]); return 2; }
    }
    load_metadata(meta);

    x86_cpu cpu;
    x86_init(&cpu, X86_MODEL_8086);

    int total_fail = 0, files_bad = 0;
    for (; i < argc; i++) {
        int f = run_file(&cpu, argv[i]);
        if (f > 0) { total_fail += f; files_bad++; }
    }
    printf("== %d failing tests in %d files\n", total_fail, files_bad);
    x86_free(&cpu);
    return total_fail ? 1 : 0;
}
