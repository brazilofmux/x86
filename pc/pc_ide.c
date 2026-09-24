/* pc_ide.c — the primary IDE (ATA) channel: ports 1F0h-1F7h and 3F6h,
 * IRQ 14, a master (disk 80h) and a slave (81h)
 *
 * Once an operating system stops calling INT 13h — NT's atapi/atdisk,
 * Windows 3.x's 32-bit disk access, Linux — it programs this: the task
 * file, PIO data through port 1F0h, and an interrupt per sector (per
 * block in multiple mode). The disks are the images INT 13h serves
 * (pc_disk.c maps them once), so the two paths never disagree.
 *
 * The drive is an instant one: a command completes before the OUT that
 * issued it returns, so BSY is never seen, DRQ is up when there is data
 * to move, and INTRQ rises as the ATA standard has it — for a read when
 * each sector (or block) is ready, for a write when each has been taken
 * and when the command completes, for non-data commands at completion.
 * Reading the status register drops INTRQ (the alternate status at 3F6h
 * does not), as does issuing the next command; nIEN in the device
 * control register keeps it off the bus. IRQ 14 is its rising edge.
 *
 * Commands: IDENTIFY DEVICE; READ and WRITE SECTORS (with and without
 * retries), READ and WRITE MULTIPLE, SET MULTIPLE MODE, READ VERIFY;
 * INITIALIZE DEVICE PARAMETERS (the CHS translation), RECALIBRATE, SEEK,
 * EXECUTE DEVICE DIAGNOSTIC, SET FEATURES, the power commands, FLUSH
 * CACHE. Anything else — IDENTIFY PACKET DEVICE among them: this is not
 * an ATAPI device — is aborted. With no slave, a read of its registers
 * finds 0. SRST (device control bit 2) resets both, leaving the ATA
 * signature in the task file.
 */
#include "pc.h"
#include <stdio.h>
#include <string.h>

enum { ST_ERR = 0x01, ST_DRQ = 0x08, ST_DSC = 0x10, ST_DF = 0x20, ST_DRDY = 0x40, ST_BSY = 0x80 };
enum { ER_AMNF = 0x01, ER_ABRT = 0x04, ER_IDNF = 0x10, ER_UNC = 0x40 };

typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t sectors;                /* capacity, LBA */
    int cyls, heads, spt;            /* default translation (IDENTIFY words 1, 3, 6) */
    int ccyls, cheads, cspt;         /* current translation (INITIALIZE DEVICE PARAMETERS) */
    int multiple;                    /* SET MULTIPLE MODE's block size, 0 = not set */
} drive;

static struct {
    drive d[2];
    int present[2];
    /* the task file (one set for the channel: both drives see the writes) */
    uint8_t error, features, count, sector, cyl_lo, cyl_hi, devhead, status, control;
    /* a data transfer in progress */
    int cmd;                         /* the command moving data, 0 none */
    int writing;
    uint32_t lba;                    /* the next sector */
    int left;                        /* sectors still to move */
    int block;                       /* sectors per DRQ block */
    int in_block;                    /* sectors moved in this block */
    uint8_t buf[512 * 16];
    int pos, len;                    /* bytes moved / in the block */
    int intrq;
    int target;                      /* the drive the transfer is for */
} ide;

static int sel(void) { return (ide.devhead >> 4) & 1; }
static drive *cur(void) { return ide.present[sel()] ? &ide.d[sel()] : NULL; }

static void irq(void) {
    if (ide.control & 0x02) return;              /* nIEN */
    if (!ide.intrq) { ide.intrq = 1; pc_irq_raise(14); }
}

/* the sector the task file names: LBA (device/head bit 6) or CHS through
 * the current translation; ~0 if CHS names no sector */
