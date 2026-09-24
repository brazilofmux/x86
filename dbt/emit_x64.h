/* emit_x64.h — x86-64 code emission for the x86 DBT.
 *
 * Grown from z80's dbt/emit_x64.h (github.com/brazilofmux/z80): the same
 * single-header static-inline style, the same emit_t as emit_a64.h so
 * the translator's cursor and patch helpers read alike on both hosts.
 * Where the Z80 needed a handful of shapes, an x86 guest re-emitted on
 * an x86 host wants nearly the whole integer instruction set at every
 * width, so the encoders here are generic over the operand SIZE (1, 2,
 * 4 or 8 bytes: the byte form of the opcode, the 66h prefix, REX.W) and
 * over a memory operand (x64_mem_t: base + index * 2^scale + disp32).
 *
 * Registers are the architectural numbers 0..15 (X64_RAX .. X64_R15).
 * A SIZE-1 operand names a byte register: 0..3 are AL CL DL BL, 4..7 are
 * AH CH DH BH — the legacy high bytes, which is what a guest AH..BH
 * needs while guest AX..BX live in RAX..RBX — and 8..15 are R8B..R15B.
 * SPL/BPL/SIL/DIL are never used. A high byte is only encodable without
 * a REX prefix, so an instruction that names one may not also name
 * R8..R15 in any operand; the encoders assert it (X64_ASSERT), and the
 * translator picks a different sequence for those cases.
 *
 * Memory operands take x64_mem_t; X64_NOREG for no index, and for no
 * base when there is an index ([index * 2^scale + disp32]). The encoder
 * handles the two ModRM special cases: a base whose low three bits are
 * 100 (RSP/R12) needs a SIB byte, and a base whose low bits are 101
 * (RBP/R13) cannot use mod=00, so a zero displacement is encoded as
 * disp8=0. RIP-relative addressing is not used: everything reachable
 * from translated code sits at a fixed offset from a pinned register.
 *
 * Opcodes: an `op` argument below 0x100 is a one-byte opcode; 0x100 | xx
 * is 0F xx. The ALU, shift and group-3/5 operations are selected by
 * their /digit or opcode octet as the manuals list them.
 */
#ifndef EMIT_X64_H
#define EMIT_X64_H

#include <stdint.h>
#include <string.h>
#include <assert.h>

#ifndef X64_ASSERT
#define X64_ASSERT(c) assert(c)
#endif

typedef struct {
    uint8_t *buf;
    uint32_t offset;
    uint32_t capacity;
} emit_t;

typedef enum {
    X64_RAX = 0, X64_RCX = 1, X64_RDX = 2,  X64_RBX = 3,
    X64_RSP = 4, X64_RBP = 5, X64_RSI = 6,  X64_RDI = 7,
    X64_R8  = 8, X64_R9  = 9, X64_R10 = 10, X64_R11 = 11,
    X64_R12 = 12, X64_R13 = 13, X64_R14 = 14, X64_R15 = 15
} x64_reg_t;

/* Byte-register names for SIZE-1 operands (see above). */
enum { X64_AL = 0, X64_CL = 1, X64_DL = 2, X64_BL = 3, X64_AH = 4, X64_CH = 5, X64_DH = 6, X64_BH = 7 };

#define X64_NOREG (-1)

typedef struct { int base, index, scale; int32_t disp; } x64_mem_t;   /* scale: log2 (0..3) */
static inline x64_mem_t x64_m(int base, int32_t disp) { x64_mem_t m = { base, X64_NOREG, 0, disp }; return m; }
static inline x64_mem_t x64_mi(int base, int index, int scale, int32_t disp) { x64_mem_t m = { base, index, scale, disp }; return m; }

/* Condition codes, the low nibble of Jcc / SETcc / CMOVcc — and exactly
 * the guest's own encoding (x86_insn.cond), which is the point. */
typedef enum {
    X64_CC_O  = 0x0, X64_CC_NO = 0x1,
    X64_CC_B  = 0x2, X64_CC_AE = 0x3,
    X64_CC_E  = 0x4, X64_CC_NE = 0x5,
    X64_CC_BE = 0x6, X64_CC_A  = 0x7,
    X64_CC_S  = 0x8, X64_CC_NS = 0x9,
    X64_CC_P  = 0xA, X64_CC_NP = 0xB,
    X64_CC_L  = 0xC, X64_CC_GE = 0xD,
    X64_CC_LE = 0xE, X64_CC_G  = 0xF
} x64_cc_t;
#define X64_CC_C  X64_CC_B
#define X64_CC_NC X64_CC_AE
#define X64_CC_Z  X64_CC_E
#define X64_CC_NZ X64_CC_NE

/* ALU octet: `op r/m, r` is (octet << 3) | 1 (| 0 for bytes), and the
 * /digit of the 80/81/83 immediate forms. */
