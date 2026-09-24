/* pc_cmos.c — the MC146818 CMOS (ports 70h/71h) and the BIOS's reports of
 * memory above 1 MB (INT 15h AH=87h, 88h, E801h, E820h)
 *
 * Extended memory is the part of the guest buffer above 1 MB: the HMA and
 * then X86_EXT_SIZE, contiguous, so a physical address above 1 MB is an
 * offset into cpu->mem like any other. A booted machine reports it all
 * (a 286 only what its 24 address lines reach) and HIMEM owns it from
 * there. Under the HLE DOS it stays unreported, as before: our own DPMI
 * host hands it out, and a program that found it through INT 15h would
 * be writing over the host's clients.
 *
 * The real-time clock is a calendar of its own, set from the host's local
 * time at power-on and run on the machine's clock (pc_now_ns: wall time,
 * or the instruction count under the repeatable vclock) — settable
 * (register B's SET freezes it, and the time registers take writes, in
 * BCD or binary, 12- or 24-hour, as register B says), with register A's
 * UIP in the last 244 us of each second and its rate select, and the
 * three interrupt sources in register C: the periodic flag (2 Hz to
 * 8 kHz), the alarm and the update-ended flag once a second. IRQF rising
 * is IRQ 8, on the slave 8259; reading C clears every flag, and until it
 * is read no further interrupt comes (the edge the 8259 needs never
 * does), as on the chip. pc_rtc_poll runs the clock forward from pc_poll,
 * which asks for it by pc.rtc_next_ns; a read of any register first
 * brings it up to date.
 *
 * The rest of the 128 bytes is RAM, with the configuration a POST would
 * leave: diskette types, equipment, base and extended memory, and the
 * checksum over 10h-2Dh.
 */
#include "pc.h"
#include <string.h>
#include <time.h>

static uint8_t nvram[128];
static uint8_t index_reg;

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

/* ---- the real-time clock ------------------------------------------------ */

#define RTC_A_UIP  0x80
#define RTC_B_SET  0x80
#define RTC_B_PIE  0x40
#define RTC_B_AIE  0x20
#define RTC_B_UIE  0x10
#define RTC_B_DM   0x04              /* binary, not BCD */
#define RTC_B_24H  0x02
#define RTC_C_IRQF 0x80
#define RTC_C_PF   0x40
#define RTC_C_AF   0x20
#define RTC_C_UF   0x10
#define NS_PER_S   1000000000ull
#define UIP_NS     244000ull         /* UIP leads the update by 244 us */

static struct {
    int sec, min, hour, wday, mday, mon, year;   /* binary; year in full */
    uint64_t second_ns;              /* when the current second began (machine time) */
    uint64_t pf_next_ns;             /* the next periodic flag */
    uint8_t alarm[3];                /* seconds, minutes, hours alarm registers, as written */
} rtc;

