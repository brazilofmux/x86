/* dos_dpmi.c — the DPMI host.
 *
 * We are the host, the way NTVDM was: a client asks through INT 2Fh
 * AX=1687h, far-calls the entry point we hand back, and wakes up in 32-bit
 * protected mode running on descriptors we built. Its own descriptors come
 * out of an LDT we own and hand out through INT 31h.
 *
 * There is no paging here and never will be (CLAUDE.md), so linear and
 * physical are the same address and the memory services are the DOS arena
 * wearing a different hat. The client runs at ring 0, which DPMI permits
 * and CWSDPMI also does; that keeps every INT out of the stack-switch path.
 *
 * Interrupts reach us the same way they do in real mode: every IDT gate
 * points into a code descriptor based at the HLE segment, so INT 21h in
 * protected mode lands on the same trap as INT 21h in real mode.
 */
#include "dos.h"
#include <string.h>
#include <stdio.h>

#define GDT_OFF   0x0000            /* within the host's block */
#define LDT_OFF   0x0400
#define IDT_OFF   0x0C00
#define TSS_OFF   0x1400
#define RMSTK_OFF 0x1800            /* real-mode stacks for excursions, one per nesting level */
#define RMSTK_LVL 0x0200
#define RM_DEPTH  8
#define CBSTK_OFF 0x2800            /* the protected-mode stack a real-mode callback runs on */
#define CBSTK_LEN 0x0800
#define CB_SLOTS  64
#define EXCSTK_OFF 0x3000           /* exception-handler stacks, one per nesting level */
#define EXCSTK_LVL 0x0400
#define EXC_DEPTH  8
#define HOST_PARAS 0x500            /* 20 KB: tables, then the excursion, callback and exception stacks */

#define SEL_HCODE 0x08              /* flat code, base 0 */
#define SEL_HDATA 0x10              /* flat data, base 0 */
#define SEL_HLE   0x18              /* code based at the HLE segment */
#define SEL_LDT   0x20
#define SEL_TSS   0x28
#define SEL_HLE16 0x30              /* the trap segment again, 16-bit: a 16-bit client's gates */
#define SEL_HSTK  0x38              /* the host's block as a stack, as wide as the client */
#define GDT_SLOTS 8
#define LDT_SLOTS 128

static struct {
    uint16_t seg;                   /* paragraph of the host's tables */
    int      active;                /* a client has switched to protected mode */
    int      is32;                  /* AX bit 0 at the mode switch: a 32-bit client */
    uint8_t  used[LDT_SLOTS];
} dpmi;

static int client_is32(void) { return dpmi.is32; }

/* --- Extended memory -----------------------------------------------------
 * With no paging, a DPMI "memory block" is just a range of the flat guest
 * buffer above the HMA and its linear address is its physical address.
 * Blocks are kept sorted by base, so first fit is a walk over the gaps
 * between them. DPMI's handle is opaque; we use the block's base, which is
 * unique for as long as the block lives.
 *
 * Fresh guest memory is already zero (it comes from a sparse shared-memory
 * object), and we deliberately do not clear a recycled block: a real host
 * hands back whatever was there, and a client that depends on zeros should
 * fail here the same way it would on CWSDPMI.
 */
#define EXT_BASE   X86_LOW_SIZE
#define EXT_TOP    X86_MEM_SIZE
#define EXT_PAGE   0x1000u
#define EXT_BLOCKS 64

static struct { uint32_t base, size; } extblk[EXT_BLOCKS];
static int extn;

static uint32_t ext_alloc(uint32_t size) {
    if (size == 0 || extn == EXT_BLOCKS) return 0;
    size = (size + EXT_PAGE - 1) & ~(EXT_PAGE - 1);
    if (size < EXT_PAGE) return 0;                      /* wrapped: absurd request */
    uint32_t at = EXT_BASE;
    int i = 0;
    for (; i < extn; i++) {
        if (extblk[i].base - at >= size) break;         /* fits in the gap before block i */
        at = extblk[i].base + extblk[i].size;
    }
    if (i == extn && EXT_TOP - at < size) return 0;
    memmove(&extblk[i + 1], &extblk[i], (size_t)(extn - i) * sizeof extblk[0]);
    extblk[i].base = at; extblk[i].size = size; extn++;
    return at;
}

static uint32_t ext_size_of(uint32_t base) {
    for (int i = 0; i < extn; i++) if (extblk[i].base == base) return extblk[i].size;
    return 0;
}

static int ext_free(uint32_t base) {
    for (int i = 0; i < extn; i++)
        if (extblk[i].base == base) {
            memmove(&extblk[i], &extblk[i + 1], (size_t)(extn - i - 1) * sizeof extblk[0]);
            extn--;
            return 1;
        }
    return 0;
}

static uint32_t ext_largest(void) {
    uint32_t at = EXT_BASE, best = 0;
    for (int i = 0; i < extn; i++) {
        if (extblk[i].base - at > best) best = extblk[i].base - at;
        at = extblk[i].base + extblk[i].size;
    }
    if (EXT_TOP - at > best) best = EXT_TOP - at;
    return best;
}

static uint32_t ext_used(void) {
    uint32_t n = 0;
    for (int i = 0; i < extn; i++) n += extblk[i].size;
    return n;
}

static uint32_t lin(uint16_t seg, uint32_t off) { return ((uint32_t)seg << 4) + off; }

/* --- Frame widths ---------------------------------------------------------
 * Everything the host hands a client a frame on — gates, exception frames,
 * the callback's IRET frame — is as wide as the client (AX bit 0 at the
 * mode switch). Stack addressing follows SS's B bit, as the CPU's does. */
static uint32_t stk_mask(const x86_cpu *c) {
    return c->pmode && X86_AR_DB(c->seg[S_SS].attr) ? 0xFFFFFFFFu : 0xFFFFu;
}
static uint32_t stk_pop(x86_cpu *c, uint32_t *sp, int w) {
    uint32_t m = stk_mask(c);
    uint32_t v = x86_rd(c, c->seg[S_SS].base, *sp & m, m, w);
    *sp += (uint32_t)w;
    return v;
}
static void stk_push(x86_cpu *c, uint32_t *sp, int w, uint32_t v) {
    uint32_t m = stk_mask(c);
    *sp -= (uint32_t)w;
    x86_wr(c, c->seg[S_SS].base, *sp & m, m, w, v);
}
static void stk_commit(x86_cpu *c, uint32_t sp) {
    uint32_t m = stk_mask(c);
    c->r[R_SP] = (c->r[R_SP] & ~m) | (sp & m);
}
/* The width of the gate frame a service was entered through: our gates go
 * to SEL_HLE (32-bit) or SEL_HLE16, and the trap segment's D bit says which. */
static int gate_width(const x86_cpu *c) { return X86_AR_DB(c->seg[S_CS].attr) ? 4 : 2; }

static int client_is32(void);

/* Whether the code that called this service is 32-bit. A 16-bit gate means
 * a 16-bit client, which settles it; through a 32-bit gate the caller's CS
 * is in the frame and its D bit decides — the same INT 31h entry serves 16-
 * and 32-bit code in one client, and DJGPP's stub declares itself 32-bit
 * while running 16-bit code. */
static int caller_is32(x86_cpu *c) {
    if (!c->pmode || gate_width(c) == 2) return 0;
    uint32_t m = stk_mask(c);
    uint16_t cs = (uint16_t)x86_rd(c, c->seg[S_SS].base, (c->r[R_SP] + 4) & m, m, 2);
    uint32_t lo, hi;
    return x86_read_desc(c, cs, &lo, &hi) ? (int)((hi >> 22) & 1) : client_is32();
}
static uint32_t es_edi(x86_cpu *c) {
    return c->seg[S_ES].base + (caller_is32(c) ? c->r[R_DI] : (c->r[R_DI] & 0xFFFF));
}

static void idt_set(x86_cpu *c, int v, uint16_t sel, uint32_t off);

