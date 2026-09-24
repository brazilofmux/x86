/* x86_decode.c — table-driven 8086..386 instruction decoder
 *
 * One pass: prefixes → opcode (+0F map) → modrm/SIB/disp → immediates.
 * The primary and 0F maps are tables of {op, dst-form, src-form, group};
 * group opcodes are resolved after modrm is read. Model-specific
 * aliasing (8086: 0F=POP CS, 60-6F=Jcc, C0/C1=RET, ...) is applied to
 * the raw opcode byte before the table lookup so the tables stay the
 * 386 truth.
 */

#include "x86.h"
#include "x86_decode.h"
#include <string.h>
#include <stdio.h>

/* Operand forms */
enum {
    F_NONE = 0,
    F_Eb, F_Ev, F_Ew, F_Ed,     /* modrm r/m: byte, v-size, word, dword */
    F_Gb, F_Gv, F_Gw,           /* modrm reg */
    F_Sw,                       /* segment register from modrm reg */
    F_Ib, F_Iv, F_Ibs, F_Iw,    /* immediates: 8, v-size, sign-extended 8, 16 */
    F_AL, F_eAX, F_CL, F_DX, F_AX16,
    F_Jb, F_Jv,                 /* relative branch targets */
    F_Ap,                       /* far pointer immediate */
    F_Ob, F_Ov,                 /* moffs */
    F_M, F_Mp,                  /* memory-only modrm: v-size, far pointer */
    F_Zb, F_Zv,                 /* register in opcode bits 2:0 */
    F_Sop,                      /* segment register in opcode bits 4:3 */
    F_1,                        /* constant 1 */
    F_b, F_v,                   /* no operand, just a size (string ops) */
    F_Cd, F_Dd, F_Td,           /* control/debug/test register from reg field */
    F_Rd,                       /* r/m as 32-bit register (mod must be 3) */
};

/* Groups (resolved after modrm) */
enum {
    G_NONE = 0,
    G_1,        /* 80-83: ALU by reg */
    G_2,        /* C0 C1 D0-D3: rotate by reg */
    G_3,        /* F6 F7 */
    G_4,        /* FE */
    G_5,        /* FF */
    G_8F,       /* POP Ev */
    G_C6,       /* MOV Eb,Ib */
    G_0F00,     /* SLDT.. */
    G_0F01,     /* SGDT.. */
    G_0FBA,     /* BT Ev,Ib .. */
};

typedef struct { uint8_t op, d, s, grp; } opdesc;

#define E(op, d, s)      { op, d, s, G_NONE }
#define G(grp, d, s)     { OP_UD, d, s, grp }
#define X()              { OP_UD, F_NONE, F_NONE, G_NONE }

