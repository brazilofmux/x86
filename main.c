/* main.c — dos-monster: run a DOS program on the HLE DOS/PC personality,
 * interpreted or translated, optionally in -V lockstep.
 *
 *   dos-monster [options] PROGRAM[.COM|.EXE] [args...]
 */
#include "core/x86.h"
#include "dbt/dbt.h"
#include "pc/pc.h"
#include "dos/dos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

static void usage(const char *prog) {
    printf("Usage: %s [options] PROGRAM[.COM|.EXE] [args...]\n\n", prog);
    printf("  -i          interpreter only\n");
    printf("  -j          JIT (default)\n");
    printf("  -V          JIT with lockstep verification against the interpreter\n");
    printf("  -S          with -V: verify after every block (no chaining)\n");
    printf("  -M N        with -V: compare guest memory every N block runs (default 1)\n");
    printf("  -m MODEL    cpu model: 86, 186, 286, 386 (default 286; DPMI clients need 386)\n");
    printf("  -C DIR      host directory to mount as C:\\ (default: PROGRAM's directory)\n");
    printf("  -A DIRS     mount A: (also -B); DIR1:DIR2:... is a diskette sequence, ESC-+ swaps\n");
    printf("  -fda IMG    diskette image as drive 00h (also -fdb); -hda IMG fixed disk 80h (also -hdb)\n");
    printf("  -ro         the images are read-only: the guest may write, the files never change\n");
    printf("  -boot a|c|IMG  boot the machine from A: or C: (IMG: -fda IMG -boot a), no HLE DOS\n");
    printf("  -t          full-screen terminal: paint the text buffer (default: echo console output)\n");
    printf("  -s          print statistics on exit\n");
    printf("  -d          trace DOS and DPMI calls (-d -d: every call, with registers)\n");
    printf("  -L N        stop after N instructions\n");
    printf("  -T SECS     stop after SECS seconds of wall-clock time\n");
    printf("  -D FILE     write the text screen to FILE on exit\n");
    printf("  -G FILE     write the screen (text mode, or mode 13h) to FILE as a PNG on exit\n");
    printf("  -w          the window is the display: text modes and graphics, from the start\n");
    printf("  -W          no window at all (default: one opens for graphics modes if stdout is a terminal)\n");
    printf("  -boot IMG   boot a disk image instead (sector 1 at 7C00; INT 13h serves the rest)\n");
    printf("  -pmtrace LO:HI  -i: dump state before each instruction in [LO,HI], QEMU -d cpu format\n");
    printf("  -pmring     -i: on the first exception, or a fetch outside memory, print the\n");
    printf("              last 400 CS:EIP and stop\n");
    printf("  -pmstop LIN -i: the same, on reaching linear address LIN\n");
    printf("  -h          this help\n");
}

static x86_dbt g_dbt;
static int g_stats;
static uint64_t g_last_insns, g_last_ns;

/* Host events between block runs; also keeps the HUD fresh. */
static double g_time_limit;       /* -T: stop after this many wall-clock seconds (0 = never) */

static int host_poll(x86_cpu *c) {
    int changed = pc_poll(c);
    if (g_time_limit > 0) {
        static uint64_t t0;
        if (!t0) t0 = pc.now_ns;
        if ((double)(pc.now_ns - t0) > g_time_limit * 1e9) { c->halted = 1; return 1; }
    }
    static int rate = -1;
    if (rate < 0) rate = getenv("X86_RATE") != NULL;
    if (rate) {
        static uint64_t li, ln, lf;
        uint64_t now = pc.now_ns;                 /* pc_poll just read the clock */
        if (now - ln > 500000000ull) {
            if (ln) fprintf(stderr, "[rate] %.1f MIPS  fallbacks %llu\n",
                            (double)(c->insn_count - li) / (double)(now - ln) * 1e3,
                            (unsigned long long)(g_dbt.interp_fallback_insns - lf));
            li = c->insn_count; ln = now; lf = g_dbt.interp_fallback_insns;
        }
    }
    if (pc.tty_mode) {
        uint64_t now = pc.now_ns;
        if (now - g_last_ns > 1000000000ull) {
            double bips = (double)(c->insn_count - g_last_insns) / (double)(now - g_last_ns);
            char hud[128];
            snprintf(hud, sizeof hud, " dos-monster  %.2f BIPS  %s", bips, g_dbt.cpu ? (g_dbt.verify ? "JIT -V" : "JIT") : "interp");
            pc_video_set_hud(hud);
            g_last_insns = c->insn_count; g_last_ns = now;
        }
    }
    return changed;
}

