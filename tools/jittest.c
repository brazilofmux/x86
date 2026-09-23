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
/* 32-bit absolute address of label + addend (flat protected mode) */
static void ABS32(int l, int add) { addfix(4, add); A.fix[A.nfix - 1].label = l; for (int k = 0; k < 4; k++) A.buf[A.len++] = 0; }
static void asm_finish(int org) {
    for (int i = 0; i < A.nfix; i++) {
        int sz = A.fix[i].size;
        int v = A.labels[A.fix[i].label] - (sz == 3 ? -org : sz == 4 ? -(org + A.fix[i].base) : A.fix[i].base);
        int n = sz == 1 ? 1 : sz == 4 ? 4 : 2;
        for (int k = 0; k < n; k++) A.buf[A.fix[i].at + k] = (uint8_t)(v >> (8 * k));
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
    case OP_LSS: case OP_LFS: case OP_LGS: case OP_POPF: case OP_HLT:
        return 0;
    case OP_LES: case OP_LDS:
        return 1;              /* DS/ES anywhere in the first megabyte: all of it is memory here */
    case OP_MOVSEG:            /* loads of DS and ES; stores of any segment register */
        return in->ops[0].kind != OPK_SREG || in->ops[0].reg == S_DS || in->ops[0].reg == S_ES;
    case OP_POP:
        return in->ops[0].kind != OPK_SREG || in->ops[0].reg == S_DS || in->ops[0].reg == S_ES;
    case OP_PUSH:
        return in->ops[0].kind != OPK_SREG;
    case OP_IN: case OP_OUT:
        return 1;
    case OP_INT: case OP_INT3:
        return 1;                    /* every vector points at the IRET stub below */
    default:
        return dbt_classify_op(in) != 0;
    }
}

/* Flat 32-bit protected mode (-P): what may go into a random block. No
 * control transfer, no segment load, nothing that raises (DIV, BOUND,
 * INTO, #UD) or changes TF/IOPL (POPF), no gates. */
static int fuzz_accept_pm(const x86_insn *in) {
    if (in->seg_override == S_CS || in->seg_override == S_FS || in->seg_override == S_GS) return 0;
    if (in->lock) return 0;
    /* ECX is a random 32-bit value as soon as anything writes it: a REP
     * string op could then run for 4G iterations (on both machines). The
     * string ops are helpers anyway. */
    if (in->rep && (in->op == OP_MOVS || in->op == OP_CMPS || in->op == OP_STOS || in->op == OP_LODS || in->op == OP_SCAS))
        return 0;
    switch (in->op) {
    case OP_JMP: case OP_CALL: case OP_RET: case OP_JCC: case OP_JCXZ: case OP_LOOP: case OP_LOOPE: case OP_LOOPNE:
    case OP_JMPF: case OP_CALLF: case OP_RETF: case OP_IRET: case OP_INTO: case OP_INT: case OP_INT3:
    case OP_MOVSEG: case OP_LES: case OP_LDS: case OP_LSS: case OP_LFS: case OP_LGS: case OP_POPF: case OP_HLT:
    case OP_AAM: case OP_BOUND: case OP_ENTER: case OP_UD:
        return 0;
    case OP_DIV: case OP_IDIV:
        return in->ops[0].size == 4;   /* #DE goes through the IDT to a HLT (pm_flat_setup) */
    case OP_POP: case OP_PUSH:
        return in->ops[0].kind != OPK_SREG;
    case OP_OUT:
        return 1;                    /* no ports on this machine: exercises the port thunk */
    case OP_IN: case OP_INS: case OP_OUTS:
        return 0;
    default:
        return dbt_classify_op_pm(in) != 0;
    }
}

/* One random flat-PM block: CS/DS/ES/SS all base 0, limit 4G, 32-bit;
 * code at CODE_PM in extended memory, the general registers pointing
 * into a random data window (ECX kept small: it is REP's and LOOP's
 * count), so memory operands mostly take the JIT's fast path and
 * sometimes — disp32, ECX as a base — its out-of-range slow path. */
