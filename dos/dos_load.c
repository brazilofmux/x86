/* dos_load.c — kernel data, the MCB arena, PSP construction, the .COM
 * and MZ loaders, and program termination.
 */
#include "dos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

dos_state dos;

/* ---- MCB arena ---------------------------------------------------------
 * MCB at segment s: 'M'/'Z' type, owner PSP (0 free), size in paragraphs,
 * 8-byte name at +8; the block is at s+1. */
static uint8_t  mcb_type(x86_cpu *c, uint16_t s)  { return pc_rd8(c, s, 0); }
static uint16_t mcb_owner(x86_cpu *c, uint16_t s) { return pc_rd16(c, s, 1); }
static uint16_t mcb_size(x86_cpu *c, uint16_t s)  { return pc_rd16(c, s, 3); }
static void mcb_set(x86_cpu *c, uint16_t s, uint8_t type, uint16_t owner, uint16_t size, const char *name) {
    pc_wr8(c, s, 0, type);
    pc_wr16(c, s, 1, owner);
    pc_wr16(c, s, 3, size);
    for (int i = 0; i < 8; i++) pc_wr8(c, s, (uint16_t)(8 + i), (uint8_t)(name && name[i] ? name[i] : 0));
}

/* Merge the free block at s with following free blocks. */
static void coalesce(x86_cpu *c, uint16_t s) {
    while (mcb_type(c, s) == 'M') {
        uint16_t next = (uint16_t)(s + 1 + mcb_size(c, s));
        if (mcb_owner(c, next) != 0) break;
        mcb_set(c, s, mcb_type(c, next), 0, (uint16_t)(mcb_size(c, s) + 1 + mcb_size(c, next)), NULL);
    }
}

uint16_t dos_mem_alloc(uint16_t paras, uint16_t owner, uint16_t *largest) {
    x86_cpu *c = dos.cpu;
    uint16_t best = 0, best_size = 0, max = 0;
    for (uint16_t s = DOS_FIRST_MCB;; s = (uint16_t)(s + 1 + mcb_size(c, s))) {
        if (mcb_owner(c, s) == 0) {
            coalesce(c, s);
            uint16_t sz = mcb_size(c, s);
            if (sz > max) max = sz;
            if (sz >= paras) {
                int take = !best;
                if (dos.alloc_strategy == 1 && best) take = sz < best_size;   /* best fit */
                else if (dos.alloc_strategy == 2) take = 1;                     /* last fit */
                if (take) { best = s; best_size = sz; }
                if (dos.alloc_strategy == 0) break;
            }
        }
        if (mcb_type(c, s) == 'Z') break;
    }
    if (largest) *largest = max;
    if (!best) return 0;
    if (dos.alloc_strategy == 2 && best_size > paras) {
        /* carve from the top of the block */
        uint16_t lead = (uint16_t)(best_size - paras - 1);
        uint8_t t = mcb_type(c, best);
        mcb_set(c, best, 'M', 0, lead, NULL);
        best = (uint16_t)(best + 1 + lead);
        mcb_set(c, best, t, owner, paras, NULL);
    } else if (best_size > paras) {
        uint8_t t = mcb_type(c, best);
        mcb_set(c, best, 'M', owner, paras, NULL);
        mcb_set(c, (uint16_t)(best + 1 + paras), t, 0, (uint16_t)(best_size - paras - 1), NULL);
    } else {
        pc_wr16(c, best, 1, owner);
    }
    return (uint16_t)(best + 1);
}

int dos_mem_free(uint16_t seg) {
    x86_cpu *c = dos.cpu;
    uint16_t s = (uint16_t)(seg - 1);
    if (seg == 0 || (mcb_type(c, s) != 'M' && mcb_type(c, s) != 'Z')) return DE_INVALID_BLOCK;
    pc_wr16(c, s, 1, 0);
    return DE_OK;
}