static const opdesc primary[256] = {
    /* 00 */ E(OP_ADD, F_Eb, F_Gb), E(OP_ADD, F_Ev, F_Gv), E(OP_ADD, F_Gb, F_Eb), E(OP_ADD, F_Gv, F_Ev),
             E(OP_ADD, F_AL, F_Ib), E(OP_ADD, F_eAX, F_Iv), E(OP_PUSH, F_Sop, F_NONE), E(OP_POP, F_Sop, F_NONE),
    /* 08 */ E(OP_OR, F_Eb, F_Gb), E(OP_OR, F_Ev, F_Gv), E(OP_OR, F_Gb, F_Eb), E(OP_OR, F_Gv, F_Ev),
             E(OP_OR, F_AL, F_Ib), E(OP_OR, F_eAX, F_Iv), E(OP_PUSH, F_Sop, F_NONE), X() /* 0F map */,
    /* 10 */ E(OP_ADC, F_Eb, F_Gb), E(OP_ADC, F_Ev, F_Gv), E(OP_ADC, F_Gb, F_Eb), E(OP_ADC, F_Gv, F_Ev),
             E(OP_ADC, F_AL, F_Ib), E(OP_ADC, F_eAX, F_Iv), E(OP_PUSH, F_Sop, F_NONE), E(OP_POP, F_Sop, F_NONE),
    /* 18 */ E(OP_SBB, F_Eb, F_Gb), E(OP_SBB, F_Ev, F_Gv), E(OP_SBB, F_Gb, F_Eb), E(OP_SBB, F_Gv, F_Ev),
             E(OP_SBB, F_AL, F_Ib), E(OP_SBB, F_eAX, F_Iv), E(OP_PUSH, F_Sop, F_NONE), E(OP_POP, F_Sop, F_NONE),
    /* 20 */ E(OP_AND, F_Eb, F_Gb), E(OP_AND, F_Ev, F_Gv), E(OP_AND, F_Gb, F_Eb), E(OP_AND, F_Gv, F_Ev),
             E(OP_AND, F_AL, F_Ib), E(OP_AND, F_eAX, F_Iv), X() /* ES: */, E(OP_DAA, F_NONE, F_NONE),
    /* 28 */ E(OP_SUB, F_Eb, F_Gb), E(OP_SUB, F_Ev, F_Gv), E(OP_SUB, F_Gb, F_Eb), E(OP_SUB, F_Gv, F_Ev),
             E(OP_SUB, F_AL, F_Ib), E(OP_SUB, F_eAX, F_Iv), X() /* CS: */, E(OP_DAS, F_NONE, F_NONE),
    /* 30 */ E(OP_XOR, F_Eb, F_Gb), E(OP_XOR, F_Ev, F_Gv), E(OP_XOR, F_Gb, F_Eb), E(OP_XOR, F_Gv, F_Ev),
             E(OP_XOR, F_AL, F_Ib), E(OP_XOR, F_eAX, F_Iv), X() /* SS: */, E(OP_AAA, F_NONE, F_NONE),
    /* 38 */ E(OP_CMP, F_Eb, F_Gb), E(OP_CMP, F_Ev, F_Gv), E(OP_CMP, F_Gb, F_Eb), E(OP_CMP, F_Gv, F_Ev),
             E(OP_CMP, F_AL, F_Ib), E(OP_CMP, F_eAX, F_Iv), X() /* DS: */, E(OP_AAS, F_NONE, F_NONE),
    /* 40 */ E(OP_INC, F_Zv, F_NONE), E(OP_INC, F_Zv, F_NONE), E(OP_INC, F_Zv, F_NONE), E(OP_INC, F_Zv, F_NONE),
             E(OP_INC, F_Zv, F_NONE), E(OP_INC, F_Zv, F_NONE), E(OP_INC, F_Zv, F_NONE), E(OP_INC, F_Zv, F_NONE),
    /* 48 */ E(OP_DEC, F_Zv, F_NONE), E(OP_DEC, F_Zv, F_NONE), E(OP_DEC, F_Zv, F_NONE), E(OP_DEC, F_Zv, F_NONE),
             E(OP_DEC, F_Zv, F_NONE), E(OP_DEC, F_Zv, F_NONE), E(OP_DEC, F_Zv, F_NONE), E(OP_DEC, F_Zv, F_NONE),
    /* 50 */ E(OP_PUSH, F_Zv, F_NONE), E(OP_PUSH, F_Zv, F_NONE), E(OP_PUSH, F_Zv, F_NONE), E(OP_PUSH, F_Zv, F_NONE),
             E(OP_PUSH, F_Zv, F_NONE), E(OP_PUSH, F_Zv, F_NONE), E(OP_PUSH, F_Zv, F_NONE), E(OP_PUSH, F_Zv, F_NONE),
    /* 58 */ E(OP_POP, F_Zv, F_NONE), E(OP_POP, F_Zv, F_NONE), E(OP_POP, F_Zv, F_NONE), E(OP_POP, F_Zv, F_NONE),
             E(OP_POP, F_Zv, F_NONE), E(OP_POP, F_Zv, F_NONE), E(OP_POP, F_Zv, F_NONE), E(OP_POP, F_Zv, F_NONE),
    /* 60 */ E(OP_PUSHA, F_NONE, F_NONE), E(OP_POPA, F_NONE, F_NONE), E(OP_BOUND, F_Gv, F_M), E(OP_ARPL, F_Ew, F_Gw),
             X() /* FS: */, X() /* GS: */, X() /* opsize */, X() /* adsize */,
    /* 68 */ E(OP_PUSH, F_Iv, F_NONE), E(OP_IMUL3, F_Gv, F_Ev), E(OP_PUSH, F_Ibs, F_NONE), E(OP_IMUL3, F_Gv, F_Ev),
             E(OP_INS, F_b, F_NONE), E(OP_INS, F_v, F_NONE), E(OP_OUTS, F_b, F_NONE), E(OP_OUTS, F_v, F_NONE),
    /* 70 */ E(OP_JCC, F_Jb, F_NONE), E(OP_JCC, F_Jb, F_NONE), E(OP_JCC, F_Jb, F_NONE), E(OP_JCC, F_Jb, F_NONE),
             E(OP_JCC, F_Jb, F_NONE), E(OP_JCC, F_Jb, F_NONE), E(OP_JCC, F_Jb, F_NONE), E(OP_JCC, F_Jb, F_NONE),
    /* 78 */ E(OP_JCC, F_Jb, F_NONE), E(OP_JCC, F_Jb, F_NONE), E(OP_JCC, F_Jb, F_NONE), E(OP_JCC, F_Jb, F_NONE),
             E(OP_JCC, F_Jb, F_NONE), E(OP_JCC, F_Jb, F_NONE), E(OP_JCC, F_Jb, F_NONE), E(OP_JCC, F_Jb, F_NONE),
    /* 80 */ G(G_1, F_Eb, F_Ib), G(G_1, F_Ev, F_Iv), G(G_1, F_Eb, F_Ib), G(G_1, F_Ev, F_Ibs),
             E(OP_TEST, F_Eb, F_Gb), E(OP_TEST, F_Ev, F_Gv), E(OP_XCHG, F_Eb, F_Gb), E(OP_XCHG, F_Ev, F_Gv),
    /* 88 */ E(OP_MOV, F_Eb, F_Gb), E(OP_MOV, F_Ev, F_Gv), E(OP_MOV, F_Gb, F_Eb), E(OP_MOV, F_Gv, F_Ev),
             E(OP_MOVSEG, F_Ew, F_Sw), E(OP_LEA, F_Gv, F_M), E(OP_MOVSEG, F_Sw, F_Ew), G(G_8F, F_Ev, F_NONE),
    /* 90 */ E(OP_NOP, F_NONE, F_NONE), E(OP_XCHG, F_eAX, F_Zv), E(OP_XCHG, F_eAX, F_Zv), E(OP_XCHG, F_eAX, F_Zv),
             E(OP_XCHG, F_eAX, F_Zv), E(OP_XCHG, F_eAX, F_Zv), E(OP_XCHG, F_eAX, F_Zv), E(OP_XCHG, F_eAX, F_Zv),
    /* 98 */ E(OP_CBW, F_NONE, F_NONE), E(OP_CWD, F_NONE, F_NONE), E(OP_CALLF, F_Ap, F_NONE), E(OP_WAIT, F_NONE, F_NONE),
             E(OP_PUSHF, F_NONE, F_NONE), E(OP_POPF, F_NONE, F_NONE), E(OP_SAHF, F_NONE, F_NONE), E(OP_LAHF, F_NONE, F_NONE),
    /* A0 */ E(OP_MOV, F_AL, F_Ob), E(OP_MOV, F_eAX, F_Ov), E(OP_MOV, F_Ob, F_AL), E(OP_MOV, F_Ov, F_eAX),
             E(OP_MOVS, F_b, F_NONE), E(OP_MOVS, F_v, F_NONE), E(OP_CMPS, F_b, F_NONE), E(OP_CMPS, F_v, F_NONE),
    /* A8 */ E(OP_TEST, F_AL, F_Ib), E(OP_TEST, F_eAX, F_Iv), E(OP_STOS, F_b, F_NONE), E(OP_STOS, F_v, F_NONE),
             E(OP_LODS, F_b, F_NONE), E(OP_LODS, F_v, F_NONE), E(OP_SCAS, F_b, F_NONE), E(OP_SCAS, F_v, F_NONE),
    /* B0 */ E(OP_MOV, F_Zb, F_Ib), E(OP_MOV, F_Zb, F_Ib), E(OP_MOV, F_Zb, F_Ib), E(OP_MOV, F_Zb, F_Ib),
             E(OP_MOV, F_Zb, F_Ib), E(OP_MOV, F_Zb, F_Ib), E(OP_MOV, F_Zb, F_Ib), E(OP_MOV, F_Zb, F_Ib),
    /* B8 */ E(OP_MOV, F_Zv, F_Iv), E(OP_MOV, F_Zv, F_Iv), E(OP_MOV, F_Zv, F_Iv), E(OP_MOV, F_Zv, F_Iv),
             E(OP_MOV, F_Zv, F_Iv), E(OP_MOV, F_Zv, F_Iv), E(OP_MOV, F_Zv, F_Iv), E(OP_MOV, F_Zv, F_Iv),
    /* C0 */ G(G_2, F_Eb, F_Ib), G(G_2, F_Ev, F_Ib), E(OP_RET, F_Iw, F_NONE), E(OP_RET, F_NONE, F_NONE),
             E(OP_LES, F_Gv, F_Mp), E(OP_LDS, F_Gv, F_Mp), G(G_C6, F_Eb, F_Ib), G(G_C6, F_Ev, F_Iv),
    /* C8 */ E(OP_ENTER, F_Iw, F_Ib), E(OP_LEAVE, F_NONE, F_NONE), E(OP_RETF, F_Iw, F_NONE), E(OP_RETF, F_NONE, F_NONE),
             E(OP_INT3, F_NONE, F_NONE), E(OP_INT, F_Ib, F_NONE), E(OP_INTO, F_NONE, F_NONE), E(OP_IRET, F_NONE, F_NONE),
    /* D0 */ G(G_2, F_Eb, F_1), G(G_2, F_Ev, F_1), G(G_2, F_Eb, F_CL), G(G_2, F_Ev, F_CL),
             E(OP_AAM, F_Ib, F_NONE), E(OP_AAD, F_Ib, F_NONE), E(OP_SALC, F_NONE, F_NONE), E(OP_XLAT, F_NONE, F_NONE),
    /* D8 */ E(OP_ESC, F_Ev, F_NONE), E(OP_ESC, F_Ev, F_NONE), E(OP_ESC, F_Ev, F_NONE), E(OP_ESC, F_Ev, F_NONE),
             E(OP_ESC, F_Ev, F_NONE), E(OP_ESC, F_Ev, F_NONE), E(OP_ESC, F_Ev, F_NONE), E(OP_ESC, F_Ev, F_NONE),
    /* E0 */ E(OP_LOOPNE, F_Jb, F_NONE), E(OP_LOOPE, F_Jb, F_NONE), E(OP_LOOP, F_Jb, F_NONE), E(OP_JCXZ, F_Jb, F_NONE),
             E(OP_IN, F_AL, F_Ib), E(OP_IN, F_eAX, F_Ib), E(OP_OUT, F_Ib, F_AL), E(OP_OUT, F_Ib, F_eAX),
    /* E8 */ E(OP_CALL, F_Jv, F_NONE), E(OP_JMP, F_Jv, F_NONE), E(OP_JMPF, F_Ap, F_NONE), E(OP_JMP, F_Jb, F_NONE),
             E(OP_IN, F_AL, F_DX), E(OP_IN, F_eAX, F_DX), E(OP_OUT, F_DX, F_AL), E(OP_OUT, F_DX, F_eAX),
    /* F0 */ X() /* LOCK */, E(OP_INT1, F_NONE, F_NONE), X() /* REPNE */, X() /* REP */,
             E(OP_HLT, F_NONE, F_NONE), E(OP_CMC, F_NONE, F_NONE), G(G_3, F_Eb, F_NONE), G(G_3, F_Ev, F_NONE),
    /* F8 */ E(OP_CLC, F_NONE, F_NONE), E(OP_STC, F_NONE, F_NONE), E(OP_CLI, F_NONE, F_NONE), E(OP_STI, F_NONE, F_NONE),
             E(OP_CLD, F_NONE, F_NONE), E(OP_STD, F_NONE, F_NONE), G(G_4, F_Eb, F_NONE), G(G_5, F_Ev, F_NONE),
};