static int days_in(int mon, int year) {
    static const uint8_t d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return mon == 2 && leap ? 29 : d[mon - 1];
}
static void rtc_tick_second(void) {
    if (++rtc.sec < 60) return;
    rtc.sec = 0;
    if (++rtc.min < 60) return;
    rtc.min = 0;
    if (++rtc.hour < 24) return;
    rtc.hour = 0;
    rtc.wday = rtc.wday % 7 + 1;
    if (++rtc.mday <= days_in(rtc.mon, rtc.year)) return;
    rtc.mday = 1;
    if (++rtc.mon <= 12) return;
    rtc.mon = 1;
    rtc.year++;
}
static int running(void) { return !(nvram[0x0B] & RTC_B_SET) && (nvram[0x0A] & 0x70) == 0x20; }
static uint64_t pf_period_ns(void) {
    int rs = nvram[0x0A] & 15;
    if (!rs) return 0;
    if (rs < 3) rs += 7;             /* 1 and 2 are 256 and 128 Hz, as 8 and 9 */
    return (NS_PER_S << (rs - 1)) / 32768u;
}
/* An hour register value in the current format, and back */
static uint8_t enc(int v) { return (nvram[0x0B] & RTC_B_DM) ? (uint8_t)v : bcd(v); }
static int dec(uint8_t v) { return (nvram[0x0B] & RTC_B_DM) ? v : unbcd(v); }
static uint8_t enc_hour(int h) {
    if (nvram[0x0B] & RTC_B_24H) return enc(h);
    int h12 = h % 12 ? h % 12 : 12;
    return (uint8_t)(enc(h12) | (h >= 12 ? 0x80 : 0));
}
static int dec_hour(uint8_t v) {
    if (nvram[0x0B] & RTC_B_24H) return dec(v);
    int h = dec(v & 0x7F) % 12;
    return (v & 0x80) ? h + 12 : h;
}
static int alarm_match(void) {
    const int now[3] = { rtc.sec, rtc.min, rtc.hour };
    for (int i = 0; i < 3; i++) {
        uint8_t a = rtc.alarm[i];
        if ((a & 0xC0) == 0xC0) continue;                /* don't care */
        if ((i == 2 ? dec_hour(a) : dec(a)) != now[i]) return 0;
    }
    return 1;
}
static void rtc_schedule(void) {
    uint64_t next = UINT64_MAX;
    uint8_t b = nvram[0x0B];
    if (nvram[0x0C] & RTC_C_IRQF) { pc.rtc_next_ns = next; return; }   /* no new edge until C is read */
    if ((b & RTC_B_PIE) && pf_period_ns()) next = rtc.pf_next_ns;
    if ((b & (RTC_B_AIE | RTC_B_UIE)) && running() && rtc.second_ns + NS_PER_S < next) next = rtc.second_ns + NS_PER_S;
    pc.rtc_next_ns = next;
}
/* The clock up to NOW: seconds (UF, AF), the periodic flag, and IRQ 8 on
 * IRQF's rising edge. */
static void rtc_advance(uint64_t now) {
    uint8_t c = nvram[0x0C];
    if (now < rtc.second_ns) {               /* the machine's clock changed hands (X86_VCLOCK after init): rebase */
        rtc.second_ns = now;
        rtc.pf_next_ns = now + pf_period_ns();
    }
    if (running()) {
        uint64_t behind = now > rtc.second_ns ? (now - rtc.second_ns) / NS_PER_S : 0;
        if (behind > 86400) {                             /* a long stall: no flags per second, just the time */
            for (uint64_t k = 0; k < behind - 1; k++) rtc_tick_second();
            rtc.second_ns += (behind - 1) * NS_PER_S;
            behind = 1;
        }
        for (uint64_t k = 0; k < behind; k++) {
            rtc_tick_second();
            rtc.second_ns += NS_PER_S;
            c |= RTC_C_UF;
            if (alarm_match()) c |= RTC_C_AF;
        }
    } else rtc.second_ns = now;                           /* frozen: the second restarts when it runs again */
    uint64_t p = pf_period_ns();
    if (p && now >= rtc.pf_next_ns) {
        c |= RTC_C_PF;
        rtc.pf_next_ns = now - (now - rtc.pf_next_ns) % p + p;   /* missed periods are one flag, as on the chip */
    }
    uint8_t b = nvram[0x0B];
    int irqf = ((c & RTC_C_PF) && (b & RTC_B_PIE)) || ((c & RTC_C_AF) && (b & RTC_B_AIE)) || ((c & RTC_C_UF) && (b & RTC_B_UIE));
    if (irqf && !(c & RTC_C_IRQF)) { c |= RTC_C_IRQF; pc_irq_raise(8); }
    nvram[0x0C] = c;
    rtc_schedule();
}
void pc_rtc_poll(uint64_t now) { rtc_advance(now); }

static void rtc_seed(void) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    rtc.sec = tm.tm_sec > 59 ? 59 : tm.tm_sec; rtc.min = tm.tm_min; rtc.hour = tm.tm_hour;
    rtc.wday = tm.tm_wday + 1; rtc.mday = tm.tm_mday; rtc.mon = tm.tm_mon + 1; rtc.year = tm.tm_year + 1900;
    uint64_t now = pc_now_ns();
    rtc.second_ns = now;
    rtc.pf_next_ns = now + pf_period_ns();
    rtc.alarm[0] = rtc.alarm[1] = rtc.alarm[2] = 0;
}