static uint32_t taskfile_lba(const drive *d) {
    if (ide.devhead & 0x40)
        return (uint32_t)ide.sector | (uint32_t)ide.cyl_lo << 8 | (uint32_t)ide.cyl_hi << 16 | (uint32_t)(ide.devhead & 0x0F) << 24;
    int cyl = ide.cyl_lo | ide.cyl_hi << 8, head = ide.devhead & 0x0F, sec = ide.sector;
    if (sec < 1 || sec > d->cspt || head >= d->cheads) return ~0u;
    return ((uint32_t)cyl * (uint32_t)d->cheads + (uint32_t)head) * (uint32_t)d->cspt + (uint32_t)(sec - 1);
}
/* ... and back, after a transfer: the task file names the last sector moved */
static void taskfile_set(const drive *d, uint32_t lba) {
    if (ide.devhead & 0x40) {
        ide.sector = (uint8_t)lba; ide.cyl_lo = (uint8_t)(lba >> 8); ide.cyl_hi = (uint8_t)(lba >> 16);
        ide.devhead = (uint8_t)((ide.devhead & 0xF0) | ((lba >> 24) & 0x0F));
        return;
    }
    uint32_t per_cyl = (uint32_t)d->cheads * (uint32_t)d->cspt;
    uint32_t cyl = lba / per_cyl, rem = lba % per_cyl;
    ide.cyl_lo = (uint8_t)cyl; ide.cyl_hi = (uint8_t)(cyl >> 8);
    ide.devhead = (uint8_t)((ide.devhead & 0xF0) | (rem / (uint32_t)d->cspt));
    ide.sector = (uint8_t)(rem % (uint32_t)d->cspt + 1);
}

static void done(uint8_t status, uint8_t error) {
    ide.status = status;
    ide.error = error;
    ide.cmd = 0;
    irq();
}
static void abort_cmd(void) { done(ST_DRDY | ST_DSC | ST_ERR, ER_ABRT); }

/* ---- IDENTIFY DEVICE ---------------------------------------------------- */

static void ident_str(uint16_t *w, int first, int words, const char *s) {
    char b[64];
    memset(b, ' ', sizeof b);
    memcpy(b, s, strlen(s) < (size_t)words * 2 ? strlen(s) : (size_t)words * 2);
    for (int i = 0; i < words; i++) w[first + i] = (uint16_t)((uint8_t)b[2 * i] << 8 | (uint8_t)b[2 * i + 1]);
}
static void identify(const drive *d, int unit) {
    uint16_t w[256];
    memset(w, 0, sizeof w);
    w[0] = 0x0040;                               /* fixed, not removable */
    w[1] = (uint16_t)d->cyls; w[3] = (uint16_t)d->heads; w[6] = (uint16_t)d->spt;
    w[4] = (uint16_t)(512 * d->spt); w[5] = 512;  /* (obsolete: bytes per track, per sector) */
    ident_str(w, 10, 10, unit ? "DM0000000002" : "DM0000000001");
    w[20] = 3; w[21] = 16;                       /* buffer type, size in sectors */
    ident_str(w, 23, 4, "1.0");
    ident_str(w, 27, 20, "DOS-MONSTER HARDDISK");
    w[47] = 0x8010;                              /* READ/WRITE MULTIPLE: up to 16 sectors */
    w[49] = 0x0200;                              /* LBA; no DMA */
    w[51] = 0x0200;                              /* PIO mode 2 timing */
    w[53] = 0x0003;                              /* words 54-58 and 64-70 valid */
    w[54] = (uint16_t)d->ccyls; w[55] = (uint16_t)d->cheads; w[56] = (uint16_t)d->cspt;
    uint32_t chs = (uint32_t)d->ccyls * (uint32_t)d->cheads * (uint32_t)d->cspt;
    w[57] = (uint16_t)chs; w[58] = (uint16_t)(chs >> 16);
    w[59] = (uint16_t)(d->multiple ? 0x0100 | d->multiple : 0);
    w[60] = (uint16_t)d->sectors; w[61] = (uint16_t)(d->sectors >> 16);
    w[64] = 0x0003;                              /* PIO modes 3 and 4 */
    w[65] = w[66] = w[67] = w[68] = 120;         /* cycle times, ns */
    w[80] = 0x001E;                              /* ATA-1 to ATA-4 */
    w[82] = 0x4000; w[83] = 0x4000; w[84] = 0x4000;   /* (valid words; nothing more claimed) */
    w[85] = 0x4000; w[86] = 0x4000; w[87] = 0x4000;
    for (int i = 0; i < 256; i++) { ide.buf[2 * i] = (uint8_t)w[i]; ide.buf[2 * i + 1] = (uint8_t)(w[i] >> 8); }
    ide.pos = 0; ide.len = 512; ide.cmd = 0xEC; ide.writing = 0; ide.left = 0;
    ide.status = ST_DRDY | ST_DSC | ST_DRQ;
    ide.error = 0;
    irq();
}

