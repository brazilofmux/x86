#ifndef EMIT_A64_H
#define EMIT_A64_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

/*
 * Lightweight AArch64 code emitter for JIT compilation.
 *
 * Lifted from ~/z80/dbt/emit_a64.h (itself from ~/riscv) — same
 * single-header static-inline style. The x86 additions (flag-setting
 * shifted arithmetic, MRS NZCV, BFXIL, TBZ/TBNZ, 32-bit pairs, X-offset
 * loads) are grouped at the end. Unused encoders cost nothing.
 *
 * The x86 translator's register convention is documented in dbt.h.
 *
 * AArch64 has fixed 4-byte instructions; the emitter writes one
 * little-endian uint32_t per instruction.
 */

/* Code buffer cursor — same shape as emit_t in emit_x64.h. */
typedef struct {
    uint8_t *buf;
    uint32_t offset;
    uint32_t capacity;
} emit_t;

/* ---- AArch64 register and condition encodings ---- */

/*
 * Register IDs are the architectural numbers 0..31. The W/X distinction
 * (32-bit vs 64-bit view) is part of the opcode, not the operand — every
 * emitter is named after the view it uses (`_w32_`, `_x64_`).
 *
 * Encoding 31 means WZR/XZR for arithmetic operations, and SP for
 * load/store/STP/LDP base-register slots — context-dependent, just like
 * real AArch64. Use A64_WZR / A64_XZR / A64_SP for clarity at call sites.
 */
typedef enum {
    A64_W0  =  0, A64_W1  =  1, A64_W2  =  2, A64_W3  =  3,
    A64_W4  =  4, A64_W5  =  5, A64_W6  =  6, A64_W7  =  7,
    A64_W8  =  8, A64_W9  =  9, A64_W10 = 10, A64_W11 = 11,
    A64_W12 = 12, A64_W13 = 13, A64_W14 = 14, A64_W15 = 15,
    A64_W16 = 16, A64_W17 = 17, A64_W18 = 18, A64_W19 = 19,
    A64_W20 = 20, A64_W21 = 21, A64_W22 = 22, A64_W23 = 23,
    A64_W24 = 24, A64_W25 = 25, A64_W26 = 26, A64_W27 = 27,
    A64_W28 = 28, A64_W29 = 29, A64_W30 = 30,
    A64_WZR = 31,
    A64_XZR = 31,
    A64_SP  = 31
} a64_reg_t;

typedef enum {
    A64_COND_EQ = 0x0,
    A64_COND_NE = 0x1,
    A64_COND_CS = 0x2, A64_COND_HS = 0x2,
    A64_COND_CC = 0x3, A64_COND_LO = 0x3,
    A64_COND_MI = 0x4,
    A64_COND_PL = 0x5,
    A64_COND_VS = 0x6,
    A64_COND_VC = 0x7,
    A64_COND_HI = 0x8,
    A64_COND_LS = 0x9,
    A64_COND_GE = 0xA,
    A64_COND_LT = 0xB,
    A64_COND_GT = 0xC,
    A64_COND_LE = 0xD,
    A64_COND_AL = 0xE
} a64_cond_t;

/* ---- Raw emit + cursor helpers ---- */

static inline void emit_inst(emit_t *e, uint32_t inst) {
    if (e->offset + 4 <= e->capacity) {
        uint8_t *p = e->buf + e->offset;
        p[0] = (uint8_t)(inst >>  0);
        p[1] = (uint8_t)(inst >>  8);
        p[2] = (uint8_t)(inst >> 16);
        p[3] = (uint8_t)(inst >> 24);
    }
    e->offset += 4;
}

static inline uint32_t emit_pos(emit_t *e) {
    return e->offset;
}

/* Patch a B (unconditional branch) at patch_offset to point at target_offset.
 * AArch64's B has a 26-bit signed immediate scaled by 4 — ±128 MB, more
 * than enough for any single JIT block. The assert catches range overflow
 * before P4 chaining stretches displacements; if it ever fires, the call
 * site needs a long-branch sequence (ADRP+ADD+BR) instead. */
static inline void emit_patch_b26(emit_t *e, uint32_t patch_offset, uint32_t target_offset) {
    int32_t disp = (int32_t)(target_offset - patch_offset);
    int32_t imm26 = disp >> 2;
    assert(imm26 >= -(1 << 25) && imm26 < (1 << 25)
           && "B target out of ±128 MB range — needs long-branch sequence");
    uint32_t inst = 0x14000000u | ((uint32_t)imm26 & 0x03FFFFFFu);
    uint8_t *p = e->buf + patch_offset;
    p[0] = (uint8_t)(inst >>  0);
    p[1] = (uint8_t)(inst >>  8);
    p[2] = (uint8_t)(inst >> 16);
    p[3] = (uint8_t)(inst >> 24);
}

/* Patch a B.cond / CBZ / CBNZ at patch_offset (19-bit imm at bits 23..5).
 * ±1 MB. P4 superblocks could approach this if a single block grows
 * large; the assert keeps any overflow from silently truncating into a
 * wild jump. */
static inline void emit_patch_cond19(emit_t *e, uint32_t patch_offset, uint32_t target_offset) {
    int32_t disp = (int32_t)(target_offset - patch_offset);
    int32_t imm19 = disp >> 2;
    assert(imm19 >= -(1 << 18) && imm19 < (1 << 18)
           && "B.cond/CBZ/CBNZ target out of ±1 MB range");
    uint32_t inst;
    memcpy(&inst, e->buf + patch_offset, 4);
    inst = (inst & ~(0x7FFFFu << 5)) | (((uint32_t)imm19 & 0x7FFFFu) << 5);
    uint8_t *p = e->buf + patch_offset;
    p[0] = (uint8_t)(inst >>  0);
    p[1] = (uint8_t)(inst >>  8);
    p[2] = (uint8_t)(inst >> 16);
    p[3] = (uint8_t)(inst >> 24);
}

/*
 * Cross-arch alias used by code in dbt_common.c-style helpers. On x86 the
 * "rel32" patch slot lives 4 bytes past the start of the branch (so the
 * displacement is `target - (patch + 4)`); on AArch64 the immediate is
 * embedded directly in the 4-byte B instruction at `patch`. We expose the
 * same name as emit_x64.h's emit_patch_rel32 so the translator can use a
 * single API for "patch the previously-emitted forward branch".
 */
static inline void emit_patch_rel32(emit_t *e, uint32_t patch_offset, uint32_t target_offset) {
    emit_patch_b26(e, patch_offset, target_offset);
}

/* ---- AArch64 logical-immediate encoder ----
 *
 * Logical immediates (used by AND/ORR/EOR-immediate forms) are a tightly
 * packed N:immr:imms field that encodes a rotated run of ones inside a
 * power-of-two-sized element. Not all 32-bit constants are encodable; the
 * caller falls back to MOV-imm + register form when this returns false.
 */
static inline bool a64_encode_logical_imm32(uint32_t val, uint32_t *encoded) {
    if (val == 0 || val == 0xFFFFFFFFu) return false;

    for (int esz = 2; esz <= 32; esz <<= 1) {
        uint32_t mask = (esz == 32) ? 0xFFFFFFFFu : ((1u << esz) - 1u);
        uint32_t elem = val & mask;

        bool uniform = true;
        for (int i = esz; i < 32; i += esz) {
            if (((val >> i) & mask) != elem) { uniform = false; break; }
        }
        if (!uniform) continue;

        for (int r = 0; r < esz; r++) {
            /* r==0 must not compute elem >> esz: for esz==32 that is a
             * shift by 32 - undefined behavior. Clang at -O3 can inline
             * this encoder with a constant operand (e.g. 1, encodable
             * only at element size 32), prove the path reaches UB, and
             * delete the CALLER's surrounding branch as unreachable -
             * silently dropping emitted instructions. Found in VCC's
             * copy of this header when the arm64 JIT's LSRA carry write
             * vanished at -O3 (vcc commit f8360ff). */
            uint32_t rotated = r ? (((elem << r) | (elem >> (esz - r))) & mask)
                                 : elem;
            if (rotated == 0) continue;
            int ones = __builtin_ctz(~rotated);
            uint32_t expected = (1u << ones) - 1u;
            if (rotated != expected) continue;
            if (ones == esz) continue;

            int s = ones - 1;
            int imms;
            switch (esz) {
                case 2:  imms = 0x3C | s; break;
                case 4:  imms = 0x38 | s; break;
                case 8:  imms = 0x30 | s; break;
                case 16: imms = 0x20 | s; break;
                case 32: imms = s;        break;
                default: continue;
            }
            *encoded = ((uint32_t)(r & 0x3F) << 6) | (uint32_t)(imms & 0x3F);
            return true;
        }
    }
    return false;
}

/* ---- Move immediate (MOVZ / MOVK / MOV pseudo) ---- */