#define CODE_PM 0x120000u
#define DE_STUB_PM (CODE_PM + 0x1000u)
#define DATA_PM 0x400000u

/* A flat 32-bit protected-mode machine: CS/DS/ES/SS base 0, limit 4G. */
static void pm_flat_setup(x86_cpu *cpu) {
    x86_init(cpu, X86_MODEL_386);
    x86_set_a20(cpu, 1);
    cpu->pmode = 1;
    cpu->cr0 |= 1;
    static const uint16_t sels[6] = { 0x10, 0x08, 0x10, 0x10, 0, 0 };
    for (int s = 0; s < 6; s++) {
        x86_seg *g = &cpu->seg[s];
        memset(g, 0, sizeof *g);
        g->sel = sels[s];
        if (!sels[s]) continue;
        g->usable = 1;
        g->base = 0;
        g->limit = 0xFFFFFFFFu;
        g->big = 1;
        g->attr = (uint16_t)((s == S_CS ? 0x9B : 0x93) | 0xC00);
    }
    cpu->gdtr.base = 0x110000; cpu->gdtr.limit = 0x17;
    /* GDT (null, flat code 08, flat data 10) and an IDT whose #DE gate
     * lands on a HLT: a DIV that faults ends the run on both machines. */
    static const uint32_t gdt[6] = { 0, 0, 0x0000FFFF, 0x00CF9B00, 0x0000FFFF, 0x00CF9300 };
    memcpy(cpu->mem + 0x110000, gdt, sizeof gdt);
    uint32_t gate[2] = { (DE_STUB_PM & 0xFFFF) | (0x08u << 16), (DE_STUB_PM & 0xFFFF0000u) | 0x8E00u };
    memcpy(cpu->mem + 0x110100, gate, sizeof gate);
    cpu->idtr.base = 0x110100; cpu->idtr.limit = 7;
    cpu->mem[DE_STUB_PM] = 0xF4;
    cpu->eip = CODE_PM;
}

enum { P_OUTER, P_INNER, P_PATCH1, P_PATCH2 };
/* PM program 0: R_DrawColumn's shape. Per outer pass, store a new step
 * into the imm32 of two `add ebp, imm32` inside the inner loop, then run
 * it. Without run-time immediates every pass invalidates and retranslates
 * the loop. */
static void pmprog0(void) {
    B(0xB9, 0x2C, 0x01, 0x00, 0x00);          /* mov ecx, 300 */
    B(0x31, 0xED);                            /* xor ebp, ebp */
    B(0xBE, 0x00, 0x00, 0x40, 0x00);          /* mov esi, 400000h */
    L(P_OUTER);
    B(0x89, 0xCB);                            /* mov ebx, ecx */
    B(0xC1, 0xE3, 0x17);                      /* shl ebx, 23 */
    B(0xB8); ABS32(P_PATCH1, 2);              /* mov eax, patch1+2 */
    B(0x89, 0x18);                            /* mov [eax], ebx */
    B(0xB8); ABS32(P_PATCH2, 2);              /* mov eax, patch2+2 */
    B(0x89, 0x18);                            /* mov [eax], ebx */
    B(0xBA, 0x14, 0x00, 0x00, 0x00);          /* mov edx, 20 */
    L(P_INNER);
    B(0x89, 0xEF);                            /* mov edi, ebp */
    L(P_PATCH1);
    B(0x81, 0xC5, 0x11, 0x11, 0x11, 0x11);    /* add ebp, 11111111h (patched) */
    B(0xC1, 0xEF, 0x19);                      /* shr edi, 25 */
    B(0x01, 0x3C, 0xBE);                      /* add [esi+edi*4], edi */
    L(P_PATCH2);
    B(0x81, 0xC5, 0x22, 0x22, 0x22, 0x22);    /* add ebp, 22222222h (patched) */
    B(0x4A);                                  /* dec edx */
    B(0x75); REL8(P_INNER);                   /* jnz inner */
    B(0xE2); REL8(P_OUTER);                   /* loop outer */
    B(0xF4);                                  /* hlt */
}

