/* x86.h — 8086..80386 CPU state, flags, memory model, public interfaces
 *
 * Everything the interpreter and the DBT share lives here. Layout is
 * chosen so that generated code can reach any register at a fixed,
 * small offset from the cpu pointer, and so that the register file
 * indexes match the instruction encoding (AX=0 CX=1 DX=2 BX=3 SP=4
 * BP=5 SI=6 DI=7; ES=0 CS=1 SS=2 DS=3 FS=4 GS=5).
 */
#ifndef X86_H
#define X86_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <setjmp.h>

/* ============================================================================
 * FLAGS
 * The low byte is the 8080 F register: CF=0 PF=2 AF=4 ZF=6 SF=7.
 * ========================================================================= */
#define X86_CF   0x0001
#define X86_F1   0x0002   /* reserved, always 1 */
#define X86_PF   0x0004
#define X86_AF   0x0010
#define X86_ZF   0x0040
#define X86_SF   0x0080
#define X86_TF   0x0100
#define X86_IF   0x0200
#define X86_DF   0x0400
#define X86_OF   0x0800
#define X86_IOPL 0x3000
#define X86_NT   0x4000
#define X86_RF   0x00010000
#define X86_VM   0x00020000

#define X86_ARITH_FLAGS (X86_CF|X86_PF|X86_AF|X86_ZF|X86_SF|X86_OF)

/* CPU models. Gate quirks on these, never on "is it a 386?" booleans. */
enum {
    X86_MODEL_8086 = 86,
    X86_MODEL_186  = 186,
    X86_MODEL_286  = 286,
    X86_MODEL_386  = 386,
};

/* Register indexes (encoding order) */
enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };
enum { R_AL, R_CL, R_DL, R_BL, R_AH, R_CH, R_DH, R_BH };
enum { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS, S_NONE = 0xFF };

/* Per-segment cached state. In real mode base = sel<<4 and limit is
 * 0xFFFF; in PM the DPMI host fills these from the descriptor. */
/* attr packs the descriptor's access byte in bits 0-7 and its flags nibble
 * (G, D/B, L, AVL) in bits 8-11 — the two bytes the processor keeps once the
 * base and limit have been unpacked. `usable` is the hidden valid bit: a
 * segment register loaded with a null selector holds no descriptor at all. */
#define X86_AR_TYPE(a)    ((a) & 0x0F)
#define X86_AR_S(a)       (((a) >> 4) & 1)        /* 1 = code/data, 0 = system */
#define X86_AR_DPL(a)     (((a) >> 5) & 3)
#define X86_AR_P(a)       (((a) >> 7) & 1)
#define X86_AR_G(a)       (((a) >> 11) & 1)
#define X86_AR_DB(a)      (((a) >> 10) & 1)
#define X86_TYPE_CODE     0x08                    /* within a code/data type */
#define X86_TYPE_CONFORM  0x04                    /* code: conforming */
#define X86_TYPE_READABLE 0x02                    /* code: readable */
#define X86_TYPE_EXPDOWN  0x04                    /* data: expand-down */
#define X86_TYPE_WRITABLE 0x02                    /* data: writable */
#define X86_TYPE_ACCESSED 0x01

typedef struct x86_seg {
    uint16_t sel;
    uint16_t attr;      /* PM: access byte + flags; 0 in real mode */
    uint8_t  usable;    /* 0 after a null selector: present but unusable */
    uint8_t  pad1[3];
    uint32_t base;
    uint32_t limit;     /* byte granular, inclusive */
    uint8_t  big;       /* D/B bit: default 32-bit operands/stack */
    uint8_t  pad[3];
} x86_seg;

