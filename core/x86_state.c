/* x86_state.c — CPU construction, reset, segment loading, dumps */

#include "x86.h"
#include <stdlib.h>
#include <string.h>

void x86_init(x86_cpu *c, int model, uint32_t mem_size) {
    memset(c, 0, sizeof(*c));
    c->model = model;
    c->mem_size = mem_size;
    c->mem = calloc(mem_size, 1);
    c->a20_mask = 0xFFFFF;          /* A20 gated off at power-on */
    c->exc = -1;
    x86_reset(c);
}

void x86_free(x86_cpu *c) {
    free(c->mem);
    c->mem = NULL;
}

/* Real-mode segment load: base = sel << 4, limit 64K, 16-bit. Phase B
 * replaces this with descriptor lookup when c->pmode is set. */
void x86_load_seg(x86_cpu *c, int s, uint16_t sel) {
    c->seg[s].sel = sel;
    c->seg[s].base = (uint32_t)sel << 4;
    c->seg[s].limit = 0xFFFF;
    c->seg[s].big = 0;
    c->seg[s].attr = 0;
}

uint32_t x86_flags_fixup(x86_cpu *c, uint32_t f) {
    f |= X86_F1;
    f &= ~0x28u;                                  /* bits 3 and 5 always 0 */
    switch (c->model) {
    case X86_MODEL_8086:
    case X86_MODEL_186:
        f |= 0xF000; f &= 0xFFFF; break;          /* 12-15 read as 1 */
    case X86_MODEL_286:
        if (!c->pmode) f &= 0x0FFF; else f &= 0x7FFF; break;
    default:
        f &= 0x00037FFF;                          /* RF/VM allowed, 15 and 18+ clear */
        if (!c->pmode) f &= ~X86_VM;
        break;
    }
    return f;
}

void x86_reset(x86_cpu *c) {
    memset(c->r, 0, sizeof c->r);
    c->eflags = x86_flags_fixup(c, 0);
    c->pmode = 0;
    c->halted = 0;
    c->int_inhibit = 0;
    c->exc = -1;
    for (int i = 0; i < 6; i++) x86_load_seg(c, i, 0);
    x86_load_seg(c, S_CS, 0xFFFF);
    c->eip = 0;
    if (c->model >= X86_MODEL_286) {              /* 286+: CS base F0000/FFFF0000, IP FFF0 */
        c->seg[S_CS].sel = 0xF000;
        c->seg[S_CS].base = c->model >= X86_MODEL_386 ? 0xFFFF0000u : 0xFF0000u;
        c->eip = 0xFFF0;
    }
}

void x86_dump(x86_cpu *c, FILE *f) {
    static const char *rn[8] = { "AX","CX","DX","BX","SP","BP","SI","DI" };
    static const char *sn[6] = { "ES","CS","SS","DS","FS","GS" };
    for (int i = 0; i < 8; i++) fprintf(f, "%s=%08X%c", rn[i], c->r[i], i == 7 ? '\n' : ' ');
    for (int i = 0; i < 6; i++) fprintf(f, "%s=%04X%c", sn[i], c->seg[i].sel, i == 5 ? ' ' : ' ');
    fprintf(f, "IP=%08X FL=%08X [%c%c%c%c%c%c%c%c%c]\n", c->eip, c->eflags,
            c->eflags & X86_OF ? 'O' : '.', c->eflags & X86_DF ? 'D' : '.',
            c->eflags & X86_IF ? 'I' : '.', c->eflags & X86_TF ? 'T' : '.',
            c->eflags & X86_SF ? 'S' : '.', c->eflags & X86_ZF ? 'Z' : '.',
            c->eflags & X86_AF ? 'A' : '.', c->eflags & X86_PF ? 'P' : '.',
            c->eflags & X86_CF ? 'C' : '.');
}