enum {
    X64_ALU_ADD = 0, X64_ALU_OR = 1, X64_ALU_ADC = 2, X64_ALU_SBB = 3,
    X64_ALU_AND = 4, X64_ALU_SUB = 5, X64_ALU_XOR = 6, X64_ALU_CMP = 7
};
/* Shift/rotate /digit of C0/C1/D0/D1/D2/D3. */
enum {
    X64_SH_ROL = 0, X64_SH_ROR = 1, X64_SH_RCL = 2, X64_SH_RCR = 3,
    X64_SH_SHL = 4, X64_SH_SHR = 5, X64_SH_SAL = 6, X64_SH_SAR = 7
};
/* Group 3 (F6/F7) /digit. */
enum { X64_G3_TEST = 0, X64_G3_NOT = 2, X64_G3_NEG = 3, X64_G3_MUL = 4, X64_G3_IMUL = 5, X64_G3_DIV = 6, X64_G3_IDIV = 7 };
/* BT family: 0F BA /digit, and the register forms 0F A3/AB/B3/BB. */
enum { X64_BT_BT = 4, X64_BT_BTS = 5, X64_BT_BTR = 6, X64_BT_BTC = 7 };

/* ---- Raw emit + cursor helpers ---- */

static inline void emit_byte(emit_t *e, uint8_t b) {
    if (e->offset < e->capacity) e->buf[e->offset] = b;
    e->offset++;
}
static inline void emit_u16(emit_t *e, uint16_t v) { emit_byte(e, (uint8_t)v); emit_byte(e, (uint8_t)(v >> 8)); }
static inline void emit_u32(emit_t *e, uint32_t v) { emit_u16(e, (uint16_t)v); emit_u16(e, (uint16_t)(v >> 16)); }
static inline void emit_u64(emit_t *e, uint64_t v) { emit_u32(e, (uint32_t)v); emit_u32(e, (uint32_t)(v >> 32)); }
static inline uint32_t emit_pos(emit_t *e) { return e->offset; }

/* Patch the rel32 field at `imm_off` so the branch lands on target_off
 * (the displacement counts from the end of the 4-byte field). */
static inline void emit_patch_rel32(emit_t *e, uint32_t imm_off, uint32_t target_off) {
    int32_t disp = (int32_t)(target_off - (imm_off + 4));
    memcpy(e->buf + imm_off, &disp, 4);
}
/* The same for a rel8 field. */
static inline void emit_patch_rel8(emit_t *e, uint32_t imm_off, uint32_t target_off) {
    int32_t disp = (int32_t)(target_off - (imm_off + 1));
    X64_ASSERT(disp >= -128 && disp <= 127);
    e->buf[imm_off] = (uint8_t)disp;
}

/* ---- Encoding primitives ---- */

static inline int x64_high8(int size, int r) { return size == 1 && r >= 4 && r <= 7; }
/* A ModRM.reg field that is an opcode extension (/digit), not a
 * register: marked so the high-byte rule does not apply to it. */
#define X64_DIGIT(n) ((n) | 0x10)

/* REX for an instruction with operand size SIZE. reg/index/base are the
 * ModRM.reg, SIB.index and ModRM.rm/SIB.base registers, or X64_NOREG.
 * A byte operand that is a high byte forbids REX (asserted); every other
 * byte register 0..3 and 8..15 is fine either way. */
static inline void x64_rex(emit_t *e, int size, int reg, int index, int base) {
    uint8_t rex = 0x40;
    if (size == 8)                          rex |= 0x08;
    if (reg >= 0 && (reg & 8))              rex |= 0x04;
    if (index >= 0 && (index & 8))          rex |= 0x02;
    if (base >= 0 && (base & 8))            rex |= 0x01;
    if (rex != 0x40) {
        X64_ASSERT(!x64_high8(size, reg) && !x64_high8(size, base));
        emit_byte(e, rex);
    }
}
static inline void x64_prefix(emit_t *e, int size) { if (size == 2) emit_byte(e, 0x66); }
static inline void x64_opcode(emit_t *e, int size, int op) {
    /* the byte form of an opcode is the even one (op - 1) for the
     * one-byte map's ALU/MOV/TEST/XCHG/INC-DEC groups, which is how every
     * caller passes it: op = the word/dword form, and size 1 turns bit 0 off */
    if (op >= 0x100) emit_byte(e, 0x0F);
    emit_byte(e, (uint8_t)(size == 1 ? (op & ~1) : op));
}
/* Two-byte-map opcodes whose byte form is NOT the even neighbour
 * (0F B6/B7 MOVZX etc.) and one-byte opcodes with no byte form are
 * passed with X64_OP_EXACT so bit 0 is left alone. */