typedef struct x86_cpu {
    /* General registers, encoding order. 8-bit view via x86_r8 macros. */
    uint32_t r[8];
    uint32_t eip;
    uint32_t eflags;

    x86_seg  seg[6];

    /* Guest memory: flat host buffer. Physical address = linear & a20_mask,
     * and anything >= mem_size is open bus (reads 0xFF, writes dropped).
     * See x86_mem.c for the A20 mirror that lets the JIT skip the mask. */
    uint8_t *mem;
    uint32_t mem_size;
    uint32_t a20_mask;
    int      mem_fd;        /* backing object of the mirrored mapping, -1 if plain */
    uint8_t  mem_mirrored;  /* 1: HMA window aliases low memory while A20 is off */
    uint8_t  pad1[3];

    /* One byte per guest byte, nonzero while a translated block covers
     * it. Always allocated (all zero without a DBT) so the store path
     * below needs no NULL test. Same mirror layout as mem. */
    uint8_t *code_bitmap;
    void   (*smc_hook)(struct x86_cpu *, uint32_t phys);   /* DBT: a store hit code */
    /* Device memory (VGA planes). A store to a byte whose bitmap entry has
     * X86_BM_DEVICE is handed to device_store after it lands, from the
     * interpreter and from translated code alike; device_read, when set,
     * takes every interpreter read of the A0000-AFFFF window. */
    void   (*device_store)(struct x86_cpu *, uint32_t phys);
    uint8_t (*device_read)(struct x86_cpu *, uint32_t phys);
    void   (*a20_hook)(struct x86_cpu *, int on);          /* DBT: the A20 gate changed */
    void   (*trace_exc)(struct x86_cpu *, int vec, uint32_t err);   /* oracle tracing */

    /* Set while the very next HLE trap is the delivery of a CPU exception
     * rather than an interrupt or an INT instruction. The two are
     * indistinguishable once the gate has been taken — vector 8 is both
     * #DF and the timer IRQ — and a DPMI host must tell them apart, since
     * only a fault belongs to the client's exception handler. */
    uint8_t  exc_delivered;
    void    *dbt;           /* owning translator, NULL when interpreting only */
    void    *jit_aux;       /* DBT aux block base, reloaded by helper-call sequences */
    uint64_t jit_budget;    /* insn budget handed to the trampoline (exit stub math) */
    uint64_t jit_cnt_save;  /* pinned budget register parked across helper calls */
    uint32_t jit_cur_lin;   /* linear address of the block making a helper call... */
    uint32_t jit_cur_hit;   /* ...set by the SMC sweep if that block got invalidated */

    /* Descriptor tables. In real mode these sit unused; CR0.PE turns them on.
     * ldtr/tr keep the cached descriptor the same way the segment registers
     * do, because that is what the processor actually consults. */
    struct { uint32_t base; uint16_t limit; } gdtr, idtr;
    x86_seg  ldtr, tr;
    uint32_t cr0;

    int      model;       /* X86_MODEL_* */
    uint8_t  pmode;       /* 0 = real mode, 1 = protected */
    uint8_t  halted;
    uint8_t  int_inhibit; /* one-instruction shadow after MOV SS / POP SS / STI */
    uint8_t  pad0;

    /* Pending exception raised during a step (X86_EXC_*), -1 if none */
    int      exc;
    uint32_t exc_err;
    /* Faults abort the instruction: x86_fault() longjmps here when armed
     * (x86_step, x86_exec_decoded) so no further state is committed. */
    jmp_buf  fault_jb;       /* _setjmp/_longjmp: plain setjmp saves the signal mask, a syscall per instruction on macOS */
    int      fault_armed;

    /* I/O port hooks (pc/ layer). NULL = open bus. */
    uint32_t (*io_read)(struct x86_cpu *, uint16_t port, int size);
    void     (*io_write)(struct x86_cpu *, uint16_t port, uint32_t val, int size);
    void     *io_ctx;

    /* High-level emulation trap: every IVT entry initially points at
     * hle_seg:vector, a segment of IRETs. Executing there calls hle()
     * instead of fetching, so BIOS/DOS services live in the host and a
     * guest that hooks a vector and chains still reaches them. The hook
     * performs the return itself (see pc/pc_bios.c). */
    uint16_t hle_seg;
    void   (*hle)(struct x86_cpu *, int vector);
    void    *hle_ctx;

    /* Stats */
    uint64_t insn_count;
} x86_cpu;

/* 8-bit register access: AL..BL are the low bytes of r[0..3], AH..BH are
 * byte 1 of r[0..3]. Little-endian host assumed (all targets are). */
static inline uint8_t *x86_r8p(x86_cpu *c, int i) {
    return (uint8_t *)&c->r[i & 3] + (i >> 2);
}
static inline uint32_t x86_get_r8(x86_cpu *c, int i)  { return *x86_r8p(c, i); }
static inline uint32_t x86_get_r16(x86_cpu *c, int i) { return c->r[i] & 0xFFFF; }
static inline uint32_t x86_get_r32(x86_cpu *c, int i) { return c->r[i]; }
static inline void x86_set_r8(x86_cpu *c, int i, uint32_t v)  { *x86_r8p(c, i) = (uint8_t)v; }
static inline void x86_set_r16(x86_cpu *c, int i, uint32_t v) { c->r[i] = (c->r[i] & 0xFFFF0000u) | (v & 0xFFFF); }
static inline void x86_set_r32(x86_cpu *c, int i, uint32_t v) { c->r[i] = v; }

static inline uint32_t x86_get_reg(x86_cpu *c, int i, int size) {
    return size == 1 ? x86_get_r8(c, i) : size == 2 ? x86_get_r16(c, i) : x86_get_r32(c, i);
}
static inline void x86_set_reg(x86_cpu *c, int i, int size, uint32_t v) {
    if (size == 1) x86_set_r8(c, i, v); else if (size == 2) x86_set_r16(c, i, v); else x86_set_r32(c, i, v);
}

/* ============================================================================
 * Physical memory. Everything funnels through these so the A20 gate,
 * the open-bus region and (later) the SMC write hook have one home.
 * ========================================================================= */
/* The code bitmap carries two things: X86_BM_CODE, a translated block
 * covers the byte (the DBT's), and X86_BM_DEVICE, the byte is device memory
 * whose store has side effects. Either way a store lands, then any nonzero
 * entry sends it to x86_store_hook. */
