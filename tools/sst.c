/* sst.c — run SingleStepTests MOO suites (8088 v2, 80286 v1, 80386 v1
 * real mode) against the interpreter.
 *
 *   sst [-v] [-n max_fail] [-m metadata.json] file.MOO.gz ...
 *
 * The CPU model comes from each file's header (8088 → 8086 model, C286
 * → 286, 386E → 386). Undefined state is masked with the file/test
 * RMSK/RM32 chunks (286/386) or the 8088 metadata.json flag masks; -M
 * disables masking. For each test: load registers and RAM, execute one
 * x86_step, compare every register the final state lists (or all, if
 * unchanged) and every listed RAM byte.
 */

#include "../core/x86.h"
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- 8088 metadata.json flag masks -------------------------------------- */
static uint16_t flag_mask[256][9];   /* [opcode][reg]; reg 8 = whole-opcode */
static int have_meta;

static void load_metadata(const char *path) {
    for (int i = 0; i < 256; i++) for (int j = 0; j < 9; j++) flag_mask[i][j] = 0xFFFF;
    FILE *f = fopen(path, "r");
    if (!f) return;
    have_meta = 1;
    char line[512];
    int cur_op = -1, cur_reg = 8, depth = 0, in_opcodes = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "\"opcodes\"", 9)) { in_opcodes = 1; depth = 1; continue; }
        if (!in_opcodes) continue;
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
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Unified register file, indexed like RG32:
 * cr0 cr3 eax ebx ecx edx esi edi ebp esp cs ds es fs gs ss eip eflags dr6 dr7 */
enum { G_CR0, G_CR3, G_EAX, G_EBX, G_ECX, G_EDX, G_ESI, G_EDI, G_EBP, G_ESP,
       G_CS, G_DS, G_ES, G_FS, G_GS, G_SS, G_EIP, G_EFL, G_DR6, G_DR7, G_N };
static const char *gname[G_N] = { "cr0","cr3","eax","ebx","ecx","edx","esi","edi","ebp","esp",
                                  "cs","ds","es","fs","gs","ss","eip","eflags","dr6","dr7" };
/* REGS (16-bit) order → unified index */
static const int r16map[14] = { G_EAX, G_EBX, G_ECX, G_EDX, G_CS, G_SS, G_DS, G_ES, G_ESP, G_EBP, G_ESI, G_EDI, G_EIP, G_EFL };

typedef struct {
    uint32_t regs[G_N];
    uint32_t present;        /* bit per unified register */
    uint32_t mask[G_N];      /* undefined-state masks (all ones = defined) */
    uint32_t mask_present;
    uint32_t nram;
    const uint8_t *ram;      /* 5-byte entries */
} state_t;

typedef struct {
    uint32_t idx;
    const char *name; uint32_t name_len;
    const uint8_t *bytes; uint32_t nbytes;
    state_t init, fin;
    int has_exc; uint8_t exc_num; uint32_t exc_flag_addr;
} test_t;

static void parse_regs16(const uint8_t *d, state_t *s, int is_mask) {
    uint16_t bits = rd16(d); const uint8_t *q = d + 2;
    for (int i = 0; i < 14; i++) if (bits & (1 << i)) {
        uint32_t v = rd16(q); q += 2;
        if (is_mask) { s->mask[r16map[i]] = v; s->mask_present |= 1u << r16map[i]; }
        else { s->regs[r16map[i]] = v; s->present |= 1u << r16map[i]; }
    }
}
static void parse_regs32(const uint8_t *d, state_t *s, int is_mask) {
    uint32_t bits = rd32(d); const uint8_t *q = d + 4;
    for (int i = 0; i < G_N; i++) if (bits & (1u << i)) {
        uint32_t v = rd32(q); q += 4;
        if (is_mask) { s->mask[i] = v; s->mask_present |= 1u << i; }
        else { s->regs[i] = v; s->present |= 1u << i; }
    }
}

