/* jittest.c — exercise the DBT against the interpreter.
 *
 *   jittest [-m 86|286] [-V] [-S] [-s] -p N        run built-in program N
 *   jittest [-m 86|286] -f COUNT [-r SEED] [-n LEN] fuzz: random straight-line
 *                                                   blocks, JIT vs interp lockstep
 *
 * Programs are hand-assembled here (no nasm on the box); the fuzzer
 * builds random blocks from bytes the decoder accepts and the backend
 * classifies as translatable, ending in HLT. Every run is a -V lockstep
 * run, so a divergence prints the first differing state and the block
 * bytes; the fuzzer then reports the seed to replay it.
 */
#include "../core/x86.h"
#include "../core/x86_decode.h"
#include "../dbt/dbt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/* ---- tiny assembler: bytes + labels + rel8/rel16 fixups ---------------- */
typedef struct { int at, label, size, base; } fixup_t;
typedef struct { uint8_t buf[4096]; int len; int labels[64]; fixup_t fix[128]; int nfix; } asm_t;
static asm_t A;
static void ab(int n, ...) { va_list ap; va_start(ap, n); for (int i = 0; i < n; i++) A.buf[A.len++] = (uint8_t)va_arg(ap, int); va_end(ap); }
#define B(...) ab(sizeof((int[]){__VA_ARGS__}) / sizeof(int), __VA_ARGS__)
static void L(int l) { A.labels[l] = A.len; }
static void addfix(int size, int base) { A.fix[A.nfix++] = (fixup_t){ A.len, 0, size, base }; }
static void REL8(int l)  { addfix(1, A.len + 1); A.fix[A.nfix - 1].label = l; A.buf[A.len++] = 0; }
static void REL16(int l) { addfix(2, A.len + 2); A.fix[A.nfix - 1].label = l; A.buf[A.len++] = 0; A.buf[A.len++] = 0; }
static void ABS16(int l) { addfix(3, 0); A.fix[A.nfix - 1].label = l; A.buf[A.len++] = 0; A.buf[A.len++] = 0; }
static void asm_finish(int org) {
    for (int i = 0; i < A.nfix; i++) {
        int v = A.labels[A.fix[i].label] - (A.fix[i].size == 3 ? -org : A.fix[i].base);
        if (A.fix[i].size == 1) A.buf[A.fix[i].at] = (uint8_t)v;
        else { A.buf[A.fix[i].at] = (uint8_t)v; A.buf[A.fix[i].at + 1] = (uint8_t)(v >> 8); }
    }
}
enum { L_LOOP1, L_SUB1, L_PATCH, L_LOOP2, L_SKIP, L_SUB2, L_DONE, L_L3, L_TAB, L_L4, L_L5, L_STR, L_L6 };