static const opdesc map0f[256] = {
    [0x00] = G(G_0F00, F_Ew, F_NONE),
    [0x01] = G(G_0F01, F_M, F_NONE),
    [0x02] = E(OP_LAR, F_Gv, F_Ew),
    [0x03] = E(OP_LSL, F_Gv, F_Ew),
    [0x06] = E(OP_CLTS, F_NONE, F_NONE),
    [0x08] = E(OP_INVD, F_NONE, F_NONE),     /* 486 (gated below) */
    [0x09] = E(OP_WBINVD, F_NONE, F_NONE),
    [0x20] = E(OP_MOVCR, F_Rd, F_Cd),
    [0x21] = E(OP_MOVDR, F_Rd, F_Dd),
    [0x22] = E(OP_MOVCR, F_Cd, F_Rd),
    [0x23] = E(OP_MOVDR, F_Dd, F_Rd),
    [0x24] = E(OP_MOVTR, F_Rd, F_Td),
    [0x26] = E(OP_MOVTR, F_Td, F_Rd),
    [0x80] = E(OP_JCC, F_Jv, F_NONE), [0x81] = E(OP_JCC, F_Jv, F_NONE),
    [0x82] = E(OP_JCC, F_Jv, F_NONE), [0x83] = E(OP_JCC, F_Jv, F_NONE),
    [0x84] = E(OP_JCC, F_Jv, F_NONE), [0x85] = E(OP_JCC, F_Jv, F_NONE),
    [0x86] = E(OP_JCC, F_Jv, F_NONE), [0x87] = E(OP_JCC, F_Jv, F_NONE),
    [0x88] = E(OP_JCC, F_Jv, F_NONE), [0x89] = E(OP_JCC, F_Jv, F_NONE),
    [0x8A] = E(OP_JCC, F_Jv, F_NONE), [0x8B] = E(OP_JCC, F_Jv, F_NONE),
    [0x8C] = E(OP_JCC, F_Jv, F_NONE), [0x8D] = E(OP_JCC, F_Jv, F_NONE),
    [0x8E] = E(OP_JCC, F_Jv, F_NONE), [0x8F] = E(OP_JCC, F_Jv, F_NONE),
    [0x90] = E(OP_SETCC, F_Eb, F_NONE), [0x91] = E(OP_SETCC, F_Eb, F_NONE),
    [0x92] = E(OP_SETCC, F_Eb, F_NONE), [0x93] = E(OP_SETCC, F_Eb, F_NONE),
    [0x94] = E(OP_SETCC, F_Eb, F_NONE), [0x95] = E(OP_SETCC, F_Eb, F_NONE),
    [0x96] = E(OP_SETCC, F_Eb, F_NONE), [0x97] = E(OP_SETCC, F_Eb, F_NONE),
    [0x98] = E(OP_SETCC, F_Eb, F_NONE), [0x99] = E(OP_SETCC, F_Eb, F_NONE),
    [0x9A] = E(OP_SETCC, F_Eb, F_NONE), [0x9B] = E(OP_SETCC, F_Eb, F_NONE),
    [0x9C] = E(OP_SETCC, F_Eb, F_NONE), [0x9D] = E(OP_SETCC, F_Eb, F_NONE),
    [0x9E] = E(OP_SETCC, F_Eb, F_NONE), [0x9F] = E(OP_SETCC, F_Eb, F_NONE),
    [0xA0] = E(OP_PUSH, F_Sop, F_NONE),  /* FS: Sop reads bits 4:3 = 4 → FS via special case */
    [0xA1] = E(OP_POP, F_Sop, F_NONE),
    [0xA3] = E(OP_BT, F_Ev, F_Gv),
    [0xA4] = E(OP_SHLD, F_Ev, F_Gv),   /* third operand Ib in imm2 */
    [0xA5] = E(OP_SHLD, F_Ev, F_Gv),   /* third operand CL */
    [0xA8] = E(OP_PUSH, F_Sop, F_NONE),
    [0xA9] = E(OP_POP, F_Sop, F_NONE),
    [0xAB] = E(OP_BTS, F_Ev, F_Gv),
    [0xAC] = E(OP_SHRD, F_Ev, F_Gv),
    [0xAD] = E(OP_SHRD, F_Ev, F_Gv),
    [0xAF] = E(OP_IMUL, F_Gv, F_Ev),   /* two-operand IMUL: dst is reg, not accumulator */
    [0xB0] = E(OP_CMPXCHG, F_Eb, F_Gb),
    [0xB1] = E(OP_CMPXCHG, F_Ev, F_Gv),
    [0xB2] = E(OP_LSS, F_Gv, F_Mp),
    [0xB3] = E(OP_BTR, F_Ev, F_Gv),
    [0xB4] = E(OP_LFS, F_Gv, F_Mp),
    [0xB5] = E(OP_LGS, F_Gv, F_Mp),
    [0xB6] = E(OP_MOVZX, F_Gv, F_Eb),
    [0xB7] = E(OP_MOVZX, F_Gv, F_Ew),
    [0xBA] = G(G_0FBA, F_Ev, F_Ib),
    [0xBB] = E(OP_BTC, F_Ev, F_Gv),
    [0xBC] = E(OP_BSF, F_Gv, F_Ev),
    [0xBD] = E(OP_BSR, F_Gv, F_Ev),
    [0xBE] = E(OP_MOVSX, F_Gv, F_Eb),
    [0xBF] = E(OP_MOVSX, F_Gv, F_Ew),
    [0xC0] = E(OP_XADD, F_Eb, F_Gb),
    [0xC1] = E(OP_XADD, F_Ev, F_Gv),
    [0xC8] = E(OP_BSWAP, F_Zv, F_NONE), [0xC9] = E(OP_BSWAP, F_Zv, F_NONE),
    [0xCA] = E(OP_BSWAP, F_Zv, F_NONE), [0xCB] = E(OP_BSWAP, F_Zv, F_NONE),
    [0xCC] = E(OP_BSWAP, F_Zv, F_NONE), [0xCD] = E(OP_BSWAP, F_Zv, F_NONE),
    [0xCE] = E(OP_BSWAP, F_Zv, F_NONE), [0xCF] = E(OP_BSWAP, F_Zv, F_NONE),
};

