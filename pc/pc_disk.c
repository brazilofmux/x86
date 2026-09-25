/* pc_disk.c — the machine's disks: diskette and fixed-disk images behind
 * INT 13h
 *
 * A booted DOS (FreeDOS, MS-DOS) reaches its disks only through the BIOS,
 * so this is the whole storage stack below it: up to two diskette drives
 * (00h, 01h) and two fixed disks (80h, 81h), each a raw image file mapped
 * MAP_SHARED — a write lands in the file with no flushing of our own, and
 * a -ro image is mapped private so the guest may write but the file never
 * changes. (The HLE DOS's host-directory drives are another thing
 * entirely; see dos/.)
 *
 * Geometry: a diskette's comes from its size (160K to 2.88M); a fixed
 * disk's from the partition table when it has one — the largest ending
 * head and sector of any entry, as DOS's FDISK wrote them — else 16 heads
 * of 63 sectors. Cylinders are whatever the size gives, capped at 1024
 * for CHS; the LBA extensions (41h-48h) reach the rest.
 *
 * A diskette drive may hold a sequence (-fda IMG1:IMG2:...): ESC-+ puts
 * the next image in and raises the change line, as swapping the disk in
 * the drive would, so DOS rereads it.
 *
 * The parameter tables sit where the AT BIOS keeps them: the diskette
 * parameter table at F000:EFC7 (INT 1Eh), the fixed-disk tables (INT
 * 41h, 46h) in the ROM segment below the traps' offsets.
 */
#include "pc.h"
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pc_diskbios.h"

#define DPT_OFF   0xEFC7                     /* diskette parameter table, as on the AT */
#define FDPT_OFF0 0xE800                     /* fixed-disk parameter tables (INT 41h, 46h), clear of the */
#define FDPT_OFF1 0xE810                     /* 8x8 font at E000-E7FF (at E3C0 they overwrote its "x" to "z") */

typedef struct {
    uint8_t *data;
    size_t size;
    int cyls, heads, spt;
    int type;                                /* diskette: CMOS drive type 1-5 */
    int changed;                             /* diskette: change line, for INT 13h AH=16h */
    const char *path;
    char **seq; int nseq, cur;               /* diskette: the images ESC-+ steps through */
    int readonly;
} disk;

static disk fd[2], hd[2];
static int nfd, nhd;

static const struct { size_t kb; int cyls, heads, spt, type; } floppy_geom[] = {
    {  160, 40, 1,  8, 1 }, {  180, 40, 1,  9, 1 }, {  320, 40, 2,  8, 1 },
    {  360, 40, 2,  9, 1 }, {  720, 80, 2,  9, 3 }, { 1200, 80, 2, 15, 2 },
    { 1440, 80, 2, 18, 4 }, { 2880, 80, 2, 36, 5 },
};

static void hd_geometry(disk *d) {
    int heads = 0, spt = 0;
    const uint8_t *mbr = d->data;
    if (d->size >= 512 && mbr[510] == 0x55 && mbr[511] == 0xAA) {
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = mbr + 446 + i * 16;
            if (!e[4]) continue;                             /* unused entry */
            if (e[5] + 1 > heads) heads = e[5] + 1;          /* ending head */
            if ((e[6] & 0x3F) > spt) spt = e[6] & 0x3F;      /* ending sector */
        }
    }
    if (heads < 1 || spt < 1) { heads = 16; spt = 63; }
    d->heads = heads; d->spt = spt;
    long c = (long)(d->size / 512 / (size_t)(heads * spt));
    d->cyls = c > 1024 ? 1024 : c < 1 ? 1 : (int)c;
}

static int map_image(disk *d, int drive, const char *path, int readonly) {
    int fdn = open(path, readonly ? O_RDONLY : O_RDWR);
    if (fdn < 0) { perror(path); return -1; }
    struct stat st;
    if (fstat(fdn, &st) < 0 || st.st_size < 512) { fprintf(stderr, "%s: not a disk image\n", path); close(fdn); return -1; }
    size_t size = (size_t)st.st_size;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, readonly ? MAP_PRIVATE : MAP_SHARED, fdn, 0);
    close(fdn);
    if (p == MAP_FAILED) { perror(path); return -1; }
    if (d->data) munmap(d->data, d->size);
    d->data = p; d->size = size; d->path = path; d->readonly = readonly;
    if (drive & 0x80) {
        hd_geometry(d);
        if ((drive & 1) + 1 > nhd) nhd = (drive & 1) + 1;
    } else {
        d->heads = 2; d->spt = 18; d->cyls = 80; d->type = 4;        /* odd sizes (test images): as 1.44M */
        for (size_t i = 0; i < sizeof floppy_geom / sizeof floppy_geom[0]; i++)
            if (floppy_geom[i].kb * 1024 == size) {
                d->cyls = floppy_geom[i].cyls; d->heads = floppy_geom[i].heads;
                d->spt = floppy_geom[i].spt; d->type = floppy_geom[i].type;
            }
        if ((drive & 1) + 1 > nfd) nfd = (drive & 1) + 1;
    }
    return 0;
}