/* ---- data --------------------------------------------------------------- */

/* the next block of a read into the buffer, DRQ up, INTRQ */
static void read_block(void) {
    drive *d = &ide.d[ide.target];
    int n = ide.left < ide.block ? ide.left : ide.block;
    if ((uint64_t)ide.lba + (uint64_t)n > d->sectors) {         /* runs off the end */
        taskfile_set(d, ide.lba < d->sectors ? ide.lba : d->sectors - 1);
        done(ST_DRDY | ST_DSC | ST_ERR, ER_IDNF);
        return;
    }
    memcpy(ide.buf, d->data + (size_t)ide.lba * 512, (size_t)n * 512);
    ide.pos = 0; ide.len = n * 512;
    ide.status = ST_DRDY | ST_DSC | ST_DRQ;
    ide.error = 0;
    irq();
}
/* a block the host has written: to the image; the next block, or done */
static void write_block(void) {
    drive *d = &ide.d[ide.target];
    int n = ide.len / 512;
    memcpy(d->data + (size_t)ide.lba * 512, ide.buf, (size_t)n * 512);
    ide.lba += (uint32_t)n; ide.left -= n;
    taskfile_set(d, ide.lba - 1);
    if (ide.left <= 0) { ide.count = 0; done(ST_DRDY | ST_DSC, 0); return; }
    ide.count = (uint8_t)ide.left;
    int m = ide.left < ide.block ? ide.left : ide.block;
    ide.pos = 0; ide.len = m * 512;
    ide.status = ST_DRDY | ST_DSC | ST_DRQ;
    irq();
}
static void begin(int write, int multiple) {
    drive *d = cur();
    if (!d) { abort_cmd(); return; }
    if (multiple && !d->multiple) { abort_cmd(); return; }
    uint32_t lba = taskfile_lba(d);
    int n = ide.count ? ide.count : 256;
    if (lba == ~0u || (uint64_t)lba + (uint64_t)n > d->sectors) { done(ST_DRDY | ST_DSC | ST_ERR, ER_IDNF); return; }
    ide.target = sel();
    ide.lba = lba; ide.left = n; ide.writing = write;
    ide.block = multiple ? d->multiple : 1;
    ide.cmd = 1;
    if (write) {                                 /* DRQ for the first block, no interrupt */
        int m = n < ide.block ? n : ide.block;
        ide.pos = 0; ide.len = m * 512;
        ide.status = ST_DRDY | ST_DSC | ST_DRQ;
        ide.error = 0;
    } else read_block();
}

/* after the host has taken the last byte of a read block */
static void read_consumed(void) {
    drive *d = &ide.d[ide.target];
    int n = ide.len / 512;
    ide.lba += (uint32_t)n; ide.left -= n;
    taskfile_set(d, ide.lba - 1);
    ide.count = (uint8_t)(ide.left > 0 ? ide.left : 0);
    if (ide.left <= 0) { ide.status = ST_DRDY | ST_DSC; ide.cmd = 0; return; }   /* no interrupt after the last */
    read_block();
}

static uint32_t data_read(int size) {
    if (!(ide.status & ST_DRQ) || ide.writing) return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFu;
    uint32_t v = 0;
    for (int i = 0; i < size && ide.pos < ide.len; i++) v |= (uint32_t)ide.buf[ide.pos++] << (8 * i);
    if (ide.pos >= ide.len) {
        if (ide.cmd == 0xEC) { ide.status = ST_DRDY | ST_DSC; ide.cmd = 0; }
        else read_consumed();
    }
    return v;
}
static void data_write(uint32_t v, int size) {
    if (!(ide.status & ST_DRQ) || !ide.writing) return;
    for (int i = 0; i < size && ide.pos < ide.len; i++) ide.buf[ide.pos++] = (uint8_t)(v >> (8 * i));
    if (ide.pos >= ide.len) write_block();
}