static void parse_state(const uint8_t *p, uint32_t len, state_t *s) {
    uint32_t pos = 0;
    while (pos + 8 <= len) {
        const uint8_t *ck = p + pos;
        uint32_t clen = rd32(ck + 4);
        const uint8_t *d = ck + 8;
        if (!memcmp(ck, "REGS", 4)) parse_regs16(d, s, 0);
        else if (!memcmp(ck, "RMSK", 4)) parse_regs16(d, s, 1);
        else if (!memcmp(ck, "RG32", 4)) parse_regs32(d, s, 0);
        else if (!memcmp(ck, "RM32", 4)) parse_regs32(d, s, 1);
        else if (!memcmp(ck, "RAM ", 4)) { s->nram = rd32(d); s->ram = d + 4; }
        pos += 8 + clen;
    }
}

static void parse_test(const uint8_t *p, uint32_t len, test_t *t, const state_t *file_masks) {
    memset(t, 0, sizeof *t);
    for (int i = 0; i < G_N; i++) t->init.mask[i] = t->fin.mask[i] = 0xFFFFFFFFu;
    if (file_masks) for (int i = 0; i < G_N; i++) if (file_masks->mask_present & (1u << i)) t->fin.mask[i] = file_masks->mask[i];
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
        else if (!memcmp(ck, "EXCP", 4)) { t->has_exc = 1; t->exc_num = d[0]; t->exc_flag_addr = rd32(d + 1); }
        pos += 8 + clen;
    }
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
static int verbose = 0, max_fail = 10, no_mask = 0;

static void apply_regs(x86_cpu *c, const uint32_t *r) {
    c->r[R_AX] = r[G_EAX]; c->r[R_BX] = r[G_EBX]; c->r[R_CX] = r[G_ECX]; c->r[R_DX] = r[G_EDX];
    c->r[R_SI] = r[G_ESI]; c->r[R_DI] = r[G_EDI]; c->r[R_BP] = r[G_EBP]; c->r[R_SP] = r[G_ESP];
    x86_load_seg(c, S_CS, (uint16_t)r[G_CS]); x86_load_seg(c, S_SS, (uint16_t)r[G_SS]);
    x86_load_seg(c, S_DS, (uint16_t)r[G_DS]); x86_load_seg(c, S_ES, (uint16_t)r[G_ES]);
    x86_load_seg(c, S_FS, (uint16_t)r[G_FS]); x86_load_seg(c, S_GS, (uint16_t)r[G_GS]);
    c->eip = r[G_EIP];
    c->eflags = r[G_EFL];
}

static void read_regs(x86_cpu *c, uint32_t *r) {
    r[G_CR0] = 0; r[G_CR3] = 0; r[G_DR6] = 0; r[G_DR7] = 0;
    r[G_EAX] = c->r[R_AX]; r[G_EBX] = c->r[R_BX]; r[G_ECX] = c->r[R_CX]; r[G_EDX] = c->r[R_DX];
    r[G_ESI] = c->r[R_SI]; r[G_EDI] = c->r[R_DI]; r[G_EBP] = c->r[R_BP]; r[G_ESP] = c->r[R_SP];
    r[G_CS] = c->seg[S_CS].sel; r[G_SS] = c->seg[S_SS].sel; r[G_DS] = c->seg[S_DS].sel;
    r[G_ES] = c->seg[S_ES].sel; r[G_FS] = c->seg[S_FS].sel; r[G_GS] = c->seg[S_GS].sel;
    r[G_EIP] = c->eip;
    r[G_EFL] = c->eflags;
}

static inline uint32_t phys_of(const x86_cpu *c, uint32_t a) { return a & c->a20_mask; }