/* Write one descriptor, given the fields rather than the packed form. */
static void set_desc(x86_cpu *c, uint32_t at, uint32_t base, uint32_t limit,
                     uint8_t access, uint8_t flags) {
    if (limit > 0xFFFFF) { limit >>= 12; flags |= 0x80; }      /* granular */
    x86_wr(c, at, 0, 0xFFFFFFFFu, 2, limit & 0xFFFF);
    x86_wr(c, at, 2, 0xFFFFFFFFu, 2, base & 0xFFFF);
    x86_wr(c, at, 4, 0xFFFFFFFFu, 1, (base >> 16) & 0xFF);
    x86_wr(c, at, 5, 0xFFFFFFFFu, 1, access);
    x86_wr(c, at, 6, 0xFFFFFFFFu, 1, (uint8_t)(((limit >> 16) & 0x0F) | (flags & 0xF0)));
    x86_wr(c, at, 7, 0xFFFFFFFFu, 1, (base >> 24) & 0xFF);
}

static uint32_t gdt_at(int i) { return lin(dpmi.seg, GDT_OFF + (uint32_t)i * 8); }
static uint32_t ldt_at(int i) { return lin(dpmi.seg, LDT_OFF + (uint32_t)i * 8); }

static void forget_client(void);            /* defined once its tables are */

/* Build the tables once, at DOS init: they cost a few KB of the arena and
 * mean the mode switch itself has nothing to allocate. */
void dpmi_init(x86_cpu *c) {
    memset(&dpmi, 0, sizeof dpmi);
    forget_client();
    dpmi.seg = dos_mem_alloc(HOST_PARAS, DOS_OWNER_SYS, NULL);   /* the host's own block */
    if (!dpmi.seg) { fprintf(stderr, "dpmi: no memory for host tables\n"); return; }
    for (uint32_t i = 0; i < HOST_PARAS * 16u; i++) x86_phys_wr8(c, lin(dpmi.seg, i), 0);

    set_desc(c, gdt_at(1), 0, 0xFFFFFFFFu, 0x9A, 0x40);                 /* flat code */
    set_desc(c, gdt_at(2), 0, 0xFFFFFFFFu, 0x92, 0x40);                 /* flat data */
    set_desc(c, gdt_at(3), (uint32_t)PC_HLE_SEG << 4, 0xFFFF, 0x9A, 0x40);  /* the trap segment */
    set_desc(c, gdt_at(4), lin(dpmi.seg, LDT_OFF), LDT_SLOTS * 8 - 1, 0x82, 0x00);
    set_desc(c, gdt_at(5), lin(dpmi.seg, TSS_OFF), 0x67, 0x89, 0x00);
    set_desc(c, gdt_at(6), (uint32_t)PC_HLE_SEG << 4, 0xFFFF, 0x9A, 0x00);  /* the trap segment, 16-bit */
    set_desc(c, gdt_at(7), lin(dpmi.seg, 0), HOST_PARAS * 16u - 1, 0x92, 0x00);   /* resized per client */

    /* Every vector traps to the host, exactly as the real-mode IVT does. */
    for (int v = 0; v < 256; v++) idt_set(c, v, SEL_HLE, (uint32_t)v);
}

/* Allocate `count` consecutive LDT slots. Returns the first selector, or 0. */
static uint16_t ldt_alloc(int count) {
    for (int i = 1; i + count <= LDT_SLOTS; i++) {
        int k = 0;
        while (k < count && !dpmi.used[i + k]) k++;
        if (k < count) { i += k; continue; }
        for (k = 0; k < count; k++) dpmi.used[i + k] = 1;
        return (uint16_t)((i << 3) | 4);               /* TI = 1, RPL 0 */
    }
    return 0;
}

static int ldt_index(uint16_t sel) {
    if (!(sel & 4)) return -1;
    int i = sel >> 3;
    return (i > 0 && i < LDT_SLOTS) ? i : -1;
}

/* A block's size in paragraphs, from the MCB that precedes it. */
static uint16_t mcb_paras(x86_cpu *c, uint16_t seg) { return pc_rd16(c, (uint16_t)(seg - 1), 3); }

/* Real-mode segments the client asked to address (service 0002). DPMI says
 * these descriptors are permanent, so the same segment gives the same
 * selector every time and nobody has to free them. Callbacks want one too,
 * for the real-mode stack they hand the client. */
static struct { uint16_t seg, sel; } seg_cache[32];
static int seg_cached;

static uint16_t sel_for_seg(x86_cpu *c, uint16_t seg) {
    for (int i = 0; i < seg_cached; i++) if (seg_cache[i].seg == seg) return seg_cache[i].sel;
    uint16_t sel = ldt_alloc(1);
    if (!sel) return 0;
    set_desc(c, ldt_at(ldt_index(sel)), (uint32_t)seg << 4, 0xFFFF, 0x92, 0x00);
    if (seg_cached < (int)(sizeof seg_cache / sizeof seg_cache[0]))
        seg_cache[seg_cached].seg = seg, seg_cache[seg_cached++].sel = sel;
    return sel;
}

/* INT 2Fh AX=1687h: yes, there is a host, and here is the way in. */
void dpmi_int2f_1687(x86_cpu *c) {
    x86_set_r16(c, R_AX, 0);                           /* available */
    x86_set_r16(c, R_BX, 1);                           /* 32-bit clients supported */
    x86_set_r8(c, R_CL, 3);                            /* 386 */
    x86_set_r16(c, R_DX, 0x005A);                      /* version 0.90 */
    x86_set_r16(c, R_SI, 0);                           /* no private data needed */
    x86_load_seg(c, S_ES, PC_HLE_SEG);
    x86_set_r16(c, R_DI, PC_HLE_DPMI_ENTRY);
}

/* The client far-called the entry point. Build its descriptors, turn on
 * protected mode, and return to it on the other side. */
