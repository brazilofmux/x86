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
    printf("  -m MODEL    cpu model: 86, 186, 286 (default 286)\n");
    printf("  -C DIR      host directory to mount as C:\\ (default: PROGRAM's directory)\n");
    printf("  -A DIRS     mount A: (also -B); DIR1:DIR2:... is a diskette sequence, ESC-+ swaps\n");
    printf("  -t          full-screen terminal: paint the text buffer (default: echo console output)\n");
    printf("  -s          print statistics on exit\n");
    printf("  -d          trace DOS calls\n");
    printf("  -L N        stop after N instructions\n");
    printf("  -D FILE     write the text screen to FILE on exit\n");
    printf("  -h          this help\n");
}

static x86_dbt g_dbt;
static int g_stats;
static uint64_t g_last_insns, g_last_ns;

/* Host events between block runs; also keeps the HUD fresh. */
static int host_poll(x86_cpu *c) {
    int changed = pc_poll(c);
    static int rate = -1;
    if (rate < 0) rate = getenv("X86_RATE") != NULL;
    if (rate) {
        static uint64_t li, ln, lf;
        uint64_t now = pc_now_ns();
        if (now - ln > 500000000ull) {
            if (ln) fprintf(stderr, "[rate] %.1f MIPS  fallbacks %llu\n",
                            (double)(c->insn_count - li) / (double)(now - ln) * 1e3,
                            (unsigned long long)(g_dbt.interp_fallback_insns - lf));
            li = c->insn_count; ln = now; lf = g_dbt.interp_fallback_insns;
        }
    }
    if (pc.tty_mode) {
        uint64_t now = pc_now_ns();
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

static int run_interp(x86_cpu *c, uint64_t limit) {
    uint32_t n = 0;
    for (;;) {
        if ((n++ & 4095) == 0 || c->halted) { host_poll(c); if (c->halted) return 0; }
        if (limit && c->insn_count >= limit) return 0;
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
    int mem_every = 1;
    const char *root = NULL, *prog = NULL, *dump = NULL, *drive_a = NULL, *drive_b = NULL;
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
        else if (!strcmp(argv[i], "-D") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "-M") && i + 1 < argc) mem_every = atoi(argv[++i]);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); usage(argv[0]); return 2; }
    }
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

    x86_cpu cpu;
    x86_init(&cpu, model);
    pc_init(&cpu, tty);
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

    if (use_jit && !dbt_jit_available(&cpu)) {
        fprintf(stderr, "dos-monster: JIT unavailable for this model/host; using the interpreter\n");
        use_jit = 0;
    }
    if (strict) setenv("X86_VERIFY_STRICT", "1", 1);

    uint64_t t0 = pc_now_ns();
    g_last_ns = t0;
    int rc;
    if (use_jit) {
        if (dbt_init(&g_dbt, &cpu) < 0) return 1;
        g_dbt.verify = verify;
        g_dbt.verify_mem_every = mem_every;
        g_dbt.insn_limit = limit;
        g_dbt.poll = host_poll;
        rc = dbt_run(&g_dbt);
    } else {
        rc = run_interp(&cpu, limit);
    }
    uint64_t t1 = pc_now_ns();

    pc_video_flush(1);
    pc_video_shutdown();
    pc_kbd_shutdown();
    if (!tty) fflush(stdout);
    if (dump) {
        FILE *f = strcmp(dump, "-") ? fopen(dump, "w") : stdout;
        if (f) { pc_video_dump(&cpu, f); if (f != stdout) fclose(f); }
    }

    if (rc < 0) {
        fprintf(stderr, "dos-monster: run failed\n");
        x86_dump(&cpu, stderr);
    }
    if (g_stats) {
        double s = (double)(t1 - t0) / 1e9;
        fprintf(stderr, "insns: %llu in %.3fs = %.1f MIPS\n", (unsigned long long)cpu.insn_count, s,
                (double)cpu.insn_count / s / 1e6);
        if (use_jit) dbt_print_stats(&g_dbt, stderr);
        fprintf(stderr, "final: "); x86_dump(&cpu, stderr);
    }
    if (use_jit) dbt_cleanup(&g_dbt);
    x86_free(&cpu);
    return rc < 0 ? 1 : (pc.exit_requested ? pc.exit_code : 0);
}
