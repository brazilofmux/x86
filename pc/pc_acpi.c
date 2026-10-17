/* pc_acpi.c — the least ACPI that names the processors (-acpi).
 *
 * The MP tables (pc_apic.c) tell Linux everything it needs, but a guest
 * that asks ACPI first and trusts only that gets the same facts here:
 * the RSDP at F000:F400 in the ROM, one of the places an OS looks, the
 * RSDT after it, a FADT (ACPI 1.0, with an empty DSDT and a FACS, for
 * interpreters that insist on loading them) and the MADT — every
 * processor's local APIC, the I/O APIC at FEC00000h with GSI base 0,
 * the PIT's IRQ 0 on GSI 2, the NMI into every LINT1. Haiku's boot
 * loader needs this: its MP-table parser compares the configuration
 * table's signature against a byte-swapped constant and rejects every
 * table there is, and real machines never showed it because their ACPI
 * came first. Off by default: an operating system's installer that sees
 * ACPI picks its ACPI HAL and then expects the rest (Windows 2000). */
#include "pc.h"
#include <string.h>

#define ACPI_OFF  0xF400                      /* F000:F400: RSDP, then the tables, each 16-byte aligned */
#define ACPI_PM_PORT 0xB000                   /* the PM1a blocks: an interpreter insists on their existence */
static int acpi_on;
void pc_acpi_enable(void) { acpi_on = 1; }
int  pc_acpi_enabled(void) { return acpi_on; }

/* The PM1a event and control blocks: the status half reads zero (no
 * event is ever pending), the enable half and the control register hold
 * what the OS writes, with SCI_EN always set — ACPI mode is on, nothing
 * has to be asked for. */
static uint8_t pm[8];
int pc_acpi_port_read(uint16_t port, uint32_t *v) {
    if (!acpi_on || port < ACPI_PM_PORT || port >= ACPI_PM_PORT + 8) return 0;
    unsigned i = port - ACPI_PM_PORT;
    *v = i < 2 ? 0 : i == 4 ? (uint32_t)(pm[4] | 1) : pm[i];
    return 1;
}
int pc_acpi_port_write(uint16_t port, uint32_t val, int size) {
    if (!acpi_on || port < ACPI_PM_PORT || port >= ACPI_PM_PORT + 8) return 0;
    for (int k = 0; k < size && port - ACPI_PM_PORT + (unsigned)k < 8; k++) pm[port - ACPI_PM_PORT + k] = (uint8_t)(val >> (8 * k));
    return 1;
}

static uint8_t sum(const uint8_t *p, int n) { uint8_t s = 0; for (int i = 0; i < n; i++) s = (uint8_t)(s + p[i]); return s; }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
/* A system description table header: signature, length, revision, OEM
 * ids, the checksum last. Returns the header length, 36. */
static int header(uint8_t *t, const char *sig, uint32_t len, uint8_t rev) {
    memset(t, 0, 36);
    memcpy(t, sig, 4); put32(t + 4, len); t[8] = rev;
    memcpy(t + 10, "DOSMNS", 6); memcpy(t + 16, "DOSMNSTR", 8); put32(t + 24, 1);
    memcpy(t + 28, "DOSM", 4); put32(t + 32, 1);
    return 36;
}
static void seal(uint8_t *t) { uint32_t len = t[4] | t[5] << 8 | t[6] << 16 | (uint32_t)t[7] << 24; t[9] = (uint8_t)(0 - sum(t, (int)len)); }

