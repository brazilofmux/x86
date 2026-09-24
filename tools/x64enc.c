/* x64enc.c — encoder check for dbt/emit_x64.h against objdump.
 *
 * Emits one instruction per case into a buffer, writes it out, and
 * tools/x64enc.sh disassembles the buffer (objdump -M intel) and compares
 * each line with the text expected here. The cases cover the encoder's
 * decisions: the 66h prefix and REX.W, the byte forms, high-byte and
 * R8..R15 operands, the SIB and disp8=0 special cases for RSP/R12 and
 * RBP/R13 bases, index scaling, imm8-versus-imm32, and the VEX forms.
 */
#include "../dbt/emit_x64.h"
#include <stdio.h>
#include <stdlib.h>

static uint8_t buf[65536];
static emit_t E = { buf, 0, sizeof buf };
static FILE *exp_out;

#define CASE(text, ...) do { __VA_ARGS__; fprintf(exp_out, "%s\n", text); } while (0)

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: x64enc code.bin expected.txt\n"); return 2; }
    exp_out = fopen(argv[2], "w");
    emit_t *e = &E;
    x64_mem_t m;

    /* moves */
    CASE("mov eax,ebx",              emit_mov_rr(e, 4, X64_RAX, X64_RBX));
    CASE("mov r8d,ecx",              emit_mov_rr(e, 4, X64_R8, X64_RCX));
    CASE("mov rcx,r9",               emit_mov_rr(e, 8, X64_RCX, X64_R9));
    CASE("mov ax,dx",                emit_mov_rr(e, 2, X64_RAX, X64_RDX));
    CASE("mov al,dl",                emit_mov_rr(e, 1, X64_AL, X64_DL));
    CASE("mov ah,al",                emit_mov_rr(e, 1, X64_AH, X64_AL));
    CASE("mov bh,ch",                emit_mov_rr(e, 1, X64_BH, X64_CH));
    CASE("mov r9b,al",               emit_mov_rr(e, 1, X64_R9, X64_AL));
    m = x64_m(X64_R15, 0);
    CASE("mov eax,DWORD PTR [r15]",  emit_mov_rm(e, 4, X64_RAX, &m));
    m = x64_mi(X64_R15, X64_RBX, 0, 0);
    CASE("mov ax,WORD PTR [r15+rbx*1]", emit_mov_rm(e, 2, X64_RAX, &m));
    m = x64_mi(X64_R14, X64_R8, 2, 0x10);
    CASE("mov DWORD PTR [r14+r8*4+0x10],ecx", emit_mov_mr(e, 4, &m, X64_RCX));
    m = x64_m(X64_RSP, 8);
    CASE("mov rax,QWORD PTR [rsp+0x8]", emit_mov_rm(e, 8, X64_RAX, &m));
    m = x64_m(X64_R12, 0);
    CASE("mov BYTE PTR [r12],al",    emit_mov_mr(e, 1, &m, X64_AL));
    m = x64_m(X64_RBP, 0);
    CASE("mov BYTE PTR [rbp+0x0],al", emit_mov_mr(e, 1, &m, X64_AL));
    m = x64_m(X64_R13, 0);
    CASE("mov WORD PTR [r13+0x0],dx", emit_mov_mr(e, 2, &m, X64_RDX));
    m = x64_m(X64_RBX, -0x200);
    CASE("mov ah,BYTE PTR [rbx-0x200]", emit_mov_rm(e, 1, X64_AH, &m));
    m = x64_mi(X64_RBX, X64_RSI, 0, 0x7F);
    CASE("mov BYTE PTR [rbx+rsi*1+0x7f],dh", emit_mov_mr(e, 1, &m, X64_DH));
    CASE("mov eax,0x12345678",       emit_mov_ri(e, 4, X64_RAX, 0x12345678));
    CASE("mov r10d,0x1",             emit_mov_ri(e, 4, X64_R10, 1));
    CASE("mov cx,0x1234",            emit_mov_ri(e, 2, X64_RCX, 0x1234));
    CASE("mov ah,0x9",               emit_mov_ri(e, 1, X64_AH, 9));
    CASE("mov r11b,0xff",            emit_mov_ri(e, 1, X64_R11, 0xFF));
    CASE("movabs r13,0x1122334455",     emit_mov_ri(e, 8, X64_R13, 0x1122334455ull));
    CASE("mov r13d,0x11223344",      emit_mov_ri(e, 8, X64_R13, 0x11223344ull));
    m = x64_m(X64_R13, 0x40);
    CASE("mov DWORD PTR [r13+0x40],0xffffffff", emit_mov_mi(e, 4, &m, 0xFFFFFFFFu));
    CASE("mov WORD PTR [r13+0x40],0x8000", emit_mov_mi(e, 2, &m, 0x8000));
    CASE("mov BYTE PTR [r13+0x40],0x7", emit_mov_mi(e, 1, &m, 7));
    CASE("mov QWORD PTR [r13+0x40],0xffffffffffffffff", emit_mov_mi(e, 8, &m, 0xFFFFFFFFu));
    CASE("movzx eax,bh",             emit_movzx_rr(e, 4, X64_RAX, 1, X64_BH));
    CASE("movzx r8d,al",             emit_movzx_rr(e, 4, X64_R8, 1, X64_AL));
    CASE("movzx r8d,r9b",            emit_movzx_rr(e, 4, X64_R8, 1, X64_R9));
    CASE("movzx eax,ax",             emit_movzx_rr(e, 4, X64_RAX, 2, X64_RAX));
    CASE("movzx r8,r9w",             emit_movzx_rr(e, 8, X64_R8, 2, X64_R9));
    m = x64_mi(X64_R15, X64_R8, 0, 0);
    CASE("movzx eax,BYTE PTR [r15+r8*1]", emit_movzx_rm(e, 4, X64_RAX, 1, &m));
    CASE("movzx r9d,WORD PTR [r15+r8*1]", emit_movzx_rm(e, 4, X64_R9, 2, &m));
    CASE("movsx eax,al",             emit_movsx_rr(e, 4, X64_RAX, 1, X64_AL));
    CASE("movsx ax,al",              emit_movsx_rr(e, 2, X64_RAX, 1, X64_AL));
    CASE("movsx edx,ax",             emit_movsx_rr(e, 4, X64_RDX, 2, X64_RAX));
    CASE("movsxd r8,eax",            emit_movsx_rr(e, 8, X64_R8, 4, X64_RAX));
    CASE("movsx ecx,BYTE PTR [r15+r8*1]", emit_movsx_rm(e, 4, X64_RCX, 1, &m));
    m = x64_mi(X64_RBX, X64_RSI, 0, 0x1234);
    CASE("lea r8d,[rbx+rsi*1+0x1234]", emit_lea(e, 4, X64_R8, &m));
    m = x64_mi(X64_R14, X64_R8, 0, 0);
    CASE("lea r9,[r14+r8*1]",        emit_lea(e, 8, X64_R9, &m));
    CASE("xchg al,ah",               emit_xchg_rr(e, 1, X64_AH, X64_AL));
    CASE("xchg ebx,eax",             emit_xchg_rr(e, 4, X64_RAX, X64_RBX));
    m = x64_m(X64_R15, 4);
    CASE("xchg WORD PTR [r15+0x4],cx", emit_xchg_rm(e, 2, X64_RCX, &m));
    CASE("cmove eax,ecx",            emit_cmov_rr(e, 4, X64_CC_E, X64_RAX, X64_RCX));
    CASE("cmovne r8w,WORD PTR [r15+0x4]", emit_cmov_rm(e, 2, X64_CC_NE, X64_R8, &m));
    CASE("setb al",                  emit_setcc_r(e, X64_CC_B, X64_AL));
    CASE("setg r8b",                 emit_setcc_r(e, X64_CC_G, X64_R8));
    CASE("seto BYTE PTR [r15+0x4]",  emit_setcc_m(e, X64_CC_O, &m));
    CASE("bswap eax",                emit_bswap(e, 4, X64_RAX));
    CASE("bswap r9",                 emit_bswap(e, 8, X64_R9));

    /* ALU */
    CASE("add eax,ebx",              emit_alu_rr(e, 4, X64_ALU_ADD, X64_RAX, X64_RBX));
    CASE("add ax,bx",                emit_alu_rr(e, 2, X64_ALU_ADD, X64_RAX, X64_RBX));
    CASE("sub ah,bl",                emit_alu_rr(e, 1, X64_ALU_SUB, X64_AH, X64_BL));
    CASE("adc r8d,r9d",              emit_alu_rr(e, 4, X64_ALU_ADC, X64_R8, X64_R9));
    CASE("cmp rax,r15",              emit_alu_rr(e, 8, X64_ALU_CMP, X64_RAX, X64_R15));
    m = x64_mi(X64_R15, X64_RBX, 0, 0);
    CASE("xor ax,WORD PTR [r15+rbx*1]", emit_alu_rm(e, 2, X64_ALU_XOR, X64_RAX, &m));
    CASE("and DWORD PTR [r15+rbx*1],ecx", emit_alu_mr(e, 4, X64_ALU_AND, &m, X64_RCX));
    CASE("or BYTE PTR [r15+rbx*1],dl", emit_alu_mr(e, 1, X64_ALU_OR, &m, X64_DL));
    CASE("add eax,0x7f",             emit_alu_ri(e, 4, X64_ALU_ADD, X64_RAX, 0x7F));
    CASE("add eax,0x80",             emit_alu_ri(e, 4, X64_ALU_ADD, X64_RAX, 0x80));
    CASE("add eax,0xffffff80",       emit_alu_ri(e, 4, X64_ALU_ADD, X64_RAX, 0xFFFFFF80u));
    CASE("sub ax,0x1234",            emit_alu_ri(e, 2, X64_ALU_SUB, X64_RAX, 0x1234));
    CASE("sub ax,0xfffe",            emit_alu_ri(e, 2, X64_ALU_SUB, X64_RAX, 0xFFFE));
    CASE("cmp ah,0x1a",              emit_alu_ri(e, 1, X64_ALU_CMP, X64_AH, 0x1A));
    CASE("cmp r9b,0x1a",             emit_alu_ri(e, 1, X64_ALU_CMP, X64_R9, 0x1A));
    CASE("sub r12,0x2",              emit_alu_ri(e, 8, X64_ALU_SUB, X64_R12, 2));
    CASE("and r12,0xffffffffffff0000", emit_alu_ri(e, 8, X64_ALU_AND, X64_R12, 0xFFFF0000u));
    m = x64_m(X64_R13, 0x100);
    CASE("sub DWORD PTR [r13+0x100],0x40", emit_alu_mi(e, 4, X64_ALU_SUB, &m, 0x40));
    CASE("cmp WORD PTR [r13+0x100],0x1234", emit_alu_mi(e, 2, X64_ALU_CMP, &m, 0x1234));
    CASE("cmp BYTE PTR [r13+0x100],0x0", emit_alu_mi(e, 1, X64_ALU_CMP, &m, 0));
    CASE("test eax,ecx",             emit_test_rr(e, 4, X64_RAX, X64_RCX));
    CASE("test ah,ah",               emit_test_rr(e, 1, X64_AH, X64_AH));
    CASE("test BYTE PTR [r13+0x100],r8b", emit_test_mr(e, 1, &m, X64_R8));
    CASE("test ecx,0x8000",          emit_test_ri(e, 4, X64_RCX, 0x8000));
    CASE("test cl,0x1f",             emit_test_ri(e, 1, X64_CL, 0x1F));
    CASE("test BYTE PTR [r13+0x100],0x80", emit_test_mi(e, 1, &m, 0x80));
    CASE("inc eax",                  emit_inc_r(e, 4, X64_RAX));
    CASE("dec cx",                   emit_dec_r(e, 2, X64_RCX));
    CASE("inc ah",                   emit_inc_r(e, 1, X64_AH));
    CASE("dec r8b",                  emit_dec_r(e, 1, X64_R8));
    CASE("inc WORD PTR [r13+0x100]", emit_inc_m(e, 2, &m));
    CASE("dec QWORD PTR [r13+0x100]", emit_dec_m(e, 8, &m));
    CASE("not eax",                  emit_g3_r(e, 4, X64_G3_NOT, X64_RAX));
    CASE("neg bh",                   emit_g3_r(e, 1, X64_G3_NEG, X64_BH));
    CASE("mul ecx",                  emit_g3_r(e, 4, X64_G3_MUL, X64_RCX));
    CASE("imul r8w",                 emit_g3_r(e, 2, X64_G3_IMUL, X64_R8));
    CASE("div bl",                   emit_g3_r(e, 1, X64_G3_DIV, X64_BL));
    CASE("idiv DWORD PTR [r13+0x100]", emit_g3_m(e, 4, X64_G3_IDIV, &m));
    CASE("imul eax,ecx",             emit_imul_rr(e, 4, X64_RAX, X64_RCX));
    CASE("imul ax,WORD PTR [r13+0x100]", emit_imul_rm(e, 2, X64_RAX, &m));
    CASE("imul eax,ecx,0xa",         emit_imul_rri(e, 4, X64_RAX, X64_RCX, 10));
    CASE("imul eax,ecx,0x1000",      emit_imul_rri(e, 4, X64_RAX, X64_RCX, 0x1000));
    CASE("imul cx,WORD PTR [r13+0x100],0xfffb", emit_imul_rmi(e, 2, X64_RCX, &m, 0xFFFB));

    /* shifts */
    CASE("shl eax,1",                emit_shift_ri(e, 4, X64_SH_SHL, X64_RAX, 1));
    CASE("shr ax,0x4",               emit_shift_ri(e, 2, X64_SH_SHR, X64_RAX, 4));
    CASE("sar ah,0x7",               emit_shift_ri(e, 1, X64_SH_SAR, X64_AH, 7));
    CASE("rol r8d,0x8",              emit_shift_ri(e, 4, X64_SH_ROL, X64_R8, 8));
    CASE("rcr BYTE PTR [r13+0x100],1", emit_shift_mi(e, 1, X64_SH_RCR, &m, 1));
    CASE("shl eax,cl",               emit_shift_rcl(e, 4, X64_SH_SHL, X64_RAX));
    CASE("ror WORD PTR [r13+0x100],cl", emit_shift_mcl(e, 2, X64_SH_ROR, &m));
    CASE("shld eax,ecx,0x4",         emit_shld_rri(e, 4, X64_RAX, X64_RCX, 4));
    CASE("shrd ax,cx,cl",            emit_shrd_rrcl(e, 2, X64_RAX, X64_RCX));
    CASE("shld DWORD PTR [r13+0x100],r8d,cl", emit_shld_mrcl(e, 4, &m, X64_R8));

    /* bits */
    CASE("bt eax,0x3",               emit_bt_ri(e, 4, X64_BT_BT, X64_RAX, 3));
    CASE("bts WORD PTR [r13+0x100],0xf", emit_bt_mi(e, 2, X64_BT_BTS, &m, 15));
    CASE("btr eax,ecx",              emit_bt_rr(e, 4, X64_BT_BTR, X64_RAX, X64_RCX));
    CASE("btc WORD PTR [r13+0x100],ax", emit_bt_mr(e, 2, X64_BT_BTC, &m, X64_RAX));
    CASE("bsf eax,ecx",              emit_bsf_rr(e, 4, X64_RAX, X64_RCX));
    CASE("bsr r8w,WORD PTR [r13+0x100]", emit_bsr_rm(e, 2, X64_R8, &m));
    CASE("xadd eax,ecx",             emit_xadd_rr(e, 4, X64_RAX, X64_RCX));
    CASE("xadd BYTE PTR [r13+0x100],cl", emit_xadd_mr(e, 1, &m, X64_CL));
    CASE("cmpxchg WORD PTR [r13+0x100],cx", emit_cmpxchg_mr(e, 2, &m, X64_RCX));

    /* flags, widths, strings */
    CASE("lahf",                     emit_lahf(e));
    CASE("sahf",                     emit_sahf(e));
    CASE("pushf",                    emit_pushfq(e));
    CASE("popf",                     emit_popfq(e));
    CASE("clc",                      emit_clc(e));
    CASE("std",                      emit_std(e));
    CASE("cbw",                      emit_cbw(e, 2));
    CASE("cwde",                     emit_cbw(e, 4));
    CASE("cwd",                      emit_cwd(e, 2));
    CASE("cdq",                      emit_cwd(e, 4));
    CASE("cqo",                      emit_cwd(e, 8));
    CASE("movs BYTE PTR es:[rdi],BYTE PTR ds:[rsi]", emit_movs(e, 1, 0));
    CASE("rep movs WORD PTR es:[rdi],WORD PTR ds:[rsi]", emit_movs(e, 2, 0xF3));
    CASE("rep stos DWORD PTR es:[rdi],eax", emit_stos(e, 4, 0xF3));
    CASE("repnz scas al,BYTE PTR es:[rdi]", emit_scas(e, 1, 0xF2));
    CASE("repz cmps WORD PTR ds:[rsi],WORD PTR es:[rdi]", emit_cmps(e, 2, 0xF3));
    CASE("lods eax,DWORD PTR ds:[rsi]", emit_lods(e, 4, 0));

    /* control flow: rel32 fields patched to the following instruction */
    { uint32_t at = emit_jcc_rel32(e, X64_CC_NE); emit_patch_rel32(e, at, emit_pos(e)); }
    CASE("jne @next", (void)0);
    { uint32_t at = emit_jmp_rel32(e); emit_patch_rel32(e, at, emit_pos(e)); }
    CASE("jmp @next", (void)0);
    { uint32_t at = emit_jcc_rel8(e, X64_CC_S); emit_patch_rel8(e, at, emit_pos(e)); }
    CASE("js @next", (void)0);
    { uint32_t at = emit_jrcxz_rel8(e); emit_patch_rel8(e, at, emit_pos(e)); }
    CASE("jrcxz @next", (void)0);
    CASE("jmp rax",                  emit_jmp_r(e, X64_RAX));
    CASE("jmp r9",                   emit_jmp_r(e, X64_R9));
    m = x64_mi(X64_R13, X64_RAX, 3, 8);
    CASE("jmp QWORD PTR [r13+rax*8+0x8]", emit_jmp_m(e, &m));
    CASE("call r10",                 emit_call_r(e, X64_R10));
    { uint32_t at = emit_call_rel32(e); emit_patch_rel32(e, at, emit_pos(e)); }
    CASE("call @next", (void)0);
    CASE("ret",                      emit_ret(e));
    CASE("push rax",                 emit_push_r(e, X64_RAX));
    CASE("push r12",                 emit_push_r(e, X64_R12));
    CASE("pop r15",                  emit_pop_r(e, X64_R15));
    CASE("push QWORD PTR [r13+rax*8+0x8]", emit_push_m(e, &m));
    CASE("int3",                     emit_int3(e));
    CASE("ud2",                      emit_ud2(e));

    /* BMI2 */
    CASE("rorx eax,ebx,0x8",         emit_rorx_rri(e, 4, X64_RAX, X64_RBX, 8));
    CASE("rorx r9,r8,0x20",          emit_rorx_rri(e, 8, X64_R9, X64_R8, 32));
    CASE("shlx eax,ebx,ecx",         emit_shlx(e, 4, X64_RAX, X64_RBX, X64_RCX));
    CASE("shrx r8d,r9d,r10d",        emit_shrx(e, 4, X64_R8, X64_R9, X64_R10));
    CASE("sarx rax,rbx,r15",         emit_sarx(e, 8, X64_RAX, X64_RBX, X64_R15));

    fclose(exp_out);
    FILE *f = fopen(argv[1], "wb");
    fwrite(buf, 1, E.offset, f);
    fclose(f);
    return 0;
}