#define X86_BM_CODE   0x01
#define X86_BM_DEVICE 0x80
void x86_store_hook(struct x86_cpu *c, uint32_t phys);

static inline uint8_t x86_phys_rd8(x86_cpu *c, uint32_t lin) {
    uint32_t p = lin & c->a20_mask;
    if (c->device_read && p - 0xA0000u < 0x10000u) return c->device_read(c, p);
    return p < c->mem_size ? c->mem[p] : 0xFF;
}
static inline void x86_phys_wr8(x86_cpu *c, uint32_t lin, uint8_t v) {
    uint32_t p = lin & c->a20_mask;
    if (p < c->mem_size) {
        c->mem[p] = v;
        if (c->code_bitmap[p]) x86_store_hook(c, p);
    }
}

/* Segment-relative access with in-segment offset wrap. offmask is
 * 0xFFFF for 16-bit addressing and 0xFFFFFFFF for 32-bit; a word that
 * straddles the wrap point really does touch base+FFFF and base+0000. */
static inline uint32_t x86_rd(x86_cpu *c, uint32_t base, uint32_t off, uint32_t offmask, int size) {
    uint32_t v = 0;
    for (int i = 0; i < size; i++)
        v |= (uint32_t)x86_phys_rd8(c, base + ((off + i) & offmask)) << (8 * i);
    return v;
}
static inline void x86_wr(x86_cpu *c, uint32_t base, uint32_t off, uint32_t offmask, int size, uint32_t v) {
    for (int i = 0; i < size; i++)
        x86_phys_wr8(c, base + ((off + i) & offmask), (uint8_t)(v >> (8 * i)));
}

/* Guest memory geometry. Low memory is 1 MB + the 64 KB HMA — everything
 * real mode can address, and the range the block cache is indexed on.
 * Extended memory sits above it, contiguous, for the DPMI host to hand
 * out; with no paging (CLAUDE.md) linear == physical, so a client's flat
 * selector is just an offset into this buffer. X86_MEM_SLACK is readable
 * slack past the end so a decode or a straddling access at the top never
 * faults. */
#define X86_LOW_SIZE  0x110000u
#define X86_EXT_SIZE  0x1000000u                     /* 16 MB, enough for DOS/4GW-era clients */
#define X86_MEM_SIZE  (X86_LOW_SIZE + X86_EXT_SIZE)
#define X86_MEM_SLACK 0x10000u

int  x86_mem_alloc(x86_cpu *c);
void x86_mem_free(x86_cpu *c);
int  x86_set_a20(x86_cpu *c, int on);

/* Exceptions (vector numbers) */
enum {
    X86_EXC_DE = 0, X86_EXC_DB = 1, X86_EXC_BP = 3, X86_EXC_OF = 4,
    X86_EXC_BR = 5, X86_EXC_UD = 6, X86_EXC_NM = 7, X86_EXC_DF = 8,
    X86_EXC_TS = 10, X86_EXC_NP = 11, X86_EXC_SS = 12, X86_EXC_GP = 13,
    X86_EXC_PF = 14,
};

/* ============================================================================
 * Public API (core/x86_state.c, core/x86_interp.c)
 * ========================================================================= */
void x86_init(x86_cpu *c, int model);
void x86_free(x86_cpu *c);
void x86_reset(x86_cpu *c);
void x86_load_seg(x86_cpu *c, int s, uint16_t sel);
int  x86_cpl(const x86_cpu *c);
int  x86_read_desc(x86_cpu *c, uint16_t sel, uint32_t *lo, uint32_t *hi);
void x86_unpack_desc(x86_seg *g, uint16_t sel, uint32_t lo, uint32_t hi);
void x86_set_accessed(x86_cpu *c, uint16_t sel, uint32_t hi);   /* real mode: base = sel<<4 */
void x86_dump(x86_cpu *c, FILE *f);

/* Execute one instruction. Returns 0 on success, 1 if halted, or a
 * negative code when the instruction could not be executed. */
int  x86_step(x86_cpu *c);

/* Run an already-decoded instruction's semantics with c->eip pointing
 * past it — the DBT's generic slow path. Does not count the instruction
 * or dispatch exceptions; the caller has excluded anything that raises. */
struct x86_insn;
void x86_exec_decoded(x86_cpu *c, const struct x86_insn *in);
void x86_deliver_exception(x86_cpu *c);   /* cpu->exc pending after a JIT run */

/* Deliver interrupt/exception vector n (pushes flags/CS/IP, loads vector). */
void x86_interrupt(x86_cpu *c, int vector, int is_sw);

/* Raise a fault from inside an instruction: records it and, when a step
 * is armed, abandons the instruction (longjmp). Never returns if armed. */
void x86_fault(x86_cpu *c, int vector, uint32_t err);

/* Flags after POPF/IRET are model dependent: 8086 forces 12-15 set,
 * 286 real mode forces them clear, 386 allows IOPL/NT. */
uint32_t x86_flags_fixup(x86_cpu *c, uint32_t f);

#endif /* X86_H */