/* Attach IMG as drive (00h, 01h, 80h or 81h); a diskette may be a
 * colon-separated sequence, the first image in the drive. */
int pc_disk_attach(int drive, const char *path, int readonly) {
    disk *d = drive & 0x80 ? &hd[drive & 1] : &fd[drive & 1];
    memset(d, 0, sizeof *d);
    if (!(drive & 0x80) && strstr(path, HOST_PATH_LIST_SEP)) {
        char *all = strdup(path);
        for (char *t = strtok(all, HOST_PATH_LIST_SEP); t; t = strtok(NULL, HOST_PATH_LIST_SEP)) {
            d->seq = realloc(d->seq, (size_t)(d->nseq + 1) * sizeof *d->seq);
            d->seq[d->nseq++] = t;
        }
        if (!d->nseq) return -1;
        path = d->seq[0];
    }
    char **seq = d->seq; int nseq = d->nseq;
    if (map_image(d, drive, path, readonly) < 0) return -1;
    d->seq = seq; d->nseq = nseq;
    return 0;
}

/* ESC-+: the next image of A:'s sequence (B:'s when A: has none). */
int pc_disk_swap(void) {
    for (int k = 0; k < 2; k++) {
        disk *d = &fd[k];
        if (d->nseq < 2) continue;
        char **seq = d->seq; int nseq = d->nseq, cur = (d->cur + 1) % d->nseq;
        if (map_image(d, k, seq[cur], d->readonly) < 0) return 0;
        d->seq = seq; d->nseq = nseq; d->cur = cur;
        d->changed = 1;
        fprintf(stderr, "[%c: %s]\n", 'A' + k, seq[cur]);
        return 1;
    }
    return 0;
}

int pc_disk_floppy_type(int drive) { return fd[drive & 1].data ? fd[drive & 1].type : 0; }

/* A fixed disk as the IDE controller sees it (pc_ide.c): the same mapped
 * image INT 13h uses, so the two paths never disagree about a sector.
 * NULL if there is no such disk. */
uint8_t *pc_disk_hd(int unit, size_t *size, int *cyls, int *heads, int *spt) {
    disk *d = &hd[unit & 1];
    if (!d->data) return NULL;
    *size = d->size; *cyls = d->cyls; *heads = d->heads; *spt = d->spt;
    return d->data;
}

int pc_disk_present(int drive) {
    disk *d = drive & 0x80 ? &hd[drive & 1] : &fd[drive & 1];
    return (drive & 0x7E) == 0 && d->data != NULL;
}

/* Sector 0 of DRIVE to 0000:7C00; 0 if it is not a boot sector. */
int pc_disk_boot(x86_cpu *c, int drive) {
    disk *d = drive & 0x80 ? &hd[drive & 1] : &fd[drive & 1];
    if (!d->data) return 0;
    for (int k = 0; k < 512; k++) x86_phys_wr8(c, 0x7C00 + (uint32_t)k, d->data[k]);
    return 1;
}

static disk *lookup(int dl) {
    if ((dl & 0x7E) != 0) return NULL;
    disk *d = dl & 0x80 ? &hd[dl & 1] : &fd[dl & 1];
    return d->data ? d : NULL;
}

static void status(x86_cpu *c, int dl, int ah) {
    pc_wr8(c, PC_BDA_SEG, dl & 0x80 ? 0x74 : 0x41, (uint8_t)ah);
    x86_set_r8(c, R_AH, (uint8_t)ah);
    if (ah) c->eflags |= X86_CF; else c->eflags &= ~X86_CF;
}