void dpmi_mode_switch(x86_cpu *c, int vector) {
    (void)vector;
    uint16_t ccs = pc_rd16(c, c->seg[S_SS].sel, (uint16_t)(c->r[R_SP] + 2));
    uint16_t cip = pc_rd16(c, c->seg[S_SS].sel, (uint16_t)c->r[R_SP]);
    uint16_t cds = c->seg[S_DS].sel, css = c->seg[S_SS].sel;
    uint32_t csp = c->r[R_SP] + 4;                     /* drop the far return address */

    if (!dpmi.seg) { c->eflags |= X86_CF; return; }

    /* AX bit 0 says the *application* is 32-bit; it does NOT describe the
     * descriptors we hand back. The client reached us with a 16-bit far
     * call from real mode and we far-return into the instruction after it,
     * so the initial CS/DS/SS/ES are always 16-bit, 64K, based on the
     * client's real-mode segments. DJGPP's stub proves the distinction:
     * it passes AX=1 and the code it returns to is 16-bit. A 32-bit client
     * builds its own 32-bit descriptors afterwards via INT 31h 0000/0009.
     * Give that stub a 32-bit CS and it mis-decodes its very first
     * instruction (the 4-byte `jc rel16` at 0x213 swallows two bytes of
     * the next one). */
    int is32 = x86_get_r16(c, R_AX) & 1;
    uint8_t f = 0x00;
    dpmi.is32 = is32;
    /* Shape the host for this client's width: a 16-bit client gets 16-bit
     * gates into a 16-bit view of the trap segment, so an INT pushes words
     * and a handler that chains with PUSHF / CALL FAR to the vector it read
     * back lands on a segment whose width also says words — pc_hle_return
     * sizes the frame by the trap segment's D bit. The host stack gets the
     * matching B bit, so a 16-bit handler's SP addresses it correctly. */
    for (int v = 0; v < 256; v++) idt_set(c, v, is32 ? SEL_HLE : SEL_HLE16, (uint32_t)v);
    set_desc(c, gdt_at(7), lin(dpmi.seg, 0), HOST_PARAS * 16u - 1, 0x92, is32 ? 0x40 : 0x00);
    x86_set_a20(c, 1);                                 /* a host always enables A20; extended memory needs it */

    uint16_t sel_cs = ldt_alloc(1), sel_ds = ldt_alloc(1);
    uint16_t sel_ss = ldt_alloc(1), sel_psp = ldt_alloc(1);
    if (!sel_cs || !sel_ds || !sel_ss || !sel_psp) { c->eflags |= X86_CF; return; }
    set_desc(c, ldt_at(ldt_index(sel_cs)),  (uint32_t)ccs << 4, 0xFFFF, 0x9A, f);
    set_desc(c, ldt_at(ldt_index(sel_ds)),  (uint32_t)cds << 4, 0xFFFF, 0x92, f);
    set_desc(c, ldt_at(ldt_index(sel_ss)),  (uint32_t)css << 4, 0xFFFF, 0x92, f);
    set_desc(c, ldt_at(ldt_index(sel_psp)), (uint32_t)dos.psp << 4, 0xFF, 0x92, f);

    c->gdtr.base = lin(dpmi.seg, GDT_OFF);  c->gdtr.limit = GDT_SLOTS * 8 - 1;
    c->idtr.base = lin(dpmi.seg, IDT_OFF);  c->idtr.limit = 256 * 8 - 1;
    c->cr0 |= 1;
    c->pmode = 1;
    x86_load_seg(c, S_DS, SEL_HDATA);                  /* something valid while we set up */
    c->ldtr.sel = SEL_LDT;
    c->ldtr.base = lin(dpmi.seg, LDT_OFF);
    c->ldtr.limit = LDT_SLOTS * 8 - 1;
    c->ldtr.attr = 0x82;
    c->ldtr.usable = 1;
    c->tr.sel = SEL_TSS;
    c->tr.base = lin(dpmi.seg, TSS_OFF);
    c->tr.limit = 0x67;
    c->tr.attr = 0x8B;
    c->tr.usable = 1;

    x86_load_seg(c, S_SS, sel_ss);
    c->r[R_SP] = csp;
    x86_load_seg(c, S_DS, sel_ds);
    x86_load_seg(c, S_ES, sel_psp);
    /* DPMI: the environment segment in the PSP becomes a selector, because
     * a client in protected mode has no other way to reach it. DJGPP's crt1
     * reads the word at PSP:2Ch and hands it straight to movedata. The
     * descriptor gets the environment block's real size rather than a blind
     * 64K, so a client that walks off the end faults here as it should. */
    uint16_t envseg = pc_rd16(c, dos.psp, 0x2C);
    if (envseg) {
        uint16_t envsel = sel_for_seg(c, envseg);
        if (envsel) {
            set_desc(c, ldt_at(ldt_index(envsel)), (uint32_t)envseg << 4,
                     ((uint32_t)mcb_paras(c, envseg) << 4) - 1, 0x92, 0x00);
            pc_wr16(c, dos.psp, 0x2C, envsel);
        }
    }

    x86_load_seg(c, S_CS, sel_cs);
    c->eip = cip;
    c->eflags &= ~X86_CF;
    dpmi.active = 1;
    pc.returned = 1;                                   /* we placed CS:IP ourselves */
    if (pc.debug) fprintf(stderr, "[dpmi] client in %d-bit protected mode at %04X:%04X\n", is32 ? 32 : 16, sel_cs, cip);
}

/* --- Real-mode excursions (INT 31h 0300/0301/0302) -----------------------
 * The client hands us a register image and asks for a real-mode interrupt
 * or far call. A host cannot simply run that nested: there is one CPU and
 * one run loop. So we do it the way the hardware would — turn protected
 * mode off, build the frame the real-mode handler expects, and let the
 * main loop carry on — with the *return* address pointing into the HLE
 * segment. When the handler finally IRETs or RETFs it lands on
 * PC_HLE_DPMI_RMRET, which traps like any other HLE service, copies the
 * registers back into the client's structure and restores protected mode.
 * Nothing about the excursion is special-cased in the interpreter, and a
 * real-mode INT 21h inside it traps to dos_int21 exactly as it always has.
 *
 * The structure (DPMI calls it the real-mode call structure) is addressed
 * through ES in protected mode, so its *linear* address is captured before
 * the switch — ES means something else on the other side.
 */
#define RM_INT   0                  /* push flags, vector from the IVT */
#define RM_FAR   1                  /* far call, RETF back */
#define RM_IRET  2                  /* far call, IRET back */

typedef struct {
    x86_seg  seg[6];
    uint32_t r[8], eip, eflags, cr0;
    int      pmode;
    uint32_t rms;                   /* linear address of the client's structure */
} rm_ctx;

static rm_ctx rm_stack[RM_DEPTH];
static int rm_depth;

/* The client's structure, by field offset. */
static uint32_t rs_rd(x86_cpu *c, uint32_t at, uint32_t off, int size) {
    return x86_rd(c, at, off, 0xFFFFFFFFu, size);
}
static void rs_wr(x86_cpu *c, uint32_t at, uint32_t off, int size, uint32_t v) {
    x86_wr(c, at, off, 0xFFFFFFFFu, size, v);
}

/* Registers in the structure are in PUSHA-ish order but not ours. */
static const struct { uint32_t off; int reg; } rm_regs[] = {
    { 0x00, R_DI }, { 0x04, R_SI }, { 0x08, R_BP },
    { 0x10, R_BX }, { 0x14, R_DX }, { 0x18, R_CX }, { 0x1C, R_AX },
};

static void rm_enter(x86_cpu *c, int kind, int vector) {
    if (rm_depth == RM_DEPTH) { x86_set_r16(c, R_AX, 0x8001); c->eflags |= X86_CF; return; }
    uint32_t rms = es_edi(c);
    uint16_t words = x86_get_r16(c, R_CX);

    rm_ctx *x = &rm_stack[rm_depth];
    memcpy(x->seg, c->seg, sizeof x->seg);
    memcpy(x->r, c->r, sizeof x->r);
    x->eip = c->eip; x->eflags = c->eflags; x->cr0 = c->cr0; x->pmode = c->pmode;
    x->rms = rms;

    /* Words the client wants handed to the real-mode procedure are taken
     * off its protected-mode stack before we leave it behind. The client's
     * own stack top is above the frame the INT 31h gate pushed: three slots
     * of the gate's width, since client and gate are both ring 0. */
    uint16_t copy[64];
    if (words > 64) words = 64;
    uint32_t stk = c->r[R_SP] + 3u * (uint32_t)gate_width(c), sm = stk_mask(c);
    for (int k = 0; k < words; k++)
        copy[k] = (uint16_t)x86_rd(c, c->seg[S_SS].base, (stk + (uint32_t)k * 2) & sm, sm, 2);

    uint16_t ss = (uint16_t)rs_rd(c, rms, 0x30, 2), sp = (uint16_t)rs_rd(c, rms, 0x2E, 2);
    if (!ss && !sp) {                                  /* the host supplies a stack */
        ss = (uint16_t)(dpmi.seg + ((RMSTK_OFF + RMSTK_LVL * rm_depth) >> 4));
        sp = RMSTK_LVL;
    }
    rm_depth++;

    c->pmode = 0;
    c->cr0 &= ~1u;
    x86_load_seg(c, S_SS, ss);
    c->r[R_SP] = sp;
    x86_load_seg(c, S_ES, (uint16_t)rs_rd(c, rms, 0x22, 2));
    x86_load_seg(c, S_DS, (uint16_t)rs_rd(c, rms, 0x24, 2));
    x86_load_seg(c, S_FS, (uint16_t)rs_rd(c, rms, 0x26, 2));
    x86_load_seg(c, S_GS, (uint16_t)rs_rd(c, rms, 0x28, 2));
    for (size_t k = 0; k < sizeof rm_regs / sizeof rm_regs[0]; k++)
        c->r[rm_regs[k].reg] = rs_rd(c, rms, rm_regs[k].off, 4);
    c->eflags = x86_flags_fixup(c, (c->eflags & 0xFFFF0000u) | (uint16_t)rs_rd(c, rms, 0x20, 2));

    for (int k = words - 1; k >= 0; k--) {             /* push in reverse: they end up in order */
        c->r[R_SP] = (c->r[R_SP] - 2) & 0xFFFF;
        x86_wr(c, c->seg[S_SS].base, c->r[R_SP], 0xFFFFFFFFu, 2, copy[k]);
    }

    /* The return address the handler will use, in the HLE segment. */
    x86_load_seg(c, S_CS, PC_HLE_SEG);
    c->eip = PC_HLE_DPMI_RMRET;
    if (kind == RM_INT) {
        x86_interrupt(c, vector, 0);                   /* pushes flags, CS:IP, reads the IVT */
    } else {
        if (kind == RM_IRET) {
            c->r[R_SP] = (c->r[R_SP] - 2) & 0xFFFF;
            x86_wr(c, c->seg[S_SS].base, c->r[R_SP], 0xFFFFFFFFu, 2, c->eflags);
        }
        c->r[R_SP] = (c->r[R_SP] - 2) & 0xFFFF;
        x86_wr(c, c->seg[S_SS].base, c->r[R_SP], 0xFFFFFFFFu, 2, PC_HLE_SEG);
        c->r[R_SP] = (c->r[R_SP] - 2) & 0xFFFF;
        x86_wr(c, c->seg[S_SS].base, c->r[R_SP], 0xFFFFFFFFu, 2, PC_HLE_DPMI_RMRET);
        uint16_t cs = (uint16_t)rs_rd(c, rms, 0x2C, 2);
        c->eip = (uint16_t)rs_rd(c, rms, 0x2A, 2);
        x86_load_seg(c, S_CS, cs);
    }
    c->eflags &= ~X86_CF;
    pc.returned = 1;                                   /* we placed CS:IP ourselves */
    if (pc.debug > 1)
        fprintf(stderr, "[dpmi] real-mode %s %02X depth %d: eax %08X ebx %08X ecx %08X edx %08X ds %04X\n",
                kind == RM_INT ? "int" : kind == RM_FAR ? "call" : "call/iret", vector, rm_depth,
                rs_rd(c, rms, 0x1C, 4), rs_rd(c, rms, 0x10, 4), rs_rd(c, rms, 0x18, 4),
                rs_rd(c, rms, 0x14, 4), (unsigned)rs_rd(c, rms, 0x24, 2));
}