static const uint8_t grp3_ops[8]  = { OP_TEST, OP_TEST, OP_NOT, OP_NEG, OP_MUL, OP_IMUL, OP_DIV, OP_IDIV };
static const uint8_t grp5_ops[8]  = { OP_INC, OP_DEC, OP_CALL, OP_CALLF, OP_JMP, OP_JMPF, OP_PUSH, OP_PUSH };
static const uint8_t grp0f00[8]   = { OP_SLDT, OP_STR, OP_LLDT, OP_LTR, OP_VERR, OP_VERW, OP_UD, OP_UD };
static const uint8_t grp0f01[8]   = { OP_SGDT, OP_SIDT, OP_LGDT, OP_LIDT, OP_SMSW, OP_UD, OP_LMSW, OP_INVLPG };   /* /7: 486 (gated below) */
static const uint8_t grp0fba[8]   = { OP_UD, OP_UD, OP_UD, OP_UD, OP_BT, OP_BTS, OP_BTR, OP_BTC };

/* ------------------------------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    int pos;
    int model;
} cursor;

static inline uint8_t  fetch8(cursor *c)  { return c->p[c->pos++]; }
static inline uint16_t fetch16(cursor *c) { uint16_t v = c->p[c->pos] | (c->p[c->pos + 1] << 8); c->pos += 2; return v; }
static inline uint32_t fetch32(cursor *c) { uint32_t v = fetch16(c); v |= (uint32_t)fetch16(c) << 16; return v; }

/* Read modrm (+SIB, +disp) and fill the EA fields. */
static void decode_modrm(cursor *c, x86_insn *in) {
    uint8_t m = fetch8(c);
    in->modrm = m;
    in->has_modrm = 1;
    in->mod = m >> 6;
    in->reg = (m >> 3) & 7;
    in->rm  = m & 7;
    in->base = in->index = -1;
    in->scale = 0;
    in->disp = 0;
    if (in->mod == 3) return;

    in->ea_valid = 1;
    int ss_default = 0;
    if (in->adsize == 2) {
        static const int8_t b16[8] = { R_BX, R_BX, R_BP, R_BP, R_SI, R_DI, R_BP, R_BX };
        static const int8_t i16[8] = { R_SI, R_DI, R_SI, R_DI, -1, -1, -1, -1 };
        if (in->mod == 0 && in->rm == 6) {
            in->disp = (int16_t)fetch16(c);
        } else {
            in->base = b16[in->rm];
            in->index = i16[in->rm];
            if (in->mod == 1) in->disp = (int8_t)fetch8(c);
            else if (in->mod == 2) in->disp = (int16_t)fetch16(c);
            ss_default = (in->rm == 2 || in->rm == 3 || in->rm == 6);
        }
    } else {
        int rm = in->rm;
        if (rm == 4) {
            uint8_t sib = fetch8(c);
            in->scale = sib >> 6;
            int idx = (sib >> 3) & 7;
            int bas = sib & 7;
            in->index = (idx == 4) ? -1 : idx;
            if (bas == 5 && in->mod == 0) {
                in->base = -1;
                in->disp = (int32_t)fetch32(c);
            } else {
                in->base = bas;
                ss_default = (bas == R_SP || bas == R_BP);
                /* CONTRACT (386, measured): with no index register the
                 * scale field is not ignored — it scales the base
                 * ([ebp*4+disp8] for SIB A5). Later parts drop this. */
                if (idx == 4 && in->scale && c->model == X86_MODEL_386) {
                    in->index = bas;
                    in->base = -1;
                }
            }
        } else if (rm == 5 && in->mod == 0) {
            in->disp = (int32_t)fetch32(c);
        } else {
            in->base = rm;
            ss_default = (rm == R_BP);
        }
        if (in->mod == 1) in->disp = (int8_t)fetch8(c);
        else if (in->mod == 2) in->disp = (int32_t)fetch32(c);
    }
    if (in->seg_override == S_NONE)
        in->seg = ss_default ? S_SS : S_DS;
}