/* -pmtrace LO:HI — dump machine state before every instruction fetched from
 * that linear range, in the same shape QEMU's -d cpu emits, so the protected
 * mode oracle's harness can read our output with the parser it already has.
 * Restricted to a range for the same reason QEMU needs -dfilter: a whole boot
 * is gigabytes otherwise. */
static uint32_t g_pmtrace_lo, g_pmtrace_hi;
static uint64_t g_pmtrace_budget = 200000;

static uint64_t g_pmtrace_left;    /* shared budget: see pm_trace */

static void pm_trace_exc(x86_cpu *c, int vec, uint32_t err) {
    /* QEMU's -d int shape, so the oracle harness reads it with one parser. */
    if (g_pmtrace_left == 0) return;
    fprintf(stderr, "     0: v=%02x e=%04x i=0 cpl=%d IP=%04x:%08x pc=%08x SP=%04x:%08x\n",
            vec, err, c->seg[S_CS].sel & 3, c->seg[S_CS].sel, c->eip,
            c->seg[S_CS].base + c->eip, c->seg[S_SS].sel, c->r[R_SP]);
}

/* Bounded: a kernel that faults in a loop would otherwise write gigabytes
 * before anyone notices it is stuck. */
static void pm_trace(x86_cpu *c) {
    if (g_pmtrace_left == 0) return;
    if (--g_pmtrace_left == 0) {
        fprintf(stderr, "pmtrace: dump limit reached, stopping trace\n");
        return;
    }
    static const char *sname[6] = { "ES", "CS", "SS", "DS", "FS", "GS" };
    static const int sorder[6] = { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS };
    fprintf(stderr, "EAX=%08x EBX=%08x ECX=%08x EDX=%08x\n",
            c->r[R_AX], c->r[R_BX], c->r[R_CX], c->r[R_DX]);
    fprintf(stderr, "ESI=%08x EDI=%08x EBP=%08x ESP=%08x\n",
            c->r[R_SI], c->r[R_DI], c->r[R_BP], c->r[R_SP]);
    fprintf(stderr, "EIP=%08x EFL=%08x [-------] CPL=%d II=0 A20=%d SMM=0 HLT=%d\n",
            c->eip, c->eflags, c->seg[S_CS].sel & 3, c->a20_mask != 0xFFFFFu, c->halted);
    for (int i = 0; i < 6; i++) {
        const x86_seg *g = &c->seg[sorder[i]];
        fprintf(stderr, "%s =%04x %08x %08x %08x\n", sname[i], g->sel, g->base, g->limit,
                (uint32_t)g->attr << 8);
    }
    /* No LDTR/TR/GDTR/IDTR state yet: printed as zero so the record parses. */
    fprintf(stderr, "LDT=0000 00000000 00000000 00000000\n");
    fprintf(stderr, "TR =0000 00000000 00000000 00000000\n");
    fprintf(stderr, "GDT=     00000000 00000000\n");
    fprintf(stderr, "IDT=     00000000 00000000\n");
    fprintf(stderr, "CR0=%08x CR2=00000000 CR3=00000000 CR4=00000000\n", c->pmode ? 0x11u : 0x10u);
}

/* -pmring: a ring of the last executed CS:EIP, dumped when the run stops.
 * A client that walks into garbage without faulting leaves no other trace
 * of where it left its own code. */
#define PMRING 400
static int g_pmring;
static uint32_t g_pmstop;
static struct { uint16_t cs, ss; uint32_t eip, lin, esp; } pmring[PMRING];
static uint64_t pmring_n;

static void pmring_dump(x86_cpu *c) {
    if (!g_pmring) return;
    fprintf(stderr, "last %d instructions:\n", PMRING);
    uint64_t first = pmring_n > PMRING ? pmring_n - PMRING : 0;
    for (uint64_t i = first; i < pmring_n; i++) {
        int k = (int)(i % PMRING);
        fprintf(stderr, "  %04X:%08X sp %04X:%08X ", pmring[k].cs, pmring[k].eip, pmring[k].ss, pmring[k].esp);
        for (int b = 0; b < 8; b++)
            fprintf(stderr, " %02X", (unsigned)x86_rd(c, pmring[k].lin, (uint32_t)b, 0xFFFFFFFFu, 1));
        fprintf(stderr, "\n");
    }
}

/* With -pmring, the first protected-mode exception is the interesting one:
 * a client that installs its own handler turns every later one into noise. */
