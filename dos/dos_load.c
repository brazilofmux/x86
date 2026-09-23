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
    dos.cur_drive = 2;
    dos_mount(2, root);
    for (int i = 0; i < DOS_MAX_HANDLES; i++) dos.handles[i].fd = -1;
    for (int i = 0; i < 5; i++) {
        dos.handles[i].fd = -2;
        dos.handles[i].refs = 1 << 20;                /* the standard devices never close */
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
    pc_set_trap(0x31, dpmi_int31, HLE_RET_FLAGS);          /* protected mode only: reached through the IDT */
    pc_set_trap(PC_HLE_DPMI_ENTRY, dpmi_mode_switch, HLE_RET_FLAGS);
    pc_set_trap(PC_HLE_DPMI_RMRET, dpmi_rm_return, HLE_RET_FLAGS);
    pc_set_trap(PC_HLE_DPMI_CB,    dpmi_callback,  HLE_RET_FLAGS);
    pc_set_trap(PC_HLE_DPMI_CBRET, dpmi_cb_return, HLE_RET_FLAGS);
    pc_set_trap(PC_HLE_DPMI_EXCRET, dpmi_exc_return, HLE_RET_FLAGS);
    pc_set_trap(PC_HLE_DPMI_RAW,  dpmi_raw_switch, HLE_RET_FLAGS);
    pc_set_trap(PC_HLE_DPMI_SAVE, dpmi_save_state, HLE_RET_FLAGS);
    pc.pm_exception = dpmi_pm_exception;
    dpmi_init(cpu);
}

/* Mount host directory/directories on a drive. A colon-separated list
 * is a diskette sequence: the first is in the drive, dos_swap_disk
 * moves to the next. */
int dos_mount(int drive, const char *dirs) {
    if (drive < 0 || drive >= 26) return -1;
    dos_drive *d = &dos.drives[drive];
    memset(d, 0, sizeof *d);
    char *list = strdup(dirs);
    for (char *tok = strtok(list, ":"); tok; tok = strtok(NULL, ":")) {
        d->disks = realloc(d->disks, sizeof(char *) * (size_t)(d->ndisks + 1));
        char *abs = realloc(NULL, DOS_MAX_PATH);
        if (!realpath(tok, abs)) { fprintf(stderr, "dos: %s: no such directory\n", tok); free(list); return -1; }
        d->disks[d->ndisks++] = abs;
    }
    free(list);
    if (!d->ndisks) return -1;
    snprintf(d->root, sizeof d->root, "%s", d->disks[0]);
    strcpy(d->cwd, "\\");
    return 0;
}

int dos_swap_disk(void) {
    int swapped = 0;
    for (int i = 0; i < 2; i++) {
        dos_drive *d = &dos.drives[i];
        if (d->ndisks < 2) continue;
        d->cur_disk = (d->cur_disk + 1) % d->ndisks;
        snprintf(d->root, sizeof d->root, "%s", d->disks[d->cur_disk]);
        strcpy(d->cwd, "\\");
        fprintf(stderr, "dos: drive %c: now %s\n", 'A' + i, d->root);
        swapped = 1;
    }
    return swapped;
}