int dos_mem_resize(uint16_t seg, uint16_t paras, uint16_t *largest) {
    x86_cpu *c = dos.cpu;
    uint16_t s = (uint16_t)(seg - 1);
    if (seg == 0 || (mcb_type(c, s) != 'M' && mcb_type(c, s) != 'Z')) return DE_INVALID_BLOCK;
    uint16_t owner = mcb_owner(c, s);
    /* absorb following free space to find the maximum this block can grow to */
    uint16_t avail = mcb_size(c, s);
    uint8_t  end_type = mcb_type(c, s);
    uint16_t n = (uint16_t)(s + 1 + avail);
    while (mcb_type(c, s) == 'M' && mcb_owner(c, n) == 0) {
        coalesce(c, n);
        avail = (uint16_t)(avail + 1 + mcb_size(c, n));
        end_type = mcb_type(c, n);
        if (end_type == 'Z') break;
        n = (uint16_t)(n + 1 + mcb_size(c, n));
    }
    if (paras > avail) { if (largest) *largest = avail; return DE_NO_MEMORY; }
    if (paras == avail) mcb_set(c, s, end_type, owner, paras, NULL);
    else {
        mcb_set(c, s, 'M', owner, paras, NULL);
        mcb_set(c, (uint16_t)(s + 1 + paras), end_type, 0, (uint16_t)(avail - paras - 1), NULL);
    }
    return DE_OK;
}

void dos_mem_free_owner(uint16_t owner) {
    x86_cpu *c = dos.cpu;
    for (uint16_t s = DOS_FIRST_MCB;; s = (uint16_t)(s + 1 + mcb_size(c, s))) {
        if (mcb_owner(c, s) == owner) pc_wr16(c, s, 1, 0);
        if (mcb_type(c, s) == 'Z') break;
    }
}

/* ---- Init ---------------------------------------------------------------- */
void dos_init(x86_cpu *cpu, const char *root) {
    memset(&dos, 0, sizeof dos);
    dos.cpu = cpu;
    snprintf(dos.root, sizeof dos.root, "%s", root);
    size_t l = strlen(dos.root);
    while (l > 1 && dos.root[l - 1] == '/') dos.root[--l] = 0;
    strcpy(dos.cwd, "\\");
    dos.cur_drive = 2;
    for (int i = 0; i < DOS_MAX_HANDLES; i++) dos.handles[i].fd = -1;
    for (int i = 0; i < 5; i++) {
        dos.handles[i].fd = i < 3 ? i : -2;
        dos.handles[i].dev = i < 3 ? 1 : (i == 3 ? 2 : 3);
        strcpy(dos.handles[i].path, i < 3 ? "CON" : i == 3 ? "AUX" : "PRN");
    }
    /* Kernel data: InDOS flag at DOS_SEG:0, error mode at :1 (34h/5D06) */
    pc_wr8(cpu, DOS_SEG, 0, 0);
    pc_wr8(cpu, DOS_SEG, 1, 0);
    /* One free block spanning the arena. */
    mcb_set(cpu, DOS_FIRST_MCB, 'Z', 0, (uint16_t)(DOS_TOP_SEG - DOS_FIRST_MCB - 1), "SC");

    pc_set_service(0x20, dos_int20, HLE_RET_FLAGS);
    pc_set_service(0x21, dos_int21, HLE_RET_FLAGS);
    pc_set_service(0x27, dos_int20, HLE_RET_FLAGS);
    pc_set_service(0x29, dos_int29, HLE_RET_IRET);
    pc_set_service(0x2F, dos_int2f, HLE_RET_FLAGS);
}