/* For the BIOS's INT 1Ah: the time and date, binary, year in full */
void pc_rtc_get(int *h, int *m, int *sec, int *year, int *mon, int *mday) {
    rtc_advance(pc_now_ns());
    *h = rtc.hour; *m = rtc.min; *sec = rtc.sec; *year = rtc.year; *mon = rtc.mon; *mday = rtc.mday;
}
void pc_rtc_set_time(int h, int m, int sec) {
    rtc_advance(pc_now_ns());
    rtc.hour = h; rtc.min = m; rtc.sec = sec; rtc.second_ns = pc_now_ns();
}
void pc_rtc_set_date(int year, int mon, int mday) {
    rtc_advance(pc_now_ns());
    rtc.year = year; rtc.mon = mon; rtc.mday = mday;
}
/* For INT 1Ah AH=06h/07h: the alarm (BCD hours, minutes, seconds) on, or off */
void pc_rtc_alarm(int on, uint8_t h, uint8_t m, uint8_t sec) {
    rtc_advance(pc_now_ns());
    if (on) {
        rtc.alarm[0] = (nvram[0x0B] & RTC_B_DM) ? (uint8_t)unbcd(sec) : sec;
        rtc.alarm[1] = (nvram[0x0B] & RTC_B_DM) ? (uint8_t)unbcd(m) : m;
        rtc.alarm[2] = (nvram[0x0B] & RTC_B_DM) ? (uint8_t)unbcd(h) : h;
    }
    nvram[0x0B] = (uint8_t)(on ? nvram[0x0B] | RTC_B_AIE : nvram[0x0B] & ~RTC_B_AIE);
    rtc_schedule();
}
int pc_rtc_alarm_on(void) { return (nvram[0x0B] & RTC_B_AIE) != 0; }

/* For INT 15h AH=83h: the periodic interrupt on or off */
void pc_rtc_pie(int on) {
    rtc_advance(pc_now_ns());
    nvram[0x0B] = (uint8_t)(on ? nvram[0x0B] | RTC_B_PIE : nvram[0x0B] & ~RTC_B_PIE);
    rtc_schedule();
}

/* Top of physical memory the model can address, and KB above 1 MB. */
static uint32_t mem_top(const x86_cpu *c) {
    uint32_t top = c->mem_size;
    if (c->model < X86_MODEL_386 && top > 0x1000000u) top = 0x1000000u;   /* 24 address lines */
    return top;
}

uint32_t pc_ext_kb(const x86_cpu *c) {
    if (!pc.booted || c->model < X86_MODEL_286) return 0;
    uint32_t top = mem_top(c);
    return top > 0x100000u ? (top - 0x100000u) / 1024 : 0;
}

/* The configuration part of the CMOS, as POST writes it. */
void pc_cmos_init(x86_cpu *c) {
    memset(nvram, 0, sizeof nvram);
    nvram[0x0A] = 0x26;                          /* 32.768 kHz base, 1024 Hz rate */
    nvram[0x0B] = 0x02;                          /* 24-hour, BCD, no interrupts */
    nvram[0x0D] = 0x80;                          /* battery good */
    rtc_seed();
    rtc_schedule();
    nvram[0x10] = (uint8_t)(pc_disk_floppy_type(0) << 4 | pc_disk_floppy_type(1));
    nvram[0x14] = (uint8_t)pc_rd16(c, PC_BDA_SEG, 0x10);
    nvram[0x15] = PC_CONV_KB & 0xFF; nvram[0x16] = PC_CONV_KB >> 8;
    uint32_t ext = pc_ext_kb(c);
    uint16_t ext16 = ext > 0xFFFF ? 0xFFFF : (uint16_t)ext;
    nvram[0x17] = nvram[0x30] = (uint8_t)ext16;
    nvram[0x18] = nvram[0x31] = (uint8_t)(ext16 >> 8);
    uint32_t top = mem_top(c);
    uint32_t above16 = pc.booted && top > 0x1000000u ? (top - 0x1000000u) >> 16 : 0;   /* 64K blocks */
    nvram[0x34] = (uint8_t)above16; nvram[0x35] = (uint8_t)(above16 >> 8);
    uint16_t sum = 0;
    for (int i = 0x10; i <= 0x2D; i++) sum += nvram[i];
    nvram[0x2E] = (uint8_t)(sum >> 8); nvram[0x2F] = (uint8_t)sum;
    /* POST sets the BIOS tick count from the clock, which is where DOS
     * takes the time of day from (it was 00:00 at every boot) */
    if (pc.booted) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        uint32_t secs = (uint32_t)(tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
        uint32_t ticks = (uint32_t)((uint64_t)secs * 1193182u / 65536u);
        pc_wr16(c, PC_BDA_SEG, 0x6C, (uint16_t)ticks);
        pc_wr16(c, PC_BDA_SEG, 0x6E, (uint16_t)(ticks >> 16));
    }
}