/* ---- commands ----------------------------------------------------------- */

static void reset_signature(void) {
    ide.error = 0x01;                            /* diagnostics passed */
    ide.count = 1; ide.sector = 1; ide.cyl_lo = 0; ide.cyl_hi = 0;   /* an ATA device */
    ide.devhead &= 0x10;
    ide.status = ST_DRDY | ST_DSC;
    ide.cmd = 0; ide.writing = 0;
}

static void command(uint8_t c) {
    ide.intrq = 0;
    if (pc.debug) fprintf(stderr, "[ide] drive %d command %02X count %02X lba/chs %02X %02X %02X dh %02X\n",
                              sel(), c, ide.count, ide.sector, ide.cyl_lo, ide.cyl_hi, ide.devhead);
    drive *d = cur();
    if (!d && c != 0x90) return;                 /* no such drive: nothing answers */
    ide.cmd = 0; ide.writing = 0;
    switch (c) {
    case 0xEC: identify(d, sel()); return;
    case 0x20: case 0x21: begin(0, 0); return;
    case 0x30: case 0x31: begin(1, 0); return;
    case 0xC4: begin(0, 1); return;
    case 0xC5: begin(1, 1); return;
    case 0x40: case 0x41: {                      /* READ VERIFY */
        uint32_t lba = taskfile_lba(d);
        int n = ide.count ? ide.count : 256;
        if (lba == ~0u || (uint64_t)lba + (uint64_t)n > d->sectors) { done(ST_DRDY | ST_DSC | ST_ERR, ER_IDNF); return; }
        taskfile_set(d, lba + (uint32_t)n - 1);
        ide.count = 0;
        done(ST_DRDY | ST_DSC, 0);
        return;
    }
    case 0xC6: {                                 /* SET MULTIPLE MODE */
        int n = ide.count;
        if (n && (n > 16 || (n & (n - 1)))) { abort_cmd(); return; }
        d->multiple = n;
        done(ST_DRDY | ST_DSC, 0);
        return;
    }
    case 0x91: {                                 /* INITIALIZE DEVICE PARAMETERS */
        int heads = (ide.devhead & 0x0F) + 1, spt = ide.count;
        if (spt < 1) { abort_cmd(); return; }
        d->cheads = heads; d->cspt = spt;
        uint32_t c2 = d->sectors / ((uint32_t)heads * (uint32_t)spt);
        d->ccyls = c2 > 65535 ? 65535 : (int)c2;
        done(ST_DRDY | ST_DSC, 0);
        return;
    }
    case 0x90:                                   /* EXECUTE DEVICE DIAGNOSTIC: both drives */
        reset_signature();
        ide.devhead &= (uint8_t)~0x10;           /* the master reports */
        irq();
        return;
    case 0x70: case 0xE7: case 0xEF: case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE6:
    case 0x94: case 0x95: case 0x96: case 0x97: case 0x99:
        done(ST_DRDY | ST_DSC, 0);               /* SEEK, FLUSH CACHE, SET FEATURES, the power states */
        return;
    case 0xE5: case 0x98:                        /* CHECK POWER MODE: active */
        ide.count = 0xFF;
        done(ST_DRDY | ST_DSC, 0);
        return;
    default:
        if ((c & 0xF0) == 0x10) { done(ST_DRDY | ST_DSC, 0); return; }   /* RECALIBRATE */
        abort_cmd();
        return;
    }
}

/* ---- ports -------------------------------------------------------------- */