/* Move COUNT sectors at LBA between the image and linear address BUF. */
static int transfer(x86_cpu *c, disk *d, uint64_t lba, int count, uint32_t buf, int write) {
    if (lba + (uint64_t)count > d->size / 512) return 0x04;          /* sector not found */
    uint8_t *p = d->data + lba * 512;
    size_t n = (size_t)count * 512;
    for (size_t b = 0; b < n; b++) {                     /* phys writes keep the SMC bitmap told */
        if (write) p[b] = x86_phys_rd8(c, buf + (uint32_t)b);
        else x86_phys_wr8(c, buf + (uint32_t)b, p[b]);
    }
    return 0;
}

static void int13(x86_cpu *c, int vector) {
    (void)vector;
    int ah = x86_get_r8(c, R_AH), dl = x86_get_r8(c, R_DL);
    disk *d = lookup(dl);
    pc_kbd_busy();
    if (pc.debug > 1)
        fprintf(stderr, "[disk] INT 13h AX=%04X CX=%04X DX=%04X ES:BX=%04X:%04X\n", x86_get_r16(c, R_AX),
                x86_get_r16(c, R_CX), x86_get_r16(c, R_DX), c->seg[S_ES].sel, x86_get_r16(c, R_BX));
    switch (ah) {
    case 0x00: case 0x0D: case 0x10: case 0x11: case 0x0C:   /* reset, ready, recalibrate, seek */
        status(c, dl, d ? 0 : (dl & 0x80 ? 0x01 : 0x80));
        return;
    case 0x01:
        x86_set_r8(c, R_AL, pc_rd8(c, PC_BDA_SEG, dl & 0x80 ? 0x74 : 0x41));
        status(c, dl, 0);
        return;
    case 0x02: case 0x03: case 0x04: {                       /* read, write, verify (CHS) */
        if (!d) { status(c, dl, dl & 0x80 ? 0x01 : 0x80); return; }
        int count = x86_get_r8(c, R_AL);
        int cl = x86_get_r8(c, R_CL), ch = x86_get_r8(c, R_CH), dh = x86_get_r8(c, R_DH);
        int cyl = ch | ((cl & 0xC0) << 2), sec = cl & 0x3F;
        if (!(dl & 0x80)) cyl = ch;
        if (sec < 1 || sec > d->spt || dh >= d->heads || count == 0) { x86_set_r8(c, R_AL, 0); status(c, dl, 0x04); return; }
        uint64_t lba = ((uint64_t)cyl * (uint64_t)d->heads + (uint64_t)dh) * (uint64_t)d->spt + (uint64_t)(sec - 1);
        uint32_t buf = ((uint32_t)c->seg[S_ES].sel << 4) + x86_get_r16(c, R_BX);
        int err = ah == 0x04 ? (lba + (uint64_t)count > d->size / 512 ? 0x04 : 0)
                             : transfer(c, d, lba, count, buf, ah == 0x03);
        x86_set_r8(c, R_AL, err ? 0 : (uint8_t)count);
        if (!err) d->changed = 0;
        status(c, dl, err);
        return;
    }
    case 0x05: {                                             /* format track: diskettes fill with F6h */
        if (!d) { status(c, dl, 0x80); return; }
        if (!(dl & 0x80)) {
            int ch = x86_get_r8(c, R_CH), dh = x86_get_r8(c, R_DH);
            uint64_t lba = ((uint64_t)ch * (uint64_t)d->heads + (uint64_t)dh) * (uint64_t)d->spt;
            if ((lba + (uint64_t)d->spt) * 512 <= d->size) memset(d->data + lba * 512, 0xF6, (size_t)d->spt * 512);
        }
        status(c, dl, 0);
        return;
    }
    case 0x08: {                                             /* drive parameters */
        if (!d) {
            /* No such drive — but DL still says how many there are: IO.SYS
             * takes it as the count of fixed disks whatever CF says, and
             * with 80h left in DL built 128 of them (MS-DOS 6.22 then
             * walked its drive chain off into garbage). */
            x86_set_r8(c, R_DL, (uint8_t)(dl & 0x80 ? nhd : nfd));
            status(c, dl, dl & 0x80 ? 0x07 : 0x01);
            return;
        }
        int mc = d->cyls - 1;
        x86_set_r8(c, R_CH, (uint8_t)mc);
        x86_set_r8(c, R_CL, (uint8_t)(d->spt | ((mc >> 2) & 0xC0)));
        x86_set_r8(c, R_DH, (uint8_t)(d->heads - 1));
        x86_set_r8(c, R_DL, (uint8_t)(dl & 0x80 ? nhd : nfd));
        if (!(dl & 0x80)) {
            x86_set_r8(c, R_BL, (uint8_t)d->type);
            x86_set_r8(c, R_BH, 0);
            x86_load_seg(c, S_ES, PC_HLE_SEG);
            x86_set_r16(c, R_DI, DPT_OFF);
        }
        x86_set_r8(c, R_AL, 0);
        status(c, dl, 0);
        return;
    }
    case 0x15:                                               /* drive type */
        if (!d) { x86_set_r8(c, R_AH, 0); c->eflags &= ~X86_CF; return; }
        if (dl & 0x80) {
            uint32_t n = (uint32_t)(d->size / 512);
            x86_set_r16(c, R_CX, (uint16_t)(n >> 16));
            x86_set_r16(c, R_DX, (uint16_t)n);
            x86_set_r8(c, R_AH, 3);
        } else {
            x86_set_r8(c, R_AH, 2);                          /* diskette with change line */
        }
        c->eflags &= ~X86_CF;
        return;
    case 0x16:                                               /* diskette change line */
        if (!d) { status(c, dl, 0x80); return; }
        if (d->changed) { d->changed = 0; status(c, dl, 0x06); } else status(c, dl, 0);
        return;
    case 0x17: case 0x18:                                    /* set media type: whatever the image is */
        if (ah == 0x18 && d) { x86_load_seg(c, S_ES, PC_HLE_SEG); x86_set_r16(c, R_DI, DPT_OFF); }
        status(c, dl, d ? 0 : 0x80);
        return;
    case 0x41:                                               /* LBA extensions installed? */
        if (x86_get_r16(c, R_BX) != 0x55AA || !d || !(dl & 0x80)) { status(c, dl, 0x01); return; }
        x86_set_r16(c, R_BX, 0xAA55);
        x86_set_r8(c, R_AH, 0x21);                           /* version 1.1 */
        x86_set_r16(c, R_CX, 0x0001);                        /* packet calls */
        c->eflags &= ~X86_CF;
        return;
    case 0x42: case 0x43: case 0x44: {                       /* LBA read, write, verify */
        if (!d || !(dl & 0x80)) { status(c, dl, 0x01); return; }
        uint16_t si = x86_get_r16(c, R_SI), ds = c->seg[S_DS].sel;
        int count = pc_rd16(c, ds, (uint16_t)(si + 2));
        uint32_t buf = ((uint32_t)pc_rd16(c, ds, (uint16_t)(si + 6)) << 4) + pc_rd16(c, ds, (uint16_t)(si + 4));
        uint64_t lba = pc_rd16(c, ds, (uint16_t)(si + 8)) | (uint64_t)pc_rd16(c, ds, (uint16_t)(si + 10)) << 16
                     | (uint64_t)pc_rd16(c, ds, (uint16_t)(si + 12)) << 32 | (uint64_t)pc_rd16(c, ds, (uint16_t)(si + 14)) << 48;
        int err = ah == 0x44 ? (lba + (uint64_t)count > d->size / 512 ? 0x04 : 0)
                             : transfer(c, d, lba, count, buf, ah == 0x43);
        if (err) pc_wr16(c, ds, (uint16_t)(si + 2), 0);
        status(c, dl, err);
        return;
    }
    case 0x48: {                                             /* LBA drive parameters */
        if (!d || !(dl & 0x80)) { status(c, dl, 0x01); return; }
        uint16_t si = x86_get_r16(c, R_SI), ds = c->seg[S_DS].sel;
        uint64_t n = d->size / 512;
        pc_wr16(c, ds, si, 0x1A);
        pc_wr16(c, ds, (uint16_t)(si + 2), 0x0002);          /* CHS information valid */
        pc_wr16(c, ds, (uint16_t)(si + 4), (uint16_t)d->cyls); pc_wr16(c, ds, (uint16_t)(si + 6), 0);
        pc_wr16(c, ds, (uint16_t)(si + 8), (uint16_t)d->heads); pc_wr16(c, ds, (uint16_t)(si + 10), 0);
        pc_wr16(c, ds, (uint16_t)(si + 12), (uint16_t)d->spt); pc_wr16(c, ds, (uint16_t)(si + 14), 0);
        for (int k = 0; k < 4; k++) pc_wr16(c, ds, (uint16_t)(si + 16 + 2 * k), (uint16_t)(n >> (16 * k)));
        pc_wr16(c, ds, (uint16_t)(si + 24), 512);
        status(c, dl, 0);
        return;
    }
    default:
        status(c, dl, 0x01);
        return;
    }
}