static uint8_t cmos_read(uint8_t idx) {
    idx &= 0x7F;
    if (idx <= 0x0D || idx == 0x32) {
        uint64_t now = pc_now_ns();
        rtc_advance(now);
        switch (idx) {
        case 0x00: return enc(rtc.sec);
        case 0x01: return rtc.alarm[0];
        case 0x02: return enc(rtc.min);
        case 0x03: return rtc.alarm[1];
        case 0x04: return enc_hour(rtc.hour);
        case 0x05: return rtc.alarm[2];
        case 0x06: return enc(rtc.wday);
        case 0x07: return enc(rtc.mday);
        case 0x08: return enc(rtc.mon);
        case 0x09: return enc(rtc.year % 100);
        case 0x32: return enc(rtc.year / 100);
        case 0x0A: {
            uint8_t a = nvram[0x0A] & 0x7F;
            if (running() && now - rtc.second_ns >= NS_PER_S - UIP_NS) a |= RTC_A_UIP;
            return a;
        }
        case 0x0C: {                                    /* flags clear on read, and IRQF with them */
            uint8_t v = nvram[0x0C];
            nvram[0x0C] = 0;
            rtc_schedule();
            return v;
        }
        default: return nvram[idx];
        }
    }
    return nvram[idx];
}

int pc_cmos_port_read(uint16_t port, uint32_t *val) {
    if (port == 0x70) { *val = 0xFF; return 1; }  /* write-only */
    if (port == 0x71) { *val = cmos_read(index_reg); return 1; }
    return 0;
}

int pc_cmos_port_write(uint16_t port, uint32_t val) {
    if (port == 0x70) { index_reg = (uint8_t)(val & 0x7F); return 1; }   /* bit 7: NMI mask */
    if (port == 0x71) {
        uint8_t i = index_reg, v = (uint8_t)val;
        if (i <= 0x0D || i == 0x32) {
            uint64_t now = pc_now_ns();
            rtc_advance(now);
            switch (i) {
            case 0x00: rtc.sec = dec(v) % 60; rtc.second_ns = now; break;
            case 0x01: rtc.alarm[0] = v; break;
            case 0x02: rtc.min = dec(v) % 60; break;
            case 0x03: rtc.alarm[1] = v; break;
            case 0x04: rtc.hour = dec_hour(v) % 24; break;
            case 0x05: rtc.alarm[2] = v; break;
            case 0x06: rtc.wday = dec(v); break;
            case 0x07: rtc.mday = dec(v); break;
            case 0x08: rtc.mon = dec(v); break;
            case 0x09: rtc.year = rtc.year / 100 * 100 + dec(v) % 100; break;
            case 0x32: rtc.year = dec(v) * 100 + rtc.year % 100; break;
            case 0x0A: {                                /* UIP is read-only; a new rate starts its period now */
                uint64_t was = pf_period_ns();
                nvram[0x0A] = v & 0x7F;
                if (pf_period_ns() != was) rtc.pf_next_ns = now + pf_period_ns();
                break;
            }
            case 0x0B:
                if (v & RTC_B_SET) v &= (uint8_t)~RTC_B_UIE;     /* SET clears UIE */
                nvram[0x0B] = v;
                break;
            default: break;                             /* C and D are read-only */
            }
            rtc_advance(now);                           /* a newly enabled flag may raise IRQ 8 at once */
            return 1;
        }
        nvram[i] = v;
        return 1;
    }
    return 0;
}

/* ---- INT 15h memory services -------------------------------------------- */

/* A physical byte above the A20 gate's reach: the BIOS's block move runs
 * in protected mode, where the gate is open whatever DOS left it at. */
static uint8_t phys_rd(x86_cpu *c, uint32_t p) { return p < c->mem_size ? c->mem[p] : 0xFF; }
static void phys_wr(x86_cpu *c, uint32_t p, uint8_t v) {
    if (p >= c->mem_size) return;
    c->mem[p] = v;
    if (c->code_bitmap[p]) x86_store_hook(c, p);
}