static int ide_read(uint16_t port, int size, uint32_t *val);
int pc_ide_port_read(uint16_t port, int size, uint32_t *val) {
    int r = ide_read(port, size, val);
    if (r && pc.debug > 2 && port != 0x1F0) fprintf(stderr, "[ide] in %03X -> %02X\n", port, *val & 0xFF);
    return r;
}
static int ide_read(uint16_t port, int size, uint32_t *val) {
    if (port == 0x3F6) { *val = cur() ? ide.status : 0; return 1; }   /* alternate status: INTRQ stays */
    if (port < 0x1F0 || port > 0x1F7) return 0;
    if (!ide.present[0] && !ide.present[1]) { *val = 0xFF; return 1; }   /* no drives: an empty bus */
    if (port == 0x1F0) { *val = data_read(size); return 1; }
    if (!cur() && port != 0x1F6) { *val = 0; return 1; }              /* a missing slave reads 0 */
    switch (port) {
    case 0x1F1: *val = ide.error; break;
    case 0x1F2: *val = ide.count; break;
    case 0x1F3: *val = ide.sector; break;
    case 0x1F4: *val = ide.cyl_lo; break;
    case 0x1F5: *val = ide.cyl_hi; break;
    case 0x1F6: *val = ide.devhead | 0xA0; break;
    default:    *val = ide.status; ide.intrq = 0; break;              /* 1F7h: status, and INTRQ drops */
    }
    return 1;
}

int pc_ide_port_write(uint16_t port, uint32_t val, int size) {
    if (pc.debug > 2 && ((port >= 0x1F1 && port <= 0x1F7) || port == 0x3F6)) fprintf(stderr, "[ide] out %03X <- %02X\n", port, val & 0xFF);
    if (port == 0x3F6) {                         /* device control: nIEN, SRST */
        uint8_t was = ide.control;
        ide.control = (uint8_t)val;
        if ((was & 0x04) && !(val & 0x04)) reset_signature();   /* SRST released */
        else if (val & 0x04) { ide.status = ST_BSY; ide.cmd = 0; }
        return 1;
    }
    if (port < 0x1F0 || port > 0x1F7) return 0;
    uint8_t v = (uint8_t)val;
    switch (port) {
    case 0x1F0: data_write(val, size); break;
    case 0x1F1: ide.features = v; break;
    case 0x1F2: ide.count = v; break;
    case 0x1F3: ide.sector = v; break;
    case 0x1F4: ide.cyl_lo = v; break;
    case 0x1F5: ide.cyl_hi = v; break;
    case 0x1F6: ide.devhead = v; break;
    default:    command(v); break;
    }
    return 1;
}

/* POST: the drives the images give, reset; INT 76h (IRQ 14) as the AT's */
void pc_ide_post(x86_cpu *c) {
    memset(&ide, 0, sizeof ide);
    for (int u = 0; u < 2; u++) {
        drive *d = &ide.d[u];
        int cyls, heads, spt;
        d->data = pc_disk_hd(u, &d->size, &cyls, &heads, &spt);
        if (!d->data) continue;
        ide.present[u] = 1;
        uint64_t n = d->size / 512;
        d->sectors = n > 0x0FFFFFFF ? 0x0FFFFFFF : (uint32_t)n;
        /* the default translation: the BIOS's heads and sectors, cylinders
         * for the whole disk (to 16383, as ATA caps word 1) */
        d->heads = heads; d->spt = spt;
        uint32_t cc = d->sectors / ((uint32_t)heads * (uint32_t)spt);
        d->cyls = cc > 16383 ? 16383 : (int)cc;
        (void)cyls;
        d->ccyls = d->cyls; d->cheads = d->heads; d->cspt = d->spt;
    }
    reset_signature();
    /* push ax; push ds; mov ax,40h; mov ds,ax; mov byte [8Eh],0FFh (the
     * hard-disk interrupt flag INT 13h waits on); mov al,20h; out A0h,al;
     * out 20h,al; pop ds; pop ax; iret */
    static const uint8_t int76[] = { 0x50, 0x1E, 0xB8, 0x40, 0x00, 0x8E, 0xD8, 0xC6, 0x06, 0x8E, 0x00, 0xFF,
                                     0xB0, 0x20, 0xE6, 0xA0, 0xE6, 0x20, 0x1F, 0x58, 0xCF };
    for (size_t i = 0; i < sizeof int76; i++) pc_wr8(c, PC_STUB_SEG, (uint16_t)(PC_STUB_INT76 + i), int76[i]);
    pc_wr16(c, 0, 0x76 * 4, PC_STUB_INT76);
    pc_wr16(c, 0, 0x76 * 4 + 2, PC_STUB_SEG);
    if (ide.present[0]) pc_irq_unmask(14);
}