/* Program 0: arithmetic loops, stack, call/ret, memory, flags, SMC. */
static void prog0(void) {
    B(0xB8, 0x34, 0x12);            /* mov ax, 1234h */
    B(0xBB, 0x00, 0x00);            /* mov bx, 0 */
    B(0xB9, 0xE8, 0x03);            /* mov cx, 1000 */
    L(L_LOOP1);
    B(0x01, 0xC3);                  /* add bx, ax */
    B(0x40);                        /* inc ax */
    B(0x11, 0xD8);                  /* adc ax, bx */
    B(0x49);                        /* dec cx */
    B(0x75); REL8(L_LOOP1);         /* jnz loop1 */
    B(0xBE, 0x00, 0x02);            /* mov si, 200h */
    B(0x89, 0x1C);                  /* mov [si], bx */
    B(0x53);                        /* push bx */
    B(0x5A);                        /* pop dx */
    B(0xE8); REL16(L_SUB1);         /* call sub1 */
    B(0xC6, 0x06); ABS16(L_PATCH); B(0x42);   /* mov byte [patch+1], 42h — the +1 is folded below */
    L(L_PATCH);
    B(0xB0, 0x00);                  /* mov al, 0  (patched to mov al,42h) */
    B(0x88, 0x44, 0x04);            /* mov [si+4], al */
    /* string + flags */
    B(0xBF, 0x00, 0x03);            /* mov di, 300h */
    B(0xB9, 0x10, 0x00);            /* mov cx, 16 */
    B(0xB8, 0x41, 0x00);            /* mov ax, 41h */
    B(0xF3, 0xAA);                  /* rep stosb */
    B(0xBE, 0x00, 0x03);            /* mov si, 300h */
    B(0xB9, 0x10, 0x00);            /* mov cx, 16 */
    L(L_LOOP2);
    B(0xAC);                        /* lodsb */
    B(0x3C, 0x41);                  /* cmp al, 41h */
    B(0x75); REL8(L_SKIP);          /* jne skip */
    B(0xFE, 0xC4);                  /* inc ah */
    L(L_SKIP);
    B(0xE2); REL8(L_LOOP2);         /* loop loop2 */
    B(0x9C);                        /* pushf */
    B(0x58);                        /* pop ax */
    B(0xF5);                        /* cmc */
    B(0xF9);                        /* stc */
    B(0xD1, 0xD3);                  /* rcl bx, 1 */
    B(0xD1, 0xE8);                  /* shr ax, 1 */
    B(0xD0, 0xF8);                  /* sar al, 1 */
    B(0xB1, 0x03);                  /* mov cl, 3 */
    B(0xD3, 0xE0);                  /* shl ax, cl */
    B(0xF7, 0xE3);                  /* mul bx */
    B(0x27);                        /* daa */
    B(0x86, 0xC4);                  /* xchg al, ah */
    B(0x98);                        /* cbw */
    B(0x99);                        /* cwd */
    B(0x8C, 0xD8);                  /* mov ax, ds */
    B(0x8E, 0xC0);                  /* mov es, ax */
    B(0x26, 0x8B, 0x1E, 0x00, 0x02);/* mov bx, es:[200h] */
    B(0xF7, 0xD3);                  /* not bx */
    B(0xF7, 0xDB);                  /* neg bx */
    B(0x8D, 0x47, 0x10);            /* lea ax, [bx+10h] */
    B(0x74); REL8(L_DONE);          /* je done (not taken) */
    B(0x81, 0xFB, 0x00, 0x80);      /* cmp bx, 8000h */
    B(0x7C); REL8(L_L3);            /* jl l3 */
    B(0x40);                        /* inc ax */
    L(L_L3);
    B(0x7E); REL8(L_L4);            /* jle l4 */
    B(0x40);
    L(L_L4);
    B(0x76); REL8(L_L5);            /* jbe l5 */
    B(0x40);
    L(L_L5);
    B(0x7A); REL8(L_L6);            /* jp l6 */
    B(0x40);
    L(L_L6);
    B(0xE3, 0x01);                  /* jcxz +1 */
    B(0x40);
    B(0xE1, 0x00);                  /* loope +0 */
    B(0xE0, 0x00);                  /* loopne +0 */
    B(0xFF, 0x16); ABS16(L_TAB);    /* call [tab] → sub2 */
    B(0xBB); ABS16(L_DONE);         /* mov bx, done */
    B(0xFF, 0xE3);                  /* jmp bx */
    B(0x40);                        /* (never) */
    L(L_DONE);
    B(0xF4);                        /* hlt */
    L(L_SUB1);
    B(0x89, 0xF7);                  /* mov di, si */
    B(0x83, 0xC7, 0x02);            /* add di, 2 */
    B(0x89, 0x15);                  /* mov [di], dx */
    B(0xC3);                        /* ret */
    L(L_TAB); ABS16(L_SUB2);        /* dw sub2 */
    L(L_SUB2);
    B(0x50);                        /* push ax */
    B(0x58);                        /* pop ax */
    B(0x83, 0x3E); ABS16(L_TAB); B(0x00);   /* cmp word [tab], 0 */
    B(0xC2, 0x00, 0x00);            /* ret 0 */
    /* fix the "patch+1": the mov byte [patch] writes byte 0 of the
     * instruction; we want byte 1 — adjust the ABS16 fixup target by +1. */
    A.labels[L_PATCH] += 1;
}