/* The fixed-disk parameter table, as the AT's: its cylinders are the
 * drive's, one more than INT 13h AH=08h lets DOS use — the AT kept the
 * last as the diagnostic cylinder, AH=08h reporting the table's count
 * less two as the highest, and Windows' WDCTRL insists on that relation
 * (pc_ide.c gives the drive the cylinder, beyond the image if need be). */
static void fdpt(x86_cpu *c, uint16_t off, const disk *d) {
    for (int k = 0; k < 16; k++) pc_wr8(c, PC_HLE_SEG, (uint16_t)(off + k), 0);
    pc_wr16(c, PC_HLE_SEG, off, (uint16_t)(d->cyls + 1));
    pc_wr8(c, PC_HLE_SEG, (uint16_t)(off + 2), (uint8_t)d->heads);
    pc_wr16(c, PC_HLE_SEG, (uint16_t)(off + 5), 0xFFFF);          /* no write precompensation */
    pc_wr8(c, PC_HLE_SEG, (uint16_t)(off + 8), d->heads > 8 ? 0x08 : 0);
    pc_wr16(c, PC_HLE_SEG, (uint16_t)(off + 12), (uint16_t)(d->cyls + 1));   /* landing zone */
    pc_wr8(c, PC_HLE_SEG, (uint16_t)(off + 14), (uint8_t)d->spt);
}