static void pmring_exc(x86_cpu *c, int vec, uint32_t err) {
    if (g_pmstop) return;                  /* -pmstop wants the run-up to its address, not this */
    fprintf(stderr, "pmring: exception %02X err %04X at %04X:%08X\n",
            vec, err, c->seg[S_CS].sel, c->eip);
    fprintf(stderr, "  EAX=%08X ECX=%08X EDX=%08X EBX=%08X ESP=%08X EBP=%08X ESI=%08X EDI=%08X\n",
            c->r[R_AX], c->r[R_CX], c->r[R_DX], c->r[R_BX], c->r[R_SP], c->r[R_BP], c->r[R_SI], c->r[R_DI]);
    fprintf(stderr, "  ES=%04X CS=%04X SS=%04X DS=%04X FS=%04X GS=%04X\n",
            c->seg[S_ES].sel, c->seg[S_CS].sel, c->seg[S_SS].sel,
            c->seg[S_DS].sel, c->seg[S_FS].sel, c->seg[S_GS].sel);
    fprintf(stderr, "  [ebp]:");
    for (int k = 0; k < 24; k += 4)
        fprintf(stderr, " +%X=%08X", k, x86_rd(c, c->seg[S_SS].base, c->r[R_BP] + (uint32_t)k, 0xFFFFFFFFu, 4));
    fprintf(stderr, "\n");
    pmring_dump(c);
    c->halted = 1;
}

static int run_interp(x86_cpu *c, uint64_t limit) {
    uint32_t n = 0;
    uint64_t waits = pc.blocked_calls;
    for (;;) {
        if (g_pmring) {
            int k = (int)(pmring_n++ % PMRING);
            pmring[k].cs = c->seg[S_CS].sel; pmring[k].eip = c->eip;
            pmring[k].lin = c->seg[S_CS].base + c->eip;
            pmring[k].ss = c->seg[S_SS].sel; pmring[k].esp = c->r[R_SP];
            /* Fetching from open bus means the guest already lost control;
             * the interesting part is the run-up, not the wreck. */
            if (g_pmstop && pmring[k].lin == g_pmstop) {
                fprintf(stderr, "pmring: reached %08X\n", g_pmstop);
                pmring_dump(c);
                return 0;
            }
            if (pmring[k].lin >= c->mem_size) {
                fprintf(stderr, "pmring: fetch outside memory at %04X:%08X (linear %08X)\n",
                        pmring[k].cs, pmring[k].eip, pmring[k].lin);
                pmring_dump(c);
                return 0;
            }
        }
        /* Every 4096 instructions, or at once after the keyboard idle wait
         * (1 ms a poll: 4096 instructions of a polling loop are seconds). */
        if ((n++ & 4095) == 0 || c->halted || pc.blocked_calls != waits) {
            waits = pc.blocked_calls;
            host_poll(c); if (c->halted) return 0;
        }
        if (limit && c->insn_count >= limit) { pmring_dump(c); return 0; }
        if (g_pmtrace_hi) {
            uint32_t lin = c->seg[S_CS].base + c->eip;
            if (lin >= g_pmtrace_lo && lin <= g_pmtrace_hi) pm_trace(c);
        }
        int exc_before = c->exc;
        (void)exc_before;
        int rc = x86_step(c);
        if (rc < 0) {
            fprintf(stderr, "interp: stopped at %04X:%04X\n", c->seg[S_CS].sel, c->eip);
            return -1;
        }
        if (rc > 0 && c->halted && !(c->eflags & X86_IF)) return 0;
    }
}