static int run_pm_prog(int n, int stats) {
    (void)n;
    memset(&A, 0, sizeof A);
    pmprog0();
    asm_finish(CODE_PM);
    x86_cpu cpu;
    pm_flat_setup(&cpu);
    cpu.r[R_SP] = DATA_PM + 0x80000;
    memcpy(cpu.mem + CODE_PM, A.buf, A.len);
    x86_dbt dbt;
    if (dbt_init(&dbt, &cpu) < 0) return 1;
    dbt.verify = 1;
    dbt.insn_limit = 10000000;
    int rc = dbt_run(&dbt);
    if (stats) dbt_print_stats(&dbt, stderr);
    printf("pm prog %d (self-patching step): %s — %llu insns, %llu SMC invalidations, %llu blocks translated\n",
           n, rc == 0 && cpu.halted ? "OK" : "FAIL", (unsigned long long)cpu.insn_count,
           (unsigned long long)dbt.smc_invalidations, (unsigned long long)dbt.blocks_translated);
    int bad = rc != 0 || !cpu.halted;
    dbt_cleanup(&dbt);
    x86_free(&cpu);
    return bad;
}
static int fuzz_one_pm(int len, uint64_t seed, int verbose) {
    rng_state = seed ? seed : 0x9E3779B97F4A7C15ull;
    uint8_t prog[2048]; int plen = 0;
    for (int i = 0; i < len; ) {
        uint8_t tmp[16];
        x86_insn in;
        for (int k = 0; k < 16; k++) tmp[k] = (uint8_t)rnd();
        x86_dec_ctx c2 = { tmp, X86_MODEL_386, 1 };
        if (!x86_decode(&c2, &in) || in.len > 10 || !fuzz_accept_pm(&in)) continue;
        if ((rnd() % 6) == 0) {
            static const uint8_t cc[] = { 0xE0, 0xE1, 0xE2, 0xE3 };
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
    pm_flat_setup(&cpu);
    for (int i = 0; i < 8; i++) cpu.r[i] = DATA_PM + (rnd() & 0xFFFFF);
    /* Two registers into conventional memory (and sometimes the VGA
     * window, plain memory here): the low half of the fast path. */
    cpu.r[R_BX] = 0x20000 + (rnd() % 0x90000);
    cpu.r[R_DI] = 0x20000 + (rnd() % 0x90000);
    cpu.r[R_CX] = rnd() & 0xFF;
    cpu.r[R_SP] = DATA_PM + 0x80000 + (rnd() & 0xFFFC);
    cpu.eflags = x86_flags_fixup(&cpu, rnd() & 0x0CD5);
    cpu.eip = CODE_PM;
    memcpy(cpu.mem + CODE_PM, prog, plen);
    for (uint32_t i = 0; i < 0x110000; i++) cpu.mem[DATA_PM + i] = (uint8_t)rnd();
    for (uint32_t i = 0x20000; i < 0xC0000; i++) cpu.mem[i] = (uint8_t)rnd();

    if (verbose) {
        fprintf(stderr, "seed=%llu:", (unsigned long long)seed);
        for (int i = 0; i < plen; i++) fprintf(stderr, " %02X", prog[i]);
        fprintf(stderr, "\n");
    }
    x86_dbt dbt;
    if (dbt_init(&dbt, &cpu) < 0) return 1;
    dbt.verify = 1;
    dbt.verify_mem_every = 1;
    dbt.insn_limit = 1000000;
    int rc = dbt_run(&dbt);
    if (verbose) dbt_print_stats(&dbt, stderr);
    if (rc != 0 || verbose) {
        fprintf(stderr, "%s seed=%llu pm len=%d:", rc ? "FAIL" : "ok", (unsigned long long)seed, len);
        for (int i = 0; i < plen; i++) fprintf(stderr, " %02X", prog[i]);
        fprintf(stderr, "\n");
        uint32_t ip = 0; uint8_t buf[16]; x86_insn in; char d[128];
        while (ip < (uint32_t)plen) {
            memcpy(buf, prog + ip, 16);
            x86_dec_ctx c3 = { buf, X86_MODEL_386, 1 };
            if (!x86_decode(&c3, &in)) break;
            fprintf(stderr, "    %06X: %s\n", CODE_PM + ip, x86_disasm(&in, d, sizeof d));
            ip += in.len;
        }
    }
    dbt_cleanup(&dbt);
    x86_free(&cpu);
    return rc != 0;
}

/* ---- Segmented 16-bit protected mode (-G) ----
 * A 386 with a 16-bit code segment and 16-bit data segments of random
 * limits: what a DOS extender's own code looks like, and the shape of a
 * KEY_SEG16 block. Faults (#GP/#SS past a limit, #DE) reach a 16-bit
 * gate onto a HLT, ending the run on both machines. */
#define CODE16 0x120000u
#define HLT16  0x1000u                     /* offset of the fault landing in CS */
extern int dbt_classify_op_seg16(const x86_insn *in);

static int fuzz_accept_seg16(const x86_insn *in) {
    if (in->seg_override == S_CS || in->seg_override == S_FS || in->seg_override == S_GS) return 0;
    if (in->lock) return 0;
    switch (in->op) {
    case OP_JMP: case OP_CALL: case OP_RET: case OP_JCC: case OP_JCXZ: case OP_LOOP: case OP_LOOPE: case OP_LOOPNE:
    case OP_JMPF: case OP_CALLF: case OP_RETF: case OP_IRET: case OP_INTO: case OP_INT: case OP_INT3:
    case OP_MOVSEG: case OP_LES: case OP_LDS: case OP_LSS: case OP_LFS: case OP_LGS: case OP_HLT:
    case OP_IN: case OP_INS: case OP_OUTS: case OP_AAM: case OP_BOUND: case OP_ENTER: case OP_UD:
        return 0;
    case OP_POP: case OP_PUSH:
        return in->ops[0].kind != OPK_SREG;
    case OP_OUT:
        return 1;
    default:
        return dbt_classify_op_seg16(in) != 0;
    }
}

/* base/limit into a descriptor pair; attr is the access byte, 16-bit (no G, no D) */
static void put_desc(x86_cpu *cpu, uint32_t at, uint32_t base, uint32_t limit, uint32_t attr) {
    uint32_t lo = (limit & 0xFFFF) | ((base & 0xFFFF) << 16);
    uint32_t hi = ((base >> 16) & 0xFF) | (attr << 8) | (((limit >> 16) & 0xF) << 16) | (base & 0xFF000000u);
    memcpy(cpu->mem + at, &lo, 4); memcpy(cpu->mem + at + 4, &hi, 4);
}
static void set_seg16(x86_cpu *cpu, int s, uint16_t sel, uint32_t base, uint32_t limit, uint32_t attr) {
    x86_seg *g = &cpu->seg[s];
    memset(g, 0, sizeof *g);
    g->sel = sel; g->usable = sel != 0; g->base = base; g->limit = limit; g->big = 0; g->attr = (uint16_t)attr;
}

static int fuzz_one_seg16(int len, uint64_t seed, int verbose) {
    rng_state = seed ? seed : 0x9E3779B97F4A7C15ull;
    uint8_t prog[2048]; int plen = 0;
    for (int i = 0; i < len; ) {
        uint8_t tmp[16];
        x86_insn in;
        for (int k = 0; k < 16; k++) tmp[k] = (uint8_t)rnd();
        x86_dec_ctx c2 = { tmp, X86_MODEL_386, 0 };
        if (!x86_decode(&c2, &in) || in.len > 10 || !fuzz_accept_seg16(&in)) continue;
        if ((rnd() % 6) == 0) {
            static const uint8_t cc[] = { 0xE0, 0xE1, 0xE2, 0xE3 };
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
    x86_init(&cpu, X86_MODEL_386);
    x86_set_a20(&cpu, 1);
    cpu.pmode = 1;
    cpu.cr0 |= 1;
    /* limits: mostly full 64K, sometimes small so accesses and pushes fault */
    uint32_t lim_ds = (rnd() & 1) ? 0xFFFF : 0x100 + (rnd() & 0x7FFF);
    uint32_t lim_es = (rnd() & 1) ? 0xFFFF : 0x100 + (rnd() & 0x7FFF);
    uint32_t lim_ss = (rnd() & 3) ? 0xFFFF : 0x100 + (rnd() & 0x7FFF);
    cpu.gdtr.base = 0x110000; cpu.gdtr.limit = 0x27;
    put_desc(&cpu, 0x110008, CODE16, 0xFFFF, 0x9B);
    put_desc(&cpu, 0x110010, 0x30000, lim_ds, 0x93);
    put_desc(&cpu, 0x110018, 0x50000, lim_es, 0x93);
    put_desc(&cpu, 0x110020, 0x70000, lim_ss, 0x93);
    set_seg16(&cpu, S_CS, 0x08, CODE16, 0xFFFF, 0x9B);
    set_seg16(&cpu, S_DS, (rnd() % 8) ? 0x10 : 0, 0x30000, lim_ds, 0x93);   /* sometimes null: #GP on use */
    set_seg16(&cpu, S_ES, (rnd() % 8) ? 0x18 : 0, 0x50000, lim_es, 0x93);
    set_seg16(&cpu, S_SS, 0x20, 0x70000, lim_ss, 0x93);
    if (!cpu.seg[S_DS].sel) { cpu.seg[S_DS].base = 0; cpu.seg[S_DS].limit = 0; cpu.seg[S_DS].attr = 0; }
    if (!cpu.seg[S_ES].sel) { cpu.seg[S_ES].base = 0; cpu.seg[S_ES].limit = 0; cpu.seg[S_ES].attr = 0; }
    /* IDT: 32 16-bit interrupt gates onto the HLT */
    cpu.idtr.base = 0x110100; cpu.idtr.limit = 32 * 8 - 1;
    for (int v = 0; v < 32; v++) {
        uint32_t lo = HLT16 | (0x08u << 16), hi = 0x8600u;
        memcpy(cpu.mem + 0x110100 + v * 8, &lo, 4); memcpy(cpu.mem + 0x110104 + v * 8, &hi, 4);
    }
    cpu.mem[CODE16 + HLT16] = 0xF4;
    for (int i = 0; i < 8; i++) cpu.r[i] = rnd();
    cpu.r[R_SP] = (cpu.r[R_SP] & 0xFFFF0000u) | (0x1000 + (rnd() & 0xEFFE));
    cpu.eflags = x86_flags_fixup(&cpu, rnd() & 0x0CD5);
    cpu.eip = 0x100;
    memcpy(cpu.mem + CODE16 + 0x100, prog, plen);
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
    dbt.verify_mem_every = 1;
    dbt.insn_limit = 1000000;
    int rc = dbt_run(&dbt);
    if (verbose) dbt_print_stats(&dbt, stderr);
    if (rc != 0 || verbose) {
        fprintf(stderr, "%s seed=%llu seg16 len=%d lim ds=%X es=%X ss=%X ds_sel=%X es_sel=%X:", rc ? "FAIL" : "ok", (unsigned long long)seed, len,
                lim_ds, lim_es, lim_ss, cpu.seg[S_DS].sel, cpu.seg[S_ES].sel);
        for (int i = 0; i < plen; i++) fprintf(stderr, " %02X", prog[i]);
        fprintf(stderr, "\n");
        uint32_t ip = 0x100; uint8_t buf[16]; x86_insn in; char d[128];
        while (ip < 0x100u + (uint32_t)plen) {
            memcpy(buf, prog + (ip - 0x100), 16);
            x86_dec_ctx c3 = { buf, X86_MODEL_386, 0 };
            if (!x86_decode(&c3, &in)) break;
            fprintf(stderr, "    %04X: %s\n", ip, x86_disasm(&in, d, sizeof d));
            ip += in.len;
        }
    }
    dbt_cleanup(&dbt);
    x86_free(&cpu);
    return rc != 0;
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
    /* 386: random upper halves too — a 16-bit op on a pinned 32-bit
     * register must neither use nor disturb them. */
    for (int i = 0; i < 8; i++) cpu.r[i] = model >= X86_MODEL_386 ? rnd() : rnd() & 0xFFFF;
    cpu.r[R_SP] &= 0xFFFFFFFEu;
    /* A quarter of the time SI and DI sit at the segment's end, where a
     * word string access straddles offset FFFF: the wrap (8086/186) or
     * the #GP with its pointer-commit rules (286/386) — the string
     * emitters' slow path. */
    if ((rnd() & 3) == 0) {
        cpu.r[R_SI] = (cpu.r[R_SI] & 0xFFFF0000u) | (0xFFFEu + (rnd() & 1));
        cpu.r[R_DI] = (cpu.r[R_DI] & 0xFFFF0000u) | (0xFFFEu + (rnd() & 1));
        cpu.r[R_CX] = (cpu.r[R_CX] & 0xFFFF0000u) | (rnd() & 7);
    }
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
    /* Faults end the run: #DE (0), #SS (12), #GP (13) restart the faulting
     * instruction on the 286+, and an IRET-and-retry would spin until the
     * instruction limit — with -V comparing memory after every one-insn
     * run, minutes per seed. (INT 0/0Ch/0Dh in a program halt it too.) */
    static const int stop_vec[] = { 0, 12, 13 };
    for (int k = 0; k < 3; k++) {
        int v = stop_vec[k];
        cpu.mem[v * 4 + 0] = 0x00; cpu.mem[v * 4 + 1] = 0x00;
        cpu.mem[v * 4 + 2] = 0x00; cpu.mem[v * 4 + 3] = 0x91;
    }
    cpu.mem[0x91000] = 0xF4;
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
    if (verbose) { fprintf(stderr, "insns %llu, halted %d, eip %04X:%04X\n", (unsigned long long)cpu.insn_count, cpu.halted, cpu.seg[S_CS].sel, cpu.eip); dbt_print_stats(&dbt, stderr); }
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
    int model = X86_MODEL_8086, prog = -1, fuzz = 0, len = 20, verify = 1, strict = 0, stats = 0, verbose = 0, pm = 0;
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
        else if (!strcmp(argv[i], "-P")) pm = 1;
        else if (!strcmp(argv[i], "-G")) pm = 2;
        else { fprintf(stderr, "usage: %s [-m 86|186|286] [-V|-N] [-S] [-s] [-P] -p N | -f COUNT [-r SEED] [-n LEN]\n", argv[0]); return 2; }
    }
    if (prog >= 0 && pm) return run_pm_prog(prog, stats);
    if (prog >= 0) return run_prog(prog, model, verify, strict, stats);
    if (fuzz) {
        int fails = 0;
        for (int i = 0; i < fuzz; i++)
            fails += pm == 2 ? fuzz_one_seg16(len, seed + (uint64_t)i, verbose)
                   : pm ? fuzz_one_pm(len, seed + (uint64_t)i, verbose) : fuzz_one(model, len, seed + (uint64_t)i, verbose);
        printf("fuzz: %d/%d failed (seeds %llu..%llu, %s, len %d)\n", fails, fuzz,
               (unsigned long long)seed, (unsigned long long)(seed + fuzz - 1), pm == 2 ? "segmented 16-bit PM" : pm ? "flat PM" : "real mode", len);
        return fails != 0;
    }
    return 2;
}