/* Program 1: 8086 quirks and helper-class ops. */
static void prog1(void) {
    B(0xB8, 0x99, 0x09);            /* mov ax, 0999h */
    B(0x27);                        /* daa */
    B(0x2F);                        /* das */
    B(0x37);                        /* aaa */
    B(0x3F);                        /* aas */
    B(0xD4, 0x0A);                  /* aam */
    B(0xD5, 0x0A);                  /* aad */
    B(0xB9, 0x05, 0x00);            /* mov cx, 5 */
    B(0xD2, 0xC0);                  /* rol al, cl */
    B(0xD2, 0xD8);                  /* rcr al, cl */
    B(0xD3, 0xC2);                  /* rol dx, cl */
    B(0xD1, 0xF6);                  /* setmo/shl si (8086: setmo) */
    B(0xF6, 0xE1);                  /* mul cl */
    B(0xF6, 0xE9);                  /* imul cl */
    B(0x9F);                        /* lahf */
    B(0x9E);                        /* sahf */
    B(0xBB, 0x00, 0x02);            /* mov bx, 200h */
    B(0xB0, 0x05);                  /* mov al, 5 */
    B(0xD7);                        /* xlat */
    B(0x54);                        /* push sp */
    B(0x8B, 0xE4);                  /* mov sp, sp */
    B(0x5B);                        /* pop bx */
    B(0x9C);                        /* pushf */
    B(0x9D);                        /* popf */
    B(0xF8);                        /* clc */
    B(0xFC);                        /* cld */
    B(0xFD);                        /* std */
    B(0xFC);                        /* cld */
    B(0xC4, 0x1E, 0x00, 0x02);      /* les bx, [200h] */
    B(0xC5, 0x36, 0x00, 0x02);      /* lds si, [200h] */
    B(0x0E);                        /* push cs */
    B(0x1F);                        /* pop ds */
    B(0x06);                        /* push es */
    B(0x07);                        /* pop es */
    B(0xE4, 0x60);                  /* in al, 60h  (refused → interp) */
    B(0xE6, 0x80);                  /* out 80h, al */
    B(0xF4);
}

/* Program 2: benchmark — 50M iterations of a 6-insn loop with a memory
 * op, a call and flag-dependent branches (300M insns). */
static void prog2(void) {
    B(0xB9, 0x00, 0x00);            /* mov cx, 0 */
    B(0xBA, 0xF4, 0x01);            /* mov dx, 500 */
    B(0xBE, 0x00, 0x02);            /* mov si, 200h */
    L(L_LOOP1);
    B(0x01, 0x0C);                  /* add [si], cx */
    B(0x41);                        /* inc cx */
    B(0x75); REL8(L_LOOP1);         /* jnz loop1  (64K inner iterations) */
    B(0xE8); REL16(L_SUB1);         /* call sub1 */
    B(0x4A);                        /* dec dx */
    B(0x75); REL8(L_LOOP1);         /* jnz loop1 */
    B(0xF4);                        /* hlt */
    L(L_SUB1);
    B(0x8B, 0x04);                  /* mov ax, [si] */
    B(0x31, 0xC3);                  /* xor bx, ax */
    B(0xC3);                        /* ret */
}

static const char *progs[] = { "loops/call/smc/strings", "8086 quirks & helper ops", "benchmark loop" };

static int run_prog(int n, int model, int verify, int strict, int stats) {
    memset(&A, 0, sizeof A);
    if (n == 0) prog0(); else if (n == 1) prog1(); else prog2();
    asm_finish(0x100);

    x86_cpu cpu;
    x86_init(&cpu, model);
    x86_load_seg(&cpu, S_CS, 0x1000);
    x86_load_seg(&cpu, S_DS, 0x1000);
    x86_load_seg(&cpu, S_ES, 0x1000);
    x86_load_seg(&cpu, S_SS, 0x1000);
    cpu.r[R_SP] = 0xFFFE;
    cpu.eip = 0x100;
    memcpy(cpu.mem + 0x10100, A.buf, A.len);
    cpu.mem[0x10200] = 0x11; cpu.mem[0x10201] = 0x22;   /* data for les/lds */

    /* Reference run on a copy */
    x86_cpu ref;
    x86_init(&ref, model);
    { uint8_t *m = ref.mem, *bm = ref.code_bitmap; int fd = ref.mem_fd; uint8_t mm = ref.mem_mirrored;
      ref = cpu; ref.mem = m; ref.code_bitmap = bm; ref.mem_fd = fd; ref.mem_mirrored = mm; }
    memcpy(ref.mem, cpu.mem, X86_LOW_SIZE);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!ref.halted && ref.insn_count < 10000000) if (x86_step(&ref) < 0) break;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ref_s = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    x86_dbt dbt;
    if (dbt_init(&dbt, &cpu) < 0) return 1;
    dbt.verify = verify;
    dbt.insn_limit = verify ? 10000000 : 0;
    if (strict) setenv("X86_VERIFY_STRICT", "1", 1);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = dbt_run(&dbt);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double jit_s = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (stats) dbt_print_stats(&dbt, stderr);
    int same = rc == 0 && (!verify || (ref.insn_count == cpu.insn_count && memcmp(ref.r, cpu.r, sizeof ref.r) == 0
               && ref.eflags == cpu.eflags && ref.eip == cpu.eip && memcmp(ref.mem, cpu.mem, X86_LOW_SIZE) == 0));
    printf("prog %d (%s): %s — %llu insns, rc=%d, jit %.3fs (%.0f MIPS)",
           n, progs[n], same ? "OK" : "MISMATCH", (unsigned long long)cpu.insn_count, rc,
           jit_s, (double)cpu.insn_count / jit_s / 1e6);
    if (verify) printf(", interp %.3fs (%.0f MIPS)", ref_s, (double)ref.insn_count / ref_s / 1e6);
    printf("\n");
    if (!same) { fprintf(stderr, "  ref: "); x86_dump(&ref, stderr); fprintf(stderr, "  jit: "); x86_dump(&cpu, stderr); }
    dbt_cleanup(&dbt);
    x86_free(&cpu); x86_free(&ref);
    return same ? 0 : 1;
}

