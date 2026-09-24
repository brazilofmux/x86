# dos-monster

An 8086–80386 PC emulator built around a dynamic binary translator: x86
real mode, protected mode, virtual-8086 mode and paging, translated to
AArch64 at run time, with the goal of running DOS software at billions of
instructions per second. It boots FreeDOS, MS-DOS 6.22 and Windows 3.11 in
386 enhanced mode from disk images, and runs DOOM, WordPerfect 5.1, Turbo
Pascal and Microsoft COBOL.

It is the successor to [z80](https://github.com/brazilofmux/z80) (Z80 +
CP/M at 4.3 BIPS), [slow-32](https://github.com/brazilofmux/slow32-public)
and [riscv](https://github.com/brazilofmux/riscv), and reuses their
translator's design: pinned guest registers, two-phase translation with
dead-flag elimination, block chaining, a span-gated self-modifying-code
bitmap, and lockstep verification against an interpreter.

| | |
|---|---|
| ![Windows 3.11, 386 enhanced mode](docs/img/win311-enhanced.png) | ![An MS-DOS Prompt VM under Windows](docs/img/win311-dos-vm.png) |
| Windows 3.11 in 386 enhanced mode | An MS-DOS Prompt: a V86 VM beside Windows |
| ![DOOM under EMM386](docs/img/doom-emm386.png) | ![WordPerfect 5.1](docs/img/wp51-msdos.png) |
| DOOM (DOS/4GW via VCPI) under MS-DOS + EMM386 | WordPerfect 5.1 under MS-DOS 6.22 |

## What runs

Two ways to run things:

- **As a PC booted from disk images** (`-boot`): a BIOS, INT 13h over
  diskette and hard-disk images, CMOS, the 8259/8254/8042, a VGA (text,
  mode 13h and unchained, 16-colour planar modes), A20, extended memory.
  Real DOS runs on it:
  - FreeDOS 1.3 — installed by its own installer; under HIMEMX, JEMMEX
    and JEMM386
  - MS-DOS 6.22 — installed by its own Setup from diskette images; under
    HIMEM and EMM386 (UMBs, EMS)
  - Windows 3.11 — installed by its own Setup; standard mode and 386
    enhanced mode, with MS-DOS Prompt VMs
  - DOOM (DOS/4GW) under all of the above: raw XMS, VCPI under EMM386,
    and DPMI inside a Windows DOS box
- **As a DOS program runner** (the default): an emulated DOS (INT 21h in
  the host, a host directory as C:, a DPMI host of our own) for running a
  program headless — WordPerfect 5.1, Turbo Pascal 5.5, MS COBOL 5.0,
  DJGPP programs, DOOM. This is the bring-up and test harness; real DOS
  from images is the target.

## How fast

On an Apple Silicon Mac, DOOM's `-timedemo demo1` (5026 frames):

| | realtics |
|---|---|
| bare MS-DOS 6.22 (HIMEM) | 93 (≈1,900 frames/s) |
| Windows 3.11 enhanced mode, MS-DOS Prompt | 174 |
| MS-DOS 6.22 + EMM386 | 180 |

Power-on to `C:\>` is 2.5 s (2 of them MS-DOS's own F5/F8 pause), and
`win` to a drawn Program Manager 0.8 s. `tests/boot/bench.sh` measures
these.

## Building

Developed and tested on macOS on Apple Silicon. The translator emits
AArch64; elsewhere `-i` (the interpreter) is what runs.

    make                 # dos-monster, tools/sst, tools/jittest

Needs a C11 compiler and zlib; SDL2 (via `pkg-config`) adds a window for
graphics modes and is optional — without it the machine is headless.
The tests also use `nasm`, `python3`, `mtools` and `qemu-system-i386`.

## Running

    ./dos-monster -m 386 -C ~/dos WP.EXE              # a program, emulated DOS
    ./dos-monster -m 386 -hda c.img -boot c           # boot a hard-disk image
    ./dos-monster -m 386 -fda a1.img:a2.img -boot a   # diskettes; ESC + swaps

`-V` runs the translator in lockstep with the interpreter and stops at
the first difference; `-s` prints statistics; `-h` lists the rest.

## Disk images

No DOS software is included — bring your own copies into `disks/`
(git-ignored). `tests/boot/README` has the recipes: where FreeDOS 1.3
comes from and how its installer is driven, and
`tests/boot/msinstall.sh` / `tests/boot/wininstall.sh`, which install
MS-DOS 6.22 and Windows 3.11 from diskette images end to end, unattended.

## Testing

- `make test-sst` — the interpreter against the
  [SingleStepTests](https://github.com/SingleStepTests) 8088 suite
  (`tests/sst8088/fetch.sh`; 286 and 386 suites too)
- `make test-jit` — the translator against the interpreter, instruction
  sequences fuzzed in real mode, flat and segmented protected mode
- `make test-pm`, `test-pm-compare`, `test-pg` — protected mode, paging
  and V86 mode against QEMU (`tools/pmoracle`: boot images that log
  their observations)
- `make test-dos` — DOS programs under the interpreter, the JIT and `-V`
- `make test-boot` — FreeDOS, MS-DOS and Windows booted from images and
  driven by `tools/expect.py`

## Layout

    core/   the x86: decoder, interpreter, paging
    dbt/    the translator (AArch64)
    pc/     the machine: BIOS, VGA, keyboard, timer, disks, CMOS, mouse
    dos/    the emulated DOS, MZ loader, DPMI host
    tools/  oracles, fuzzers, expect.py, the native BIOS routine
    tests/  DOS test programs, boot tests, test-suite runners

`CLAUDE.md` is the design document: scope, architecture, and the rules
learned the hard way.

## License

MIT — see `LICENSE`.