/* Fill one operand from its form code. Returns 0 on a form that
 * requires a memory operand but got mod==3 (→ #UD). */
static int fill_operand(cursor *c, x86_insn *in, x86_operand *o, int form) {
    o->kind = OPK_NONE;
    o->size = in->opsize;
    switch (form) {
    case F_NONE: o->size = 0; break;
    case F_b: o->size = 1; break;
    case F_v: break;
    case F_Eb: case F_Ev: case F_Ew: case F_Ed:
        o->size = form == F_Eb ? 1 : form == F_Ew ? 2 : form == F_Ed ? 4 : in->opsize;
        if (in->mod == 3) { o->kind = OPK_REG; o->reg = in->rm; }
        else o->kind = OPK_MEM;
        break;
    case F_M: case F_Mp:
        if (in->mod == 3) return 0;
        o->kind = OPK_MEM;
        o->size = form == F_M ? in->opsize : (in->opsize == 2 ? 4 : 6);
        break;
    case F_Gb: case F_Gv: case F_Gw:
        o->kind = OPK_REG; o->reg = in->reg;
        o->size = form == F_Gb ? 1 : form == F_Gw ? 2 : in->opsize;
        break;
    case F_Sw:
        /* 8086/186 alias reg 4..7 onto ES..DS; the 286 has no FS/GS and raises #UD */
        o->kind = OPK_SREG; o->size = 2;
        o->reg = c->model < X86_MODEL_286 ? (in->reg & 3) : in->reg;
        if (c->model == X86_MODEL_286 && in->reg >= 4) return 0;
        if (o->reg >= 6) return 0;                                    /* 386: no segment register 6/7 */
        break;
    case F_Ib:  o->kind = OPK_IMM; o->size = 1; o->imm_enc = (uint8_t)(c->pos | 1 << 4); o->imm = fetch8(c); break;
    case F_Iw:  o->kind = OPK_IMM; o->size = 2; o->imm_enc = (uint8_t)(c->pos | 2 << 4); o->imm = fetch16(c); break;
    case F_Iv:  o->kind = OPK_IMM; o->imm_enc = (uint8_t)(c->pos | in->opsize << 4); o->imm = in->opsize == 2 ? fetch16(c) : fetch32(c); break;
    case F_Ibs: o->kind = OPK_IMM; o->imm_enc = (uint8_t)(c->pos | 1 << 4 | 0x80); o->imm = (int8_t)fetch8(c); if (in->opsize == 2) o->imm &= 0xFFFF; break;
    case F_1:   o->kind = OPK_IMM; o->size = 1; o->imm = 1; break;
    case F_AL:  o->kind = OPK_REG; o->reg = R_AL; o->size = 1; break;
    case F_CL:  o->kind = OPK_REG; o->reg = R_CL; o->size = 1; break;
    case F_eAX: o->kind = OPK_REG; o->reg = R_AX; break;
    case F_AX16: o->kind = OPK_REG; o->reg = R_AX; o->size = 2; break;
    case F_DX:  o->kind = OPK_REG; o->reg = R_DX; o->size = 2; break;
    case F_Jb:  o->kind = OPK_IMM; o->imm = (int8_t)fetch8(c); break;
    case F_Jv:  o->kind = OPK_IMM; o->imm = in->opsize == 2 ? (int16_t)fetch16(c) : (int32_t)fetch32(c); break;
    case F_Ap:
        o->kind = OPK_IMM;
        o->imm = in->opsize == 2 ? fetch16(c) : fetch32(c);
        in->imm2 = fetch16(c);
        break;
    case F_Ob: case F_Ov:
        o->kind = OPK_MEM;
        o->size = form == F_Ob ? 1 : in->opsize;
        in->ea_valid = 1;
        in->base = in->index = -1;
        in->disp = in->adsize == 2 ? fetch16(c) : (int32_t)fetch32(c);
        if (in->seg_override == S_NONE) in->seg = S_DS;
        break;
    case F_Zb: o->kind = OPK_REG; o->reg = in->opcode & 7; o->size = 1; break;
    case F_Zv: o->kind = OPK_REG; o->reg = (in->opcode2 ? in->opcode2 : in->opcode) & 7; break;   /* 0F C8+r: BSWAP */
    case F_Sop:
        o->kind = OPK_SREG; o->size = 2;
        o->reg = in->opcode2 ? (in->opcode2 & 8 ? S_GS : S_FS) : (in->opcode >> 3) & 3;
        break;
    case F_Cd: o->kind = OPK_CR; o->reg = in->reg; o->size = 4; break;
    case F_Dd: o->kind = OPK_DR; o->reg = in->reg; o->size = 4; break;
    case F_Td: o->kind = OPK_TR; o->reg = in->reg; o->size = 4; break;
    case F_Rd: o->kind = OPK_REG; o->reg = in->rm; o->size = 4; break;
    }
    return 1;
}