static inline void emit_movz_w32(emit_t *e, a64_reg_t rd, uint16_t imm16, int shift) {
    uint32_t hw = (uint32_t)shift / 16u;
    uint32_t inst = 0x52800000u | (hw << 21) | ((uint32_t)imm16 << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_movk_w32(emit_t *e, a64_reg_t rd, uint16_t imm16, int shift) {
    uint32_t hw = (uint32_t)shift / 16u;
    uint32_t inst = 0x72800000u | (hw << 21) | ((uint32_t)imm16 << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* MOVN Wd, #imm16, LSL #shift  →  Wd = ~(imm16 << shift). Used to load
 * sparse all-ones constants in one instruction (e.g. 0xFFFFFF00 = MOVN #0xFF). */
static inline void emit_movn_w32(emit_t *e, a64_reg_t rd, uint16_t imm16, int shift) {
    uint32_t hw = (uint32_t)shift / 16u;
    uint32_t inst = 0x12800000u | (hw << 21) | ((uint32_t)imm16 << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_movz_x64(emit_t *e, a64_reg_t rd, uint16_t imm16, int shift) {
    uint32_t hw = (uint32_t)shift / 16u;
    uint32_t inst = 0xD2800000u | (hw << 21) | ((uint32_t)imm16 << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_movk_x64(emit_t *e, a64_reg_t rd, uint16_t imm16, int shift) {
    uint32_t hw = (uint32_t)shift / 16u;
    uint32_t inst = 0xF2800000u | (hw << 21) | ((uint32_t)imm16 << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* MOV Wd, #imm32 — emits 1 or 2 instructions, picking the shortest of
 * MOVZ (single non-zero halfword), MOVN (single non-zero halfword in the
 * inverted constant — covers 0xFFFFxxxx / 0xxxxxFFFF style values), or
 * MOVZ+MOVK as the general fallback. */
static inline void emit_mov_w32_imm32(emit_t *e, a64_reg_t rd, uint32_t imm) {
    uint16_t lo = (uint16_t)(imm & 0xFFFF);
    uint16_t hi = (uint16_t)(imm >> 16);

    if (hi == 0)             { emit_movz_w32(e, rd, lo, 0);  return; }
    if (lo == 0)             { emit_movz_w32(e, rd, hi, 16); return; }

    uint32_t inv = ~imm;
    uint16_t nlo = (uint16_t)(inv & 0xFFFF);
    uint16_t nhi = (uint16_t)(inv >> 16);
    if (nhi == 0)            { emit_movn_w32(e, rd, nlo, 0);  return; }
    if (nlo == 0)            { emit_movn_w32(e, rd, nhi, 16); return; }

    emit_movz_w32(e, rd, lo, 0);
    emit_movk_w32(e, rd, hi, 16);
}

/* MOV Xd, #imm64 — up to 4 instructions. */
static inline void emit_mov_x64_imm64(emit_t *e, a64_reg_t rd, uint64_t imm) {
    uint16_t w0 = (uint16_t)(imm        & 0xFFFF);
    uint16_t w1 = (uint16_t)((imm >> 16) & 0xFFFF);
    uint16_t w2 = (uint16_t)((imm >> 32) & 0xFFFF);
    uint16_t w3 = (uint16_t)((imm >> 48) & 0xFFFF);
    bool first = true;
    if (w0 != 0 || (w1 == 0 && w2 == 0 && w3 == 0)) {
        emit_movz_x64(e, rd, w0, 0);
        first = false;
    }
    if (w1 != 0) {
        if (first) { emit_movz_x64(e, rd, w1, 16); first = false; }
        else       { emit_movk_x64(e, rd, w1, 16); }
    }
    if (w2 != 0) {
        if (first) { emit_movz_x64(e, rd, w2, 32); first = false; }
        else       { emit_movk_x64(e, rd, w2, 32); }
    }
    if (w3 != 0) {
        if (first) { emit_movz_x64(e, rd, w3, 48); }
        else       { emit_movk_x64(e, rd, w3, 48); }
    }
}

/* ---- Register-register move ---- */

static inline void emit_mov_w32_w32(emit_t *e, a64_reg_t rd, a64_reg_t rs) {
    /* ORR Wd, WZR, Ws */
    uint32_t inst = 0x2A0003E0u | ((uint32_t)(rs & 0x1F) << 16) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_mov_x64_x64(emit_t *e, a64_reg_t rd, a64_reg_t rs) {
    /* ORR Xd, XZR, Xs */
    uint32_t inst = 0xAA0003E0u | ((uint32_t)(rs & 0x1F) << 16) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* ---- Load/store with unsigned immediate offset ---- */

/* LDR Wt, [Xn, #imm]  (offset must be 4-aligned, 0..16380) */
static inline void emit_ldr_w32_imm(emit_t *e, a64_reg_t rt, a64_reg_t rn, uint32_t byte_offset) {
    uint32_t scaled = byte_offset / 4u;
    uint32_t inst = 0xB9400000u | ((scaled & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    emit_inst(e, inst);
}

/* STR Wt, [Xn, #imm] */
static inline void emit_str_w32_imm(emit_t *e, a64_reg_t rt, a64_reg_t rn, uint32_t byte_offset) {
    uint32_t scaled = byte_offset / 4u;
    uint32_t inst = 0xB9000000u | ((scaled & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    emit_inst(e, inst);
}

/* LDR Xt, [Xn, #imm]  (offset must be 8-aligned, 0..32760) */
static inline void emit_ldr_x64_imm(emit_t *e, a64_reg_t rt, a64_reg_t rn, uint32_t byte_offset) {
    uint32_t scaled = byte_offset / 8u;
    uint32_t inst = 0xF9400000u | ((scaled & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    emit_inst(e, inst);
}

/* STR Xt, [Xn, #imm] */
static inline void emit_str_x64_imm(emit_t *e, a64_reg_t rt, a64_reg_t rn, uint32_t byte_offset) {
    uint32_t scaled = byte_offset / 8u;
    uint32_t inst = 0xF9000000u | ((scaled & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    emit_inst(e, inst);
}

/* LDRB Wt, [Xn, #imm] */
static inline void emit_ldrb_imm(emit_t *e, a64_reg_t rt, a64_reg_t rn, uint32_t byte_offset) {
    uint32_t inst = 0x39400000u | ((byte_offset & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    emit_inst(e, inst);
}

/* LDRSB Wt, [Xn, #imm]  (sign-extend to 32-bit) */
static inline void emit_ldrsb_w32_imm(emit_t *e, a64_reg_t rt, a64_reg_t rn, uint32_t byte_offset) {
    uint32_t inst = 0x39C00000u | ((byte_offset & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    emit_inst(e, inst);
}

/* LDRH Wt, [Xn, #imm]  (offset 2-aligned) */
static inline void emit_ldrh_imm(emit_t *e, a64_reg_t rt, a64_reg_t rn, uint32_t byte_offset) {
    uint32_t scaled = byte_offset / 2u;
    uint32_t inst = 0x79400000u | ((scaled & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    emit_inst(e, inst);
}

/* LDRSH Wt, [Xn, #imm]  (sign-extend halfword to 32-bit) */
static inline void emit_ldrsh_w32_imm(emit_t *e, a64_reg_t rt, a64_reg_t rn, uint32_t byte_offset) {
    uint32_t scaled = byte_offset / 2u;
    uint32_t inst = 0x79C00000u | ((scaled & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    emit_inst(e, inst);
}

/* STRB Wt, [Xn, #imm] */
static inline void emit_strb_imm(emit_t *e, a64_reg_t rt, a64_reg_t rn, uint32_t byte_offset) {
    uint32_t inst = 0x39000000u | ((byte_offset & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    emit_inst(e, inst);
}

/* STRH Wt, [Xn, #imm]  (offset 2-aligned) */
static inline void emit_strh_imm(emit_t *e, a64_reg_t rt, a64_reg_t rn, uint32_t byte_offset) {
    uint32_t scaled = byte_offset / 2u;
    uint32_t inst = 0x79000000u | ((scaled & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    emit_inst(e, inst);
}

/* ---- Load/store with register offset, UXTW extension (guest-mem accesses) ----
 *
 * The translator computes a 32-bit guest virtual address in some Wm, then
 * forms the host pointer as `mem_base + (uint32_t)Wm`. The UXTW form does
 * exactly this in one instruction:  ldr Wt, [Xn, Wm, UXTW]  ≡  ldr Wt,
 * [Xn + ZeroExtend64(Wm)].
 */
static inline void emit_ldst_reg_uxtw(emit_t *e, uint32_t size, uint32_t opc,
                                       a64_reg_t rt, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = ((size & 3u) << 30) | ((opc & 3u) << 22) | 0x38204800u
                  | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) <<  5)
                  | (rt & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_ldr_w32_reg_uxtw(emit_t *e, a64_reg_t rt, a64_reg_t rn, a64_reg_t rm) {
    emit_ldst_reg_uxtw(e, 2, 1, rt, rn, rm);
}
static inline void emit_str_w32_reg_uxtw(emit_t *e, a64_reg_t rt, a64_reg_t rn, a64_reg_t rm) {
    emit_ldst_reg_uxtw(e, 2, 0, rt, rn, rm);
}
static inline void emit_ldrb_reg_uxtw(emit_t *e, a64_reg_t rt, a64_reg_t rn, a64_reg_t rm) {
    emit_ldst_reg_uxtw(e, 0, 1, rt, rn, rm);
}
static inline void emit_ldrsb_w32_reg_uxtw(emit_t *e, a64_reg_t rt, a64_reg_t rn, a64_reg_t rm) {
    emit_ldst_reg_uxtw(e, 0, 3, rt, rn, rm);
}
static inline void emit_ldrh_reg_uxtw(emit_t *e, a64_reg_t rt, a64_reg_t rn, a64_reg_t rm) {
    emit_ldst_reg_uxtw(e, 1, 1, rt, rn, rm);
}
static inline void emit_ldrsh_w32_reg_uxtw(emit_t *e, a64_reg_t rt, a64_reg_t rn, a64_reg_t rm) {
    emit_ldst_reg_uxtw(e, 1, 3, rt, rn, rm);
}
static inline void emit_strb_reg_uxtw(emit_t *e, a64_reg_t rt, a64_reg_t rn, a64_reg_t rm) {
    emit_ldst_reg_uxtw(e, 0, 0, rt, rn, rm);
}
static inline void emit_strh_reg_uxtw(emit_t *e, a64_reg_t rt, a64_reg_t rn, a64_reg_t rm) {
    emit_ldst_reg_uxtw(e, 1, 0, rt, rn, rm);
}

/* LDR Xt, [Xn, Xm, LSL #3]  (8-byte register-scaled load) — used by the
 * inline-cache probe to fetch native_code from the block_entry_t. */
static inline void emit_ldr_x64_reg_lsl3(emit_t *e, a64_reg_t rt, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0xF8607800u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    emit_inst(e, inst);
}

/* ---- Integer arithmetic (32-bit / 64-bit) ---- */

static inline void emit_add_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0x0B000000u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_add_w32_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm12) {
    uint32_t inst = 0x11000000u | ((imm12 & 0xFFFu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* ADD Wd, Wn, #imm12, LSL #12 — adds imm12 << 12. Lets the pinned-register
 * backend reach the +0x10000 (bitmap) and +0x20000 (cache) segments of the
 * JIT aux block in a single instruction. */
static inline void emit_add_w32_imm_lsl12(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm12) {
    uint32_t inst = 0x11400000u | ((imm12 & 0xFFFu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_sub_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0x4B000000u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_sub_w32_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm12) {
    uint32_t inst = 0x51000000u | ((imm12 & 0xFFFu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_neg_w32(emit_t *e, a64_reg_t rd, a64_reg_t rm) {
    emit_sub_w32(e, rd, A64_WZR, rm);
}

/* MUL Wd, Wn, Wm  (MADD with Ra=WZR) */
static inline void emit_mul_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0x1B007C00u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* MSUB Wd, Wn, Wm, Wa  →  Wd = Wa - Wn*Wm  (used to compute REM = a - (a/b)*b) */
static inline void emit_msub_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm, a64_reg_t ra) {
    uint32_t inst = 0x1B008000u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(ra & 0x1F) << 10)
                  | ((uint32_t)(rn & 0x1F) <<  5) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_sdiv_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0x1AC00C00u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_udiv_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0x1AC00800u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* SMULL Xd, Wn, Wm  (signed 32x32 → 64) — supports MULH variants. */
static inline void emit_smull(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0x9B207C00u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* UMULL Xd, Wn, Wm  (unsigned 32x32 → 64) — supports MULHU. */
static inline void emit_umull(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0x9BA07C00u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* SMADDL Xd, Wn, Wm, Xa  (signed 32x32+64 → 64) */
static inline void emit_smaddl(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm, a64_reg_t ra) {
    uint32_t inst = 0x9B200000u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(ra & 0x1F) << 10)
                  | ((uint32_t)(rn & 0x1F) <<  5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* MUL Xd, Xn, Xm  (64-bit MADD with Ra=XZR) — used by MULHSU to combine a
 * sign-extended and a zero-extended operand into a 64-bit product. */
static inline void emit_mul_x64(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0x9B007C00u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* SXTW Xd, Wn  (SBFM Xd, Xn, #0, #31) — sign-extend a 32-bit value to 64. */
static inline void emit_sxtw_x64_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn) {
    uint32_t inst = 0x93407C00u | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* LSL Xd, Xn, #shift  (UBFM Xd, Xn, #(64-shift), #(63-shift)) */
static inline void emit_lsl_x64_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t shift) {
    shift &= 63u;
    if (shift == 0) {
        if (rd != rn) emit_mov_x64_x64(e, rd, rn);
        return;
    }
    uint32_t immr = (64u - shift) & 63u;
    uint32_t imms = 63u - shift;
    /* UBFM Xd, Xn, immr, imms: 1 10 100110 1 immr imms Rn Rd  = 0xD3400000 */
    uint32_t inst = 0xD3400000u | ((immr & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* EOR Xd, Xn, Xm */
static inline void emit_eor_x64(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0xCA000000u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* MVN Xd, Xm  (ORN Xd, XZR, Xm) */
static inline void emit_mvn_x64(emit_t *e, a64_reg_t rd, a64_reg_t rm) {
    uint32_t inst = 0xAA2003E0u | ((uint32_t)(rm & 0x1F) << 16) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* ORR Xd, Xn, Xm, LSL #shift */
static inline void emit_orr_x64_lsl(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm, uint32_t shift) {
    /* Shift type 00 = LSL; imm6 at bits 15:10 */
    uint32_t inst = 0xAA000000u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((shift & 0x3Fu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* ADD Xd, Xn, #imm12  (64-bit add immediate) */
static inline void emit_add_x64_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm12) {
    uint32_t inst = 0x91000000u | ((imm12 & 0xFFFu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* ADD Xd, Xn, Xm */
static inline void emit_add_x64(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0x8B000000u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* SUB Xd, Xn, Xm */
static inline void emit_sub_x64(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0xCB000000u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* ADD Xd, Xn, Wm, UXTW  (zero-extend 32-bit then add — handy for forming
 * host pointers from 32-bit guest addresses without an UXTW move). */
static inline void emit_add_x64_w32_uxtw(emit_t *e, a64_reg_t xd, a64_reg_t xn, a64_reg_t wm) {
    uint32_t inst = 0x8B204000u | ((uint32_t)(wm & 0x1F) << 16)
                  | ((uint32_t)(xn & 0x1F) << 5) | (xd & 0x1F);
    emit_inst(e, inst);
}

/* ---- Logical (32-bit) ---- */

static inline void emit_and_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0x0A000000u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline bool emit_and_w32_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm) {
    uint32_t enc;
    if (!a64_encode_logical_imm32(imm, &enc)) return false;
    uint32_t inst = 0x12000000u | (enc << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
    return true;
}

static inline void emit_orr_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0x2A000000u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline bool emit_orr_w32_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm) {
    uint32_t enc;
    if (!a64_encode_logical_imm32(imm, &enc)) return false;
    uint32_t inst = 0x32000000u | (enc << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
    return true;
}

static inline void emit_eor_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0x4A000000u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline bool emit_eor_w32_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm) {
    uint32_t enc;
    if (!a64_encode_logical_imm32(imm, &enc)) return false;
    uint32_t inst = 0x52000000u | (enc << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
    return true;
}

static inline void emit_mvn_w32(emit_t *e, a64_reg_t rd, a64_reg_t rm) {
    /* ORN Wd, WZR, Wm */
    uint32_t inst = 0x2A2003E0u | ((uint32_t)(rm & 0x1F) << 16) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* ORR Wd, Wn, Wm, LSL #shift — shifted-register OR. Assembles hi:lo
 * halfwords (e.g. POP's hi<<8|lo, PUSH AF's A<<8|F) in one insn. */
static inline void emit_orr_w32_lsl(emit_t *e, a64_reg_t rd, a64_reg_t rn,
                                    a64_reg_t rm, uint32_t shift) {
    uint32_t inst = 0x2A000000u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((shift & 0x1Fu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* BFI Wd, Wn, #lsb, #width — insert the low `width` bits of Wn into Wd
 * at bit position lsb, leaving the rest of Wd intact (BFM alias). The
 * pinned-register backend writes one 8-bit half of a guest 16-bit pair
 * this way without disturbing the other half. */
static inline void emit_bfi_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn,
                                uint32_t lsb, uint32_t width) {
    uint32_t immr = (32u - lsb) & 31u;
    uint32_t imms = (width - 1u) & 31u;
    uint32_t inst = 0x33000000u | ((immr & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* UBFX Wd, Wn, #lsb, #width — extract `width` bits at lsb, zero-extended
 * (UBFM alias). Reads one 8-bit half of a pinned guest register pair. */
static inline void emit_ubfx_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn,
                                 uint32_t lsb, uint32_t width) {
    uint32_t immr = lsb & 31u;
    uint32_t imms = (lsb + width - 1u) & 31u;
    uint32_t inst = 0x53000000u | ((immr & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_tst_w32(emit_t *e, a64_reg_t rn, a64_reg_t rm) {
    /* ANDS WZR, Wn, Wm */
    uint32_t inst = 0x6A00001Fu | ((uint32_t)(rm & 0x1F) << 16) | ((uint32_t)(rn & 0x1F) << 5);
    emit_inst(e, inst);
}

/* TST Wn, #imm — ANDS with WZR destination. Returns false if imm is not
 * a valid logical immediate (caller falls back to MOVZ+TST reg). */
static inline bool emit_tst_w32_imm(emit_t *e, a64_reg_t rn, uint32_t imm) {
    uint32_t enc;
    if (!a64_encode_logical_imm32(imm, &enc)) return false;
    uint32_t inst = 0x72000000u | (enc << 10) | ((uint32_t)(rn & 0x1F) << 5) | 0x1F;
    emit_inst(e, inst);
    return true;
}

/* CSEL Wd, Wn, Wm, cond — 32-bit conditional select. cond uses the
 * standard a64_cond_t encoding. If `cond` holds, Wd = Wn; else Wd = Wm. */
static inline void emit_csel_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn,
                                  a64_reg_t rm, a64_cond_t cond) {
    uint32_t inst = 0x1A800000u
                  | ((uint32_t)(rm & 0x1F) << 16)
                  | (((uint32_t)cond & 0xF) << 12)
                  | ((uint32_t)(rn & 0x1F) << 5)
                  | (rd & 0x1F);
    emit_inst(e, inst);
}

/* LDR Xt, [Xn, Wm, UXTW] — 8-byte load with the 32-bit offset zero-
 * extended (no scale). Companion to emit_ldr_w32_reg_uxtw; used by the
 * block-chain probe to fetch native_code at cache[idx] + 8. */
static inline void emit_ldr_x64_reg_uxtw(emit_t *e, a64_reg_t rt, a64_reg_t rn, a64_reg_t rm) {
    emit_ldst_reg_uxtw(e, 3, 1, rt, rn, rm);
}

/* ---- Shifts (variable + immediate, 32-bit) ---- */

static inline void emit_lslv_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0x1AC02000u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_lsrv_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0x1AC02400u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_asrv_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0x1AC02800u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* LSL Wd, Wn, #shift  (UBFM alias) */
static inline void emit_lsl_w32_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t shift) {
    shift &= 31u;
    if (shift == 0) {
        if (rd != rn) emit_mov_w32_w32(e, rd, rn);
        return;
    }
    uint32_t immr = (32u - shift) & 31u;
    uint32_t imms = 31u - shift;
    uint32_t inst = 0x53000000u | ((immr & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* LSR Wd, Wn, #shift  (UBFM alias) */
static inline void emit_lsr_w32_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t shift) {
    shift &= 31u;
    if (shift == 0) {
        if (rd != rn) emit_mov_w32_w32(e, rd, rn);
        return;
    }
    uint32_t inst = 0x53000000u | ((shift & 0x3Fu) << 16) | (31u << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* ASR Wd, Wn, #shift  (SBFM alias) */
static inline void emit_asr_w32_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t shift) {
    shift &= 31u;
    if (shift == 0) {
        if (rd != rn) emit_mov_w32_w32(e, rd, rn);
        return;
    }
    uint32_t inst = 0x13000000u | ((shift & 0x3Fu) << 16) | (31u << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* LSR Xd, Xn, #shift — used to extract MULH high half via 64-bit shift. */
static inline void emit_lsr_x64_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t shift) {
    shift &= 63u;
    if (shift == 0) {
        if (rd != rn) emit_mov_x64_x64(e, rd, rn);
        return;
    }
    uint32_t inst = 0xD340FC00u | ((shift & 0x3Fu) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_asr_x64_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t shift) {
    shift &= 63u;
    if (shift == 0) {
        if (rd != rn) emit_mov_x64_x64(e, rd, rn);
        return;
    }
    uint32_t inst = 0x9340FC00u | ((shift & 0x3Fu) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* ---- Compare ---- */

static inline void emit_cmp_w32_w32(emit_t *e, a64_reg_t rn, a64_reg_t rm) {
    /* SUBS WZR, Wn, Wm */
    uint32_t inst = 0x6B00001Fu | ((uint32_t)(rm & 0x1F) << 16) | ((uint32_t)(rn & 0x1F) << 5);
    emit_inst(e, inst);
}

static inline void emit_cmp_w32_imm(emit_t *e, a64_reg_t rn, uint32_t imm12) {
    /* SUBS WZR, Wn, #imm12 */
    uint32_t inst = 0x7100001Fu | ((imm12 & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5);
    emit_inst(e, inst);
}

static inline void emit_cmn_w32_imm(emit_t *e, a64_reg_t rn, uint32_t imm12) {
    /* ADDS WZR, Wn, #imm12 — compares against negative immediates by
     * adding the magnitude. */
    uint32_t inst = 0x3100001Fu | ((imm12 & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5);
    emit_inst(e, inst);
}

/* ---- Conditional set ---- */

/* CSET Wd, cond  (CSINC Wd, WZR, WZR, invert(cond)) */
static inline void emit_cset_w32(emit_t *e, a64_reg_t rd, a64_cond_t cond) {
    uint32_t inv_cond = (uint32_t)cond ^ 1u;
    uint32_t inst = 0x1A9F07E0u | ((inv_cond & 0xFu) << 12) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* ---- Branches ---- */

/* B PC-relative, ±128 MB. byte_offset==0 is the placeholder branch that
 * gets fixed up later via emit_patch_b26 — both forms share the same
 * range check. */
static inline void emit_b(emit_t *e, int32_t byte_offset) {
    int32_t imm26 = byte_offset >> 2;
    assert(imm26 >= -(1 << 25) && imm26 < (1 << 25));
    uint32_t inst = 0x14000000u | ((uint32_t)imm26 & 0x03FFFFFFu);
    emit_inst(e, inst);
}

/* B.cond PC-relative, ±1 MB. */
static inline void emit_b_cond(emit_t *e, a64_cond_t cond, int32_t byte_offset) {
    int32_t imm19 = byte_offset >> 2;
    assert(imm19 >= -(1 << 18) && imm19 < (1 << 18));
    uint32_t inst = 0x54000000u | (((uint32_t)imm19 & 0x7FFFFu) << 5) | (cond & 0xFu);
    emit_inst(e, inst);
}

static inline void emit_cbz_w32(emit_t *e, a64_reg_t rt, int32_t byte_offset) {
    int32_t imm19 = byte_offset >> 2;
    assert(imm19 >= -(1 << 18) && imm19 < (1 << 18));
    uint32_t inst = 0x34000000u | (((uint32_t)imm19 & 0x7FFFFu) << 5) | (rt & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_cbnz_w32(emit_t *e, a64_reg_t rt, int32_t byte_offset) {
    int32_t imm19 = byte_offset >> 2;
    assert(imm19 >= -(1 << 18) && imm19 < (1 << 18));
    uint32_t inst = 0x35000000u | (((uint32_t)imm19 & 0x7FFFFu) << 5) | (rt & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_cbz_x64(emit_t *e, a64_reg_t rt, int32_t byte_offset) {
    int32_t imm19 = byte_offset >> 2;
    assert(imm19 >= -(1 << 18) && imm19 < (1 << 18));
    uint32_t inst = 0xB4000000u | (((uint32_t)imm19 & 0x7FFFFu) << 5) | (rt & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_br(emit_t *e, a64_reg_t rn) {
    uint32_t inst = 0xD61F0000u | ((uint32_t)(rn & 0x1F) << 5);
    emit_inst(e, inst);
}

static inline void emit_blr(emit_t *e, a64_reg_t rn) {
    uint32_t inst = 0xD63F0000u | ((uint32_t)(rn & 0x1F) << 5);
    emit_inst(e, inst);
}

static inline void emit_ret(emit_t *e) {
    /* RET X30 */
    emit_inst(e, 0xD65F03C0u);
}

/* RET Xn — return through an arbitrary register. Same hardware return-
 * stack prediction as RET X30: any BL/BLR pushes the predictor, any RET
 * pops it, regardless of which register carries the target. */
static inline void emit_ret_reg(emit_t *e, a64_reg_t rn) {
    uint32_t inst = 0xD65F0000u | ((uint32_t)(rn & 0x1F) << 5);
    emit_inst(e, inst);
}

/* BL PC-relative, ±128 MB. byte_offset==0 is a placeholder for
 * emit_patch_bl26. */
static inline void emit_bl(emit_t *e, int32_t byte_offset) {
    int32_t imm26 = byte_offset >> 2;
    assert(imm26 >= -(1 << 25) && imm26 < (1 << 25));
    uint32_t inst = 0x94000000u | ((uint32_t)imm26 & 0x03FFFFFFu);
    emit_inst(e, inst);
}

/* Patch a BL at patch_offset to land on target_offset (same imm26 field
 * as B, different opcode — emit_patch_b26 would silently turn it into a
 * plain B and lose the hardware return-stack push). */
static inline void emit_patch_bl26(emit_t *e, uint32_t patch_offset, uint32_t target_offset) {
    int32_t disp = (int32_t)(target_offset - patch_offset);
    int32_t imm26 = disp >> 2;
    assert(imm26 >= -(1 << 25) && imm26 < (1 << 25));
    uint32_t inst = 0x94000000u | ((uint32_t)imm26 & 0x03FFFFFFu);
    uint8_t *p = e->buf + patch_offset;
    p[0] = (uint8_t)(inst >>  0);
    p[1] = (uint8_t)(inst >>  8);
    p[2] = (uint8_t)(inst >> 16);
    p[3] = (uint8_t)(inst >> 24);
}

/* CMP SP, Xm — SUBS XZR, SP, Xm, UXTX. The extended-register form is the
 * only ADDS/SUBS encoding that accepts SP as Rn. */
static inline void emit_cmp_sp_x64(emit_t *e, a64_reg_t rm) {
    uint32_t inst = 0xEB200000u | ((uint32_t)(rm & 0x1F) << 16)
                  | (3u << 13) | (31u << 5) | 31u;
    emit_inst(e, inst);
}

/* CMP Xn, #imm12, LSL #12 */
static inline void emit_cmp_x64_imm_lsl12(emit_t *e, a64_reg_t rn, uint32_t imm12) {
    uint32_t inst = 0xF140001Fu | ((imm12 & 0xFFFu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5);
    emit_inst(e, inst);
}

/* SUB Xd, Xn, #imm12, LSL #12 */
static inline void emit_sub_x64_imm_lsl12(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm12) {
    uint32_t inst = 0xD1400000u | ((imm12 & 0xFFFu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}

/* ---- Stack: STP/LDP with arbitrary base register (64-bit) ---- */

static inline void emit_stp_x64_pre(emit_t *e, a64_reg_t rt1, a64_reg_t rt2, a64_reg_t rn, int imm) {
    int32_t scaled = imm / 8;
    uint32_t inst = 0xA9800000u | (((uint32_t)scaled & 0x7Fu) << 15)
                  | ((uint32_t)(rt2 & 0x1F) << 10)
                  | ((uint32_t)(rn  & 0x1F) <<  5) | (rt1 & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_ldp_x64_post(emit_t *e, a64_reg_t rt1, a64_reg_t rt2, a64_reg_t rn, int imm) {
    int32_t scaled = imm / 8;
    uint32_t inst = 0xA8C00000u | (((uint32_t)scaled & 0x7Fu) << 15)
                  | ((uint32_t)(rt2 & 0x1F) << 10)
                  | ((uint32_t)(rn  & 0x1F) <<  5) | (rt1 & 0x1F);
    emit_inst(e, inst);
}

/* STP Xt1, Xt2, [Xn, #imm]   (unsigned offset, no write-back) */
static inline void emit_stp_x64_off(emit_t *e, a64_reg_t rt1, a64_reg_t rt2, a64_reg_t rn, int imm) {
    int32_t scaled = imm / 8;
    uint32_t inst = 0xA9000000u | (((uint32_t)scaled & 0x7Fu) << 15)
                  | ((uint32_t)(rt2 & 0x1F) << 10)
                  | ((uint32_t)(rn  & 0x1F) <<  5) | (rt1 & 0x1F);
    emit_inst(e, inst);
}

/* LDP Xt1, Xt2, [Xn, #imm]   (unsigned offset, no write-back) */
static inline void emit_ldp_x64_off(emit_t *e, a64_reg_t rt1, a64_reg_t rt2, a64_reg_t rn, int imm) {
    int32_t scaled = imm / 8;
    uint32_t inst = 0xA9400000u | (((uint32_t)scaled & 0x7Fu) << 15)
                  | ((uint32_t)(rt2 & 0x1F) << 10)
                  | ((uint32_t)(rn  & 0x1F) <<  5) | (rt1 & 0x1F);
    emit_inst(e, inst);
}

/* SP-based pre/post-index variants — same encoding with Rn=31. */
static inline void emit_stp_pre_sp(emit_t *e, a64_reg_t rt1, a64_reg_t rt2, int imm) {
    emit_stp_x64_pre(e, rt1, rt2, A64_SP, imm);
}

static inline void emit_ldp_post_sp(emit_t *e, a64_reg_t rt1, a64_reg_t rt2, int imm) {
    emit_ldp_x64_post(e, rt1, rt2, A64_SP, imm);
}

/* ---- Misc ---- */

static inline void emit_nop(emit_t *e)         { emit_inst(e, 0xD503201Fu); }
static inline void emit_brk(emit_t *e, uint16_t imm) {
    emit_inst(e, 0xD4200000u | ((uint32_t)imm << 5));
}

/* ============================================================================
 * Floating-point scalar (V-register file). The V0..V31 registers can be
 * accessed as B/H/S/D/Q views; the riscv DBT only uses the S (32-bit
 * single-precision) and D (64-bit double-precision) views. FP register
 * numbers in the parameter list below refer to the same V0..V31 numbering.
 *
 * Encodings ported from slow-32/tools/dbt/emit_a64.c.
 * ============================================================================ */

/* ---- FP <-> GP transfer ---- */

/* FMOV Sd, Wn  — bitcast 32-bit GP to single FP register */
static inline void emit_fmov_s_w32(emit_t *e, int sd, a64_reg_t wn) {
    uint32_t inst = 0x1E270000u | ((uint32_t)(wn & 0x1F) << 5) | (uint32_t)(sd & 0x1F);
    emit_inst(e, inst);
}

/* FMOV Wd, Sn  — extract single FP register bits into 32-bit GP */
static inline void emit_fmov_w32_s(emit_t *e, a64_reg_t wd, int sn) {
    uint32_t inst = 0x1E260000u | ((uint32_t)(sn & 0x1F) << 5) | (uint32_t)(wd & 0x1F);
    emit_inst(e, inst);
}

/* FMOV Dd, Xn  — bitcast 64-bit GP to double FP register */
static inline void emit_fmov_d_x64(emit_t *e, int dd, a64_reg_t xn) {
    uint32_t inst = 0x9E670000u | ((uint32_t)(xn & 0x1F) << 5) | (uint32_t)(dd & 0x1F);
    emit_inst(e, inst);
}

/* FMOV Xd, Dn  — extract double FP register bits into 64-bit GP */
static inline void emit_fmov_x64_d(emit_t *e, a64_reg_t xd, int dn) {
    uint32_t inst = 0x9E660000u | ((uint32_t)(dn & 0x1F) << 5) | (uint32_t)(xd & 0x1F);
    emit_inst(e, inst);
}

/* FMOV Sd, Sn  — register-to-register (S form, single precision) */
static inline void emit_fmov_s_s(emit_t *e, int sd, int sn) {
    uint32_t inst = 0x1E204000u | ((uint32_t)(sn & 0x1F) << 5) | (uint32_t)(sd & 0x1F);
    emit_inst(e, inst);
}

/* FMOV Dd, Dn  — register-to-register (D form, double precision) */
static inline void emit_fmov_d_d(emit_t *e, int dd, int dn) {
    uint32_t inst = 0x1E604000u | ((uint32_t)(dn & 0x1F) << 5) | (uint32_t)(dd & 0x1F);
    emit_inst(e, inst);
}

/* ---- FP load/store with unsigned imm offset ---- */

/* LDR Sd, [Xn, #imm]   (offset 4-aligned) */
static inline void emit_ldr_s_imm(emit_t *e, int sd, a64_reg_t rn, uint32_t byte_offset) {
    uint32_t scaled = byte_offset / 4u;
    uint32_t inst = 0xBD400000u | ((scaled & 0xFFFu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (uint32_t)(sd & 0x1F);
    emit_inst(e, inst);
}

/* STR Sd, [Xn, #imm] */
static inline void emit_str_s_imm(emit_t *e, int sd, a64_reg_t rn, uint32_t byte_offset) {
    uint32_t scaled = byte_offset / 4u;
    uint32_t inst = 0xBD000000u | ((scaled & 0xFFFu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (uint32_t)(sd & 0x1F);
    emit_inst(e, inst);
}

/* LDR Dd, [Xn, #imm]   (offset 8-aligned) */
static inline void emit_ldr_d_imm(emit_t *e, int dd, a64_reg_t rn, uint32_t byte_offset) {
    uint32_t scaled = byte_offset / 8u;
    uint32_t inst = 0xFD400000u | ((scaled & 0xFFFu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (uint32_t)(dd & 0x1F);
    emit_inst(e, inst);
}

/* STR Dd, [Xn, #imm] */
static inline void emit_str_d_imm(emit_t *e, int dd, a64_reg_t rn, uint32_t byte_offset) {
    uint32_t scaled = byte_offset / 8u;
    uint32_t inst = 0xFD000000u | ((scaled & 0xFFFu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (uint32_t)(dd & 0x1F);
    emit_inst(e, inst);
}

/* ---- FP load/store with [Xn, Wm, UXTW] addressing (guest-mem accesses) ---- */

/* LDR Sd, [Xn, Wm, UXTW] */
static inline void emit_ldr_s_reg_uxtw(emit_t *e, int sd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0xBC604800u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (uint32_t)(sd & 0x1F);
    emit_inst(e, inst);
}

/* STR Sd, [Xn, Wm, UXTW] */
static inline void emit_str_s_reg_uxtw(emit_t *e, int sd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0xBC204800u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (uint32_t)(sd & 0x1F);
    emit_inst(e, inst);
}

/* LDR Dd, [Xn, Wm, UXTW] */
static inline void emit_ldr_d_reg_uxtw(emit_t *e, int dd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0xFC604800u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (uint32_t)(dd & 0x1F);
    emit_inst(e, inst);
}

/* STR Dd, [Xn, Wm, UXTW] */
static inline void emit_str_d_reg_uxtw(emit_t *e, int dd, a64_reg_t rn, a64_reg_t rm) {
    uint32_t inst = 0xFC204800u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5) | (uint32_t)(dd & 0x1F);
    emit_inst(e, inst);
}

/* ---- FP arithmetic (2-source) ---- */

/* FP 2-source: 00011110 type 1 Rm opcode Rn Rd
 *   type: 00=S, 01=D
 *   opcode: 0010=FMUL, 0110=FDIV, 1010=FADD, 1110=FSUB,
 *           0100=FMAX, 0101=FMIN, 1000=FNMUL, … */
static inline void emit_fp_2src(emit_t *e, int type, int opcode, int rd, int rn, int rm) {
    uint32_t inst = 0x1E200800u | ((uint32_t)type << 22)
                  | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(opcode & 0x3F) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5)
                  | (uint32_t)(rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_fadd_s(emit_t *e, int rd, int rn, int rm) { emit_fp_2src(e, 0, 0x0A, rd, rn, rm); }
static inline void emit_fadd_d(emit_t *e, int rd, int rn, int rm) { emit_fp_2src(e, 1, 0x0A, rd, rn, rm); }
static inline void emit_fsub_s(emit_t *e, int rd, int rn, int rm) { emit_fp_2src(e, 0, 0x0E, rd, rn, rm); }
static inline void emit_fsub_d(emit_t *e, int rd, int rn, int rm) { emit_fp_2src(e, 1, 0x0E, rd, rn, rm); }
static inline void emit_fmul_s(emit_t *e, int rd, int rn, int rm) { emit_fp_2src(e, 0, 0x02, rd, rn, rm); }
static inline void emit_fmul_d(emit_t *e, int rd, int rn, int rm) { emit_fp_2src(e, 1, 0x02, rd, rn, rm); }
static inline void emit_fdiv_s(emit_t *e, int rd, int rn, int rm) { emit_fp_2src(e, 0, 0x06, rd, rn, rm); }
static inline void emit_fdiv_d(emit_t *e, int rd, int rn, int rm) { emit_fp_2src(e, 1, 0x06, rd, rn, rm); }
static inline void emit_fmin_s(emit_t *e, int rd, int rn, int rm) { emit_fp_2src(e, 0, 0x14, rd, rn, rm); }
static inline void emit_fmin_d(emit_t *e, int rd, int rn, int rm) { emit_fp_2src(e, 1, 0x14, rd, rn, rm); }
static inline void emit_fmax_s(emit_t *e, int rd, int rn, int rm) { emit_fp_2src(e, 0, 0x10, rd, rn, rm); }
static inline void emit_fmax_d(emit_t *e, int rd, int rn, int rm) { emit_fp_2src(e, 1, 0x10, rd, rn, rm); }

/* ---- FP arithmetic (1-source) ----
 *
 * The 1-source opcode field isn't a clean linear sequence, so each base
 * value is hand-coded against assembler output. */
static inline void emit_fp_1src_base(emit_t *e, uint32_t base, int type, int rd, int rn) {
    uint32_t inst = base | ((uint32_t)(type & 1) << 22)
                  | ((uint32_t)(rn & 0x1F) << 5)
                  | (uint32_t)(rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_fsqrt_s(emit_t *e, int rd, int rn) { emit_fp_1src_base(e, 0x1E21C000u, 0, rd, rn); }
static inline void emit_fsqrt_d(emit_t *e, int rd, int rn) { emit_fp_1src_base(e, 0x1E21C000u, 1, rd, rn); }
static inline void emit_fabs_s (emit_t *e, int rd, int rn) { emit_fp_1src_base(e, 0x1E20C000u, 0, rd, rn); }
static inline void emit_fabs_d (emit_t *e, int rd, int rn) { emit_fp_1src_base(e, 0x1E20C000u, 1, rd, rn); }
static inline void emit_fneg_s (emit_t *e, int rd, int rn) { emit_fp_1src_base(e, 0x1E214000u, 0, rd, rn); }
static inline void emit_fneg_d (emit_t *e, int rd, int rn) { emit_fp_1src_base(e, 0x1E214000u, 1, rd, rn); }

/* ---- FP fused multiply-add ----
 *
 * AArch64 FMA semantics (note these differ from RV in sign for FMSUB/FNMSUB):
 *   FMADD  Sd, Sn, Sm, Sa : Sd = Sa + Sn*Sm
 *   FMSUB  Sd, Sn, Sm, Sa : Sd = Sa - Sn*Sm
 *   FNMSUB Sd, Sn, Sm, Sa : Sd = -Sa + Sn*Sm   ( = Sn*Sm - Sa)
 *   FNMADD Sd, Sn, Sm, Sa : Sd = -Sa - Sn*Sm   ( = -(Sa + Sn*Sm))
 *
 * RV mappings (let dbt_a64.c choose the right host op):
 *   RV FMADD  rd = rs1*rs2 + rs3   →  AArch64 FMADD
 *   RV FMSUB  rd = rs1*rs2 - rs3   →  AArch64 FNMSUB
 *   RV FNMSUB rd = -(rs1*rs2) + rs3 →  AArch64 FMSUB
 *   RV FNMADD rd = -(rs1*rs2) - rs3 →  AArch64 FNMADD
 *
 * Encoding: 0001 1111 type o1 Rm o0 Ra Rn Rd
 *   o1 = 0/1, o0 = 0/1 picks among FMADD(00) / FMSUB(01) / FNMADD(10) / FNMSUB(11). */
static inline void emit_fp_3src(emit_t *e, int type, int o1, int o0,
                                int rd, int rn, int rm, int ra) {
    uint32_t inst = 0x1F000000u | ((uint32_t)type << 22)
                  | ((uint32_t)(o1 & 1) << 21)
                  | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(o0 & 1) << 15)
                  | ((uint32_t)(ra & 0x1F) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5)
                  | (uint32_t)(rd & 0x1F);
    emit_inst(e, inst);
}

static inline void emit_fmadd_s (emit_t *e, int rd, int rn, int rm, int ra) { emit_fp_3src(e, 0, 0, 0, rd, rn, rm, ra); }
static inline void emit_fmadd_d (emit_t *e, int rd, int rn, int rm, int ra) { emit_fp_3src(e, 1, 0, 0, rd, rn, rm, ra); }
static inline void emit_fmsub_s (emit_t *e, int rd, int rn, int rm, int ra) { emit_fp_3src(e, 0, 0, 1, rd, rn, rm, ra); }
static inline void emit_fmsub_d (emit_t *e, int rd, int rn, int rm, int ra) { emit_fp_3src(e, 1, 0, 1, rd, rn, rm, ra); }
static inline void emit_fnmadd_s(emit_t *e, int rd, int rn, int rm, int ra) { emit_fp_3src(e, 0, 1, 0, rd, rn, rm, ra); }
static inline void emit_fnmadd_d(emit_t *e, int rd, int rn, int rm, int ra) { emit_fp_3src(e, 1, 1, 0, rd, rn, rm, ra); }
static inline void emit_fnmsub_s(emit_t *e, int rd, int rn, int rm, int ra) { emit_fp_3src(e, 0, 1, 1, rd, rn, rm, ra); }
static inline void emit_fnmsub_d(emit_t *e, int rd, int rn, int rm, int ra) { emit_fp_3src(e, 1, 1, 1, rd, rn, rm, ra); }

/* ---- FP compare ----
 *
 * FCMP Rn, Rm sets NZCV; the RV F[EQ/LT/LE] result is read back via CSET.
 * Mapping:
 *   FEQ → CSET COND_EQ
 *   FLT → CSET COND_MI  (after FCMP, MI = N=1 = "less than")
 *                       — careful: NaN-aware semantics differ from
 *                         signed-int LT. Use COND_MI to match RV's
 *                         "ordered and less-than".
 *   FLE → CSET COND_LS  (low-or-same = !HI = ordered and not-greater)
 */
static inline void emit_fcmp_s(emit_t *e, int rn, int rm) {
    uint32_t inst = 0x1E202000u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5);
    emit_inst(e, inst);
}

static inline void emit_fcmp_d(emit_t *e, int rn, int rm) {
    uint32_t inst = 0x1E602000u | ((uint32_t)(rm & 0x1F) << 16)
                  | ((uint32_t)(rn & 0x1F) << 5);
    emit_inst(e, inst);
}

/* ---- FP convert ---- */

/* FCVT Sd, Dn  (double → single) */
static inline void emit_fcvt_s_d(emit_t *e, int sd, int dn) {
    uint32_t inst = 0x1E624000u | ((uint32_t)(dn & 0x1F) << 5) | (uint32_t)(sd & 0x1F);
    emit_inst(e, inst);
}

/* FCVT Dd, Sn  (single → double) */
static inline void emit_fcvt_d_s(emit_t *e, int dd, int sn) {
    uint32_t inst = 0x1E22C000u | ((uint32_t)(sn & 0x1F) << 5) | (uint32_t)(dd & 0x1F);
    emit_inst(e, inst);
}

/* FCVTZ[SU] (FP → int, round toward zero):
 *     0 sf 0 11110 type 1 00 [00=fcvtns, 01=fcvtnu, … 24=fcvtzs, 25=fcvtzu] Rn Rd
 * sf=0 W (32-bit), sf=1 X (64-bit). type=00 single, 01 double. */
static inline void emit_fcvtzs_w32_s(emit_t *e, a64_reg_t wd, int sn) {
    emit_inst(e, 0x1E380000u | ((uint32_t)(sn & 0x1F) << 5) | (uint32_t)(wd & 0x1F));
}
static inline void emit_fcvtzu_w32_s(emit_t *e, a64_reg_t wd, int sn) {
    emit_inst(e, 0x1E390000u | ((uint32_t)(sn & 0x1F) << 5) | (uint32_t)(wd & 0x1F));
}
static inline void emit_fcvtzs_w32_d(emit_t *e, a64_reg_t wd, int dn) {
    emit_inst(e, 0x1E780000u | ((uint32_t)(dn & 0x1F) << 5) | (uint32_t)(wd & 0x1F));
}
static inline void emit_fcvtzu_w32_d(emit_t *e, a64_reg_t wd, int dn) {
    emit_inst(e, 0x1E790000u | ((uint32_t)(dn & 0x1F) << 5) | (uint32_t)(wd & 0x1F));
}

/* SCVTF / UCVTF (int → FP):
 *   0 sf 0 11110 type 1 00 010 Rn Rd  (SCVTF)
 *   0 sf 0 11110 type 1 00 011 Rn Rd  (UCVTF) */
static inline void emit_scvtf_s_w32(emit_t *e, int sd, a64_reg_t wn) {
    emit_inst(e, 0x1E220000u | ((uint32_t)(wn & 0x1F) << 5) | (uint32_t)(sd & 0x1F));
}
static inline void emit_ucvtf_s_w32(emit_t *e, int sd, a64_reg_t wn) {
    emit_inst(e, 0x1E230000u | ((uint32_t)(wn & 0x1F) << 5) | (uint32_t)(sd & 0x1F));
}
static inline void emit_scvtf_d_w32(emit_t *e, int dd, a64_reg_t wn) {
    emit_inst(e, 0x1E620000u | ((uint32_t)(wn & 0x1F) << 5) | (uint32_t)(dd & 0x1F));
}
static inline void emit_ucvtf_d_w32(emit_t *e, int dd, a64_reg_t wn) {
    emit_inst(e, 0x1E630000u | ((uint32_t)(wn & 0x1F) << 5) | (uint32_t)(dd & 0x1F));
}


/* ============================================================================
 * x86 DBT additions
 * ========================================================================= */

/* ---- Add/sub, shifted register, optional flag setting ----
 * sf=0 W / 1 X; sub=1 for SUB; setflags=1 for ADDS/SUBS. shift: 0 LSL,
 * 1 LSR, 2 ASR. The x86 backend does its N-bit arithmetic in the top
 * bits of a 32-bit register (LSL #16 / #24 operands) so NZCV comes out
 * exactly as the guest's SF/ZF/CF/OF. */
static inline void emit_addsub_shifted(emit_t *e, int sf, int sub, int setflags,
                                       a64_reg_t rd, a64_reg_t rn, a64_reg_t rm,
                                       uint32_t shift, uint32_t amount) {
    uint32_t inst = 0x0B000000u | ((uint32_t)sf << 31) | ((uint32_t)sub << 30)
                  | ((uint32_t)setflags << 29) | ((shift & 3u) << 22)
                  | ((uint32_t)(rm & 0x1F) << 16) | ((amount & 0x3Fu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}
static inline void emit_adds_w32_lsl(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm, uint32_t sh) {
    emit_addsub_shifted(e, 0, 0, 1, rd, rn, rm, 0, sh);
}
static inline void emit_subs_w32_lsl(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm, uint32_t sh) {
    emit_addsub_shifted(e, 0, 1, 1, rd, rn, rm, 0, sh);
}
static inline void emit_add_w32_lsl(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm, uint32_t sh) {
    emit_addsub_shifted(e, 0, 0, 0, rd, rn, rm, 0, sh);
}
static inline void emit_sub_w32_lsl(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm, uint32_t sh) {
    emit_addsub_shifted(e, 0, 1, 0, rd, rn, rm, 0, sh);
}
static inline void emit_adds_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    emit_addsub_shifted(e, 0, 0, 1, rd, rn, rm, 0, 0);
}
static inline void emit_subs_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    emit_addsub_shifted(e, 0, 1, 1, rd, rn, rm, 0, 0);
}

/* ADDS/SUBS Wd, Wn, #imm, using the plain or shifted-by-12 immediate
 * form. Returns 0 when the value fits neither and the caller must
 * materialise it in a register. */
static inline int emit_addsubs_w32_imm_any(emit_t *e, int sub, a64_reg_t rd, a64_reg_t rn, uint32_t v) {
    uint32_t base = sub ? 0x71000000u : 0x31000000u;
    if (v < 4096) {
        emit_inst(e, base | (v << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
        return 1;
    }
    if ((v & 0xFFFu) == 0 && (v >> 12) < 4096) {
        emit_inst(e, base | (1u << 22) | ((v >> 12) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
        return 1;
    }
    return 0;
}

/* ADDS/SUBS Wd, Wn, #imm12 */
static inline void emit_adds_w32_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm12) {
    emit_inst(e, 0x31000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}
static inline void emit_subs_w32_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm12) {
    emit_inst(e, 0x71000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}
/* SUB Xd, Xn, #imm12 / SUBS Xd, Xn, #imm12 */
static inline void emit_sub_x64_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm12) {
    emit_inst(e, 0xD1000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}
static inline void emit_subs_x64_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm12) {
    emit_inst(e, 0xF1000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}

/* ADCS / SBCS Wd, Wn, Wm (register only — no shifted form exists). */
static inline void emit_adcs_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    emit_inst(e, 0x3A000000u | ((uint32_t)(rm & 0x1F) << 16) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}
static inline void emit_sbcs_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    emit_inst(e, 0x7A000000u | ((uint32_t)(rm & 0x1F) << 16) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}

/* ---- Logical, shifted register ---- opc: 0 AND, 1 ORR, 2 EOR, 3 ANDS.
 * shift: 0 LSL 1 LSR 2 ASR 3 ROR. n=1 inverts Rm (BIC/ORN/EON/BICS). */
static inline void emit_logical_shifted(emit_t *e, uint32_t opc, int n, a64_reg_t rd, a64_reg_t rn,
                                        a64_reg_t rm, uint32_t shift, uint32_t amount) {
    uint32_t inst = 0x0A000000u | ((opc & 3u) << 29) | ((shift & 3u) << 22) | ((uint32_t)n << 21)
                  | ((uint32_t)(rm & 0x1F) << 16) | ((amount & 0x1Fu) << 10)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    emit_inst(e, inst);
}
static inline void emit_ands_w32_lsl(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm, uint32_t sh) {
    emit_logical_shifted(e, 3, 0, rd, rn, rm, 0, sh);
}
static inline void emit_ands_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    emit_logical_shifted(e, 3, 0, rd, rn, rm, 0, 0);
}
static inline void emit_eor_w32_lsl(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm, uint32_t sh) {
    emit_logical_shifted(e, 2, 0, rd, rn, rm, 0, sh);
}
static inline void emit_eor_w32_lsr(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm, uint32_t sh) {
    emit_logical_shifted(e, 2, 0, rd, rn, rm, 1, sh);
}
static inline void emit_orr_w32_lsr(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm, uint32_t sh) {
    emit_logical_shifted(e, 1, 0, rd, rn, rm, 1, sh);
}
static inline void emit_and_w32_lsr(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm, uint32_t sh) {
    emit_logical_shifted(e, 0, 0, rd, rn, rm, 1, sh);
}
static inline void emit_bic_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    emit_logical_shifted(e, 0, 1, rd, rn, rm, 0, 0);
}
static inline void emit_and_x64(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    emit_inst(e, 0x8A000000u | ((uint32_t)(rm & 0x1F) << 16) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}
static inline void emit_orr_x64(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    emit_inst(e, 0xAA000000u | ((uint32_t)(rm & 0x1F) << 16) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}

/* AND Wd, Wn, #imm with a register fallback through `tmp`. */
static inline void emit_and_w32_imm_any(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm, a64_reg_t tmp) {
    if (emit_and_w32_imm(e, rd, rn, imm)) return;
    emit_mov_w32_imm32(e, tmp, imm);
    emit_and_w32(e, rd, rn, tmp);
}
static inline void emit_orr_w32_imm_any(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm, a64_reg_t tmp) {
    if (emit_orr_w32_imm(e, rd, rn, imm)) return;
    emit_mov_w32_imm32(e, tmp, imm);
    emit_orr_w32(e, rd, rn, tmp);
}
static inline void emit_eor_w32_imm_any(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm, a64_reg_t tmp) {
    if (emit_eor_w32_imm(e, rd, rn, imm)) return;
    emit_mov_w32_imm32(e, tmp, imm);
    emit_eor_w32(e, rd, rn, tmp);
}
static inline void emit_tst_w32_imm_any(emit_t *e, a64_reg_t rn, uint32_t imm, a64_reg_t tmp) {
    if (emit_tst_w32_imm(e, rn, imm)) return;
    emit_mov_w32_imm32(e, tmp, imm);
    emit_tst_w32(e, rn, tmp);
}

/* ---- System register: NZCV ---- */
static inline void emit_mrs_nzcv(emit_t *e, a64_reg_t xt) { emit_inst(e, 0xD53B4200u | (xt & 0x1F)); }
static inline void emit_msr_nzcv(emit_t *e, a64_reg_t xt) { emit_inst(e, 0xD51B4200u | (xt & 0x1F)); }

/* ---- Bitfield ---- */

/* BFXIL Wd, Wn, #lsb, #width — Wd[width-1:0] = Wn[lsb+width-1:lsb], rest
 * of Wd kept. Writes a 16-bit result computed in the top half of a
 * register straight into the low half of a pinned guest register. */
static inline void emit_bfxil_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t lsb, uint32_t width) {
    uint32_t immr = lsb & 31u, imms = (lsb + width - 1u) & 31u;
    emit_inst(e, 0x33000000u | (immr << 16) | (imms << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}
/* CMP Xn, Wm, SXTW */
static inline void emit_cmp_x64_w32_sxtw(emit_t *e, a64_reg_t xn, a64_reg_t wm) {
    emit_inst(e, 0xEB20C01Fu | ((uint32_t)(wm & 0x1F) << 16) | ((uint32_t)(xn & 0x1F) << 5));
}

/* CCMP Wn, #imm5, #nzcv, cond — flags = cond ? (Wn - imm5) : nzcv */
static inline void emit_ccmp_w32_imm(emit_t *e, a64_reg_t rn, uint32_t imm5, uint32_t nzcv, a64_cond_t cond) {
    emit_inst(e, 0x7A400800u | ((imm5 & 31u) << 16) | (((uint32_t)cond & 15u) << 12)
                 | ((uint32_t)(rn & 0x1F) << 5) | (nzcv & 15u));
}

/* EXTR Wd, Wn, Wm, #lsb — bits lsb+31..lsb of the 64-bit Wn:Wm */
static inline void emit_extr_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm, uint32_t lsb) {
    emit_inst(e, 0x13800000u | ((uint32_t)(rm & 0x1F) << 16) | ((lsb & 31u) << 10)
                 | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}

/* SBFX Wd, Wn, #lsb, #width (sign-extending extract) */
static inline void emit_sbfx_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t lsb, uint32_t width) {
    uint32_t immr = lsb & 31u, imms = (lsb + width - 1u) & 31u;
    emit_inst(e, 0x13000000u | (immr << 16) | (imms << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}
static inline void emit_sxtb_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn) { emit_sbfx_w32(e, rd, rn, 0, 8); }
static inline void emit_sxth_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn) { emit_sbfx_w32(e, rd, rn, 0, 16); }
static inline void emit_uxth_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn) { emit_ubfx_w32(e, rd, rn, 0, 16); }
static inline void emit_uxtb_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn) { emit_ubfx_w32(e, rd, rn, 0, 8); }

/* ROR Wd, Wn, #s  (EXTR Wd, Wn, Wn, #s) and RORV */
static inline void emit_ror_w32_imm(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t s) {
    emit_inst(e, 0x13800000u | ((uint32_t)(rn & 0x1F) << 16) | ((s & 0x1Fu) << 10)
                 | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}
static inline void emit_rorv_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm) {
    emit_inst(e, 0x1AC02C00u | ((uint32_t)(rm & 0x1F) << 16) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}
/* CLZ / RBIT (32-bit) */
static inline void emit_clz_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn) {
    emit_inst(e, 0x5AC01000u | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}
static inline void emit_rbit_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn) {
    emit_inst(e, 0x5AC00000u | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}

/* CSINC Wd, Wn, Wm, cond ; CINC Wd, Wn, cond = CSINC Wd, Wn, Wn, !cond */
static inline void emit_csinc_w32(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm, a64_cond_t cond) {
    emit_inst(e, 0x1A800400u | ((uint32_t)(rm & 0x1F) << 16) | (((uint32_t)cond & 0xF) << 12)
                 | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}
static inline void emit_csel_x64(emit_t *e, a64_reg_t rd, a64_reg_t rn, a64_reg_t rm, a64_cond_t cond) {
    emit_inst(e, 0x9A800000u | ((uint32_t)(rm & 0x1F) << 16) | (((uint32_t)cond & 0xF) << 12)
                 | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}

/* ---- Load/store, register offset, general form ----
 * option: 2 UXTW, 3 LSL (X), 6 SXTW, 7 SXTX. S=1 scales Rm by the access size. */
static inline void emit_ldst_reg(emit_t *e, uint32_t size, uint32_t opc, a64_reg_t rt, a64_reg_t rn,
                                 a64_reg_t rm, uint32_t option, uint32_t S) {
    uint32_t inst = ((size & 3u) << 30) | ((opc & 3u) << 22) | 0x38200800u
                  | ((uint32_t)(rm & 0x1F) << 16) | ((option & 7u) << 13) | ((S & 1u) << 12)
                  | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    emit_inst(e, inst);
}
/* LDRB Wt, [Xn, Xm] / LDRH Wt, [Xn, Xm, LSL #1] / LDR Xt, [Xn, Xm, LSL #3] */
static inline void emit_ldrb_reg_x(emit_t *e, a64_reg_t rt, a64_reg_t rn, a64_reg_t rm) {
    emit_ldst_reg(e, 0, 1, rt, rn, rm, 3, 0);
}
static inline void emit_ldrh_reg_x_lsl1(emit_t *e, a64_reg_t rt, a64_reg_t rn, a64_reg_t rm) {
    emit_ldst_reg(e, 1, 1, rt, rn, rm, 3, 1);
}
static inline void emit_ldr_x64_reg_x(emit_t *e, a64_reg_t rt, a64_reg_t rn, a64_reg_t rm) {
    emit_ldst_reg(e, 3, 1, rt, rn, rm, 3, 0);
}
static inline void emit_ldr_w32_reg_x_lsl2(emit_t *e, a64_reg_t rt, a64_reg_t rn, a64_reg_t rm) {
    emit_ldst_reg(e, 2, 1, rt, rn, rm, 3, 1);
}
/* UXTW-indexed 32-bit and 64-bit stores */
static inline void emit_str_x64_reg_uxtw(emit_t *e, a64_reg_t rt, a64_reg_t rn, a64_reg_t rm) {
    emit_ldst_reg_uxtw(e, 3, 0, rt, rn, rm);
}

/* ---- 32-bit register pairs (guest r[8] lives as 8 × uint32) ---- */
static inline void emit_stp_w32_off(emit_t *e, a64_reg_t rt1, a64_reg_t rt2, a64_reg_t rn, int imm) {
    int32_t scaled = imm / 4;
    emit_inst(e, 0x29000000u | (((uint32_t)scaled & 0x7Fu) << 15) | ((uint32_t)(rt2 & 0x1F) << 10)
                 | ((uint32_t)(rn & 0x1F) << 5) | (rt1 & 0x1F));
}
static inline void emit_ldp_w32_off(emit_t *e, a64_reg_t rt1, a64_reg_t rt2, a64_reg_t rn, int imm) {
    int32_t scaled = imm / 4;
    emit_inst(e, 0x29400000u | (((uint32_t)scaled & 0x7Fu) << 15) | ((uint32_t)(rt2 & 0x1F) << 10)
                 | ((uint32_t)(rn & 0x1F) << 5) | (rt1 & 0x1F));
}

/* ---- TBZ / TBNZ (test bit and branch, ±32 KB) ---- */
static inline void emit_tbz_x64(emit_t *e, a64_reg_t rt, uint32_t bit, int32_t byte_offset) {
    int32_t imm14 = byte_offset >> 2;
    assert(imm14 >= -(1 << 13) && imm14 < (1 << 13));
    emit_inst(e, 0x36000000u | ((bit >> 5) << 31) | ((bit & 31u) << 19)
                 | (((uint32_t)imm14 & 0x3FFFu) << 5) | (rt & 0x1F));
}
static inline void emit_tbnz_x64(emit_t *e, a64_reg_t rt, uint32_t bit, int32_t byte_offset) {
    int32_t imm14 = byte_offset >> 2;
    assert(imm14 >= -(1 << 13) && imm14 < (1 << 13));
    emit_inst(e, 0x37000000u | ((bit >> 5) << 31) | ((bit & 31u) << 19)
                 | (((uint32_t)imm14 & 0x3FFFu) << 5) | (rt & 0x1F));
}
static inline void emit_patch_tb14(emit_t *e, uint32_t patch_offset, uint32_t target_offset) {
    int32_t disp = (int32_t)(target_offset - patch_offset);
    int32_t imm14 = disp >> 2;
    assert(imm14 >= -(1 << 13) && imm14 < (1 << 13) && "TBZ/TBNZ target out of ±32 KB range");
    uint32_t inst;
    memcpy(&inst, e->buf + patch_offset, 4);
    inst = (inst & ~(0x3FFFu << 5)) | (((uint32_t)imm14 & 0x3FFFu) << 5);
    memcpy(e->buf + patch_offset, &inst, 4);
}

/* ADD Xd, Xn, Wm, UXTW #shift (shift 0..4) — host pointer from a guest
 * offset, optionally scaled (cache index × 16). */
static inline void emit_add_x64_w32_uxtw_lsl(emit_t *e, a64_reg_t xd, a64_reg_t xn, a64_reg_t wm, uint32_t sh) {
    emit_inst(e, 0x8B204000u | ((uint32_t)(wm & 0x1F) << 16) | ((sh & 7u) << 10)
                 | ((uint32_t)(xn & 0x1F) << 5) | (xd & 0x1F));
}
/* ADD Xd, Xn, #imm12, LSL #12 */
static inline void emit_add_x64_imm_lsl12(emit_t *e, a64_reg_t rd, a64_reg_t rn, uint32_t imm12) {
    emit_inst(e, 0x91400000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F));
}
/* CMP Xn, Xm */
static inline void emit_cmp_x64_x64(emit_t *e, a64_reg_t rn, a64_reg_t rm) {
    emit_inst(e, 0xEB00001Fu | ((uint32_t)(rm & 0x1F) << 16) | ((uint32_t)(rn & 0x1F) << 5));
}
/* MOV Wd, #imm — 16-bit values in one instruction, else two. */
static inline void emit_mov_w32_imm16(emit_t *e, a64_reg_t rd, uint32_t imm) {
    emit_movz_w32(e, rd, (uint16_t)imm, 0);
}
/* ADD/SUB Wd, Wn, #imm (any 32-bit immediate; uses tmp when out of imm12 range) */
static inline void emit_add_w32_imm_any(emit_t *e, a64_reg_t rd, a64_reg_t rn, int32_t imm, a64_reg_t tmp) {
    if (imm >= 0 && imm < 4096) { emit_add_w32_imm(e, rd, rn, (uint32_t)imm); return; }
    if (imm < 0 && -imm < 4096) { emit_sub_w32_imm(e, rd, rn, (uint32_t)-imm); return; }
    emit_mov_w32_imm32(e, tmp, (uint32_t)imm);
    emit_add_w32(e, rd, rn, tmp);
}

#endif /* EMIT_A64_H */
