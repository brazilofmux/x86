# DOOM's timedemo against other emulators

The comparison in the top-level README, recorded so it can be rerun and
argued with. `doom-timedemo.sh` here runs it: dos-monster, Bochs and
DOSBox-X one after another on the same host (`-q` adds QEMU), each
printing DOOM's own `timed 5026 gametics in N realtics`. A realtic is
1/35 s of the guest's clock, and every one of these keeps its guest
clock to the host's, so realtics are wall time for the demo itself.

## The workload

- DOOM shareware **v1.9**: `DOOM.EXE` 709,905 bytes (MD5
  `e2382b7dc47ae2433d26b6e6bc312999`), `DOOM1.WAD` 4,196,020 bytes (MD5
  `f0cefca49926d00903cf57551d901abe`).
- `doom -timedemo demo1`: 5026 gametics, the whole of demo 1.
- No `DEFAULT.CFG`: each run starts from a folder holding only those two
  files, so DOOM's built-in defaults apply (sound and music off — no card
  is configured — screen size and detail at their defaults).
- 16 MB of guest memory in Bochs, DOSBox-X and QEMU; dos-monster's
  default machine has 17 MB (the first MB and 16 above it).

## Each emulator

**dos-monster** (`-m 386 -W -hda c.img -boot c`): boots MS-DOS 6.22 from
`disks/msdos622/c.img` as `tests/boot/msinstall.sh` leaves it, with a
`\DOOM` folder added:

    CONFIG.SYS   DEVICE=C:\DOS\SETVER.EXE / DEVICE=C:\DOS\HIMEM.SYS / DOS=HIGH / FILES=30
    AUTOEXEC.BAT C:\DOS\SMARTDRV.EXE /X / PROMPT $p$g / PATH C:\DOS / SET TEMP=C:\DOS

then `tools/expect.py` types `cd \doom` and `doom -timedemo demo1` at
the prompt. `-W`: no window (the VGA is emulated, nothing is drawn on the
host). A 386 with no FPU, as DOOM needs none.

**Bochs 3.1** (Homebrew) boots the same image, with `timedemo.bat` as its
AUTOEXEC.BAT (so no SMARTDRV; the timing starts after the WAD is read
either way), and `bochsrc` here: 16 MB, the disk as 260/16/63 with no
translation, `clock: sync=realtime`, no display, and **no `cpu:` line —
Bochs's default CPU model**. The script polls the image for `C:\TD.TXT`.

**DOSBox-X 2026.08.31** (Homebrew) runs **its own built-in DOS, not
MS-DOS 6.22**: `dosbox-x.conf` here mounts a host folder with the two
files as C:, `memsize=16`, `cputype=auto`, `cycles=max`,
`core=dynamic` (on arm64 the same core as `dynamic_rec`, DOSBox-X's
dynrec: the two measure alike) or `core=normal`, and `output=surface`
with **its window on screen** — it has no headless mode, so it is the
one of the four that draws every frame to the host's display. Run as
`dosbox-x -conf dosbox-x.conf -nomenu -fastlaunch`; the conf exits when
the demo ends.

**QEMU 11** (`qemu-system-i386 -m 16 -drive file=c.img,format=raw
-display none`, TCG) boots the same image as Bochs. On this Mac it has
never finished: still mid-demo after 90 s in three runs on 2026-09-24,
and the runs wedged. `-q` tries again with a 10-minute limit.

## Results

Apple Silicon Mac: Mac17,6, Apple M5 Max, macOS 26.6.2.

2026-09-25, `doom-timedemo.sh`:

| | runs (realtics) | median | vs dos-monster |
|---|---|---|---|
| dos-monster (a007727 era, AArch64) | 92, 93, 94 | **93** | |
| DOSBox-X, `core=dynamic` / `dynamic_rec` | 637, 661, 672, 676 / 647, 680 | 667 | 7.2× |
| DOSBox-X, `core=normal` | 1,154 | 1,154 | 12.4× |
| Bochs 3.1, default CPU | 1,224, 1,217, 1,229 | 1,224 | 13.2× |

2026-09-24, the first measurement (in the README until 2026-09-25):
dos-monster 93, DOSBox-X dynamic 494, DOSBox-X normal 1,270, Bochs 1,347 —
one run each. The DOSBox-X dynamic number has not reproduced since with
the same configuration (637–680 in six runs); nothing in the recorded
settings differs, so the likeliest difference is the window, which
DOSBox-X draws into every frame (on screen, occluded or minimized all
cost differently). Treat DOSBox-X's numbers as depending on that.

One workload, one machine: a data point, not a ranking.