static int form_needs_modrm(int f) {
    switch (f) {
    case F_Eb: case F_Ev: case F_Ew: case F_Ed: case F_Gb: case F_Gv: case F_Gw:
    case F_Sw: case F_M: case F_Mp: case F_Cd: case F_Dd: case F_Td: case F_Rd:
        return 1;
    }
    return 0;
}

int x86_decode(const x86_dec_ctx *ctx, x86_insn *in) {
    cursor c = { ctx->bytes, 0, ctx->model };
    memset(in, 0, sizeof(*in));
    in->seg_override = S_NONE;
    in->seg = S_DS;
    in->base = in->index = -1;

    int model = ctx->model;
    int op32 = ctx->def32, ad32 = ctx->def32;

    /* Prefixes. 8086 has no limit and no 66/67/64/65; those are
     * ordinary opcodes there (handled by the alias remap below). */
    for (;;) {
        if (c.pos >= X86_MAX_INSN) return 0;
        uint8_t b = c.p[c.pos];
        switch (b) {
        case 0x26: in->seg_override = in->seg = S_ES; break;
        case 0x2E: in->seg_override = in->seg = S_CS; break;
        case 0x36: in->seg_override = in->seg = S_SS; break;
        case 0x3E: in->seg_override = in->seg = S_DS; break;
        case 0x64: if (model < X86_MODEL_386) goto done_prefix; in->seg_override = in->seg = S_FS; break;
        case 0x65: if (model < X86_MODEL_386) goto done_prefix; in->seg_override = in->seg = S_GS; break;
        case 0x66: if (model < X86_MODEL_386) goto done_prefix; op32 = !ctx->def32; break;
        case 0x67: if (model < X86_MODEL_386) goto done_prefix; ad32 = !ctx->def32; break;
        case 0xF0: in->lock = 1; break;
        case 0xF1: if (model > X86_MODEL_8086) goto done_prefix; in->lock = 1; break; /* 8086: LOCK alias */
        case 0xF2: in->rep = 0xF2; break;
        case 0xF3: in->rep = 0xF3; break;
        default: goto done_prefix;
        }
        c.pos++;
    }
done_prefix:
    in->opsize = op32 ? 4 : 2;
    in->adsize = ad32 ? 4 : 2;

    uint8_t opc = fetch8(&c);
    in->opcode = opc;
    const opdesc *d;

    if (model == X86_MODEL_8086) {
        /* 8086/8088 aliasing: the decoder PLA ignores bits it does not test */
        if (opc >= 0x60 && opc <= 0x6F) opc += 0x10;        /* Jcc */
        else if (opc == 0xC0 || opc == 0xC1) opc += 2;      /* RET */
        else if (opc == 0xC8 || opc == 0xC9) opc += 2;      /* RETF */
        else if (opc == 0x0F) {                             /* POP CS */
            in->op = OP_POP;
            in->ops[0].kind = OPK_SREG; in->ops[0].reg = S_CS; in->ops[0].size = 2;
            in->len = c.pos;
            return in->len;
        }
        d = &primary[opc];
        if (opc == 0xF1) { in->op = OP_LOCK_ONLY; in->len = c.pos; return in->len; }
    } else if (opc == 0x0F) {
        if (model < X86_MODEL_286) { in->op = OP_UD; in->len = c.pos; return in->len; }
        in->opcode2 = fetch8(&c);
        d = &map0f[in->opcode2];
        /* The map is the 386 truth; a 286 only has the system group. */
        if (model < X86_MODEL_386 && in->opcode2 > 0x06) d = &map0f[0x07];
        if (in->opcode2 >= 0x80 && in->opcode2 <= 0x9F) in->cond = in->opcode2 & 15;
    } else {
        d = &primary[opc];
    }

    if (opc >= 0x70 && opc <= 0x7F) in->cond = opc & 15;

    in->op = d->op;
    if (d->grp != G_NONE || form_needs_modrm(d->d) || form_needs_modrm(d->s))
        decode_modrm(&c, in);

    /* Resolve groups: pick the op, and adjust operand forms where the
     * reg field changes them. */
    int fd = d->d, fs = d->s;
    switch (d->grp) {
    case G_NONE: break;
    case G_1: in->op = OP_ADD + in->reg; break;                            /* ADD..CMP */
    case G_2:
        in->op = OP_ROL + in->reg;
        /* /6: undocumented SETMO/SETMOC on the 8086, SHL alias on 186+ */
        if (in->reg == 6) in->op = model == X86_MODEL_8086 ? OP_SETMO : OP_SHL;
        break;
    case G_3:
        in->op = grp3_ops[in->reg];
        if (in->reg <= 1) fs = (fd == F_Eb) ? F_Ib : F_Iv;                /* TEST Ev,Iv (/1 is an alias) */
        break;
    case G_4:
        /* FE: /0 INC, /1 DEC. On 8086 /2-/7 behave like FF with byte
         * operands; 186+ #UD. Model that as the FF ops on a byte
         * operand and let the interpreter refuse what it must. */
        in->op = grp5_ops[in->reg];
        if (in->reg >= 2 && model > X86_MODEL_8086) in->op = OP_UD;
        if (in->reg == 3 || in->reg == 5) fd = F_Mp;
        break;
    case G_5:
        in->op = grp5_ops[in->reg];
        if (in->reg == 7 && model > X86_MODEL_8086) in->op = OP_UD;
        if (in->reg == 3 || in->reg == 5) fd = F_Mp;
        break;
    case G_8F: in->op = (in->reg && model >= X86_MODEL_286) ? OP_UD : OP_POP; break;   /* 8086 ignores reg; 286+ #UD */
    case G_C6: in->op = (in->reg && model >= X86_MODEL_286) ? OP_UD : OP_MOV; break;
    case G_0F00: in->op = grp0f00[in->reg]; break;
    case G_0F01:
        in->op = grp0f01[in->reg];
        if (in->reg == 4 || in->reg == 6) fd = F_Ew;                       /* SMSW/LMSW allow registers */
        break;
    case G_0FBA: in->op = grp0fba[in->reg]; break;
    }

    if (!fill_operand(&c, in, &in->ops[0], fd)) { in->op = OP_UD; goto out; }
    if (!fill_operand(&c, in, &in->ops[1], fs)) { in->op = OP_UD; goto out; }

    /* Odd immediates that the two-operand form can't express */
    switch (in->op) {
    case OP_IMUL3:
        if (opc == 0x6B) { in->imm2 = (int8_t)fetch8(&c); if (in->opsize == 2) in->imm2 &= 0xFFFF; }
        else in->imm2 = in->opsize == 2 ? fetch16(&c) : fetch32(&c);
        break;
    case OP_SHLD: case OP_SHRD:
        if (in->opcode2 == 0xA4 || in->opcode2 == 0xAC) in->imm2 = fetch8(&c);
        else in->imm2 = 0xFFFFFFFF;                                         /* marker: count is CL */
        break;
    case OP_ENTER:
        /* fill_operand fetched Iw into ops[0] and Ib into ops[1]: keep both there */
        break;
    default: break;
    }

    /* Far-pointer memory operands for CALLF/JMPF through FF /3 /5 */
    if ((in->op == OP_CALLF || in->op == OP_JMPF) && in->ops[0].kind == OPK_MEM)
        in->ops[0].size = in->opsize == 2 ? 4 : 6;

    /* SETCC / 0F Jcc already have cond; Jcc in the primary map too */
    if (in->op == OP_SETCC) in->cond = in->opcode2 & 15;

    /* The 486's additions are #UD on a 386: CMPXCHG, XADD, BSWAP, INVD,
     * WBINVD, INVLPG. CPU-detection code probes exactly these. */
    if (model < X86_MODEL_486) {
        switch (in->op) {
        case OP_CMPXCHG: case OP_XADD: case OP_BSWAP: case OP_INVD: case OP_WBINVD: case OP_INVLPG:
            in->op = OP_UD; break;
        default: break;
        }
    }

    /* 386+: LOCK is only legal on the read-modify-write ALU ops with a
     * memory destination (and XCHG with a memory operand); anywhere
     * else it is #UD. Earlier parts treat it as a no-op prefix. */
    if (in->lock && model >= X86_MODEL_386 && in->op != OP_UD) {
        int ok = 0;
        switch (in->op) {
        case OP_ADD: case OP_OR: case OP_ADC: case OP_SBB: case OP_AND: case OP_SUB: case OP_XOR:
        case OP_INC: case OP_DEC: case OP_NOT: case OP_NEG:
        case OP_BTS: case OP_BTR: case OP_BTC: case OP_XADD: case OP_CMPXCHG:
            ok = in->ops[0].kind == OPK_MEM; break;
        case OP_XCHG:
            ok = in->ops[0].kind == OPK_MEM || in->ops[1].kind == OPK_MEM; break;
        default: break;
        }
        if (!ok) in->op = OP_UD;
    }

out:
    if (c.pos > X86_MAX_INSN) return 0;
    in->len = c.pos;
    return in->len;
}