/* ---- PSP ------------------------------------------------------------------ */
static void build_psp(x86_cpu *c, uint16_t psp, uint16_t parent, uint16_t env, uint16_t top, const char *args) {
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
    pc_wr16(c, psp, 0x16, parent);
    /* Job file table: inherited from the parent, or the five standard handles */
    for (int i = 0; i < 20; i++)
        pc_wr8(c, psp, (uint16_t)(0x18 + i), parent != psp ? pc_rd8(c, parent, (uint16_t)(0x18 + i))
                                                          : (uint8_t)(i < 5 ? (i == 3 ? 3 : i == 4 ? 4 : 1) : 0xFF));
    pc_wr16(c, psp, 0x2C, env);
    pc_wr16(c, psp, 0x32, 20);
    pc_wr16(c, psp, 0x34, 0x18); pc_wr16(c, psp, 0x36, psp);
    if (parent != psp) dos_jft_inherit(c, psp);
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

typedef struct {
    int      is_exe;
    uint32_t hdr, code_len;              /* header bytes to skip, image bytes to load */
    uint16_t min_alloc, max_alloc;       /* paragraphs beyond the image */
    uint16_t ss, sp, cs, ip;             /* relative to the load segment */
    uint16_t nrel, relofs;
} exe_info;

static uint8_t *read_image(const char *host_path, size_t *size) {
    int fd = open(host_path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st; fstat(fd, &st);
    *size = (size_t)st.st_size;
    uint8_t *img = malloc(*size + 16);
    if (read(fd, img, *size) != (ssize_t)*size) { close(fd); free(img); return NULL; }
    close(fd);
    return img;
}

static void parse_image(const uint8_t *img, size_t size, exe_info *e) {
    memset(e, 0, sizeof *e);
    e->is_exe = size >= 0x1C && ((img[0] == 'M' && img[1] == 'Z') || (img[0] == 'Z' && img[1] == 'M'));
    if (!e->is_exe) {
        e->code_len = (uint32_t)(size > 0xFF00 ? 0xFF00 : size);
        e->max_alloc = 0xFFFF;
        return;
    }
    uint16_t hdr_paras = img[8] | (img[9] << 8);
    e->hdr = (uint32_t)hdr_paras * 16;
    uint16_t pages = img[4] | (img[5] << 8), last = img[2] | (img[3] << 8);
    uint32_t image = (uint32_t)pages * 512 - (last ? 512 - last : 0);
    if (image > size) image = (uint32_t)size;
    e->code_len = image > e->hdr ? image - e->hdr : 0;
    e->min_alloc = img[0x0A] | (img[0x0B] << 8);
    e->max_alloc = img[0x0C] | (img[0x0D] << 8);
    e->ss = img[0x0E] | (img[0x0F] << 8); e->sp = img[0x10] | (img[0x11] << 8);
    e->ip = img[0x14] | (img[0x15] << 8); e->cs = img[0x16] | (img[0x17] << 8);
    e->nrel = img[6] | (img[7] << 8); e->relofs = img[0x18] | (img[0x19] << 8);
}

/* Copy the image to segment `load` and relocate it by `reloc` (the same
 * as `load` for a program, the caller's factor for an overlay). */
static void place_image(x86_cpu *c, const uint8_t *img, size_t size, const exe_info *e, uint16_t load, uint16_t reloc) {
    uint32_t base = (uint32_t)load << 4;
    for (uint32_t i = 0; i < e->code_len; i++) x86_phys_wr8(c, base + i, img[e->hdr + i]);
    for (uint16_t i = 0; i < e->nrel; i++) {
        uint32_t p = e->relofs + (uint32_t)i * 4;
        if (p + 4 > size) break;
        uint16_t off = img[p] | (img[p + 1] << 8), seg = img[p + 2] | (img[p + 3] << 8);
        uint32_t a = base + ((uint32_t)seg << 4) + off;
        uint16_t v = (uint16_t)(x86_phys_rd8(c, a) | (x86_phys_rd8(c, a + 1) << 8));
        v = (uint16_t)(v + reloc);
        x86_phys_wr8(c, a, (uint8_t)v); x86_phys_wr8(c, a + 1, (uint8_t)(v >> 8));
    }
}

/* Environment for a new process: either a fresh one (the root program)
 * or a copy of the parent's strings, followed as DOS does by the count
 * word 1 and the program's full name. Returns the segment, 0 = no memory. */
static uint16_t make_env(x86_cpu *c, uint16_t parent_env, const char *dos_full) {
    char env[2048]; size_t el = 0;
    if (parent_env) {
        for (uint32_t i = 0;; i++) {
            uint8_t b = pc_rd8(c, parent_env, (uint16_t)i);
            if (el + 4 >= sizeof env) break;
            env[el++] = (char)b;
            if (b == 0 && (i == 0 || pc_rd8(c, parent_env, (uint16_t)(i - 1)) == 0)) break;   /* double NUL */
        }
        if (el == 1) env[el++] = 0;                                   /* empty parent env: still "\0\0" */
    } else {
        el += 1 + (size_t)sprintf(env + el, "PATH=C:\\");
        el += 1 + (size_t)sprintf(env + el, "COMSPEC=C:\\COMMAND.COM");
        el += 1 + (size_t)sprintf(env + el, "PROMPT=$P$G");
        env[el++] = 0;
    }
    env[el++] = 1; env[el++] = 0;
    el += 1 + (size_t)snprintf(env + el, sizeof env - el, "%s", dos_full);
    uint16_t paras = (uint16_t)((el + 15) / 16);
    uint16_t seg = dos_mem_alloc(paras, DOS_OWNER_SYS, NULL);
    if (!seg) return 0;
    for (size_t i = 0; i < el; i++) pc_wr8(c, seg, (uint16_t)i, (uint8_t)env[i]);
    return seg;
}

/* Create a process from an image: PSP, memory, environment ownership,
 * registers. `parent` is the creating process (the PSP itself for the
 * root program). On success the new PSP is current; with `run` clear
 * the entry state is returned instead of being loaded into the CPU. */
static int create_process(x86_cpu *c, const uint8_t *img, size_t size, const char *dos_full, const char *tail,
                          uint16_t env_seg, uint16_t parent, int run,
                          uint16_t *o_ss, uint16_t *o_sp, uint16_t *o_cs, uint16_t *o_ip) {
    exe_info e; parse_image(img, size, &e);
    uint16_t largest = 0;
    dos_mem_alloc(0xFFFF, 0, &largest);              /* probe: how much is there */
    uint32_t code_paras = (e.code_len + 15) / 16;
    uint32_t need = 0x10 + code_paras + (e.is_exe ? e.min_alloc : 1);
    uint32_t want = 0x10 + code_paras + (e.is_exe ? e.max_alloc : 0xFFFF);
    if (want > 0xFFFF) want = 0xFFFF;
    if (want < need) want = need;
    if (need > largest) { if (pc.debug) fprintf(stderr, "[dos] load: need %u paragraphs, %u free\n", need, largest); return DE_NO_MEMORY; }
    uint16_t alloc = (uint16_t)(want > largest ? largest : want);
    uint16_t psp = dos_mem_alloc(alloc, 0, NULL);
    if (!psp) return DE_NO_MEMORY;
    pc_wr16(c, psp - 1, 1, psp);
    if (env_seg && mcb_owner(c, (uint16_t)(env_seg - 1)) == DOS_OWNER_SYS) pc_wr16(c, env_seg - 1, 1, psp);   /* our copy: the child owns it */
    const char *base = strrchr(dos_full, '\\'); base = base ? base + 1 : dos_full;
    for (int i = 0; i < 8; i++) pc_wr8(c, (uint16_t)(psp - 1), (uint16_t)(8 + i), (uint8_t)(i < (int)strcspn(base, ".") ? toupper((unsigned char)base[i]) : 0));
    uint16_t top = (uint16_t)(psp + alloc);
    build_psp(c, psp, parent == 0 ? psp : parent, env_seg, top, tail);
    dos.psp = psp;
    dos.dta_seg = psp; dos.dta_off = 0x80;

    uint16_t load = (uint16_t)(psp + 0x10);
    uint16_t ss, sp, cs, ip;
    if (!e.is_exe) {
        place_image(c, img, size, &e, load, load);
        ss = cs = psp; ip = 0x100; sp = 0xFFFE;
        if (alloc < 0x1000) sp = (uint16_t)((alloc << 4) - 2);      /* small block: stack at its top */
        pc_wr16(c, psp, sp, 0);                      /* RET to PSP:0 → INT 20h */
    } else {
        place_image(c, img, size, &e, load, load);
        cs = (uint16_t)(e.cs + load); ip = e.ip;
        ss = (uint16_t)(e.ss + load); sp = e.sp;
    }
    if (o_ss) { *o_ss = ss; *o_sp = sp; *o_cs = cs; *o_ip = ip; }
    if (run) {
        set_seg(c, S_CS, cs); set_seg(c, S_SS, ss);
        set_seg(c, S_DS, psp); set_seg(c, S_ES, psp);
        c->eip = ip; c->r[R_SP] = sp;
        c->r[R_AX] = 0;                              /* AL/AH: drive validity of FCB args */
        c->r[R_BX] = c->r[R_CX] = c->r[R_DX] = c->r[R_SI] = c->r[R_DI] = c->r[R_BP] = 0;
        c->eflags |= X86_IF;
        c->halted = 0;
    }
    return DE_OK;
}

int dos_load_program(x86_cpu *c, const char *host_path, const char *dos_name, const char *args) {
    size_t size;
    uint8_t *img = read_image(host_path, &size);
    if (!img) { fprintf(stderr, "dos: cannot open %s\n", host_path); return -1; }
    char full[DOS_MAX_PATH];
    snprintf(full, sizeof full, "%c:%s%s", 'A' + dos.cur_drive, dos_name[0] == '\\' ? "" : "\\", dos_name);
    uint16_t env = make_env(c, 0, full);
    int e = create_process(c, img, size, full, args, env, 0, 1, NULL, NULL, NULL, NULL);
    free(img);
    if (e) { fprintf(stderr, "dos: cannot load %s (error %d)\n", dos_name, e); return -1; }
    dos.root_psp = dos.psp;
    return 0;
}

/* INT 21h/4Bh. The caller's INT frame is still on its stack and its
 * SS:SP already sit in its PSP:2Eh (the dispatcher stores them on every
 * call); INT 22h is pointed at the return address so the child's exit
 * comes back right after the INT 21h, frame popped, CF clear. */
int dos_exec(x86_cpu *c, const char *dos_path, int mode, uint16_t pb_seg, uint16_t pb_off) {
    char host[DOS_MAX_PATH], full[DOS_MAX_PATH];
    int exists, is_dir;
    int e = dos_resolve(dos_path, host, sizeof host, &exists, &is_dir);
    if (e) return e;
    if (!exists || is_dir) return DE_FILE_NOT_FOUND;
    dos_fullname(dos_path, full, sizeof full);
    size_t size;
    uint8_t *img = read_image(host, &size);
    if (!img) return DE_ACCESS_DENIED;

    if (mode == 3) {                                  /* overlay: raw image at a segment, no PSP */
        uint16_t load = pc_rd16(c, pb_seg, pb_off), reloc = pc_rd16(c, pb_seg, (uint16_t)(pb_off + 2));
        exe_info ei; parse_image(img, size, &ei);
        if (pc.debug) fprintf(stderr, "[dos] EXEC overlay %s at %04X (reloc %04X)\n", full, load, reloc);
        place_image(c, img, size, &ei, load, reloc);
        free(img);
        return DE_OK;
    }
    if (mode != 0 && mode != 1) { free(img); return DE_INVALID_FN; }

    uint16_t env_seg = pc_rd16(c, pb_seg, pb_off);
    uint16_t tail_off = pc_rd16(c, pb_seg, (uint16_t)(pb_off + 2)), tail_seg = pc_rd16(c, pb_seg, (uint16_t)(pb_off + 4));
    char tail[128]; int n = pc_rd8(c, tail_seg, tail_off);
    if (n > 126) n = 126;
    for (int i = 0; i < n; i++) tail[i] = (char)pc_rd8(c, tail_seg, (uint16_t)(tail_off + 1 + i));
    tail[n] = 0;
    if (pc.debug) fprintf(stderr, "[dos] EXEC %s [%s] mode %d env %04X from PSP %04X\n", full, tail, mode, env_seg, dos.psp);

    if (env_seg == 0) {
        env_seg = make_env(c, pc_rd16(c, dos.psp, 0x2C), full);
        if (!env_seg) { free(img); return DE_NO_MEMORY; }
    }
    /* INT 22h ← the caller's return address (top of its INT frame) */
    uint16_t sp = (uint16_t)c->r[R_SP], ss = c->seg[S_SS].sel;
    uint16_t parent = dos.psp;
    pc_wr16(c, 0, 0x22 * 4, pc_rd16(c, ss, sp));
    pc_wr16(c, 0, 0x22 * 4 + 2, pc_rd16(c, ss, (uint16_t)(sp + 2)));

    /* the parent's registers, restored when the child exits (DOS 3+ keeps all but AX) */
    if (dos.n_exec < 16) {
        dos.exec_save[dos.n_exec].ds = c->seg[S_DS].sel; dos.exec_save[dos.n_exec].es = c->seg[S_ES].sel;
        memcpy(dos.exec_save[dos.n_exec].r, c->r, sizeof dos.exec_save[dos.n_exec].r);
    }
    uint16_t o_ss, o_sp, o_cs, o_ip;
    e = create_process(c, img, size, full, tail, env_seg, parent, mode == 0, &o_ss, &o_sp, &o_cs, &o_ip);
    free(img);
    if (e) {
        if (mcb_owner(c, (uint16_t)(env_seg - 1)) == DOS_OWNER_SYS) dos_mem_free(env_seg);   /* our copy, never handed over */
        pc_wr16(c, 0, 0x22 * 4, pc_rd16(c, parent, 0x0A));                  /* INT 22h back */
        pc_wr16(c, 0, 0x22 * 4 + 2, pc_rd16(c, parent, 0x0C));
        return e;
    }
    uint16_t psp = dos.psp;
    if (dos.n_exec < 16) dos.exec_save[dos.n_exec++].psp = psp;
    /* FCBs from the parameter block (pointers may be null) */
    for (int f = 0; f < 2; f++) {
        uint16_t po = pc_rd16(c, pb_seg, (uint16_t)(pb_off + 6 + f * 4)), ps = pc_rd16(c, pb_seg, (uint16_t)(pb_off + 8 + f * 4));
        if (!po && !ps) continue;
        for (int i = 0; i < 16; i++) pc_wr8(c, psp, (uint16_t)((f ? 0x6C : 0x5C) + i), pc_rd8(c, ps, (uint16_t)(po + i)));
    }
    if (mode == 1) {
        pc_wr16(c, pb_seg, (uint16_t)(pb_off + 0x0E), o_sp); pc_wr16(c, pb_seg, (uint16_t)(pb_off + 0x10), o_ss);
        pc_wr16(c, pb_seg, (uint16_t)(pb_off + 0x12), o_ip); pc_wr16(c, pb_seg, (uint16_t)(pb_off + 0x14), o_cs);
        /* the entry AX (FCB drive validity) goes on the child's stack, as DOS does */
        pc_wr16(c, o_ss, (uint16_t)(o_sp - 2), 0);
        pc_wr16(c, pb_seg, (uint16_t)(pb_off + 0x0E), (uint16_t)(o_sp - 2));
        return DE_OK;
    }
    pc.returned = 1;                                  /* control is now the child's */
    return DE_OK;
}

/* Terminate the current process. A child (its PSP is not the program we
 * loaded) returns to its parent the DOS way: vectors 22h-24h restored
 * from the child PSP, the parent's PSP made current, its SS:SP taken
 * from parent PSP:2Eh, and control passed to the INT 22h address. The
 * root program stops the machine. */
void dos_terminate(x86_cpu *c, int code, int keep_paras) {
    uint16_t psp = dos.psp;
    if (pc.debug) fprintf(stderr, "[dos] terminate code %d, PSP %04X (root %04X)\n", code, psp, dos.root_psp);
    dos_jft_release(c, psp);
    if (!keep_paras) dos_mem_free_owner(psp);
    else dos_mem_resize(psp, keep_paras, NULL);   /* TSR: keep this much of the program block */
    dos.return_code = code | (keep_paras ? 0x300 : 0);   /* 4Dh: AH = termination type */
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
    if (dos.n_exec && dos.exec_save[dos.n_exec - 1].psp == psp) {
        /* Back from INT 21h/4Bh: the parent's INT frame is at its saved
         * SS:SP; pop it (RETF 2 style), restore its registers, CF clear. */
        dos.n_exec--;
        uint32_t ax = c->r[R_AX], sp2 = c->r[R_SP];
        memcpy(c->r, dos.exec_save[dos.n_exec].r, sizeof c->r);
        c->r[R_AX] = ax; c->r[R_SP] = sp2;
        x86_load_seg(c, S_DS, dos.exec_save[dos.n_exec].ds);
        x86_load_seg(c, S_ES, dos.exec_save[dos.n_exec].es);
        c->eflags &= ~X86_CF;
        pc_hle_return(c, HLE_RET_FLAGS);
        c->eflags &= ~X86_CF;
    }
    pc.returned = 1;                 /* we transferred control ourselves */
}