/* INT 13h, the ROM tables and the BIOS data area's view of the drives. */
void pc_disk_install(x86_cpu *c) {
    static const uint8_t dpt[11] = { 0xDF, 0x02, 0x25, 0x02, 0x12, 0x1B, 0xFF, 0x6C, 0xF6, 0x0F, 0x08 };
    for (int k = 0; k < 11; k++) pc_wr8(c, PC_HLE_SEG, (uint16_t)(DPT_OFF + k), dpt[k]);
    if (fd[0].data) pc_wr8(c, PC_HLE_SEG, DPT_OFF + 4, (uint8_t)fd[0].spt);
    pc_wr16(c, 0, 0x1E * 4, DPT_OFF); pc_wr16(c, 0, 0x1E * 4 + 2, PC_HLE_SEG);
    if (hd[0].data) { fdpt(c, FDPT_OFF0, &hd[0]); pc_wr16(c, 0, 0x41 * 4, FDPT_OFF0); pc_wr16(c, 0, 0x41 * 4 + 2, PC_HLE_SEG); }
    if (hd[1].data) { fdpt(c, FDPT_OFF1, &hd[1]); pc_wr16(c, 0, 0x46 * 4, FDPT_OFF1); pc_wr16(c, 0, 0x46 * 4 + 2, PC_HLE_SEG); }
    /* equipment word: bit 0 any diskette, bits 6-7 their count less one */
    uint16_t eq = pc_rd16(c, PC_BDA_SEG, 0x10) & ~(uint16_t)0x00C1;
    int n = nfd ? nfd : 1;                       /* an AT always has drive A:, image or not */
    eq |= 0x0001 | (uint16_t)((n - 1) << 6);
    pc_wr16(c, PC_BDA_SEG, 0x10, eq);
    pc_wr8(c, PC_BDA_SEG, 0x75, (uint8_t)nhd);
    pc_wr8(c, PC_BDA_SEG, 0x41, 0);
    pc_wr8(c, PC_BDA_SEG, 0x74, 0);
    pc_set_service(0x13, int13, HLE_RET_FLAGS);
    pc_ide_post(c);                              /* the same disks, at the controller */
    /* An AT with a hard disk: INT 13h is the native fixed-disk BIOS
     * (tools/diskbios.asm), which drives the controller and passes the
     * rest — diskettes, the parameter queries — on to the host's at
     * F000:0013. An XT keeps the host's for everything. */
    if (nhd && c->model >= X86_MODEL_286) {
        for (size_t i = 0; i < sizeof diskbios; i++) pc_wr8(c, PC_STUB_SEG, (uint16_t)(PC_STUB_DISKBIOS + i), diskbios[i]);
        pc_wr16(c, 0, 0x13 * 4, PC_STUB_DISKBIOS);
        pc_wr16(c, 0, 0x13 * 4 + 2, PC_STUB_SEG);
    }
}