/* The handler returned. Registers go back to the client; so does the CPU. */
void dpmi_rm_return(x86_cpu *c, int vector) {
    (void)vector;
    if (rm_depth == 0) { fprintf(stderr, "dpmi: real-mode return with no excursion\n"); c->halted = 1; return; }
    rm_ctx *x = &rm_stack[--rm_depth];
    uint32_t rms = x->rms;

    for (size_t k = 0; k < sizeof rm_regs / sizeof rm_regs[0]; k++)
        rs_wr(c, rms, rm_regs[k].off, 4, c->r[rm_regs[k].reg]);
    rs_wr(c, rms, 0x20, 2, c->eflags & 0xFFFF);
    rs_wr(c, rms, 0x22, 2, c->seg[S_ES].sel);
    rs_wr(c, rms, 0x24, 2, c->seg[S_DS].sel);
    rs_wr(c, rms, 0x26, 2, c->seg[S_FS].sel);
    rs_wr(c, rms, 0x28, 2, c->seg[S_GS].sel);
    rs_wr(c, rms, 0x2E, 2, c->r[R_SP] & 0xFFFF);
    rs_wr(c, rms, 0x30, 2, c->seg[S_SS].sel);

    memcpy(c->seg, x->seg, sizeof x->seg);
    memcpy(c->r, x->r, sizeof x->r);
    c->eip = x->eip; c->cr0 = x->cr0; c->pmode = x->pmode;
    c->eflags = x86_flags_fixup(c, x->eflags & ~(uint32_t)X86_CF);   /* the service itself succeeded */
    /* The context we saved was the INT 31h trap's, not the client's: finish
     * the service the ordinary way, popping the frame the gate pushed. */
    pc_hle_return(c, HLE_RET_FLAGS);
}

/* --- DOS memory blocks ---------------------------------------------------
 * INT 31h 0100 hands the client both a real-mode segment and a selector for
 * the same memory. A block longer than 64K needs an array of descriptors,
 * one per 64K window, because the client may address it with 16-bit code;
 * service 0003 says consecutive selectors are 8 apart, which is what the
 * LDT gives us. The block's own MCB records its size, so 0101 and 0102 can
 * work out how many descriptors there were without keeping a second table.
 */
static int desc_count(uint16_t paras) {
    uint32_t bytes = (uint32_t)paras << 4;
    return bytes ? (int)((bytes + 0xFFFF) >> 16) : 1;
}

static void set_dos_descs(x86_cpu *c, uint16_t sel, uint16_t seg, uint16_t paras, int n) {
    uint32_t bytes = (uint32_t)paras << 4;
    for (int k = 0; k < n; k++) {
        uint32_t left = bytes - (uint32_t)k * 0x10000u;
        uint32_t limit = left >= 0x10000u ? 0xFFFFu : (left ? left - 1 : 0);
        set_desc(c, ldt_at(ldt_index(sel) + k), ((uint32_t)seg << 4) + (uint32_t)k * 0x10000u,
                 limit, 0x92, 0x00);
    }
}

static uint16_t dos_descs(x86_cpu *c, uint16_t seg, uint16_t paras) {
    int n = desc_count(paras);
    uint16_t sel = ldt_alloc(n);
    if (sel) set_dos_descs(c, sel, seg, paras, n);
    return sel;
}

/* Recover the real-mode segment a DOS-block selector describes. */
static uint16_t dos_seg_of(x86_cpu *c, uint16_t sel) {
    int i = ldt_index(sel);
    if (i < 0 || !dpmi.used[i]) return 0;
    uint32_t at = ldt_at(i);
    uint32_t base = (uint32_t)x86_rd(c, at, 2, 0xFFFFFFFFu, 2)
                  | ((uint32_t)x86_rd(c, at, 4, 0xFFFFFFFFu, 1) << 16)
                  | ((uint32_t)x86_rd(c, at, 7, 0xFFFFFFFFu, 1) << 24);
    return (base & 0xF) || base >= 0x100000u ? 0 : (uint16_t)(base >> 4);
}

/* Read a descriptor's packed halves out of the table it lives in. */
static void get_desc(x86_cpu *c, uint32_t at, uint32_t *lo, uint32_t *hi) {
    *lo = x86_rd(c, at, 0, 0xFFFFFFFFu, 4);
    *hi = x86_rd(c, at, 4, 0xFFFFFFFFu, 4);
}

/* --- Real-mode callbacks (INT 31h 0303/0304) -----------------------------
 * The other direction: real-mode code calls the client. The client gives us
 * a protected-mode procedure and a structure to describe the call in, and
 * we hand back a real-mode address inside the HLE segment — so calling it
 * traps to us the same way a BIOS service does. Callback n lives at
 * PC_HLE_SEG:(n << 8 | PC_HLE_DPMI_CB), which the trap reads as vector
 * PC_HLE_DPMI_CB with n in the high byte of EIP; that gives 255 callbacks
 * out of one vector without the trap having to learn anything new.
 *
 * The client's procedure ends with an IRET, so the frame we build for it
 * returns to PC_HLE_DPMI_CBRET, and the structure it leaves in ES:EDI says
 * where real mode picks up. Unlike an excursion, nothing needs saving: the
 * structure describes the whole return.
 */
static struct { int used; uint16_t cs, rms_sel; uint32_t eip, rms_off; } cb[CB_SLOTS];

/* --- Protected-mode exceptions (INT 31h 0202/0203) -----------------------
 * A client's exception handler is NOT an interrupt handler and must not be
 * reached by pointing an IDT gate at it: DPMI hands it a wider frame and
 * expects a far RET, not an IRET.
 *
 *   ESP+00  return EIP     ESP+0C  EIP of the faulting instruction
 *   ESP+04  return CS      ESP+10  its CS
 *   ESP+08  error code     ESP+14  EFLAGS
 *                          ESP+18  ESP        ESP+1C  SS
 *
 * The handler may rewrite everything from +0C down — that is how DJGPP's
 * emu387 steps past the instruction it just emulated — so the return
 * trampoline reads the frame back rather than trusting what it pushed.
 * The IDT keeps pointing at the HLE segment, which is how we get here.
 */