/* ------------------------------------------------------------------------
 * Disassembler — for traces and -V diffs, not for beauty.
 * --------------------------------------------------------------------- */
static const char *op_names[OP__COUNT] = {
    [OP_ADD]="add",[OP_OR]="or",[OP_ADC]="adc",[OP_SBB]="sbb",[OP_AND]="and",[OP_SUB]="sub",[OP_XOR]="xor",[OP_CMP]="cmp",
    [OP_ROL]="rol",[OP_ROR]="ror",[OP_RCL]="rcl",[OP_RCR]="rcr",[OP_SHL]="shl",[OP_SHR]="shr",[OP_SAL]="sal",[OP_SAR]="sar",
    [OP_TEST]="test",[OP_NOT]="not",[OP_NEG]="neg",[OP_MUL]="mul",[OP_IMUL]="imul",[OP_DIV]="div",[OP_IDIV]="idiv",
    [OP_INC]="inc",[OP_DEC]="dec",[OP_MOV]="mov",[OP_MOVSEG]="mov",[OP_XCHG]="xchg",[OP_LEA]="lea",[OP_NOP]="nop",
    [OP_PUSH]="push",[OP_POP]="pop",[OP_PUSHA]="pusha",[OP_POPA]="popa",[OP_PUSHF]="pushf",[OP_POPF]="popf",
    [OP_CALL]="call",[OP_CALLF]="call far",[OP_JMP]="jmp",[OP_JMPF]="jmp far",[OP_JCC]="jcc",[OP_JCXZ]="jcxz",
    [OP_LOOP]="loop",[OP_LOOPE]="loope",[OP_LOOPNE]="loopne",
    [OP_RET]="ret",[OP_RETF]="retf",[OP_IRET]="iret",[OP_INT]="int",[OP_INT3]="int3",[OP_INTO]="into",
    [OP_CBW]="cbw",[OP_CWD]="cwd",[OP_SAHF]="sahf",[OP_LAHF]="lahf",[OP_XLAT]="xlat",
    [OP_MOVS]="movs",[OP_CMPS]="cmps",[OP_STOS]="stos",[OP_LODS]="lods",[OP_SCAS]="scas",[OP_INS]="ins",[OP_OUTS]="outs",
    [OP_IN]="in",[OP_OUT]="out",[OP_LES]="les",[OP_LDS]="lds",[OP_LSS]="lss",[OP_LFS]="lfs",[OP_LGS]="lgs",
    [OP_CLC]="clc",[OP_STC]="stc",[OP_CMC]="cmc",[OP_CLD]="cld",[OP_STD]="std",[OP_CLI]="cli",[OP_STI]="sti",
    [OP_HLT]="hlt",[OP_WAIT]="wait",[OP_ESC]="esc",[OP_LOCK_ONLY]="lock",
    [OP_AAA]="aaa",[OP_AAS]="aas",[OP_AAM]="aam",[OP_AAD]="aad",[OP_DAA]="daa",[OP_DAS]="das",[OP_SALC]="salc",
    [OP_ENTER]="enter",[OP_LEAVE]="leave",[OP_BOUND]="bound",[OP_ARPL]="arpl",[OP_IMUL3]="imul",
    [OP_MOVZX]="movzx",[OP_MOVSX]="movsx",[OP_SETCC]="setcc",
    [OP_BT]="bt",[OP_BTS]="bts",[OP_BTR]="btr",[OP_BTC]="btc",[OP_BSF]="bsf",[OP_BSR]="bsr",[OP_SHLD]="shld",[OP_SHRD]="shrd",
    [OP_CMPXCHG]="cmpxchg",[OP_XADD]="xadd",[OP_BSWAP]="bswap",
    [OP_LAR]="lar",[OP_LSL]="lsl",[OP_CLTS]="clts",[OP_SGDT]="sgdt",[OP_SIDT]="sidt",[OP_LGDT]="lgdt",[OP_LIDT]="lidt",
    [OP_SLDT]="sldt",[OP_STR]="str",[OP_LLDT]="lldt",[OP_LTR]="ltr",[OP_VERR]="verr",[OP_VERW]="verw",[OP_SMSW]="smsw",[OP_LMSW]="lmsw",
    [OP_MOVCR]="mov",[OP_MOVDR]="mov",[OP_MOVTR]="mov",[OP_INT1]="int1",[OP_SETMO]="setmo",[OP_UD]="(bad)",
    [OP_INVD]="invd",[OP_WBINVD]="wbinvd",[OP_INVLPG]="invlpg",
};
static const char *cc_names[16] = { "o","no","b","ae","e","ne","be","a","s","ns","p","np","l","ge","le","g" };
static const char *r8n[8]  = { "al","cl","dl","bl","ah","ch","dh","bh" };
static const char *r16n[8] = { "ax","cx","dx","bx","sp","bp","si","di" };
static const char *r32n[8] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi" };
static const char *sregn[6] = { "es","cs","ss","ds","fs","gs" };