/* ---- fuzzer ------------------------------------------------------------ */
static uint64_t rng_state;
static uint32_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17; return (uint32_t)rng_state; }

/* Accept an instruction into a random block? Straight-line, no
 * segment change, no CS override (would write our own code), no
 * exceptions, no host services except IN/OUT (harmless with no ports). */
static int fuzz_accept(const x86_insn *in) {
    if (in->seg_override == S_CS) return 0;
    switch (in->op) {
    case OP_JMP: case OP_CALL: case OP_RET: case OP_JCC: case OP_JCXZ: case OP_LOOP: case OP_LOOPE: case OP_LOOPNE:
    case OP_JMPF: case OP_CALLF: case OP_RETF: case OP_IRET: case OP_INTO:
    case OP_MOVSEG: case OP_LES: case OP_LDS: case OP_LSS: case OP_LFS: case OP_LGS: case OP_POPF: case OP_HLT:
        return 0;
    case OP_POP: case OP_PUSH:
        return in->ops[0].kind != OPK_SREG;
    case OP_IN: case OP_OUT:
        return 1;
    case OP_INT: case OP_INT3:
        return 1;                    /* every vector points at the IRET stub below */
    default:
        return dbt_classify_op(in) != 0;
    }
}

static int fuzz_one(int model, int len, uint64_t seed, int verbose) {
    rng_state = seed ? seed : 0x9E3779B97F4A7C15ull;
    uint8_t prog[2048]; int plen = 0;
    x86_dec_ctx ctx = { prog, model, 0 };
    (void)ctx;
    for (int i = 0; i < len; ) {
        uint8_t tmp[16];
        x86_insn in;
        for (int k = 0; k < 16; k++) tmp[k] = (uint8_t)rnd();
        /* Bias toward short, common encodings: sometimes force a simple opcode byte. */
        x86_dec_ctx c2 = { tmp, model, 0 };
        if (!x86_decode(&c2, &in) || in.len > 8 || !fuzz_accept(&in)) continue;
        /* 1 in 6: precede it with a conditional that skips exactly this
         * insn. Sometimes two in a row, and sometimes a LOOP/JCXZ rather
         * than a Jcc: a conditional directly after another conditional,
         * or after a compare with a LOOP in between, is where a backend
         * that reuses host condition flags gets caught out. */
        if ((rnd() % 6) == 0) {
            static const uint8_t cc[] = { 0xE0, 0xE1, 0xE2, 0xE3 };   /* LOOPNE/LOOPE/LOOP/JCXZ */
            int dbl = (rnd() % 3) == 0;
            if (dbl) {
                uint32_t r = rnd();
                prog[plen++] = (r & 3) ? (uint8_t)(0x70 + ((r >> 4) & 15)) : cc[(r >> 4) & 3];
                prog[plen++] = (uint8_t)(2 + in.len);
                i++;
            }
            uint32_t r2 = rnd();
            prog[plen++] = (r2 & 3) ? (uint8_t)(0x70 + ((r2 >> 4) & 15)) : cc[(r2 >> 4) & 3];
            prog[plen++] = (uint8_t)in.len;
            i++;
        }
        memcpy(prog + plen, tmp, in.len); plen += in.len; i++;
    }
    prog[plen++] = 0xF4;

    x86_cpu cpu;
    x86_init(&cpu, model);
    x86_load_seg(&cpu, S_CS, 0x1000);
    x86_load_seg(&cpu, S_DS, 0x3000);
    x86_load_seg(&cpu, S_ES, 0x5000);
    x86_load_seg(&cpu, S_SS, 0x7000);
    for (int i = 0; i < 8; i++) cpu.r[i] = rnd() & 0xFFFF;
    cpu.r[R_SP] &= 0xFFFE;
    cpu.eflags = x86_flags_fixup(&cpu, rnd() & 0x0CD5);
    cpu.eip = 0x100;
    memcpy(cpu.mem + 0x10100, prog, plen);
    /* Interrupt landing zone: every vector points at 9000:0000, an IRET
     * back to the instruction after the INT. Exercises the frame push,
     * the CS load and the return edge under -V. */
    for (int v = 0; v < 256; v++) {
        cpu.mem[v * 4 + 0] = 0x00; cpu.mem[v * 4 + 1] = 0x00;
        cpu.mem[v * 4 + 2] = 0x00; cpu.mem[v * 4 + 3] = 0x90;
    }
    cpu.mem[0x90000] = 0xCF;
    for (int i = 0; i < 0x30000; i++) cpu.mem[0x30000 + i] = (uint8_t)rnd();
    for (int i = 0; i < 0x10000; i++) cpu.mem[0x70000 + i] = (uint8_t)rnd();

    if (verbose) {
        fprintf(stderr, "seed=%llu:", (unsigned long long)seed);
        for (int i = 0; i < plen; i++) fprintf(stderr, " %02X", prog[i]);
        fprintf(stderr, "\n");
    }
    x86_dbt dbt;
    if (dbt_init(&dbt, &cpu) < 0) return 1;
    dbt.verify = 1;
    dbt.insn_limit = 1000000;
    int rc = dbt_run(&dbt);
    if (rc != 0 || verbose) {
        fprintf(stderr, "%s seed=%llu model=%d len=%d:", rc ? "FAIL" : "ok", (unsigned long long)seed, model, len);
        for (int i = 0; i < plen; i++) fprintf(stderr, " %02X", prog[i]);
        fprintf(stderr, "\n");
        if (rc || verbose) {
            uint32_t ip = 0x100; uint8_t buf[16]; x86_insn in; char d[128];
            while (ip < 0x100u + (uint32_t)plen) {
                memcpy(buf, prog + (ip - 0x100), 16);
                x86_dec_ctx c3 = { buf, model, 0 };
                if (!x86_decode(&c3, &in)) break;
                fprintf(stderr, "    %04X: %s\n", ip, x86_disasm(&in, d, sizeof d));
                ip += in.len;
            }
        }
    }
    dbt_cleanup(&dbt);
    x86_free(&cpu);
    return rc != 0;
}