static struct { int used; uint16_t cs; uint32_t eip; } exch[32];
static int exc_depth;

static int vec_has_err(int v) { return v == 8 || (v >= 10 && v <= 14) || v == 17; }

/* What a 16-bit frame cannot carry — the high halves of the client's ESP
 * and EFLAGS — is kept here, one entry per nesting level. */
static struct { uint32_t esp, eflags; } exc_saved[EXC_DEPTH];

int dpmi_pm_exception(x86_cpu *c, int vector) {
    /* The caller has already established that this is a fault and not an
     * interrupt sharing its vector — vector 8 is both #DF and the timer. */
    if (vector >= 32 || !exch[vector].used) return 0;
    if (exc_depth == EXC_DEPTH) {
        fprintf(stderr, "dpmi: exception %02X nested too deeply\n", vector);
        c->halted = 1;
        return 1;
    }

    /* Unwind the frame the gate pushed and rebuild it in DPMI's shape,
     * which is as wide as the client. */
    int gw = gate_width(c), w = dpmi.is32 ? 4 : 2;
    uint32_t sp = c->r[R_SP];
    uint32_t err = vec_has_err(vector) ? stk_pop(c, &sp, gw) : 0;
    uint32_t eip = stk_pop(c, &sp, gw);
    uint32_t cs  = stk_pop(c, &sp, gw);
    uint32_t fl  = stk_pop(c, &sp, gw);
    stk_commit(c, sp);
    uint32_t oldsp = c->r[R_SP], oldss = c->seg[S_SS].sel;
    if (gw == 2) fl = (c->eflags & 0xFFFF0000u) | fl;
    exc_saved[exc_depth].esp = oldsp;
    exc_saved[exc_depth].eflags = fl;

    /* The handler runs on a stack of ours. DPMI requires that, and it is
     * not mere tidiness: DJGPP's handler builds its signal frame just below
     * the *client's* faulting ESP and writes straight over a frame left
     * there — which is exactly how this was found. */
    x86_load_seg(c, S_SS, SEL_HSTK);
    sp = EXCSTK_OFF + EXCSTK_LVL * (uint32_t)(exc_depth + 1);
    c->r[R_SP] = 0;
    exc_depth++;

    stk_push(c, &sp, w, oldss);
    stk_push(c, &sp, w, oldsp);
    stk_push(c, &sp, w, fl);
    stk_push(c, &sp, w, cs);
    stk_push(c, &sp, w, eip);
    stk_push(c, &sp, w, err);
    stk_push(c, &sp, w, w == 4 ? SEL_HLE : SEL_HLE16);
    stk_push(c, &sp, w, PC_HLE_DPMI_EXCRET);
    stk_commit(c, sp);

    x86_load_seg(c, S_CS, exch[vector].cs);
    c->eip = exch[vector].eip;
    c->eflags = x86_flags_fixup(c, c->eflags & ~(uint32_t)(X86_IF | X86_TF));
    pc.returned = 1;
    if (pc.debug > 1) fprintf(stderr, "[dpmi] exception %02X err %04X at %04X:%08X → %04X:%08X\n",
                              vector, err, (unsigned)cs, eip, exch[vector].cs, exch[vector].eip);
    return 1;
}

/* The handler far-returned. Whatever is left of the frame — possibly
 * rewritten by the handler — says where the client resumes. */
void dpmi_exc_return(x86_cpu *c, int vector) {
    (void)vector;
    if (exc_depth > 0) exc_depth--;
    int w = dpmi.is32 ? 4 : 2;
    uint32_t sp = c->r[R_SP];
    (void)stk_pop(c, &sp, w);                          /* error code */
    uint32_t eip = stk_pop(c, &sp, w);
    uint32_t cs  = stk_pop(c, &sp, w);
    uint32_t fl  = stk_pop(c, &sp, w);
    uint32_t nsp = stk_pop(c, &sp, w);
    uint32_t nss = stk_pop(c, &sp, w);
    if (w == 2) {                                      /* put back what the words could not hold */
        nsp = (exc_saved[exc_depth].esp & 0xFFFF0000u) | nsp;
        fl  = (exc_saved[exc_depth].eflags & 0xFFFF0000u) | fl;
    }
    x86_load_seg(c, S_SS, (uint16_t)nss);
    c->r[R_SP] = nsp;
    x86_load_seg(c, S_CS, (uint16_t)cs);
    c->eip = eip;
    c->eflags = x86_flags_fixup(c, fl);
    pc.returned = 1;
}

/* An IDT gate, as INT 31h 0202-0205 read and write it. */
static void idt_get(x86_cpu *c, int v, uint16_t *sel, uint32_t *off) {
    uint32_t at = lin(dpmi.seg, IDT_OFF + (uint32_t)v * 8);
    *sel = (uint16_t)x86_rd(c, at, 2, 0xFFFFFFFFu, 2);
    *off = (uint32_t)x86_rd(c, at, 0, 0xFFFFFFFFu, 2) | ((uint32_t)x86_rd(c, at, 6, 0xFFFFFFFFu, 2) << 16);
}
static void idt_set(x86_cpu *c, int v, uint16_t sel, uint32_t off) {
    /* Interrupt gates (IF cleared on entry), DPL 3, as wide as whatever they
     * lead to: our own trap segments say so themselves, and a client's
     * handler is as wide as the client — a 16-bit handler returns with a
     * 16-bit IRET and must be given a 16-bit frame. */
    int g32 = sel == SEL_HLE ? 1 : sel == SEL_HLE16 ? 0 : dpmi.is32;
    uint32_t at = lin(dpmi.seg, IDT_OFF + (uint32_t)v * 8);
    x86_wr(c, at, 0, 0xFFFFFFFFu, 2, off & 0xFFFF);
    x86_wr(c, at, 2, 0xFFFFFFFFu, 2, sel);
    x86_wr(c, at, 4, 0xFFFFFFFFu, 1, 0);
    x86_wr(c, at, 5, 0xFFFFFFFFu, 1, g32 ? 0xEE : 0xE6);
    x86_wr(c, at, 6, 0xFFFFFFFFu, 2, g32 ? off >> 16 : 0);
}

/* Real-mode code called a callback address. Describe the call in the
 * client's structure and enter its procedure in protected mode. */
void dpmi_callback(x86_cpu *c, int vector) {
    (void)vector;
    int n = (int)(c->eip >> 8);
    if (n <= 0 || n >= CB_SLOTS || !cb[n].used) {
        fprintf(stderr, "dpmi: call to unallocated real-mode callback %d\n", n);
        c->halted = 1;
        return;
    }
    uint32_t rms = ((uint32_t)cb[n].rms_sel << 4) + cb[n].rms_off;   /* the selector's base, in real mode */
    uint32_t lo, hi;
    if (x86_read_desc(c, cb[n].rms_sel, &lo, &hi)) {
        x86_seg g; x86_unpack_desc(&g, cb[n].rms_sel, lo, hi);
        rms = g.base + cb[n].rms_off;
    }

    /* Take the far return address off the real-mode stack and put it in the
     * structure, so a client that changes nothing returns to its caller. */
    uint16_t ss = c->seg[S_SS].sel, sp = (uint16_t)c->r[R_SP];
    uint16_t rip = (uint16_t)x86_rd(c, c->seg[S_SS].base, sp, 0xFFFFFFFFu, 2);
    uint16_t rcs = (uint16_t)x86_rd(c, c->seg[S_SS].base, (uint32_t)(uint16_t)(sp + 2), 0xFFFFFFFFu, 2);
    sp = (uint16_t)(sp + 4);

    for (size_t k = 0; k < sizeof rm_regs / sizeof rm_regs[0]; k++)
        rs_wr(c, rms, rm_regs[k].off, 4, c->r[rm_regs[k].reg]);
    rs_wr(c, rms, 0x20, 2, c->eflags & 0xFFFF);
    rs_wr(c, rms, 0x22, 2, c->seg[S_ES].sel);
    rs_wr(c, rms, 0x24, 2, c->seg[S_DS].sel);
    rs_wr(c, rms, 0x26, 2, c->seg[S_FS].sel);
    rs_wr(c, rms, 0x28, 2, c->seg[S_GS].sel);
    rs_wr(c, rms, 0x2A, 2, rip);
    rs_wr(c, rms, 0x2C, 2, rcs);
    rs_wr(c, rms, 0x2E, 2, sp);
    rs_wr(c, rms, 0x30, 2, ss);

    uint16_t stk_sel = sel_for_seg(c, ss);
    c->pmode = 1;
    c->cr0 |= 1;
    x86_load_seg(c, S_SS, SEL_HSTK);                   /* the host's own callback stack */
    c->r[R_SP] = 0;
    x86_load_seg(c, S_DS, stk_sel);  c->r[R_SI] = sp;  /* DS:ESI = the real-mode stack */
    x86_load_seg(c, S_ES, cb[n].rms_sel); c->r[R_DI] = cb[n].rms_off;
    c->eflags = x86_flags_fixup(c, c->eflags & ~(uint32_t)(X86_IF | X86_TF));

    /* The procedure ends in an IRET as wide as the client. */
    int w = dpmi.is32 ? 4 : 2;
    uint32_t hsp = CBSTK_OFF + CBSTK_LEN;
    stk_push(c, &hsp, w, c->eflags);
    stk_push(c, &hsp, w, w == 4 ? SEL_HLE : SEL_HLE16);
    stk_push(c, &hsp, w, PC_HLE_DPMI_CBRET);
    stk_commit(c, hsp);
    x86_load_seg(c, S_CS, cb[n].cs);
    c->eip = cb[n].eip;
    pc.returned = 1;
    if (pc.debug) fprintf(stderr, "[dpmi] callback %d → %04X:%08X\n", n, cb[n].cs, cb[n].eip);
}