/* ---- PSP ------------------------------------------------------------------ */
static void build_psp(x86_cpu *c, uint16_t psp, uint16_t env, uint16_t top, const char *args) {
    for (int i = 0; i < 256; i++) pc_wr8(c, psp, (uint16_t)i, 0);
    pc_wr8(c, psp, 0, 0xCD); pc_wr8(c, psp, 1, 0x20);
    pc_wr16(c, psp, 2, top);
    pc_wr8(c, psp, 5, 0x9A);                            /* far call to DOS: CALL F000:00C0? use INT 21 stub */
    pc_wr16(c, psp, 6, 0x00C0); pc_wr16(c, psp, 8, 0xF01D);   /* traditional 0xF01D:00C0 style, unused */
    /* INT 22/23/24 vectors: current handlers */
    for (int i = 0; i < 3; i++) {
        pc_wr16(c, psp, (uint16_t)(0x0A + i * 4), pc_rd16(c, 0, (uint16_t)((0x22 + i) * 4)));
        pc_wr16(c, psp, (uint16_t)(0x0C + i * 4), pc_rd16(c, 0, (uint16_t)((0x22 + i) * 4 + 2)));
    }
    pc_wr16(c, psp, 0x16, psp);                         /* parent: self (we are the shell) */
    for (int i = 0; i < 20; i++) pc_wr8(c, psp, (uint16_t)(0x18 + i), (uint8_t)(i < 5 ? (i == 3 ? 3 : i == 4 ? 4 : 1) : 0xFF));
    pc_wr16(c, psp, 0x2C, env);
    pc_wr16(c, psp, 0x32, 20);
    pc_wr16(c, psp, 0x34, 0x18); pc_wr16(c, psp, 0x36, psp);
    pc_wr16(c, psp, 0x38, 0xFFFF); pc_wr16(c, psp, 0x3A, 0xFFFF);
    pc_wr8(c, psp, 0x50, 0xCD); pc_wr8(c, psp, 0x51, 0x21); pc_wr8(c, psp, 0x52, 0xCB);
    /* FCBs from the first two args */
    const char *a = args;
    for (int f = 0; f < 2; f++) {
        uint16_t fcb = f ? 0x6C : 0x5C;
        while (*a == ' ') a++;
        pc_wr8(c, psp, fcb, 0);
        for (int i = 0; i < 11; i++) pc_wr8(c, psp, (uint16_t)(fcb + 1 + i), ' ');
        if (isalpha((unsigned char)a[0]) && a[1] == ':') { pc_wr8(c, psp, fcb, (uint8_t)(toupper((unsigned char)a[0]) - 'A' + 1)); a += 2; }
        int i = 0;
        while (*a && *a != ' ' && *a != '.' && i < 8) pc_wr8(c, psp, (uint16_t)(fcb + 1 + i++), (uint8_t)toupper((unsigned char)*a++));
        while (*a && *a != ' ' && *a != '.') a++;
        if (*a == '.') { a++; i = 0; while (*a && *a != ' ' && i < 3) pc_wr8(c, psp, (uint16_t)(fcb + 9 + i++), (uint8_t)toupper((unsigned char)*a++)); }
        while (*a && *a != ' ') a++;
    }
    size_t n = strlen(args); if (n > 126) n = 126;
    pc_wr8(c, psp, 0x80, (uint8_t)n);
    for (size_t i = 0; i < n; i++) pc_wr8(c, psp, (uint16_t)(0x81 + i), (uint8_t)args[i]);
    pc_wr8(c, psp, (uint16_t)(0x81 + n), 0x0D);
}

/* INT 21h/26h and 55h: a new PSP at seg, cloned from the current one,
 * with the current process as parent and the live 22h-24h vectors. */
void dos_make_child_psp(x86_cpu *c, uint16_t seg, uint16_t top) {
    for (int i = 0; i < 256; i++) pc_wr8(c, seg, (uint16_t)i, pc_rd8(c, dos.psp, (uint16_t)i));
    pc_wr16(c, seg, 2, top);
    for (int i = 0; i < 3; i++) {
        pc_wr16(c, seg, (uint16_t)(0x0A + i * 4), pc_rd16(c, 0, (uint16_t)((0x22 + i) * 4)));
        pc_wr16(c, seg, (uint16_t)(0x0C + i * 4), pc_rd16(c, 0, (uint16_t)((0x22 + i) * 4 + 2)));
    }
    pc_wr16(c, seg, 0x16, dos.psp);
}

/* ---- Loader --------------------------------------------------------------- */
static void set_seg(x86_cpu *c, int s, uint16_t v) { x86_load_seg(c, s, v); }