static void fmt_operand(const x86_insn *in, const x86_operand *o, char *b, size_t n) {
    switch (o->kind) {
    case OPK_NONE: b[0] = 0; break;
    case OPK_REG:
        snprintf(b, n, "%s", o->size == 1 ? r8n[o->reg] : o->size == 2 ? r16n[o->reg] : r32n[o->reg]);
        break;
    case OPK_SREG: snprintf(b, n, "%s", sregn[o->reg]); break;
    case OPK_IMM:  snprintf(b, n, "0x%x", o->imm); break;
    case OPK_CR:   snprintf(b, n, "cr%d", o->reg); break;
    case OPK_DR:   snprintf(b, n, "dr%d", o->reg); break;
    case OPK_TR:   snprintf(b, n, "tr%d", o->reg); break;
    case OPK_MEM: {
        const char **rn = in->adsize == 2 ? r16n : r32n;
        char t[64] = "";
        size_t p = 0;
        if (in->base >= 0) p += snprintf(t + p, sizeof t - p, "%s", rn[in->base]);
        if (in->index >= 0) p += snprintf(t + p, sizeof t - p, "%s%s*%d", p ? "+" : "", rn[in->index], 1 << in->scale);
        if (in->disp || !p) p += snprintf(t + p, sizeof t - p, p ? "%+d" : "0x%x", in->disp);
        snprintf(b, n, "%s [%s:%s]",
                 o->size == 1 ? "byte" : o->size == 2 ? "word" : o->size == 4 ? "dword" : "far", sregn[in->seg], t);
        break;
    }
    }
}

char *x86_disasm(const x86_insn *in, char *buf, size_t n) {
    char a[96], b[96];
    fmt_operand(in, &in->ops[0], a, sizeof a);
    fmt_operand(in, &in->ops[1], b, sizeof b);
    const char *nm = op_names[in->op] ? op_names[in->op] : "?";
    char nmbuf[16];
    if (in->op == OP_JCC) { snprintf(nmbuf, sizeof nmbuf, "j%s", cc_names[in->cond]); nm = nmbuf; }
    if (in->op == OP_SETCC) { snprintf(nmbuf, sizeof nmbuf, "set%s", cc_names[in->cond]); nm = nmbuf; }
    const char *pre = in->rep == 0xF3 ? "rep " : in->rep == 0xF2 ? "repne " : in->lock ? "lock " : "";
    if (in->ops[1].kind && in->op == OP_IMUL3)
        snprintf(buf, n, "%s%s %s, %s, 0x%x", pre, nm, a, b, in->imm2);
    else if (in->ops[1].kind)
        snprintf(buf, n, "%s%s %s, %s", pre, nm, a, b);
    else if (in->ops[0].kind)
        snprintf(buf, n, "%s%s %s", pre, nm, a);
    else
        snprintf(buf, n, "%s%s", pre, nm);
    if (in->op == OP_CALLF || in->op == OP_JMPF) {
        if (in->ops[0].kind == OPK_IMM) snprintf(buf, n, "%s 0x%x:0x%x", nm, in->imm2, in->ops[0].imm);
    }
    return buf;
}
