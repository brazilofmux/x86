/* x86_decode.h — decoded instruction form shared by interpreter and DBT
 *
 * The decoder turns raw bytes into an x86_insn: an internal opcode, two
 * operand descriptors (dst, src) and one shared effective-address
 * description. Group opcodes are resolved to their real op; 8086
 * aliases are resolved according to the CPU model. Nothing here
 * touches CPU state — the decoder reads only the byte buffer it is
 * handed, so the JIT can decode straight out of guest memory.
 */
#ifndef X86_DECODE_H
#define X86_DECODE_H

#include <stdint.h>
#include <stddef.h>

/* Internal opcodes. OP_UD is 0 so that holes in the designated-
 * initializer opcode maps decode as invalid. OP_ADD..OP_CMP follow the
 * ALU encoding in bits 5:3 of opcodes 00-3F and the reg field of group 1
 * (80-83); the rotate group follows the reg field of C0/C1/D0-D3. Keep
 * those orders. */
enum x86_op {
    OP_UD = 0,                  /* invalid opcode (#UD on 186+) */
    OP_ADD, OP_OR, OP_ADC, OP_SBB, OP_AND, OP_SUB, OP_XOR, OP_CMP,
    OP_ROL, OP_ROR, OP_RCL, OP_RCR, OP_SHL, OP_SHR, OP_SAL, OP_SAR,
    OP_TEST, OP_NOT, OP_NEG, OP_MUL, OP_IMUL, OP_DIV, OP_IDIV,
    OP_INC, OP_DEC,
    OP_MOV, OP_MOVSEG,          /* MOVSEG: dst or src is a segment register */
    OP_XCHG, OP_LEA, OP_NOP,
    OP_PUSH, OP_POP, OP_PUSHA, OP_POPA, OP_PUSHF, OP_POPF,
    OP_CALL, OP_CALLF, OP_JMP, OP_JMPF, OP_JCC, OP_JCXZ,
    OP_LOOP, OP_LOOPE, OP_LOOPNE,
    OP_RET, OP_RETF, OP_IRET, OP_INT, OP_INT3, OP_INTO,
    OP_CBW, OP_CWD, OP_SAHF, OP_LAHF, OP_XLAT,
    OP_MOVS, OP_CMPS, OP_STOS, OP_LODS, OP_SCAS, OP_INS, OP_OUTS,
    OP_IN, OP_OUT,
    OP_LES, OP_LDS, OP_LSS, OP_LFS, OP_LGS,
    OP_CLC, OP_STC, OP_CMC, OP_CLD, OP_STD, OP_CLI, OP_STI,
    OP_HLT, OP_WAIT, OP_ESC, OP_LOCK_ONLY,
    OP_AAA, OP_AAS, OP_AAM, OP_AAD, OP_DAA, OP_DAS, OP_SALC,
    OP_ENTER, OP_LEAVE, OP_BOUND, OP_ARPL, OP_IMUL3,
    OP_MOVZX, OP_MOVSX, OP_SETCC,
    OP_BT, OP_BTS, OP_BTR, OP_BTC, OP_BSF, OP_BSR, OP_SHLD, OP_SHRD,
    OP_CMPXCHG, OP_XADD, OP_BSWAP,
    OP_LAR, OP_LSL, OP_CLTS, OP_SGDT, OP_SIDT, OP_LGDT, OP_LIDT,
    OP_SLDT, OP_STR, OP_LLDT, OP_LTR, OP_VERR, OP_VERW, OP_SMSW, OP_LMSW,
    OP_MOVCR, OP_MOVDR, OP_MOVTR,
    OP_INT1,
    OP_SETMO,                   /* 8086 undocumented D0-D3 /6: operand = -1 (if count != 0) */
    OP_INVD, OP_WBINVD, OP_INVLPG,   /* 486: cache and TLB maintenance */
    OP__COUNT
};

/* Operand kinds */
enum {
    OPK_NONE = 0,
    OPK_REG,    /* general register: reg index, size 1/2/4 (8-bit uses AL..BH index) */
    OPK_SREG,   /* segment register */
    OPK_MEM,    /* memory via insn->ea (size from operand) */
    OPK_IMM,    /* immediate in operand.imm */
    OPK_CR, OPK_DR, OPK_TR,
};