static int run_test(x86_cpu *c, const test_t *t, uint16_t meta_fmask, int *fail_count) {
    uint32_t init[G_N];
    memcpy(init, t->init.regs, sizeof init);
    apply_regs(c, init);
    c->halted = 0;
    for (uint32_t i = 0; i < t->init.nram; i++) {
        const uint8_t *e = t->init.ram + i * 5;
        uint32_t a = phys_of(c, rd32(e));
        if (a < c->mem_size) c->mem[a] = e[4];
    }

    int rc = x86_step(c);
    /* 286/386 tests follow the instruction with HLT and record the
     * state after it (the exception vectors point at HLTs as well). */
    if (rc == 0 && c->model >= X86_MODEL_286 && !c->halted) {
        uint32_t next = phys_of(c, c->seg[S_CS].base + (c->eip & 0xFFFF));
        if (next < c->mem_size && c->mem[next] == 0xF4) rc = x86_step(c);
    }

    uint32_t expect[G_N], got[G_N];
    memcpy(expect, init, sizeof expect);
    for (int i = 0; i < G_N; i++) if (t->fin.present & (1u << i)) expect[i] = t->fin.regs[i];
    read_regs(c, got);

    int bad = 0;
    char msg[1024]; int mp = 0;
    for (int i = 0; i < G_N; i++) {
        if (i == G_CR0 || i == G_CR3 || i == G_DR6 || i == G_DR7) continue;   /* no PM/debug state in real-mode tests */
        uint32_t m = no_mask ? 0xFFFFFFFFu : t->fin.mask[i];
        if (i == G_EFL && !no_mask && c->model == X86_MODEL_8086) m &= meta_fmask;
        if (i == G_EFL && c->model == X86_MODEL_8086) m &= 0xFFFF;
        if ((expect[i] & m) != (got[i] & m)) {
            bad = 1;
            mp += snprintf(msg + mp, sizeof msg - mp, " %s: want %08X got %08X", gname[i], expect[i] & m, got[i] & m);
        }
    }
    for (uint32_t i = 0; i < t->fin.nram; i++) {
        const uint8_t *e = t->fin.ram + i * 5;
        uint32_t raw = rd32(e), a = phys_of(c, raw);
        if (a >= c->mem_size) continue;
        uint8_t want = e[4], have = c->mem[a];
        /* the flags word pushed by an exception may hold undefined bits */
        if (t->has_exc && !no_mask && (raw == t->exc_flag_addr || raw == t->exc_flag_addr + 1)) {
            uint32_t fm = t->fin.mask[G_EFL];
            if (c->model == X86_MODEL_8086) fm &= meta_fmask;
            uint8_t bm = (uint8_t)(fm >> (raw == t->exc_flag_addr ? 0 : 8));
            want &= bm; have &= bm;
        }
        if (have != want) {
            bad = 1;
            if (mp < 900) mp += snprintf(msg + mp, sizeof msg - mp, " [%06X]: want %02X got %02X", raw, e[4], c->mem[a]);
        }
    }
    if (rc < 0) { bad = 1; mp += snprintf(msg + mp, sizeof msg - mp, " (step rc=%d)", rc); }

    if (bad && (*fail_count)++ < max_fail) {
        printf("  FAIL #%u %.*s  bytes:", t->idx, (int)t->name_len, t->name);
        for (uint32_t i = 0; i < t->nbytes; i++) printf(" %02X", t->bytes[i]);
        if (t->has_exc) printf("  (exception %u)", t->exc_num);
        printf("\n       %s\n", msg);
        if (verbose) {
            printf("       init:");
            for (int i = 0; i < G_N; i++) if (t->init.present & (1u << i)) printf(" %s=%X", gname[i], t->init.regs[i]);
            printf("\n       ram:");
            for (uint32_t i = 0; i < t->init.nram; i++) {
                const uint8_t *e = t->init.ram + i * 5;
                printf(" %06X=%02X", rd32(e), e[4]);
            }
            printf("\n");
        }
    }

    for (uint32_t i = 0; i < t->init.nram; i++) { uint32_t a = phys_of(c, rd32(t->init.ram + i * 5)); if (a < c->mem_size) c->mem[a] = 0; }
    for (uint32_t i = 0; i < t->fin.nram; i++) { uint32_t a = phys_of(c, rd32(t->fin.ram + i * 5)); if (a < c->mem_size) c->mem[a] = 0; }
    return !bad;
}