int main(int argc, char **argv) {
    int model = X86_MODEL_8086, prog = -1, fuzz = 0, len = 20, verify = 1, strict = 0, stats = 0, verbose = 0;
    uint64_t seed = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) { int m = atoi(argv[++i]); model = m == 86 ? X86_MODEL_8086 : m == 186 ? X86_MODEL_186 : m == 386 ? X86_MODEL_386 : X86_MODEL_286; }
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) prog = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) fuzz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) len = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-V")) verify = 1;
        else if (!strcmp(argv[i], "-N")) verify = 0;
        else if (!strcmp(argv[i], "-S")) strict = 1;
        else if (!strcmp(argv[i], "-s")) stats = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else { fprintf(stderr, "usage: %s [-m 86|186|286] [-V|-N] [-S] [-s] -p N | -f COUNT [-r SEED] [-n LEN]\n", argv[0]); return 2; }
    }
    if (prog >= 0) return run_prog(prog, model, verify, strict, stats);
    if (fuzz) {
        int fails = 0;
        for (int i = 0; i < fuzz; i++) fails += fuzz_one(model, len, seed + (uint64_t)i, verbose);
        printf("fuzz: %d/%d failed (seeds %llu..%llu, model %d, len %d)\n", fails, fuzz,
               (unsigned long long)seed, (unsigned long long)(seed + fuzz - 1), model, len);
        return fails != 0;
    }
    return 2;
}
