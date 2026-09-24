/* hwflags.c — the host's flags against the interpreter's, instruction by
 * instruction, for the operations whose flags Intel documents as
 * undefined: logic ops (AF), shifts and rotates by more than one
 * (OF, AF), MUL/IMUL/DIV (everything but CF/OF), BSF/BSR, SHLD/SHRD.
 *
 * The x86-64 backend re-emits these natively; where the silicon under it
 * differs from the interpreter's 8086/286/386 contract, the emitter has
 * to fix the bits up when they are live (or -V would see it). This tool
 * says which ones. It builds a tiny native stub per case — load the
 * registers and the flags, run the instruction's own bytes (a 16-bit
 * operand size becomes a 66h prefix), capture RFLAGS — and runs the same
 * bytes through the interpreter. Register-only forms; x86-64 host only.
 *
 *   hwflags [-m 86|286|386] [-n COUNT]
 */
#include "../core/x86.h"
#include "../core/x86_decode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef struct { uint32_t eax, ecx, edx, ebx; uint64_t eflags; uint64_t pad; } regs_t;

static uint8_t *stub;

/* stub: push rbx; mov eax,[rdi]; mov ecx,[rdi+4]; mov edx,[rdi+8]; mov ebx,[rdi+12];
 *       push [rdi+16]; popfq; <insn>; pushfq; pop [rdi+16];
 *       mov [rdi],eax; ... ; pop rbx; ret */
static void build_stub(const uint8_t *insn, int len) {
    static const uint8_t pro[] = { 0x53, 0x8B, 0x07, 0x8B, 0x4F, 0x04, 0x8B, 0x57, 0x08, 0x8B, 0x5F, 0x0C, 0xFF, 0x77, 0x10, 0x9D };
    static const uint8_t epi[] = { 0x9C, 0x8F, 0x47, 0x10, 0x89, 0x07, 0x89, 0x4F, 0x04, 0x89, 0x57, 0x08, 0x89, 0x5F, 0x0C, 0x5B, 0xC3 };
    uint8_t *p = stub;
    memcpy(p, pro, sizeof pro); p += sizeof pro;
    memcpy(p, insn, (size_t)len); p += len;
    memcpy(p, epi, sizeof epi);
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
/* operands that exercise the edge cases: small, all-ones, sign bit, random */
static uint32_t pick(void) {
    switch (rnd() & 7) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 0xFFFFFFFFu;
    case 3: return 0x80000000u >> (rnd() % 32);
    case 4: return rnd() & 0xFF;
    case 5: return rnd() & 0xFFFF;
    default: return rnd();
    }
}

typedef struct { const char *name; uint8_t bytes[8]; int len; int size; } case_t;

/* r/m byte forms: ModRM C0|reg<<3|rm with reg = the /digit or register */
#define MODRM(d, r) (uint8_t)(0xC0 | ((d) << 3) | (r))