int main(int argc, char **argv) {
    int use_jit = 1, verify = 0, strict = 0, model = X86_MODEL_286, tty = 0, debug = 0;
    uint64_t limit = 0;
    int mem_every = 0;          /* -M N: whole-memory -V compare every N block runs (0: the DBT default) */
    const char *root = NULL, *prog = NULL, *dump = NULL, *gdump = NULL, *drive_a = NULL, *drive_b = NULL;
    int window = -1;                         /* -w / -W; -1: a window if stdout is a terminal */
    const char *boot_img = NULL, *img_fd[2] = { 0 }, *img_hd[2] = { 0 };
    int img_ro = 0;
    int i;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-' || !strcmp(argv[i], "-")) break;
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else if (!strcmp(argv[i], "-i")) use_jit = 0;
        else if (!strcmp(argv[i], "-j")) use_jit = 1;
        else if (!strcmp(argv[i], "-V")) { use_jit = 1; verify = 1; }
        else if (!strcmp(argv[i], "-S")) strict = 1;
        else if (!strcmp(argv[i], "-t")) tty = 1;
        else if (!strcmp(argv[i], "-s")) g_stats = 1;
        else if (!strcmp(argv[i], "-d")) debug++;
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            int m = atoi(argv[++i]);
            model = m == 86 ? X86_MODEL_8086 : m == 186 ? X86_MODEL_186 : m == 286 ? X86_MODEL_286 : X86_MODEL_386;
        }
        else if (!strcmp(argv[i], "-C") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "-A") && i + 1 < argc) drive_a = argv[++i];
        else if (!strcmp(argv[i], "-B") && i + 1 < argc) drive_b = argv[++i];
        else if (!strcmp(argv[i], "-L") && i + 1 < argc) limit = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) g_time_limit = atof(argv[++i]);
        else if (!strcmp(argv[i], "-D") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "-G") && i + 1 < argc) gdump = argv[++i];
        else if (!strcmp(argv[i], "-w")) window = 1;
        else if (!strcmp(argv[i], "-W")) window = 0;
        else if (!strcmp(argv[i], "-M") && i + 1 < argc) mem_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-boot") && i + 1 < argc) boot_img = argv[++i];
        else if (!strcmp(argv[i], "-fda") && i + 1 < argc) img_fd[0] = argv[++i];
        else if (!strcmp(argv[i], "-fdb") && i + 1 < argc) img_fd[1] = argv[++i];
        else if (!strcmp(argv[i], "-hda") && i + 1 < argc) img_hd[0] = argv[++i];
        else if (!strcmp(argv[i], "-hdb") && i + 1 < argc) img_hd[1] = argv[++i];
        else if (!strcmp(argv[i], "-ro")) img_ro = 1;
        else if (!strcmp(argv[i], "-pmring")) g_pmring = 1;
        else if (!strcmp(argv[i], "-pmstop") && i + 1 < argc) { g_pmring = 1; g_pmstop = (uint32_t)strtoul(argv[++i], NULL, 0); }
        else if (!strcmp(argv[i], "-pmtrace") && i + 1 < argc) {
            const char *a = argv[++i];
            g_pmtrace_lo = (uint32_t)strtoul(a, NULL, 0);
            const char *colon = strchr(a, ':');
            g_pmtrace_hi = colon ? (uint32_t)strtoul(colon + 1, NULL, 0) : g_pmtrace_lo;
            g_pmtrace_left = g_pmtrace_budget;
        }
        else { fprintf(stderr, "unknown option %s\n", argv[i]); usage(argv[0]); return 2; }
    }
    x86_cpu cpu;
    int boot_drive = -1;
    if (boot_img) {
        if (!strcmp(boot_img, "a") || !strcmp(boot_img, "A")) boot_drive = 0x00;
        else if (!strcmp(boot_img, "c") || !strcmp(boot_img, "C")) boot_drive = 0x80;
        else { img_fd[0] = boot_img; boot_drive = 0x00; }
    } else if (i >= argc && (img_fd[0] || img_hd[0])) {
        boot_drive = img_fd[0] ? 0x00 : 0x80;        /* images and no program: boot them */
    }
    for (int k = 0; k < 2; k++) {
        if (img_fd[k] && pc_disk_attach(k, img_fd[k], img_ro) < 0) return 1;
        if (img_hd[k] && pc_disk_attach(0x80 | k, img_hd[k], img_ro) < 0) return 1;
    }
    if (boot_drive >= 0) {
        /* Boot the machine as a BIOS would: sector one of the boot drive
         * at 0000:7C00 in real mode, DL the drive, and everything after
         * it through INT 13h. */
        if (!pc_disk_present(boot_drive)) { fprintf(stderr, "-boot: no image for drive %02Xh\n", boot_drive); return 1; }
        x86_init(&cpu, model);
        pc_init(&cpu, tty);
        pc_disk_install(&cpu);
        pc.debug = debug;
        pc.booted = 1;
        pc.boot_drive = boot_drive;
        pc.swap_disk = pc_disk_swap;
        pc_disk_boot(&cpu, boot_drive);
        x86_load_seg(&cpu, S_CS, 0);
        x86_load_seg(&cpu, S_DS, 0);
        x86_load_seg(&cpu, S_ES, 0);
        x86_load_seg(&cpu, S_SS, 0);
        cpu.eip = 0x7C00;
        cpu.r[R_SP] = 0x7C00;
        cpu.r[R_DX] = (uint32_t)boot_drive;
        x86_set_a20(&cpu, 1);                  /* SeaBIOS (the oracle's QEMU) boots with A20 on */
    } else {
        if (i >= argc) { usage(argv[0]); return 2; }
        prog = argv[i++];

        /* Command tail: the remaining arguments joined with spaces. */
        char args[128] = "";
        for (int k = i; k < argc; k++) {
            if (strlen(args) + strlen(argv[k]) + 2 > sizeof args) break;
            strcat(args, " ");
            strcat(args, argv[k]);
        }

        /* Split the program path into host directory (drive root) and name. */
        char progdir[PATH_MAX], progname[256], host_prog[PATH_MAX];
        const char *slash = strrchr(prog, '/');
        if (slash) { snprintf(progdir, sizeof progdir, "%.*s", (int)(slash - prog), prog); snprintf(progname, sizeof progname, "%s", slash + 1); }
        else { strcpy(progdir, "."); snprintf(progname, sizeof progname, "%s", prog); }
        if (!root) root = progdir;
        char root_abs[PATH_MAX];
        if (!realpath(root, root_abs)) { perror(root); return 1; }
        snprintf(host_prog, sizeof host_prog, "%s", prog);
        struct stat st;
        if (stat(host_prog, &st) != 0) {
            /* try .COM / .EXE */
            snprintf(host_prog, sizeof host_prog, "%s.exe", prog);
            if (stat(host_prog, &st) != 0) snprintf(host_prog, sizeof host_prog, "%s.com", prog);
            if (stat(host_prog, &st) != 0) { fprintf(stderr, "%s: not found\n", prog); return 1; }
            snprintf(progname, sizeof progname, "%s", strrchr(host_prog, '/') ? strrchr(host_prog, '/') + 1 : host_prog);
        }
        char dos_name[64]; size_t n = 0;
        for (const char *p = progname; *p && n + 1 < sizeof dos_name; p++) dos_name[n++] = (char)toupper((unsigned char)*p);
        dos_name[n] = 0;

        x86_init(&cpu, model);
        pc_init(&cpu, tty);
        pc_disk_install(&cpu);
        pc.debug = debug;
        dos_init(&cpu, root_abs);
        if (drive_a && dos_mount(0, drive_a) < 0) return 1;
        if (drive_b && dos_mount(1, drive_b) < 0) return 1;
        pc.swap_disk = dos_swap_disk;
        /* Start on the drive and in the directory that hold the program (A:
         * for an installer, C:\WP51 for WP), so the PSP environment names it
         * the way DOS would: C:\WP51\WP.EXE. */
        char dos_path[DOS_MAX_PATH];
        snprintf(dos_path, sizeof dos_path, "%s", dos_name);
        {
            char pd[PATH_MAX];
            if (realpath(progdir, pd))
                for (int d = 0; d < 26; d++) {
                    const char *r = dos.drives[d].root;
                    size_t rl = strlen(r);
                    if (!r[0] || strncmp(pd, r, rl) != 0 || (pd[rl] && pd[rl] != '/')) continue;
                    dos.cur_drive = d;
                    char cwd[DOS_MAX_PATH]; size_t n = 0;
                    for (const char *p = pd + rl; *p && n + 1 < sizeof cwd; p++) cwd[n++] = (char)(*p == '/' ? '\\' : toupper((unsigned char)*p));
                    cwd[n] = 0;
                    snprintf(dos.drives[d].cwd, sizeof dos.drives[d].cwd, "%s", cwd[0] ? cwd : "\\");
                    snprintf(dos_path, sizeof dos_path, "%s\\%s", cwd, dos_name);
                    break;
                }
        }
        if (dos_load_program(&cpu, host_prog, dos_path, args) < 0) return 1;
    }

    if (use_jit && !dbt_jit_available(&cpu)) {
        fprintf(stderr, "dos-monster: JIT unavailable for this model/host; using the interpreter\n");
        use_jit = 0;
    }
    if (strict) setenv("X86_VERIFY_STRICT", "1", 1);

    pc_sdl_allow(window >= 0 ? window : isatty(1), prog ? prog : boot_img ? boot_img : img_hd[0] ? img_hd[0] : img_fd[0]);
    pc_sdl_text(window == 1);
    /* A mouse to take input from: the window, the -t terminal's mouse
     * reports, or a script's (X86_MOUSE=1: SGR reports on stdin). */
    if (pc_sdl_window_allowed() || tty || getenv("X86_MOUSE")) {
        pc_mouse_install(&cpu);
        if (tty) pc_kbd_mouse_reporting();
    }
    if (g_pmtrace_hi) cpu.trace_exc = pm_trace_exc;
    else if (g_pmring) cpu.trace_exc = pmring_exc;

    uint64_t t0 = pc_now_ns();
    g_last_ns = t0;
    int rc;
    if (use_jit) {
        if (dbt_init(&g_dbt, &cpu) < 0) return 1;
        g_dbt.verify = verify;
        if (mem_every > 0) g_dbt.verify_mem_every = mem_every;
        g_dbt.insn_limit = limit;
        g_dbt.poll = host_poll;
        dbt_sample_start();
    }
    for (;;) {
        rc = use_jit ? dbt_run(&g_dbt) : run_interp(&cpu, limit);
        if (rc < 0 || !pc.reboot) break;
        pc_reboot(&cpu);                       /* the booted machine reset itself */
        if (use_jit) { dbt_cache_invalidate_all(&g_dbt); g_dbt.shadow_stale = 1; }
    }
    uint64_t t1 = pc_now_ns();

    pc_video_flush(1);
    pc_video_shutdown();
    pc_sdl_shutdown();
    pc_kbd_shutdown();
    if (!tty) fflush(stdout);
    if (dump) {
        FILE *f = strcmp(dump, "-") ? fopen(dump, "w") : stdout;
        if (f) { pc_video_dump(&cpu, f); if (f != stdout) fclose(f); }
    }
    /* X86_MEMDUMP=FILE: guest memory (the first 1 MB + 64K) at exit, for
     * disassembling where a run ended (ndisasm -o, -e) */
    if (getenv("X86_MEMDUMP")) {
        FILE *f = fopen(getenv("X86_MEMDUMP"), "wb");
        if (f) { fwrite(cpu.mem, 1, cpu.mem_size < 0x110000 ? cpu.mem_size : 0x110000, f); fclose(f); }
    }
    if (gdump && pc_video_png(&cpu, gdump) < 0)
        fprintf(stderr, "-G %s: the screen is in neither a text mode nor mode 13h\n", gdump);

    if (rc < 0) {
        fprintf(stderr, "dos-monster: run failed\n");
        x86_dump(&cpu, stderr);
    }
    if (g_stats) {
        double s = (double)(t1 - t0) / 1e9;
        double blocked = (double)pc.blocked_ns / 1e9;
        fprintf(stderr, "insns: %llu in %.3fs = %.1f MIPS\n", (unsigned long long)cpu.insn_count, s,
                (double)cpu.insn_count / s / 1e6);
        /* Time spent deliberately idle (waiting on stdin, honouring a guest
         * delay) is not emulation cost: the rate excluding it is the one
         * that says how fast we actually run. */
        if (pc.blocked_ns)
            fprintf(stderr, "  host idle:              %.3fs in %llu waits  → %.1f MIPS while running\n",
                    blocked, (unsigned long long)pc.blocked_calls,
                    s > blocked ? (double)cpu.insn_count / (s - blocked) / 1e6 : 0.0);
        fprintf(stderr, "  timer: %llu IRQ 0s, %.1f Hz average\n", (unsigned long long)pc.ticks_delivered,
                s > 0 ? (double)pc.ticks_delivered / s : 0.0);
        if (use_jit) dbt_print_stats(&g_dbt, stderr);
        { extern uint64_t pc_vga_stores; if (pc_vga_stores) fprintf(stderr, "  VGA planar stores:      %llu\n", (unsigned long long)pc_vga_stores); }
        pc_svcprof_dump(stderr);
        fprintf(stderr, "final: "); x86_dump(&cpu, stderr);
    }
    if (use_jit) dbt_sample_report(&g_dbt, stderr);
    if (use_jit) dbt_cleanup(&g_dbt);
    /* X86_MEM_DUMP=<path>: the whole guest memory at exit, for locating
     * hot blocks from a profile (ndisasm -b 32 -o ADDR -e ADDR). */
    if (getenv("X86_MEM_DUMP")) {
        FILE *f = fopen(getenv("X86_MEM_DUMP"), "wb");
        if (f) { fwrite(cpu.mem, 1, cpu.mem_size, f); fclose(f); }
    }
    x86_free(&cpu);
    return rc < 0 ? 1 : (pc.exit_requested ? pc.exit_code : 0);
}
