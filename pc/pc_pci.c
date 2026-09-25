/* pc_pci.c — a PCI bus: configuration mechanism #1 and a host bridge
 *
 * Present only when asked for (-pci): the ISA machine the DOS, Windows and
 * NT suites boot stays one. With it, the machine is what QEMU and
 * VirtualBox present at 00:00.0, an Intel 440FX host bridge (8086:1237),
 * and nothing else yet — a bus for the network cards to come.
 *
 * Configuration mechanism #1: a dword write to CF8h selects bus, device,
 * function and register (bit 31 enables); CFCh-CFFh read and write that
 * dword's bytes. A selected slot with nothing in it reads all ones, as
 * does anything while bit 31 is clear. Only bus 0 exists.
 *
 * The BIOS32 Service Directory and the 32-bit PCI BIOS that protected-mode
 * software calls (tools/pcibios.asm) go in at F3000h: real instructions,
 * programming CF8h/CFCh like any other caller.
 */
#include "pc.h"
#include <string.h>
#include "pc_pcibios.h"

static struct {
    int present;
    uint32_t addr;                  /* CF8h */
    uint8_t host[256];              /* 00:00.0's configuration space */
} pci;

void pc_pci_enable(void) { pci.present = 1; }
int  pc_pci_present(void) { return pci.present; }

/* POST: the host bridge's registers at reset; the BIOS32 directory and the
 * PCI BIOS, with the directory's checksum */
void pc_pci_post(x86_cpu *c) {
    if (!pci.present) return;
    memset(pci.host, 0, sizeof pci.host);
    static const uint8_t id[16] = {
        0x86, 0x80, 0x37, 0x12,     /* vendor 8086h, device 1237h: 82441FX */
        0x06, 0x00, 0x00, 0x02,     /* command: memory, bus master; status: medium DEVSEL */
        0x02, 0x00, 0x00, 0x06,     /* revision 2; class 06h (bridge), subclass 00h (host) */
        0x00, 0x00, 0x00, 0x00,     /* header type 0, single function */
    };
    memcpy(pci.host, id, sizeof id);
    pci.addr = 0;
    for (size_t i = 0; i < sizeof pcibios; i++) pc_wr8(c, PC_STUB_SEG, (uint16_t)(PC_STUB_PCIBIOS + i), pcibios[i]);
    uint8_t sum = 0;
    for (int i = 0; i < 16; i++) sum = (uint8_t)(sum + pcibios[i]);
    pc_wr8(c, PC_STUB_SEG, (uint16_t)(PC_STUB_PCIBIOS + 10), (uint8_t)(0 - sum));
}

/* the selected configuration dword, or NULL for an empty slot */
static uint8_t *space(void) {
    if (!(pci.addr & 0x80000000u)) return NULL;
    uint32_t bus = (pci.addr >> 16) & 0xFF, dev = (pci.addr >> 11) & 0x1F, fn = (pci.addr >> 8) & 7;
    if (bus == 0 && dev == 0 && fn == 0) return pci.host;
    return NULL;
}

/* what software may change in the host bridge: the command register's
 * low byte, and the latency timer; the rest reads as it was set */
static void host_write(unsigned reg, uint8_t v) {
    if (reg == 0x04) pci.host[reg] = v & 0x06;
    else if (reg == 0x0D) pci.host[reg] = v;
}

int pc_pci_port_read(uint16_t port, int size, uint32_t *val) {
    if (!pci.present) return 0;
    if (port == 0xCF8 && size == 4) { *val = pci.addr; return 1; }
    if (port < 0xCFC || port > 0xCFF) return 0;
    uint8_t *s = space();
    unsigned reg = (pci.addr & 0xFC) + (port & 3u);
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)(s && reg + (unsigned)i < 256 ? s[reg + (unsigned)i] : 0xFF) << (8 * i);
    *val = v;
    return 1;
}

int pc_pci_port_write(uint16_t port, uint32_t val, int size) {
    if (!pci.present) return 0;
    if (port == 0xCF8 && size == 4) { pci.addr = val & 0x80FFFFFCu; return 1; }
    if (port < 0xCFC || port > 0xCFF) return 0;
    uint8_t *s = space();
    unsigned reg = (pci.addr & 0xFC) + (port & 3u);
    if (s == pci.host)
        for (int i = 0; i < size; i++) host_write(reg + (unsigned)i, (uint8_t)(val >> (8 * i)));
    return 1;
}