/* AH=87h: move CX words between the source and destination descriptors
 * of the GDT at ES:SI (entries 2 and 3), 24-bit bases (32 on a 386). */
static void block_move(x86_cpu *c) {
    uint32_t gdt = ((uint32_t)c->seg[S_ES].sel << 4) + x86_get_r16(c, R_SI);
    uint32_t n = (uint32_t)x86_get_r16(c, R_CX) * 2;
    uint32_t base[2];
    for (int k = 0; k < 2; k++) {
        uint32_t d = gdt + 0x10 + 8u * (uint32_t)k;
        base[k] = x86_phys_rd8(c, d + 2) | (uint32_t)x86_phys_rd8(c, d + 3) << 8 | (uint32_t)x86_phys_rd8(c, d + 4) << 16;
        if (c->model >= X86_MODEL_386) base[k] |= (uint32_t)x86_phys_rd8(c, d + 7) << 24;
    }
    if (x86_get_r16(c, R_CX) > 0x8000) { x86_set_r8(c, R_AH, 0x01); c->eflags |= X86_CF; return; }
    for (uint32_t i = 0; i < n; i++) phys_wr(c, base[1] + i, phys_rd(c, base[0] + i));
    x86_set_r8(c, R_AH, 0);
    c->eflags &= ~X86_CF;
    c->eflags |= X86_ZF;
}

/* E820h: the address map, one range per call (EBX continues). */
static void e820(x86_cpu *c) {
    uint32_t top = mem_top(c);
    static const struct { uint32_t base, len, type; } fixed[2] = {
        { 0x00000, PC_CONV_KB * 1024u, 1 },       /* conventional memory */
        { 0xF0000, 0x10000, 2 },                  /* the ROM */
    };
    uint32_t idx = c->r[R_BX], base, len, type;
    if (idx < 2) { base = fixed[idx].base; len = fixed[idx].len; type = fixed[idx].type; }
    else if (idx == 2 && top > 0x100000u) { base = 0x100000u; len = top - 0x100000u; type = 1; }
    else { x86_set_r8(c, R_AH, 0x86); c->eflags |= X86_CF; return; }
    uint32_t buf = ((uint32_t)c->seg[S_ES].sel << 4) + x86_get_r16(c, R_DI);
    uint32_t v[5] = { base, 0, len, 0, type };
    for (int k = 0; k < 5; k++)
        for (int b = 0; b < 4; b++) x86_phys_wr8(c, buf + 4u * (uint32_t)k + (uint32_t)b, (uint8_t)(v[k] >> (8 * b)));
    int last = idx == 2 || (idx == 1 && top <= 0x100000u);
    c->r[R_AX] = 0x534D4150;                     /* 'SMAP' */
    c->r[R_CX] = 20;
    c->r[R_BX] = last ? 0 : idx + 1;
    c->eflags &= ~X86_CF;
}

/* The memory functions of INT 15h; 0 if AX is not one of them. Only a
 * booted machine has extended memory to report (see the top). */
int pc_int15_memory(x86_cpu *c) {
    int ah = x86_get_r8(c, R_AH);
    uint16_t ax = x86_get_r16(c, R_AX);
    uint32_t ext = pc_ext_kb(c);
    if (ah == 0x88) {
        x86_set_r16(c, R_AX, (uint16_t)(ext > 0xFFFF ? 0xFFFF : ext));
        c->eflags &= ~X86_CF;
        return 1;
    }
    if (ah == 0x87) {
        if (!pc.booted) return 0;
        block_move(c);
        return 1;
    }
    if (ax == 0xE801 && pc.booted) {
        uint32_t below16 = ext > 15360 ? 15360 : ext;              /* KB, 1 MB to 16 MB */
        uint32_t above16 = ext > 15360 ? (ext - 15360) / 64 : 0;   /* 64K blocks */
        x86_set_r16(c, R_AX, (uint16_t)below16); x86_set_r16(c, R_CX, (uint16_t)below16);
        x86_set_r16(c, R_BX, (uint16_t)above16); x86_set_r16(c, R_DX, (uint16_t)above16);
        c->eflags &= ~X86_CF;
        return 1;
    }
    if (ax == 0xE820 && pc.booted && c->model >= X86_MODEL_386 && c->r[R_DX] == 0x534D4150) {
        e820(c);
        return 1;
    }
    return 0;
}