static int model_for_cpu(const char *id) {
    if (!strncmp(id, "8088", 4) || !strncmp(id, "8086", 4) || !strncmp(id, "88 ", 3)) return X86_MODEL_8086;   /* the 8088 v2 files say "88  " */
    if (!strncmp(id, "V20", 3) || !strncmp(id, "V30", 3) || !strncmp(id, "186", 3)) return X86_MODEL_186;
    if (!strncmp(id, "C286", 4)) return X86_MODEL_286;
    if (!strncmp(id, "386", 3)) return X86_MODEL_386;
    return -1;
}

static int run_file(x86_cpu **cpup, const char *path) {
    size_t len;
    uint8_t *buf = read_gz(path, &len);
    if (!buf) { fprintf(stderr, "cannot read %s\n", path); return -1; }
    if (len < 8 || memcmp(buf, "MOO ", 4)) { fprintf(stderr, "%s: not a MOO file\n", path); free(buf); return -1; }
    uint32_t hlen = rd32(buf + 4);
    int model = hlen >= 8 ? model_for_cpu((const char *)buf + 8 + hlen - 4) : X86_MODEL_8086;   /* cpu id is the header's last 4 bytes (v1.0 has no reserved pair) */
    if (model < 0) { fprintf(stderr, "%s: unknown cpu id %.4s\n", path, buf + 8 + hlen - 4); free(buf); return -1; }
    x86_cpu *c = *cpup;
    if (!c || c->model != model) {
        if (c) { x86_free(c); free(c); }
        c = calloc(1, sizeof *c);
        x86_init(c, model);
        if (model >= X86_MODEL_286) x86_set_a20(c, 1);   /* 24/32-bit address lines */
        *cpup = c;
    }

    /* opcode and optional reg field from the filename for the 8088 metadata masks */
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    int opc = (int)strtol(base, NULL, 16);
    int reg = 8;
    if (base[2] == '.' && base[3] >= '0' && base[3] <= '7' && base[4] == '.') reg = base[3] - '0';
    uint16_t fmask = (model == X86_MODEL_8086 && have_meta) ? flag_mask[opc & 0xFF][reg] : 0xFFFF;

    state_t file_masks; memset(&file_masks, 0, sizeof file_masks);
    uint32_t pos = 8 + hlen;
    int pass = 0, fail = 0, fail_shown = 0;
    while (pos + 8 <= len) {
        const uint8_t *ck = buf + pos;
        uint32_t clen = rd32(ck + 4);
        if (!memcmp(ck, "RMSK", 4)) parse_regs16(ck + 8, &file_masks, 1);
        else if (!memcmp(ck, "RM32", 4)) parse_regs32(ck + 8, &file_masks, 1);
        else if (!memcmp(ck, "TEST", 4)) {
            test_t t;
            parse_test(ck + 8, clen, &t, &file_masks);
            uint16_t m = fmask;
            if (model == X86_MODEL_8086 && have_meta && reg == 8 && t.nbytes) {
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
    printf("%-16s %6d pass %6d fail%s\n", base, pass, fail, fail ? "  <---" : "");
    free(buf);
    return fail;
}

int main(int argc, char **argv) {
    const char *meta = NULL;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-M")) no_mask = 1;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) max_fail = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) meta = argv[++i];
        else { fprintf(stderr, "usage: %s [-v] [-M] [-n maxfail] [-m metadata.json] file.MOO.gz...\n", argv[0]); return 2; }
    }
    if (meta) load_metadata(meta);
    else if (i < argc) {
        /* default: metadata.json next to the first file, if any (8088 suite) */
        char p[4096]; const char *s = strrchr(argv[i], '/');
        snprintf(p, sizeof p, "%.*smetadata.json", s ? (int)(s - argv[i] + 1) : 0, argv[i]);
        load_metadata(p);
    }

    x86_cpu *cpu = NULL;
    int total_fail = 0, files_bad = 0;
    for (; i < argc; i++) {
        int f = run_file(&cpu, argv[i]);
        if (f > 0) { total_fail += f; files_bad++; }
    }
    printf("== %d failing tests in %d files\n", total_fail, files_bad);
    if (cpu) { x86_free(cpu); free(cpu); }
    return total_fail ? 1 : 0;
}