int dos_load_program(x86_cpu *c, const char *host_path, const char *dos_name, const char *args) {
    int fd = open(host_path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "dos: cannot open %s\n", host_path); return -1; }
    struct stat st; fstat(fd, &st);
    size_t size = (size_t)st.st_size;
    uint8_t *img = malloc(size + 16);
    if (read(fd, img, size) != (ssize_t)size) { close(fd); free(img); return -1; }
    close(fd);

    /* Environment block: PATH, COMSPEC, then the program's full name. */
    char env[512]; size_t el = 0;
    el += 1 + (size_t)sprintf(env + el, "PATH=C:\\");
    el += 1 + (size_t)sprintf(env + el, "COMSPEC=C:\\COMMAND.COM");
    el += 1 + (size_t)sprintf(env + el, "PROMPT=$P$G");
    env[el++] = 0;
    env[el++] = 1; env[el++] = 0;
    el += 1 + (size_t)sprintf(env + el, "C:\\%s", dos_name);
    uint16_t env_paras = (uint16_t)((el + 15) / 16);
    uint16_t env_seg = dos_mem_alloc(env_paras, 0, NULL);
    for (size_t i = 0; i < el; i++) pc_wr8(c, env_seg, (uint16_t)i, (uint8_t)env[i]);

    int is_exe = size >= 0x1C && img[0] == 'M' && img[1] == 'Z';
    if (!is_exe && size >= 0x1C && img[0] == 'Z' && img[1] == 'M') is_exe = 1;
    uint16_t largest = 0;
    dos_mem_alloc(0xFFFF, 0, &largest);              /* probe: how much is there */
    uint16_t psp = dos_mem_alloc(largest, 0, NULL);  /* programs get everything, like COMMAND.COM does */
    if (!psp) { free(img); return -1; }
    pc_wr16(c, env_seg - 1, 1, psp);                 /* env owned by the program */
    pc_wr16(c, psp - 1, 1, psp);
    for (int i = 0; i < 8; i++) pc_wr8(c, (uint16_t)(psp - 1), (uint16_t)(8 + i), (uint8_t)(i < (int)strcspn(dos_name, ".") && i < 8 ? toupper((unsigned char)dos_name[i]) : 0));
    uint16_t top = (uint16_t)(psp + largest);
    build_psp(c, psp, env_seg, top, args);
    dos.psp = psp;
    dos.root_psp = psp;
    dos.dta_seg = psp; dos.dta_off = 0x80;

    if (!is_exe) {
        uint16_t load = (uint16_t)(psp + 0x10);
        if (size > 0xFF00) size = 0xFF00;
        for (size_t i = 0; i < size; i++) pc_wr8(c, psp, (uint16_t)(0x100 + i), img[i]);
        (void)load;
        set_seg(c, S_CS, psp); set_seg(c, S_DS, psp); set_seg(c, S_ES, psp); set_seg(c, S_SS, psp);
        c->eip = 0x100;
        c->r[R_SP] = 0xFFFE;
        pc_wr16(c, psp, 0xFFFE, 0);                  /* RET to PSP:0 → INT 20h */
        c->r[R_AX] = 0;                              /* AL/AH: drive validity of FCB args */
        c->r[R_BX] = c->r[R_CX] = c->r[R_DX] = c->r[R_SI] = c->r[R_DI] = c->r[R_BP] = 0;
    } else {
        uint16_t hdr_paras = img[8] | (img[9] << 8);
        uint32_t hdr = (uint32_t)hdr_paras * 16;
        uint16_t pages = img[4] | (img[5] << 8), last = img[2] | (img[3] << 8);
        uint32_t image = (uint32_t)pages * 512 - (last ? 512 - last : 0);
        if (image > size) image = (uint32_t)size;
        uint32_t code_len = image > hdr ? image - hdr : 0;
        uint16_t min_alloc = img[0x0A] | (img[0x0B] << 8);
        uint16_t load = (uint16_t)(psp + 0x10);
        uint32_t need = (code_len + 15) / 16 + min_alloc + 0x10;
        if (need > largest) { fprintf(stderr, "dos: program needs %u paragraphs, %u free\n", need, largest); free(img); return -1; }
        uint32_t base = (uint32_t)load << 4;
        for (uint32_t i = 0; i < code_len; i++) x86_phys_wr8(c, base + i, img[hdr + i]);
        uint16_t nrel = img[6] | (img[7] << 8), relofs = img[0x18] | (img[0x19] << 8);
        for (uint16_t i = 0; i < nrel; i++) {
            uint32_t p = relofs + (uint32_t)i * 4;
            if (p + 4 > size) break;
            uint16_t off = img[p] | (img[p + 1] << 8), seg = img[p + 2] | (img[p + 3] << 8);
            uint32_t a = base + ((uint32_t)seg << 4) + off;
            uint16_t v = (uint16_t)(x86_phys_rd8(c, a) | (x86_phys_rd8(c, a + 1) << 8));
            v = (uint16_t)(v + load);
            x86_phys_wr8(c, a, (uint8_t)v); x86_phys_wr8(c, a + 1, (uint8_t)(v >> 8));
        }
        uint16_t ss = img[0x0E] | (img[0x0F] << 8), sp = img[0x10] | (img[0x11] << 8);
        uint16_t ip = img[0x14] | (img[0x15] << 8), cs = img[0x16] | (img[0x17] << 8);
        set_seg(c, S_CS, (uint16_t)(cs + load)); set_seg(c, S_SS, (uint16_t)(ss + load));
        set_seg(c, S_DS, psp); set_seg(c, S_ES, psp);
        c->eip = ip; c->r[R_SP] = sp;
        c->r[R_AX] = 0;
        c->r[R_BX] = c->r[R_CX] = c->r[R_DX] = c->r[R_SI] = c->r[R_DI] = c->r[R_BP] = 0;
    }
    free(img);
    c->halted = 0;
    return 0;
}