void pc_acpi_tables(x86_cpu *c) {
    if (!acpi_on || !pc_apic_present()) return;
    uint8_t buf[1024];
    memset(buf, 0, sizeof buf);
    uint32_t base = ((uint32_t)PC_HLE_SEG << 4) + ACPI_OFF;
    int at = 0;
    /* the tables, laid out after the RSDP (20 bytes, at 0): the RSDT at 32
     * (44 bytes), the MADT from 80, then the FACS on a 64-byte boundary */
    int rsdt_at = 32, fadt_at, facs_at, dsdt_at, madt_at;
    /* MADT */
    madt_at = 80;
    uint8_t *m = buf + madt_at;
    int n = header(m, "APIC", 0, 1);
    put32(m + n, 0xFEE00000u); n += 4;              /* local APIC address */
    put32(m + n, 1); n += 4;                        /* PCAT_COMPAT: the 8259s are there */
    for (int i = 0; i < pc.ncpus; i++) {            /* processor local APICs */
        m[n++] = 0; m[n++] = 8; m[n++] = (uint8_t)i; m[n++] = (uint8_t)i; put32(m + n, 1); n += 4;
    }
    m[n++] = 1; m[n++] = 12; m[n++] = PC_MAX_CPUS; m[n++] = 0;   /* I/O APIC: id past the processors', GSI base 0 */
    put32(m + n, 0xFEC00000u); n += 4; put32(m + n, 0); n += 4;
    m[n++] = 2; m[n++] = 10; m[n++] = 0; m[n++] = 0;             /* ISA IRQ 0 is on GSI 2 */
    put32(m + n, 2); n += 4; m[n++] = 0; m[n++] = 0;
    m[n++] = 4; m[n++] = 6; m[n++] = 0xFF; m[n++] = 0; m[n++] = 0; m[n++] = 1;   /* NMI into every processor's LINT1 */
    put32(m + 4, (uint32_t)n); seal(m);
    at = madt_at + ((n + 63) & ~63);
    /* FACS: 64 bytes, 64-byte aligned, its own signature and length, no checksum */
    facs_at = at;
    memcpy(buf + facs_at, "FACS", 4); put32(buf + facs_at + 4, 64);
    at = facs_at + 64;
    /* DSDT: a header and nothing to interpret */
    dsdt_at = at;
    header(buf + dsdt_at, "DSDT", 36, 1); seal(buf + dsdt_at);
    at = dsdt_at + 48;
    /* FADT (ACPI 1.0, 116 bytes): the FACS and the DSDT, SCI on IRQ 9, no
     * PM registers to speak of; flags: the PIT-like PM timer absent */
    fadt_at = at;
    uint8_t *f = buf + fadt_at;
    header(f, "FACP", 116, 1);
    put32(f + 36, base + (uint32_t)facs_at);
    put32(f + 40, base + (uint32_t)dsdt_at);
    f[45] = 0;                                       /* preferred PM profile: unspecified */
    f[46] = 9; f[47] = 0;                            /* SCI_INT */
    put32(f + 56, ACPI_PM_PORT); f[88] = 4;         /* PM1a event block: 4 bytes at B000h, nothing ever pending */
    put32(f + 64, ACPI_PM_PORT + 4); f[89] = 2;     /* PM1a control block: 2 bytes at B004h, SCI_EN reads set */
    put32(f + 112, 1u);                              /* flags: WBINVD works */
    seal(f);
    at = fadt_at + 128;
    /* RSDT: the FADT and the MADT */
    uint8_t *r = buf + rsdt_at;
    header(r, "RSDT", 36 + 8, 1);
    put32(r + 36, base + (uint32_t)fadt_at);
    put32(r + 40, base + (uint32_t)madt_at);
    seal(r);
    /* RSDP (ACPI 1.0: 20 bytes) */
    memcpy(buf, "RSD PTR ", 8);
    memcpy(buf + 9, "DOSMNS", 6);
    buf[15] = 0;                                     /* revision 0: an RSDT */
    put32(buf + 16, base + (uint32_t)rsdt_at);
    buf[8] = (uint8_t)(0 - sum(buf, 20));
    for (int i = 0; i < at; i++) pc_wr8(c, PC_HLE_SEG, (uint16_t)(ACPI_OFF + i), buf[i]);
}