typedef struct x86_operand {
    uint8_t  kind;
    uint8_t  reg;
    uint8_t  size;   /* bytes: 1, 2, 4; 6 for far pointer memory operands (16:32) */
    uint8_t  imm_enc;   /* OPK_IMM read from the byte stream: X86_IMM_AT/LEN/SX; 0 if implicit */
    uint32_t imm;
} x86_operand;

/* Where an immediate came from in the instruction bytes — so the DBT can
 * read a value that self-modifying code keeps patching from memory at run
 * time rather than baking it into the translation. */
#define X86_IMM_AT(e)   ((e) & 0x0F)          /* byte offset within the instruction */
#define X86_IMM_LEN(e)  (((e) >> 4) & 0x07)   /* encoded bytes: 1, 2 or 4 */
#define X86_IMM_SX(e)   (((e) >> 7) & 1)      /* sign-extended to the operand size */

typedef struct x86_insn {
    uint8_t  len;        /* total bytes including prefixes */
    uint8_t  op;         /* enum x86_op */
    uint8_t  opsize;     /* 2 or 4: effective operand size for "v" operands */
    uint8_t  adsize;     /* 2 or 4: effective address size */

    uint8_t  opcode;     /* primary opcode byte (after prefixes) */
    uint8_t  opcode2;    /* second byte for 0F map, else 0 */
    uint8_t  modrm;      /* raw modrm byte, valid if has_modrm */
    uint8_t  has_modrm;

    uint8_t  mod, reg, rm;
    uint8_t  cond;       /* Jcc/SETcc condition (0-15) */

    uint8_t  seg;        /* segment for the memory operand (after override / default) */
    uint8_t  seg_override; /* S_NONE if no override prefix */
    uint8_t  rep;        /* 0, 0xF3 (REP/REPE), 0xF2 (REPNE) */
    uint8_t  lock;

    /* Effective address: base + index*scale + disp. base/index are
     * register indexes or -1. Real mode SP/BP defaults to SS, else DS. */
    int8_t   base, index;
    uint8_t  scale;
    uint8_t  ea_valid;   /* has a memory operand (mod != 3) */
    int32_t  disp;

    x86_operand ops[2];  /* [0] = destination, [1] = source */
    uint32_t imm2;       /* second immediate: ENTER level, far-pointer segment, 3-operand IMUL */
} x86_insn;

/* Decode context. bytes must hold at least 16 valid bytes (the decoder
 * never reads past X86_MAX_INSN). defaults come from CS.big / SS.big. */
#define X86_MAX_INSN 15

typedef struct x86_dec_ctx {
    const uint8_t *bytes;
    int model;
    uint8_t def32;       /* CS D bit: default 32-bit operand/address size */
} x86_dec_ctx;

/* Returns insn->len (>0) on success, 0 if the bytes do not form a valid
 * instruction within 15 bytes (too many prefixes). OP_UD is a *valid*
 * decode of an invalid opcode. */
int x86_decode(const x86_dec_ctx *ctx, x86_insn *insn);

/* Disassemble for -V diffs and tracing. Returns buf. */
char *x86_disasm(const x86_insn *insn, char *buf, size_t n);

/* Condition evaluation shared with the DBT's liveness pass. */
static inline int x86_cond(uint32_t f, int cc) {
    int r;
    switch (cc >> 1) {
    case 0: r = (f & 0x800) != 0; break;                               /* O  */
    case 1: r = (f & 0x001) != 0; break;                               /* B  */
    case 2: r = (f & 0x040) != 0; break;                               /* E  */
    case 3: r = (f & 0x041) != 0; break;                               /* BE */
    case 4: r = (f & 0x080) != 0; break;                               /* S  */
    case 5: r = (f & 0x004) != 0; break;                               /* P  */
    case 6: r = ((f >> 7) ^ (f >> 11)) & 1; break;                     /* L  */
    default: r = (((f >> 7) ^ (f >> 11)) & 1) | ((f >> 6) & 1); break; /* LE */
    }
    return r ^ (cc & 1);
}

#endif /* X86_DECODE_H */