/* Terminate the current process. A child (its PSP is not the program we
 * loaded) returns to its parent the DOS way: vectors 22h-24h restored
 * from the child PSP, the parent's PSP made current, its SS:SP taken
 * from parent PSP:2Eh, and control passed to the INT 22h address. The
 * root program stops the machine. */
void dos_terminate(x86_cpu *c, int code, int keep_paras) {
    uint16_t psp = dos.psp;
    if (pc.debug) fprintf(stderr, "[dos] terminate code %d, PSP %04X (root %04X)\n", code, psp, dos.root_psp);
    for (int i = 5; i < DOS_MAX_HANDLES; i++)
        if (dos.handles[i].fd >= 0 && dos.handles[i].owner_psp == psp) { close(dos.handles[i].fd); dos.handles[i].fd = -1; }
    if (!keep_paras) dos_mem_free_owner(psp);
    dos.return_code = code;
    uint16_t parent = pc_rd16(c, psp, 0x16);
    if (psp == dos.root_psp || parent == psp || parent == 0) {
        dos.terminated = 1;
        pc.exit_requested = 1;
        pc.exit_code = code;
        c->halted = 1;
        return;
    }
    for (int i = 0; i < 3; i++) {
        pc_wr16(c, 0, (uint16_t)((0x22 + i) * 4), pc_rd16(c, psp, (uint16_t)(0x0A + i * 4)));
        pc_wr16(c, 0, (uint16_t)((0x22 + i) * 4 + 2), pc_rd16(c, psp, (uint16_t)(0x0C + i * 4)));
    }
    dos.psp = parent;
    uint16_t ip = pc_rd16(c, psp, 0x0A), cs = pc_rd16(c, psp, 0x0C);
    uint16_t sp = pc_rd16(c, parent, 0x2E), ss = pc_rd16(c, parent, 0x30);
    if (pc.debug) fprintf(stderr, "[dos]   child exit → parent %04X at %04X:%04X (IVT 22h = %04X:%04X), SS:SP %04X:%04X (was %04X:%04X)\n",
                          parent, cs, ip, pc_rd16(c, 0, 0x22 * 4 + 2), pc_rd16(c, 0, 0x22 * 4), ss, sp, c->seg[S_SS].sel, (uint16_t)c->r[R_SP]);
    if (ss || sp) { x86_load_seg(c, S_SS, ss); c->r[R_SP] = sp; }
    x86_load_seg(c, S_CS, cs);
    c->eip = ip;
    c->eflags |= X86_IF;
    pc.returned = 1;                 /* we transferred control ourselves */
}