int main(int argc, char **argv) {
    int model = X86_MODEL_386, count = 2000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) model = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) count = atoi(argv[++i]);
    }
    if (model == 86) model = X86_MODEL_8086;
    stub = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    x86_cpu cpu;
    x86_init(&cpu, model);

    case_t cases[256]; int nc = 0;
    const char *sizes[] = { "", "b", "w", "", "d" };
    for (int size = 1; size <= 4; size <<= 1) {
        char nm[32];
        int w = size == 1 ? 0 : 1;
        /* logic: and/or/xor/test eax, ecx */
        static const char *alu[] = { "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp" };
        for (int a = 0; a < 8; a++) {
            snprintf(nm, sizeof nm, "%s%s", alu[a], sizes[size]);
            cases[nc] = (case_t){ strdup(nm), { (uint8_t)((a << 3) | w), MODRM(1, 0) }, 2, size }; nc++;
        }
        snprintf(nm, sizeof nm, "test%s", sizes[size]);
        cases[nc] = (case_t){ strdup(nm), { (uint8_t)(0x84 | w), MODRM(1, 0) }, 2, size }; nc++;
        snprintf(nm, sizeof nm, "inc%s", sizes[size]);
        cases[nc] = (case_t){ strdup(nm), { (uint8_t)(0xFE | w), MODRM(0, 0) }, 2, size }; nc++;
        snprintf(nm, sizeof nm, "dec%s", sizes[size]);
        cases[nc] = (case_t){ strdup(nm), { (uint8_t)(0xFE | w), MODRM(1, 0) }, 2, size }; nc++;
        snprintf(nm, sizeof nm, "neg%s", sizes[size]);
        cases[nc] = (case_t){ strdup(nm), { (uint8_t)(0xF6 | w), MODRM(3, 0) }, 2, size }; nc++;
        /* shifts/rotates by 1, by imm (3 and size*8-1 and over-width), by cl */
        static const char *sh[] = { "rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar" };
        for (int s = 0; s < 8; s++) {
            if (s == 6) continue;
            snprintf(nm, sizeof nm, "%s%s,1", sh[s], sizes[size]);
            cases[nc] = (case_t){ strdup(nm), { (uint8_t)(0xD0 | w), MODRM(s, 0) }, 2, size }; nc++;
            int counts[] = { 3, size * 8 - 1, size * 8, size * 8 + 1, 17, 31 };
            for (int k = 0; k < 6; k++) {
                snprintf(nm, sizeof nm, "%s%s,%d", sh[s], sizes[size], counts[k]);
                cases[nc] = (case_t){ strdup(nm), { (uint8_t)(0xC0 | w), MODRM(s, 0), (uint8_t)counts[k] }, 3, size }; nc++;
            }
            snprintf(nm, sizeof nm, "%s%s,cl", sh[s], sizes[size]);
            cases[nc] = (case_t){ strdup(nm), { (uint8_t)(0xD2 | w), MODRM(s, 0) }, 2, size }; nc++;
        }
        /* mul/imul/div/idiv ecx (one-operand) */
        static const char *g3[] = { "mul", "imul", "div", "idiv" };
        for (int g = 0; g < 4; g++) {
            snprintf(nm, sizeof nm, "%s%s", g3[g], sizes[size]);
            cases[nc] = (case_t){ strdup(nm), { (uint8_t)(0xF6 | w), MODRM(4 + g, 1) }, 2, size }; nc++;
        }
        if (size > 1) {
            snprintf(nm, sizeof nm, "imul2%s", sizes[size]);
            cases[nc] = (case_t){ strdup(nm), { 0x0F, 0xAF, MODRM(0, 1) }, 3, size }; nc++;
            snprintf(nm, sizeof nm, "imul3%s", sizes[size]);
            cases[nc] = (case_t){ strdup(nm), { 0x6B, MODRM(0, 1), 0x7B }, 3, size }; nc++;
            snprintf(nm, sizeof nm, "bsf%s", sizes[size]);
            cases[nc] = (case_t){ strdup(nm), { 0x0F, 0xBC, MODRM(0, 1) }, 3, size }; nc++;
            snprintf(nm, sizeof nm, "bsr%s", sizes[size]);
            cases[nc] = (case_t){ strdup(nm), { 0x0F, 0xBD, MODRM(0, 1) }, 3, size }; nc++;
            snprintf(nm, sizeof nm, "shld%s,5", sizes[size]);
            cases[nc] = (case_t){ strdup(nm), { 0x0F, 0xA4, MODRM(1, 0), 5 }, 4, size }; nc++;
            snprintf(nm, sizeof nm, "shrd%s,cl", sizes[size]);
            cases[nc] = (case_t){ strdup(nm), { 0x0F, 0xAD, MODRM(1, 0) }, 3, size }; nc++;
            snprintf(nm, sizeof nm, "bt%s", sizes[size]);
            cases[nc] = (case_t){ strdup(nm), { 0x0F, 0xA3, MODRM(1, 0) }, 3, size }; nc++;
        }
    }

    printf("%-12s %8s  %s\n", "case", "differ", "bits that differ (host vs interpreter), and how often each");
    int total_bad = 0;
    for (int ci = 0; ci < nc; ci++) {
        case_t *cs = &cases[ci];
        uint8_t host[16]; int hl = 0;
        if (cs->size == 2) host[hl++] = 0x66;
        memcpy(host + hl, cs->bytes, (size_t)cs->len); hl += cs->len;
        build_stub(host, hl);
        __builtin___clear_cache((char *)stub, (char *)stub + 64);
        void (*run)(regs_t *) = (void (*)(regs_t *))stub;

        uint8_t guest[16]; int gl = 0;
        if (cs->size == 4 && model < X86_MODEL_386) continue;
        if (cs->size == 4) guest[gl++] = 0x66;         /* 16-bit code segment: 66h selects 32 bits */
        memcpy(guest + gl, cs->bytes, (size_t)cs->len); gl += cs->len;
        x86_dec_ctx ctx = { guest, model, 0 };
        x86_insn in;
        if (!x86_decode(&ctx, &in)) { printf("%-12s undecodable\n", cs->name); continue; }

        uint32_t bad = 0, bad_bits[16] = { 0 }, samples = 0;
        for (int t = 0; t < count; t++) {
            regs_t r = { pick(), pick(), pick(), pick(), (rnd() & X86_ARITH_FLAGS) | X86_F1, 0 };
            if (cs->size == 1 && (cs->bytes[0] == 0xD2 || cs->bytes[0] == 0xD3)) r.ecx = (r.ecx & ~0xFFu) | (rnd() & 0x1F);
            /* divides: a nonzero divisor and a quotient that fits */
            if (!strncmp(cs->name, "div", 3) || !strncmp(cs->name, "idiv", 4)) {
                uint32_t m = cs->size == 1 ? 0xFF : cs->size == 2 ? 0xFFFF : 0xFFFFFFFFu;
                if ((r.ecx & m) == 0) r.ecx |= 1;
                if (cs->size == 1) r.eax &= 0xFF; else r.edx = 0;
                if (!strncmp(cs->name, "idiv", 4)) { if (cs->size == 1) r.eax &= 0x7F; else if (cs->size == 2) r.eax &= 0x7FFF; else r.eax &= 0x7FFFFFFF; }
            }
            regs_t h = r;
            run(&h);
            /* the same on the interpreter */
            cpu.r[R_AX] = r.eax; cpu.r[R_CX] = r.ecx; cpu.r[R_DX] = r.edx; cpu.r[R_BX] = r.ebx;
            cpu.eflags = (cpu.eflags & ~X86_ARITH_FLAGS) | (r.eflags & X86_ARITH_FLAGS);
            cpu.eip = 0x100; cpu.exc = -1;
            x86_exec_decoded(&cpu, &in);
            if (cpu.exc >= 0) { cpu.exc = -1; continue; }
            samples++;
            uint32_t hf = (uint32_t)h.eflags & X86_ARITH_FLAGS, gf = cpu.eflags & X86_ARITH_FLAGS;
            int regs_ok = (h.eax == cpu.r[R_AX]) && (h.ecx == cpu.r[R_CX]) && (h.edx == cpu.r[R_DX]) && (h.ebx == cpu.r[R_BX]);
            if (hf != gf || !regs_ok) {
                bad++;
                for (int b = 0; b < 12; b++) if (((hf ^ gf) >> b) & 1) bad_bits[b]++;
                if (!regs_ok) bad_bits[15]++;
            }
        }
        if (bad) {
            total_bad++;
            printf("%-12s %4u/%-4u ", cs->name, bad, samples);
            static const char *fn[12] = { "CF", "", "PF", "", "AF", "", "ZF", "SF", "", "", "", "OF" };
            for (int b = 0; b < 12; b++) if (bad_bits[b]) printf(" %s:%u", fn[b], bad_bits[b]);
            if (bad_bits[15]) printf(" REGS:%u", bad_bits[15]);
            printf("\n");
        }
    }
    printf("%d of %d cases differ (model %d)\n", total_bad, nc, model);
    return 0;
}