#define X64_OP_EXACT 0x200
static inline void x64_opcode_exact(emit_t *e, int op) {
    if (op & 0x100) { emit_byte(e, 0x0F); emit_byte(e, (uint8_t)op); }
    else emit_byte(e, (uint8_t)op);
}
static inline void x64_op(emit_t *e, int size, int op) {
    if (op & X64_OP_EXACT) x64_opcode_exact(e, op & ~X64_OP_EXACT);
    else x64_opcode(e, size, op);
}

static inline void x64_modrm_mem(emit_t *e, int reg, const x64_mem_t *m) {
    int base = m->base, index = m->index;
    X64_ASSERT(index != X64_RSP);                  /* RSP cannot be an index */
    if (base == X64_NOREG) {
        /* [index * 2^scale + disp32]: mod 00, SIB with base 101 and no base */
        X64_ASSERT(index != X64_NOREG);            /* absolute [disp32] would be RIP-relative here */
        emit_byte(e, (uint8_t)((reg & 7) << 3 | 4));
        emit_byte(e, (uint8_t)((m->scale << 6) | ((index & 7) << 3) | 5));
        emit_u32(e, (uint32_t)m->disp);
        return;
    }
    int need_sib = (index != X64_NOREG) || ((base & 7) == 4);
    int mod;
    if (m->disp == 0 && (base & 7) != 5) mod = 0;
    else if (m->disp >= -128 && m->disp <= 127) mod = 1;
    else mod = 2;
    if (need_sib) {
        emit_byte(e, (uint8_t)((mod << 6) | ((reg & 7) << 3) | 4));
        int idx = (index != X64_NOREG) ? (index & 7) : 4;
        emit_byte(e, (uint8_t)((m->scale << 6) | (idx << 3) | (base & 7)));
    } else {
        emit_byte(e, (uint8_t)((mod << 6) | ((reg & 7) << 3) | (base & 7)));
    }
    if (mod == 1)      emit_byte(e, (uint8_t)m->disp);
    else if (mod == 2) emit_u32(e, (uint32_t)m->disp);
}
static inline void x64_modrm_reg(emit_t *e, int reg, int rm) {
    emit_byte(e, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

/* op reg, [mem]   (or op [mem], reg — the direction is in the opcode) */
static inline void x64_rm(emit_t *e, int size, int op, int reg, const x64_mem_t *m) {
    x64_prefix(e, size);
    x64_rex(e, size, reg, m->index, m->base);
    x64_op(e, size, op);
    x64_modrm_mem(e, reg, m);
}
/* op reg, rm   (register direct) */
static inline void x64_rr(emit_t *e, int size, int op, int reg, int rm) {
    x64_prefix(e, size);
    x64_rex(e, size, reg, X64_NOREG, rm);
    x64_op(e, size, op);
    x64_modrm_reg(e, reg, rm);
}
/* The immediate that follows an r/m,imm form: SIZE bytes, 4 at most. */
static inline void x64_imm(emit_t *e, int size, uint32_t imm) {
    if (size == 1) emit_byte(e, (uint8_t)imm);
    else if (size == 2) emit_u16(e, (uint16_t)imm);
    else emit_u32(e, imm);
}
static inline int x64_fits8(int32_t v) { return v >= -128 && v <= 127; }

/* ======================================================================
 * Moves
 * ====================================================================== */

/* mov dst, src (registers) */
static inline void emit_mov_rr(emit_t *e, int size, int dst, int src) {
    if (dst == src && size != 1) return;
    x64_rr(e, size, 0x89, src, dst);
}
/* mov dst, [mem] */
static inline void emit_mov_rm(emit_t *e, int size, int dst, const x64_mem_t *m) { x64_rm(e, size, 0x8B, dst, m); }
/* mov [mem], src */
static inline void emit_mov_mr(emit_t *e, int size, const x64_mem_t *m, int src) { x64_rm(e, size, 0x89, src, m); }
/* mov reg, imm (size 1, 2, 4; a 64-bit register gets the zero-extended
 * 32-bit form when the value fits, else the 10-byte imm64) */
static inline void emit_mov_ri(emit_t *e, int size, int dst, uint64_t imm) {
    if (size == 8) {
        if (imm >> 32) {
            x64_rex(e, 8, X64_NOREG, X64_NOREG, dst);
            emit_byte(e, (uint8_t)(0xB8 | (dst & 7)));
            emit_u64(e, imm);
            return;
        }
        size = 4;
    }
    x64_prefix(e, size);
    x64_rex(e, size, X64_NOREG, X64_NOREG, dst);
    emit_byte(e, (uint8_t)((size == 1 ? 0xB0 : 0xB8) | (dst & 7)));
    x64_imm(e, size, (uint32_t)imm);
}
/* mov [mem], imm (size 1, 2, 4; 8 takes a sign-extended imm32) */
static inline void emit_mov_mi(emit_t *e, int size, const x64_mem_t *m, uint32_t imm) {
    x64_rm(e, size, 0xC7, X64_DIGIT(0), m);
    x64_imm(e, size == 8 ? 4 : size, imm);
}
/* movzx dst(32/64), src of SIZE 1 or 2 (register) */
static inline void emit_movzx_rr(emit_t *e, int dsize, int dst, int ssize, int src) {
    X64_ASSERT(ssize == 1 || ssize == 2);
    x64_prefix(e, dsize == 2 ? 2 : 4);
    /* REX: the source's high-byte rule applies to the rm operand */
    {
        uint8_t rex = 0x40;
        if (dsize == 8) rex |= 0x08;
        if (dst & 8) rex |= 0x04;
        if (src & 8) rex |= 0x01;
        if (rex != 0x40) { X64_ASSERT(!x64_high8(ssize, src)); emit_byte(e, rex); }
    }
    emit_byte(e, 0x0F); emit_byte(e, ssize == 1 ? 0xB6 : 0xB7);
    x64_modrm_reg(e, dst, src);
}
static inline void emit_movzx_rm(emit_t *e, int dsize, int dst, int ssize, const x64_mem_t *m) {
    X64_ASSERT(ssize == 1 || ssize == 2);
    x64_rm(e, dsize, X64_OP_EXACT | 0x100 | (ssize == 1 ? 0xB6 : 0xB7), dst, m);
}
static inline void emit_movsx_rr(emit_t *e, int dsize, int dst, int ssize, int src) {
    X64_ASSERT(ssize == 1 || ssize == 2 || (ssize == 4 && dsize == 8));
    if (ssize == 4) { x64_rr(e, 8, X64_OP_EXACT | 0x63, dst, src); return; }   /* movsxd */
    x64_prefix(e, dsize == 2 ? 2 : 4);
    {
        uint8_t rex = 0x40;
        if (dsize == 8) rex |= 0x08;
        if (dst & 8) rex |= 0x04;
        if (src & 8) rex |= 0x01;
        if (rex != 0x40) { X64_ASSERT(!x64_high8(ssize, src)); emit_byte(e, rex); }
    }
    emit_byte(e, 0x0F); emit_byte(e, ssize == 1 ? 0xBE : 0xBF);
    x64_modrm_reg(e, dst, src);
}
static inline void emit_movsx_rm(emit_t *e, int dsize, int dst, int ssize, const x64_mem_t *m) {
    X64_ASSERT(ssize == 1 || ssize == 2 || (ssize == 4 && dsize == 8));
    if (ssize == 4) { x64_rm(e, 8, X64_OP_EXACT | 0x63, dst, m); return; }
    x64_rm(e, dsize, X64_OP_EXACT | 0x100 | (ssize == 1 ? 0xBE : 0xBF), dst, m);
}
/* lea dst, [mem] (size 4 or 8) */
static inline void emit_lea(emit_t *e, int size, int dst, const x64_mem_t *m) { x64_rm(e, size, X64_OP_EXACT | 0x8D, dst, m); }
/* xchg a, b (registers; no short form, so both are r/m) */
static inline void emit_xchg_rr(emit_t *e, int size, int a, int b) { x64_rr(e, size, 0x87, a, b); }
static inline void emit_xchg_rm(emit_t *e, int size, int r, const x64_mem_t *m) { x64_rm(e, size, 0x87, r, m); }
/* cmovcc dst, src (size 2, 4, 8) */
static inline void emit_cmov_rr(emit_t *e, int size, int cc, int dst, int src) { x64_rr(e, size, X64_OP_EXACT | 0x100 | (0x40 | cc), dst, src); }
static inline void emit_cmov_rm(emit_t *e, int size, int cc, int dst, const x64_mem_t *m) { x64_rm(e, size, X64_OP_EXACT | 0x100 | (0x40 | cc), dst, m); }
/* setcc r8 / byte [mem] */
static inline void emit_setcc_r(emit_t *e, int cc, int r) { x64_rr(e, 1, X64_OP_EXACT | 0x100 | (0x90 | cc), X64_DIGIT(0), r); }
static inline void emit_setcc_m(emit_t *e, int cc, const x64_mem_t *m) { x64_rm(e, 1, X64_OP_EXACT | 0x100 | (0x90 | cc), X64_DIGIT(0), m); }
/* bswap r32/r64 */
static inline void emit_bswap(emit_t *e, int size, int r) {
    x64_rex(e, size, X64_NOREG, X64_NOREG, r);
    emit_byte(e, 0x0F); emit_byte(e, (uint8_t)(0xC8 | (r & 7)));
}

/* ======================================================================
 * ALU: add/or/adc/sbb/and/sub/xor/cmp at any width
 * ====================================================================== */

static inline void emit_alu_rr(emit_t *e, int size, int alu, int dst, int src) { x64_rr(e, size, (alu << 3) | 1, src, dst); }
static inline void emit_alu_rm(emit_t *e, int size, int alu, int dst, const x64_mem_t *m) { x64_rm(e, size, (alu << 3) | 3, dst, m); }
static inline void emit_alu_mr(emit_t *e, int size, int alu, const x64_mem_t *m, int src) { x64_rm(e, size, (alu << 3) | 1, src, m); }
/* op r/m, imm: the sign-extended imm8 form (83) when it fits, else 81
 * (80 for bytes). For a 64-bit destination the immediate is 32 bits. */
static inline void emit_alu_ri(emit_t *e, int size, int alu, int dst, uint32_t imm) {
    int32_t s = size == 1 ? (int8_t)imm : size == 2 ? (int16_t)imm : (int32_t)imm;
    if (size != 1 && x64_fits8(s)) { x64_rr(e, size, X64_OP_EXACT | 0x83, X64_DIGIT(alu), dst); emit_byte(e, (uint8_t)s); }
    else { x64_rr(e, size, 0x81, X64_DIGIT(alu), dst); x64_imm(e, size == 8 ? 4 : size, imm); }
}
static inline void emit_alu_mi(emit_t *e, int size, int alu, const x64_mem_t *m, uint32_t imm) {
    int32_t s = size == 1 ? (int8_t)imm : size == 2 ? (int16_t)imm : (int32_t)imm;
    if (size != 1 && x64_fits8(s)) { x64_rm(e, size, X64_OP_EXACT | 0x83, X64_DIGIT(alu), m); emit_byte(e, (uint8_t)s); }
    else { x64_rm(e, size, 0x81, X64_DIGIT(alu), m); x64_imm(e, size == 8 ? 4 : size, imm); }
}
/* test a, b / test r/m, imm */
static inline void emit_test_rr(emit_t *e, int size, int a, int b) { x64_rr(e, size, 0x85, b, a); }
static inline void emit_test_mr(emit_t *e, int size, const x64_mem_t *m, int r) { x64_rm(e, size, 0x85, r, m); }
static inline void emit_test_ri(emit_t *e, int size, int r, uint32_t imm) { x64_rr(e, size, 0xF7, X64_DIGIT(X64_G3_TEST), r); x64_imm(e, size == 8 ? 4 : size, imm); }
static inline void emit_test_mi(emit_t *e, int size, const x64_mem_t *m, uint32_t imm) { x64_rm(e, size, 0xF7, X64_DIGIT(X64_G3_TEST), m); x64_imm(e, size == 8 ? 4 : size, imm); }
/* inc/dec r/m */
static inline void emit_inc_r(emit_t *e, int size, int r) { x64_rr(e, size, 0xFF, X64_DIGIT(0), r); }
static inline void emit_dec_r(emit_t *e, int size, int r) { x64_rr(e, size, 0xFF, X64_DIGIT(1), r); }
static inline void emit_inc_m(emit_t *e, int size, const x64_mem_t *m) { x64_rm(e, size, 0xFF, X64_DIGIT(0), m); }
static inline void emit_dec_m(emit_t *e, int size, const x64_mem_t *m) { x64_rm(e, size, 0xFF, X64_DIGIT(1), m); }
/* group 3: not/neg/mul/imul/div/idiv r/m */
static inline void emit_g3_r(emit_t *e, int size, int g3, int r) { x64_rr(e, size, 0xF7, X64_DIGIT(g3), r); }
static inline void emit_g3_m(emit_t *e, int size, int g3, const x64_mem_t *m) { x64_rm(e, size, 0xF7, X64_DIGIT(g3), m); }
/* imul dst, src[, imm]: the two- and three-operand forms (size 2, 4, 8) */
static inline void emit_imul_rr(emit_t *e, int size, int dst, int src) { x64_rr(e, size, X64_OP_EXACT | 0x100 | 0xAF, dst, src); }
static inline void emit_imul_rm(emit_t *e, int size, int dst, const x64_mem_t *m) { x64_rm(e, size, X64_OP_EXACT | 0x100 | 0xAF, dst, m); }
static inline void emit_imul_rri(emit_t *e, int size, int dst, int src, uint32_t imm) {
    int32_t s = size == 2 ? (int16_t)imm : (int32_t)imm;
    if (x64_fits8(s)) { x64_rr(e, size, X64_OP_EXACT | 0x6B, dst, src); emit_byte(e, (uint8_t)s); }
    else { x64_rr(e, size, X64_OP_EXACT | 0x69, dst, src); x64_imm(e, size == 8 ? 4 : size, imm); }
}
static inline void emit_imul_rmi(emit_t *e, int size, int dst, const x64_mem_t *m, uint32_t imm) {
    int32_t s = size == 2 ? (int16_t)imm : (int32_t)imm;
    if (x64_fits8(s)) { x64_rm(e, size, X64_OP_EXACT | 0x6B, dst, m); emit_byte(e, (uint8_t)s); }
    else { x64_rm(e, size, X64_OP_EXACT | 0x69, dst, m); x64_imm(e, size == 8 ? 4 : size, imm); }
}

/* ======================================================================
 * Shifts and rotates: by 1, by imm8, by CL
 * ====================================================================== */

static inline void emit_shift_ri(emit_t *e, int size, int sh, int r, uint8_t n) {
    if (n == 1) x64_rr(e, size, 0xD1, X64_DIGIT(sh), r);
    else { x64_rr(e, size, 0xC1, X64_DIGIT(sh), r); emit_byte(e, n); }
}
static inline void emit_shift_mi(emit_t *e, int size, int sh, const x64_mem_t *m, uint8_t n) {
    if (n == 1) x64_rm(e, size, 0xD1, X64_DIGIT(sh), m);
    else { x64_rm(e, size, 0xC1, X64_DIGIT(sh), m); emit_byte(e, n); }
}
static inline void emit_shift_rcl(emit_t *e, int size, int sh, int r) { x64_rr(e, size, 0xD3, X64_DIGIT(sh), r); }
static inline void emit_shift_mcl(emit_t *e, int size, int sh, const x64_mem_t *m) { x64_rm(e, size, 0xD3, X64_DIGIT(sh), m); }
/* shld/shrd r/m, r, imm8 | cl  (size 2, 4, 8) */
static inline void emit_shld_rri(emit_t *e, int size, int dst, int src, uint8_t n) { x64_rr(e, size, X64_OP_EXACT | 0x100 | 0xA4, src, dst); emit_byte(e, n); }
static inline void emit_shrd_rri(emit_t *e, int size, int dst, int src, uint8_t n) { x64_rr(e, size, X64_OP_EXACT | 0x100 | 0xAC, src, dst); emit_byte(e, n); }
static inline void emit_shld_rrcl(emit_t *e, int size, int dst, int src) { x64_rr(e, size, X64_OP_EXACT | 0x100 | 0xA5, src, dst); }
static inline void emit_shrd_rrcl(emit_t *e, int size, int dst, int src) { x64_rr(e, size, X64_OP_EXACT | 0x100 | 0xAD, src, dst); }
static inline void emit_shld_mri(emit_t *e, int size, const x64_mem_t *m, int src, uint8_t n) { x64_rm(e, size, X64_OP_EXACT | 0x100 | 0xA4, src, m); emit_byte(e, n); }
static inline void emit_shrd_mri(emit_t *e, int size, const x64_mem_t *m, int src, uint8_t n) { x64_rm(e, size, X64_OP_EXACT | 0x100 | 0xAC, src, m); emit_byte(e, n); }
static inline void emit_shld_mrcl(emit_t *e, int size, const x64_mem_t *m, int src) { x64_rm(e, size, X64_OP_EXACT | 0x100 | 0xA5, src, m); }
static inline void emit_shrd_mrcl(emit_t *e, int size, const x64_mem_t *m, int src) { x64_rm(e, size, X64_OP_EXACT | 0x100 | 0xAD, src, m); }

/* ======================================================================
 * Bit operations
 * ====================================================================== */

/* bt/bts/btr/btc r/m, imm8 | reg  (size 2, 4, 8) */
static inline void emit_bt_ri(emit_t *e, int size, int bt, int r, uint8_t bit) { x64_rr(e, size, X64_OP_EXACT | 0x100 | 0xBA, X64_DIGIT(bt), r); emit_byte(e, bit); }
static inline void emit_bt_mi(emit_t *e, int size, int bt, const x64_mem_t *m, uint8_t bit) { x64_rm(e, size, X64_OP_EXACT | 0x100 | 0xBA, X64_DIGIT(bt), m); emit_byte(e, bit); }
static inline void emit_bt_rr(emit_t *e, int size, int bt, int r, int bitreg) { x64_rr(e, size, X64_OP_EXACT | 0x100 | (0xA3 + 8 * (bt - X64_BT_BT)), bitreg, r); }
static inline void emit_bt_mr(emit_t *e, int size, int bt, const x64_mem_t *m, int bitreg) { x64_rm(e, size, X64_OP_EXACT | 0x100 | (0xA3 + 8 * (bt - X64_BT_BT)), bitreg, m); }
/* bsf/bsr dst, r/m */
static inline void emit_bsf_rr(emit_t *e, int size, int dst, int src) { x64_rr(e, size, X64_OP_EXACT | 0x100 | 0xBC, dst, src); }
static inline void emit_bsr_rr(emit_t *e, int size, int dst, int src) { x64_rr(e, size, X64_OP_EXACT | 0x100 | 0xBD, dst, src); }
static inline void emit_bsf_rm(emit_t *e, int size, int dst, const x64_mem_t *m) { x64_rm(e, size, X64_OP_EXACT | 0x100 | 0xBC, dst, m); }
static inline void emit_bsr_rm(emit_t *e, int size, int dst, const x64_mem_t *m) { x64_rm(e, size, X64_OP_EXACT | 0x100 | 0xBD, dst, m); }
/* xadd / cmpxchg r/m, r */
static inline void emit_xadd_rr(emit_t *e, int size, int dst, int src) { x64_rr(e, size, 0x100 | 0xC1, src, dst); }
static inline void emit_xadd_mr(emit_t *e, int size, const x64_mem_t *m, int src) { x64_rm(e, size, 0x100 | 0xC1, src, m); }
static inline void emit_cmpxchg_rr(emit_t *e, int size, int dst, int src) { x64_rr(e, size, 0x100 | 0xB1, src, dst); }
static inline void emit_cmpxchg_mr(emit_t *e, int size, const x64_mem_t *m, int src) { x64_rm(e, size, 0x100 | 0xB1, src, m); }

/* ======================================================================
 * Flags, sign extension, string ops
 * ====================================================================== */

static inline void emit_lahf(emit_t *e)  { emit_byte(e, 0x9F); }
static inline void emit_sahf(emit_t *e)  { emit_byte(e, 0x9E); }
static inline void emit_pushfq(emit_t *e) { emit_byte(e, 0x9C); }
static inline void emit_popfq(emit_t *e)  { emit_byte(e, 0x9D); }
static inline void emit_clc(emit_t *e) { emit_byte(e, 0xF8); }
static inline void emit_stc(emit_t *e) { emit_byte(e, 0xF9); }
static inline void emit_cmc(emit_t *e) { emit_byte(e, 0xF5); }
static inline void emit_cld(emit_t *e) { emit_byte(e, 0xFC); }
static inline void emit_std(emit_t *e) { emit_byte(e, 0xFD); }
/* cbw (size 2: AL→AX) / cwde (4) / cdqe (8); cwd / cdq / cqo */
static inline void emit_cbw(emit_t *e, int size) { x64_prefix(e, size); if (size == 8) emit_byte(e, 0x48); emit_byte(e, 0x98); }
static inline void emit_cwd(emit_t *e, int size) { x64_prefix(e, size); if (size == 8) emit_byte(e, 0x48); emit_byte(e, 0x99); }
/* String instructions at SIZE, optionally REP (F3) / REPNE (F2). The
 * host's RSI/RDI/RCX and DF are the operands, as the guest's would be. */
static inline void x64_string(emit_t *e, int size, int rep, uint8_t op) {
    if (rep) emit_byte(e, (uint8_t)rep);
    x64_prefix(e, size);
    if (size == 8) emit_byte(e, 0x48);
    emit_byte(e, (uint8_t)(size == 1 ? op : op | 1));
}
static inline void emit_movs(emit_t *e, int size, int rep) { x64_string(e, size, rep, 0xA4); }
static inline void emit_cmps(emit_t *e, int size, int rep) { x64_string(e, size, rep, 0xA6); }
static inline void emit_stos(emit_t *e, int size, int rep) { x64_string(e, size, rep, 0xAA); }
static inline void emit_lods(emit_t *e, int size, int rep) { x64_string(e, size, rep, 0xAC); }
static inline void emit_scas(emit_t *e, int size, int rep) { x64_string(e, size, rep, 0xAE); }

/* ======================================================================
 * Control flow
 * ====================================================================== */

/* Jcc/JMP rel32 with a zero placeholder; returns the rel32 field's
 * offset for emit_patch_rel32. */
static inline uint32_t emit_jcc_rel32(emit_t *e, int cc) {
    emit_byte(e, 0x0F); emit_byte(e, (uint8_t)(0x80 | cc));
    uint32_t at = emit_pos(e); emit_u32(e, 0); return at;
}
static inline uint32_t emit_jmp_rel32(emit_t *e) {
    emit_byte(e, 0xE9);
    uint32_t at = emit_pos(e); emit_u32(e, 0); return at;
}
static inline void emit_jmp_rel32_to(emit_t *e, uint32_t target_off) { emit_patch_rel32(e, emit_jmp_rel32(e), target_off); }
static inline void emit_jcc_rel32_to(emit_t *e, int cc, uint32_t target_off) { emit_patch_rel32(e, emit_jcc_rel32(e, cc), target_off); }
/* The rel8 forms, for short forward skips; returns the rel8 field's offset. */
static inline uint32_t emit_jcc_rel8(emit_t *e, int cc) { emit_byte(e, (uint8_t)(0x70 | cc)); uint32_t at = emit_pos(e); emit_byte(e, 0); return at; }
static inline uint32_t emit_jmp_rel8(emit_t *e) { emit_byte(e, 0xEB); uint32_t at = emit_pos(e); emit_byte(e, 0); return at; }
/* jrcxz / jecxz rel8: the one flag-free conditional branch */
static inline uint32_t emit_jrcxz_rel8(emit_t *e) { emit_byte(e, 0xE3); uint32_t at = emit_pos(e); emit_byte(e, 0); return at; }
static inline void emit_jmp_r(emit_t *e, int r) { x64_rr(e, 4, X64_OP_EXACT | 0xFF, X64_DIGIT(4), r); }
static inline void emit_jmp_m(emit_t *e, const x64_mem_t *m) { x64_rm(e, 4, X64_OP_EXACT | 0xFF, X64_DIGIT(4), m); }
static inline void emit_call_r(emit_t *e, int r) { x64_rr(e, 4, X64_OP_EXACT | 0xFF, X64_DIGIT(2), r); }
static inline void emit_call_m(emit_t *e, const x64_mem_t *m) { x64_rm(e, 4, X64_OP_EXACT | 0xFF, X64_DIGIT(2), m); }
static inline uint32_t emit_call_rel32(emit_t *e) { emit_byte(e, 0xE8); uint32_t at = emit_pos(e); emit_u32(e, 0); return at; }
static inline void emit_call_rel32_to(emit_t *e, uint32_t target_off) { emit_patch_rel32(e, emit_call_rel32(e), target_off); }
static inline void emit_ret(emit_t *e) { emit_byte(e, 0xC3); }
static inline void emit_push_r(emit_t *e, int r) { x64_rex(e, 4, X64_NOREG, X64_NOREG, r); emit_byte(e, (uint8_t)(0x50 | (r & 7))); }
static inline void emit_pop_r(emit_t *e, int r)  { x64_rex(e, 4, X64_NOREG, X64_NOREG, r); emit_byte(e, (uint8_t)(0x58 | (r & 7))); }
static inline void emit_push_m(emit_t *e, const x64_mem_t *m) { x64_rm(e, 4, X64_OP_EXACT | 0xFF, X64_DIGIT(6), m); }
static inline void emit_pop_m(emit_t *e, const x64_mem_t *m)  { x64_rm(e, 4, X64_OP_EXACT | 0x8F, X64_DIGIT(0), m); }
static inline void emit_int3(emit_t *e) { emit_byte(e, 0xCC); }
static inline void emit_ud2(emit_t *e)  { emit_byte(e, 0x0F); emit_byte(e, 0x0B); }
static inline void emit_nop(emit_t *e)  { emit_byte(e, 0x90); }

/* ======================================================================
 * BMI2 (VEX-encoded, flag-free): rorx, shlx/shrx/sarx. Only emitted
 * when the host has them (dbt_x64.c checks CPUID once).
 * ====================================================================== */

/* three-byte VEX: C4 [R X B m-mmmm] [W vvvv L pp] */
static inline void x64_vex3(emit_t *e, int w, int reg, int index, int base, int map, int vvvv, int pp) {
    emit_byte(e, 0xC4);
    emit_byte(e, (uint8_t)(((reg & 8) ? 0 : 0x80) | ((index >= 0 && (index & 8)) ? 0 : 0x40) | ((base & 8) ? 0 : 0x20) | map));
    emit_byte(e, (uint8_t)((w << 7) | ((~vvvv & 15) << 3) | pp));
}
/* rorx dst, src, imm8  (size 4 or 8): rotate right, no flags */
static inline void emit_rorx_rri(emit_t *e, int size, int dst, int src, uint8_t n) {
    x64_vex3(e, size == 8, dst, X64_NOREG, src, 3, 0, 3);   /* map 0F3A, pp = F2 */
    emit_byte(e, 0xF0); x64_modrm_reg(e, dst, src); emit_byte(e, n);
}
/* shlx/shrx/sarx dst, src, count  (size 4 or 8): shifts, no flags */
static inline void emit_shlx(emit_t *e, int size, int dst, int src, int cnt) { x64_vex3(e, size == 8, dst, X64_NOREG, src, 2, cnt, 1); emit_byte(e, 0xF7); x64_modrm_reg(e, dst, src); }
static inline void emit_shrx(emit_t *e, int size, int dst, int src, int cnt) { x64_vex3(e, size == 8, dst, X64_NOREG, src, 2, cnt, 3); emit_byte(e, 0xF7); x64_modrm_reg(e, dst, src); }
static inline void emit_sarx(emit_t *e, int size, int dst, int src, int cnt) { x64_vex3(e, size == 8, dst, X64_NOREG, src, 2, cnt, 2); emit_byte(e, 0xF7); x64_modrm_reg(e, dst, src); }

#endif /* EMIT_X64_H */