/* The client's callback procedure IRETed. Real mode resumes wherever the
 * structure it left in ES:EDI says. */
void dpmi_cb_return(x86_cpu *c, int vector) {
    (void)vector;
    uint32_t rms = c->seg[S_ES].base + (dpmi.is32 ? c->r[R_DI] : (c->r[R_DI] & 0xFFFF));
    uint16_t cs = (uint16_t)rs_rd(c, rms, 0x2C, 2), ip = (uint16_t)rs_rd(c, rms, 0x2A, 2);
    uint16_t ss = (uint16_t)rs_rd(c, rms, 0x30, 2), sp = (uint16_t)rs_rd(c, rms, 0x2E, 2);
    uint32_t r[8], fl = rs_rd(c, rms, 0x20, 2);
    uint16_t es = (uint16_t)rs_rd(c, rms, 0x22, 2), ds = (uint16_t)rs_rd(c, rms, 0x24, 2);
    uint16_t fs = (uint16_t)rs_rd(c, rms, 0x26, 2), gs = (uint16_t)rs_rd(c, rms, 0x28, 2);
    memcpy(r, c->r, sizeof r);
    for (size_t k = 0; k < sizeof rm_regs / sizeof rm_regs[0]; k++)
        r[rm_regs[k].reg] = rs_rd(c, rms, rm_regs[k].off, 4);

    c->pmode = 0;
    c->cr0 &= ~1u;
    memcpy(c->r, r, sizeof r);
    x86_load_seg(c, S_SS, ss); c->r[R_SP] = sp;
    x86_load_seg(c, S_ES, es); x86_load_seg(c, S_DS, ds);
    x86_load_seg(c, S_FS, fs); x86_load_seg(c, S_GS, gs);
    x86_load_seg(c, S_CS, cs); c->eip = ip;
    c->eflags = x86_flags_fixup(c, (c->eflags & 0xFFFF0000u) | fl);
    pc.returned = 1;
}

/* Everything a client accumulates, dropped between runs: dos_init may be
 * called more than once in a process (the fuzzers do), and a stale LDT slot
 * or callback would outlive the client that owned it. */
static void forget_client(void) {
    memset(exch, 0, sizeof exch);
    memset(cb, 0, sizeof cb);
    memset(extblk, 0, sizeof extblk);
    memset(seg_cache, 0, sizeof seg_cache);
    extn = 0; seg_cached = 0; rm_depth = 0; exc_depth = 0;
}

