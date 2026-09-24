/* x86_fpu.h — the x87: a 387 beside a 386, or a 486DX's own
 *
 * Eight 80-bit registers addressed as a stack from TOP, the control,
 * status and tag words, and the last instruction and operand pointers
 * that FSTENV and FSAVE report. The arithmetic is Berkeley SoftFloat's
 * (core/softfloat); what makes it an x87 — the stack, tags, masked and
 * unmasked responses, the denormal and stack-fault flags, precision
 * control, the save formats — is x86_fpu.c.
 */
#ifndef X86_FPU_H
#define X86_FPU_H

#include <stdint.h>

struct x86_cpu;
struct x86_insn;

typedef struct x86_fpu {
    uint64_t sig[8];      /* physical registers R0-R7: significand, explicit integer bit */
    uint16_t sexp[8];     /* sign and 15-bit biased exponent */
    uint16_t cw, sw;      /* SW carries TOP in bits 11-13 */
    uint8_t  empty;       /* bit n: Rn is empty (the tag word's 11); other tags follow the contents */
    uint8_t  ferr;        /* FERR#: an unmasked exception with CR0.NE clear (IRQ 13, once there is a slave PIC) */
    uint16_t fop;         /* last non-control instruction: opcode bits 10-8, ModRM 7-0 */
    uint16_t fcs, fds;    /* its CS and its operand's segment */
    uint32_t fip, fdp;    /* its offset and its operand's */
    /* C1 after a rounded result, owed: whether the rounding went up takes
     * a second rounding to learn, and nothing sees C1 until SW is read
     * (FNSTSW, FSTENV, FSAVE) — by which time most results have had C1
     * overwritten. So the operation is kept here and done again only then. */
    uint8_t  c1_kind;     /* 0 none, 1 arith (c1_op), 2 FST m32 (c1_op 0) or m64 (1) */
    uint8_t  c1_op;
    uint16_t c1_cw;       /* RC and PC then */
    uint16_t c1_ase, c1_bse, c1_rse;
    uint64_t c1_a, c1_b, c1_r;   /* operands, and the rounded result (or stored bits) */
} x86_fpu;

/* CR0 bits the coprocessor interface uses */
#define X86_CR0_MP  0x02u
#define X86_CR0_EM  0x04u
#define X86_CR0_TS  0x08u
#define X86_CR0_NE  0x20u

#define X86_EXC_MF  16

void x86_fpu_reset(struct x86_cpu *c);          /* power-on: CW 0040h, tags all valid +0 */
void x86_fpu_finit(struct x86_cpu *c);          /* FNINIT, as the BIOS does at POST */
void x86_fpu_exec(struct x86_cpu *c, const struct x86_insn *in, uint32_t ea, uint32_t start_ip);
void x86_fpu_wait(struct x86_cpu *c);           /* WAIT/FWAIT: report a pending unmasked exception */
void x86_fpu_sync(struct x86_cpu *c);           /* settle an owed C1 (before SW is compared or read from outside) */

/* The interpreter's memory path for an FPU operand of N bytes at the
 * instruction's segment and OFF: limit, alignment check (#AC, to ALIGN)
 * and paging as for any data access (core/x86_interp.c). */
void x86_fpu_mrd(struct x86_cpu *c, const struct x86_insn *in, uint32_t off, int n, int align, uint8_t *buf);
void x86_fpu_mwr(struct x86_cpu *c, const struct x86_insn *in, uint32_t off, int n, int align, const uint8_t *buf);

#endif