/* INT 31h. */
void dpmi_int31(x86_cpu *c, int vector) {
    (void)vector;
    uint16_t ax = x86_get_r16(c, R_AX);
    if (pc.debug > 1)
        fprintf(stderr, "[dpmi] INT 31h AX=%04X BX=%04X CX=%04X DX=%04X SI=%08X DI=%08X DS=%04X ES=%04X\n",
                ax, x86_get_r16(c, R_BX), x86_get_r16(c, R_CX), x86_get_r16(c, R_DX),
                c->r[R_SI], c->r[R_DI], c->seg[S_DS].sel, c->seg[S_ES].sel);
    c->eflags &= ~X86_CF;
    switch (ax) {
    case 0x0000: {                                     /* allocate LDT descriptors */
        uint16_t sel = ldt_alloc(x86_get_r16(c, R_CX));
        if (!sel) { x86_set_r16(c, R_AX, 8); c->eflags |= X86_CF; break; }
        int n = x86_get_r16(c, R_CX);
        for (int k = 0; k < n; k++)
            set_desc(c, ldt_at(ldt_index(sel) + k), 0, 0, 0x92, 0x40);
        x86_set_r16(c, R_AX, sel);
        break;
    }
    case 0x0001: {                                     /* free a descriptor */
        int i = ldt_index(x86_get_r16(c, R_BX));
        if (i < 0) { x86_set_r16(c, R_AX, 0x8022); c->eflags |= X86_CF; break; }
        dpmi.used[i] = 0;
        break;
    }
    case 0x0002: {                                     /* real-mode segment to descriptor */
        uint16_t sel = sel_for_seg(c, x86_get_r16(c, R_BX));
        if (!sel) { x86_set_r16(c, R_AX, 8); c->eflags |= X86_CF; break; }
        x86_set_r16(c, R_AX, sel);
        break;
    }
    case 0x0003:                                       /* selector increment */
        x86_set_r16(c, R_AX, 8);
        break;
    case 0x0004:                                       /* reserved (lock selector): no error */
    case 0x0005:                                       /* reserved (unlock selector): no error */
        break;
    case 0x0006: {                                     /* get segment base */
        int i = ldt_index(x86_get_r16(c, R_BX));
        if (i < 0) { x86_set_r16(c, R_AX, 0x8022); c->eflags |= X86_CF; break; }
        uint32_t at = ldt_at(i);
        uint32_t base = (uint32_t)x86_rd(c, at, 2, 0xFFFFFFFFu, 2)
                      | ((uint32_t)x86_rd(c, at, 4, 0xFFFFFFFFu, 1) << 16)
                      | ((uint32_t)x86_rd(c, at, 7, 0xFFFFFFFFu, 1) << 24);
        x86_set_r16(c, R_CX, (uint16_t)(base >> 16));
        x86_set_r16(c, R_DX, (uint16_t)base);
        break;
    }
    case 0x0007: {                                     /* set segment base */
        int i = ldt_index(x86_get_r16(c, R_BX));
        if (i < 0) { x86_set_r16(c, R_AX, 0x8022); c->eflags |= X86_CF; break; }
        uint32_t at = ldt_at(i);
        uint32_t base = ((uint32_t)x86_get_r16(c, R_CX) << 16) | x86_get_r16(c, R_DX);
        x86_wr(c, at, 2, 0xFFFFFFFFu, 2, base & 0xFFFF);
        x86_wr(c, at, 4, 0xFFFFFFFFu, 1, (base >> 16) & 0xFF);
        x86_wr(c, at, 7, 0xFFFFFFFFu, 1, (base >> 24) & 0xFF);
        break;
    }
    case 0x0008: {                                     /* set segment limit */
        int i = ldt_index(x86_get_r16(c, R_BX));
        if (i < 0) { x86_set_r16(c, R_AX, 0x8022); c->eflags |= X86_CF; break; }
        uint32_t at = ldt_at(i);
        uint32_t limit = ((uint32_t)x86_get_r16(c, R_CX) << 16) | x86_get_r16(c, R_DX);
        uint8_t flags = (uint8_t)x86_rd(c, at, 6, 0xFFFFFFFFu, 1) & 0xF0;
        if (limit > 0xFFFFF) { limit >>= 12; flags |= 0x80; }
        x86_wr(c, at, 0, 0xFFFFFFFFu, 2, limit & 0xFFFF);
        x86_wr(c, at, 6, 0xFFFFFFFFu, 1, (uint8_t)(((limit >> 16) & 0x0F) | flags));
        break;
    }
    case 0x0009: {                                     /* set access rights */
        int i = ldt_index(x86_get_r16(c, R_BX));
        if (i < 0) { x86_set_r16(c, R_AX, 0x8022); c->eflags |= X86_CF; break; }
        uint32_t at = ldt_at(i);
        uint16_t rights = x86_get_r16(c, R_CX);
        x86_wr(c, at, 5, 0xFFFFFFFFu, 1, rights & 0xFF);
        uint8_t flags = (uint8_t)x86_rd(c, at, 6, 0xFFFFFFFFu, 1) & 0x0F;
        x86_wr(c, at, 6, 0xFFFFFFFFu, 1, (uint8_t)(flags | ((rights >> 8) & 0xF0)));
        break;
    }
    case 0x000A: {                                     /* create alias descriptor */
        int i = ldt_index(x86_get_r16(c, R_BX));
        if (i < 0 || !dpmi.used[i]) { x86_set_r16(c, R_AX, 0x8022); c->eflags |= X86_CF; break; }
        uint16_t sel = ldt_alloc(1);
        if (!sel) { x86_set_r16(c, R_AX, 8); c->eflags |= X86_CF; break; }
        uint32_t lo, hi;
        get_desc(c, ldt_at(i), &lo, &hi);
        hi = (hi & ~0x00000F00u) | 0x00000200u;        /* same base and limit, as writable data */
        x86_wr(c, ldt_at(ldt_index(sel)), 0, 0xFFFFFFFFu, 4, lo);
        x86_wr(c, ldt_at(ldt_index(sel)), 4, 0xFFFFFFFFu, 4, hi);
        x86_set_r16(c, R_AX, sel);
        break;
    }
    case 0x000B: {                                     /* get descriptor */
        int i = ldt_index(x86_get_r16(c, R_BX));
        if (i < 0) { x86_set_r16(c, R_AX, 0x8022); c->eflags |= X86_CF; break; }
        uint32_t lo, hi, at = es_edi(c);
        get_desc(c, ldt_at(i), &lo, &hi);
        x86_wr(c, at, 0, 0xFFFFFFFFu, 4, lo);
        x86_wr(c, at, 4, 0xFFFFFFFFu, 4, hi);
        break;
    }
    case 0x000C: {                                     /* set descriptor */
        int i = ldt_index(x86_get_r16(c, R_BX));
        if (i < 0) { x86_set_r16(c, R_AX, 0x8022); c->eflags |= X86_CF; break; }
        uint32_t at = es_edi(c);
        x86_wr(c, ldt_at(i), 0, 0xFFFFFFFFu, 4, x86_rd(c, at, 0, 0xFFFFFFFFu, 4));
        x86_wr(c, ldt_at(i), 4, 0xFFFFFFFFu, 4, x86_rd(c, at, 4, 0xFFFFFFFFu, 4));
        break;
    }
    case 0x0200: {                                     /* get real-mode interrupt vector */
        uint32_t v = x86_rd(c, 0, (uint32_t)x86_get_r8(c, R_BL) * 4, 0xFFFFFFFFu, 4);
        x86_set_r16(c, R_CX, (uint16_t)(v >> 16));
        x86_set_r16(c, R_DX, (uint16_t)v);
        break;
    }
    case 0x0201:                                       /* set real-mode interrupt vector */
        x86_wr(c, 0, (uint32_t)x86_get_r8(c, R_BL) * 4, 0xFFFFFFFFu, 4,
               ((uint32_t)x86_get_r16(c, R_CX) << 16) | x86_get_r16(c, R_DX));
        break;
    case 0x0202: {                                     /* get exception handler */
        int v = x86_get_r8(c, R_BL) & 0x1F;
        x86_set_r16(c, R_CX, exch[v].used ? exch[v].cs : (dpmi.is32 ? SEL_HLE : SEL_HLE16));
        c->r[R_DX] = exch[v].used ? exch[v].eip : (uint32_t)v;
        break;
    }
    case 0x0203: {                                     /* set exception handler */
        int v = x86_get_r8(c, R_BL) & 0x1F;
        exch[v].used = 1;
        exch[v].cs = x86_get_r16(c, R_CX);
        exch[v].eip = c->r[R_DX];
        break;
    }
    case 0x0204: {                                     /* get protected-mode interrupt vector */
        uint16_t sel; uint32_t off;
        idt_get(c, x86_get_r8(c, R_BL), &sel, &off);
        x86_set_r16(c, R_CX, sel);
        c->r[R_DX] = off;
        break;
    }
    case 0x0205:                                       /* set protected-mode interrupt vector */
        /* An interrupt handler *is* reached through the IDT: it gets the
         * ordinary frame and returns with IRET. Only exceptions (0203) need
         * the host in the middle. */
        idt_set(c, x86_get_r8(c, R_BL), x86_get_r16(c, R_CX), c->r[R_DX]);
        break;
    case 0x0600: case 0x0601:                          /* lock / unlock linear region */
    case 0x0602: case 0x0603:                          /* mark real-mode region (un)pageable */
        break;                                         /* nothing is ever paged out */
    case 0x0604:                                       /* get page size */
        x86_set_r16(c, R_BX, 0);
        x86_set_r16(c, R_CX, EXT_PAGE);
        break;
    case 0x0900:                                       /* disable virtual interrupts */
    case 0x0901:                                       /* enable virtual interrupts */
    case 0x0902: {                                     /* get virtual interrupt state */
        int was = (c->eflags & X86_IF) != 0;
        if (ax == 0x0900) c->eflags &= ~(uint32_t)X86_IF;
        else if (ax == 0x0901) c->eflags |= X86_IF;
        x86_set_r8(c, R_AL, (uint8_t)was);
        break;
    }
    case 0x0E00:                                       /* get coprocessor status */
        /* Bit 0 present, 1 client-enabled, 2 host emulating, 3 client
         * emulating, 4-7 type. There is no FPU here and we do not pretend
         * to emulate one, so the only bit that can be set is the client's. */
        x86_set_r16(c, R_AX, (c->cr0 & 0x4) ? 0x0008 : 0x0000);
        break;
    case 0x0E01: {                                     /* set coprocessor emulation */
        uint16_t bits = x86_get_r16(c, R_BX);
        /* Bit 0 enables the coprocessor, bit 1 says the client supplies the
         * emulation. Asking for real hardware we do not have is the one
         * request we refuse; every other state sets CR0.EM, so coprocessor
         * instructions arrive as #NM rather than quietly doing nothing. */
        if ((bits & 3) == 1) { x86_set_r16(c, R_AX, 0x8025); c->eflags |= X86_CF; break; }
        c->cr0 |= 0x4;
        break;
    }
    case 0x0303: {                                     /* allocate real-mode callback */
        int n = 1;
        while (n < CB_SLOTS && cb[n].used) n++;
        if (n == CB_SLOTS) { x86_set_r16(c, R_AX, 0x8015); c->eflags |= X86_CF; break; }
        cb[n].used = 1;
        uint32_t om = caller_is32(c) ? 0xFFFFFFFFu : 0xFFFFu;
        cb[n].cs = c->seg[S_DS].sel;  cb[n].eip = c->r[R_SI] & om;
        cb[n].rms_sel = c->seg[S_ES].sel; cb[n].rms_off = c->r[R_DI] & om;
        x86_set_r16(c, R_CX, PC_HLE_SEG);
        x86_set_r16(c, R_DX, (uint16_t)((n << 8) | PC_HLE_DPMI_CB));
        if (pc.debug) fprintf(stderr, "[dpmi] callback %d = %04X:%08X, structure %04X:%08X\n",
                              n, cb[n].cs, cb[n].eip, cb[n].rms_sel, cb[n].rms_off);
        break;
    }
    case 0x0304: {                                     /* free real-mode callback */
        uint16_t off = x86_get_r16(c, R_DX);
        int n = off >> 8;
        if (x86_get_r16(c, R_CX) != PC_HLE_SEG || (off & 0xFF) != PC_HLE_DPMI_CB
            || n <= 0 || n >= CB_SLOTS || !cb[n].used) {
            x86_set_r16(c, R_AX, 0x8024); c->eflags |= X86_CF; break;
        }
        cb[n].used = 0;
        break;
    }
    case 0x0300:                                       /* simulate real-mode interrupt */
        rm_enter(c, RM_INT, x86_get_r8(c, R_BL));
        return;                                        /* rm_enter placed CS:IP */
    case 0x0301:                                       /* call real-mode procedure, RETF */
        rm_enter(c, RM_FAR, 0);
        return;
    case 0x0302:                                       /* call real-mode procedure, IRET */
        rm_enter(c, RM_IRET, 0);
        return;
    case 0x0100: {                                     /* allocate DOS memory block */
        uint16_t paras = x86_get_r16(c, R_BX), largest = 0;
        uint16_t seg = dos_mem_alloc(paras, dos.psp, &largest);
        if (!seg) { x86_set_r16(c, R_AX, 8); x86_set_r16(c, R_BX, largest); c->eflags |= X86_CF; break; }
        uint16_t sel = dos_descs(c, seg, paras);
        if (!sel) { dos_mem_free(seg); x86_set_r16(c, R_AX, 8); x86_set_r16(c, R_BX, 0); c->eflags |= X86_CF; break; }
        x86_set_r16(c, R_AX, seg);
        x86_set_r16(c, R_DX, sel);
        if (pc.debug) fprintf(stderr, "[dpmi] dos alloc %u paras at %04X, selector %04X\n", paras, seg, sel);
        break;
    }
    case 0x0101: {                                     /* free DOS memory block */
        uint16_t sel = x86_get_r16(c, R_DX);
        uint16_t seg = dos_seg_of(c, sel);
        if (!seg) { x86_set_r16(c, R_AX, 0x8022); c->eflags |= X86_CF; break; }
        int n = desc_count(mcb_paras(c, seg));
        for (int k = 0; k < n; k++) dpmi.used[ldt_index(sel) + k] = 0;
        dos_mem_free(seg);
        break;
    }
    case 0x0102: {                                     /* resize DOS memory block */
        uint16_t sel = x86_get_r16(c, R_DX), paras = x86_get_r16(c, R_BX), largest = 0;
        uint16_t seg = dos_seg_of(c, sel);
        if (!seg) { x86_set_r16(c, R_AX, 0x8022); c->eflags |= X86_CF; break; }
        int had = desc_count(mcb_paras(c, seg)), want = desc_count(paras);
        int i = ldt_index(sel);
        for (int k = had; k < want; k++)                /* the array can only grow in place */
            if (i + k >= LDT_SLOTS || dpmi.used[i + k]) {
                x86_set_r16(c, R_AX, 0x8011); c->eflags |= X86_CF; goto done_0102;
            }
        if (dos_mem_resize(seg, paras, &largest) != DE_OK) {
            x86_set_r16(c, R_AX, 8); x86_set_r16(c, R_BX, largest); c->eflags |= X86_CF; break;
        }
        for (int k = want; k < had; k++) dpmi.used[i + k] = 0;
        for (int k = had; k < want; k++) dpmi.used[i + k] = 1;
        set_dos_descs(c, sel, seg, paras, want);
    done_0102:
        break;
    }
    case 0x0500: {                                     /* get free memory information */
        uint32_t at = es_edi(c);
        for (int k = 0; k < 0x30; k += 4) x86_wr(c, at, (uint32_t)k, 0xFFFFFFFFu, 4, 0xFFFFFFFFu);
        uint32_t largest = ext_largest(), free_pages = (EXT_TOP - EXT_BASE - ext_used()) / EXT_PAGE;
        x86_wr(c, at, 0x00, 0xFFFFFFFFu, 4, largest);            /* largest free block, bytes */
        x86_wr(c, at, 0x04, 0xFFFFFFFFu, 4, largest / EXT_PAGE); /* max unlocked page allocation */
        x86_wr(c, at, 0x08, 0xFFFFFFFFu, 4, largest / EXT_PAGE); /* max locked: no paging, same thing */
        x86_wr(c, at, 0x0C, 0xFFFFFFFFu, 4, (EXT_TOP - EXT_BASE) / EXT_PAGE);
        x86_wr(c, at, 0x10, 0xFFFFFFFFu, 4, free_pages);
        x86_wr(c, at, 0x14, 0xFFFFFFFFu, 4, free_pages);
        x86_wr(c, at, 0x18, 0xFFFFFFFFu, 4, (EXT_TOP - EXT_BASE) / EXT_PAGE);
        x86_wr(c, at, 0x1C, 0xFFFFFFFFu, 4, free_pages);
        x86_wr(c, at, 0x20, 0xFFFFFFFFu, 4, 0);                  /* no paging file */
        break;
    }
    case 0x0501: {                                     /* allocate memory block */
        uint32_t size = ((uint32_t)x86_get_r16(c, R_BX) << 16) | x86_get_r16(c, R_CX);
        uint32_t base = ext_alloc(size);
        if (!base) { x86_set_r16(c, R_AX, 0x8012); c->eflags |= X86_CF; break; }
        x86_set_r16(c, R_BX, (uint16_t)(base >> 16)); x86_set_r16(c, R_CX, (uint16_t)base);
        x86_set_r16(c, R_SI, (uint16_t)(base >> 16)); x86_set_r16(c, R_DI, (uint16_t)base);
        if (pc.debug) fprintf(stderr, "[dpmi] alloc %u bytes at %08X\n", size, base);
        break;
    }
    case 0x0502: {                                     /* free memory block */
        uint32_t h = ((uint32_t)x86_get_r16(c, R_SI) << 16) | x86_get_r16(c, R_DI);
        if (!ext_free(h)) { x86_set_r16(c, R_AX, 0x8023); c->eflags |= X86_CF; }
        break;
    }
    case 0x0503: {                                     /* resize memory block */
        uint32_t h = ((uint32_t)x86_get_r16(c, R_SI) << 16) | x86_get_r16(c, R_DI);
        uint32_t size = ((uint32_t)x86_get_r16(c, R_BX) << 16) | x86_get_r16(c, R_CX);
        uint32_t old_size = ext_size_of(h);
        if (!old_size) { x86_set_r16(c, R_AX, 0x8023); c->eflags |= X86_CF; break; }
        /* No paging means no remapping: move the contents into a new block.
         * Allocate before freeing so a failed grow leaves the old one intact. */
        uint32_t base = ext_alloc(size);
        if (!base) { x86_set_r16(c, R_AX, 0x8012); c->eflags |= X86_CF; break; }
        uint32_t keep = size < old_size ? size : old_size;
        memmove(c->mem + base, c->mem + h, keep);
        ext_free(h);
        x86_set_r16(c, R_BX, (uint16_t)(base >> 16)); x86_set_r16(c, R_CX, (uint16_t)base);
        x86_set_r16(c, R_SI, (uint16_t)(base >> 16)); x86_set_r16(c, R_DI, (uint16_t)base);
        break;
    }
    case 0x0400:                                       /* version */
        x86_set_r16(c, R_AX, 0x005A);                  /* 0.90 */
        x86_set_r16(c, R_BX, 0x0001);                  /* 32-bit, no paging */
        x86_set_r8(c, R_CL, 3);
        x86_set_r16(c, R_DX, 0xFFFF);
        break;
    default:
        if (pc.debug) fprintf(stderr, "[dpmi] unimplemented INT 31h AX=%04X\n", ax);
        x86_set_r16(c, R_AX, 0x8001);
        c->eflags |= X86_CF;
        break;
    }
}
