# dos-monster

An 8086–80486 PC emulator built around a dynamic binary translator: x86
real mode, protected mode, virtual-8086 mode and paging, and the 387/486
floating-point unit, translated to
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

The same timedemo, same machine, bare MS-DOS in each:

| | realtics | |
|---|---|---|
| **dos-monster** | **93** | |
| DOSBox-X 2026.08, `core=dynamic` (dynrec), `cycles=max` | 494 | 5.3× |
| Bochs 3.1 (`clock: sync=realtime`) | 1,347 | 14.5× |
| DOSBox-X, `core=normal` | 1,270 | 13.7× |

QEMU 11 (TCG, `qemu-system-i386`) was still in the middle of the demo
after 90 seconds, and none of three runs lived to print a result, so it
has no number here. One workload, one machine: take it as a data point.

### Why it is fast

A translator is fast when the code it emits does only the guest's work.
What dos-monster does to get there:

- **Guest registers live in host registers.** AX–DI, the DS/ES/SS
  segment bases, FLAGS, the guest-memory base and the instruction
  counter are pinned to AArch64 registers for the life of translated
  code. A guest `ADD AX,BX` is a host `ADD`, not a load, an add and a
  store through a CPU-state structure.
- **Flags are dead until proven live — decided at translation time.**
  Each block is decoded whole, then scanned backwards for which flags
  anything reads before they are overwritten; only those are computed.
  Most arithmetic's flags die immediately, and a compare followed by a
  jump becomes a host compare and branch on the host's own condition
  codes. That is static dead-flag elimination, not lazy flags: there is
  no deferred-flag state to save and replay at run time.
- **Blocks jump straight to blocks.** An exit to a known target is
  patched into a direct branch once the target is translated; indirect
  jumps and returns probe the block cache inline. Near and far CALL and
  RET, INT n and OUT stay inside translated code.
- **Guest memory is one host buffer.** A real-mode address is a pinned
  segment base plus an offset. The 1 MB wrap is not masked on every
  access: the 64 KB above 1 MB is a second mapping of the first 64 KB
  while A20 is off, and of the real memory once it is on. The A20 state is
  part of each block's key, so flipping the gate (HIMEM does it thousands
  of times while it tests memory) costs nothing.
- **Self-modifying code costs one byte-load per store.** A bitmap marks
  every byte translated code covers; a store checks its byte and moves on.
  Code that keeps patching itself — DOOM's renderer rewrites immediates in
  its inner loops — gets those immediates read at run time instead of
  being retranslated after every patch.
- **No cycle counting.** Nothing is paced per instruction. Interrupts are
  taken when a pinned counter runs out at a block boundary, so the hot
  path carries no event checks.
- **Paging stays in translated code.** Under a memory manager or Windows,
  a guest access goes through a software TLB inline, blocks are keyed by
  address space so switching page tables (a VCPI client does it on every
  DOS call) keeps them, and stores to video memory under paging take the
  same fast path as without it.

And it is checked: `-V` runs every translated block in lockstep with the
interpreter, and the full DOOM timedemo under EMM386 — 5.5 billion
instructions — runs clean that way.

## Building

Developed on macOS on Apple Silicon; the AArch64 backend is the mature
one. The x86-64 backend (`dbt/dbt_x64.c`, Linux) runs real-mode code
inline and everything else through the interpreter's helpers so far —
correct under `-V`, not yet fast. The backend is chosen by the host
(`BACKEND=a64|x64` overrides it, for the golden set).

    make                 # dos-monster, tools/sst, tools/jittest
    make ARCH=a64        # cross-compile the AArch64 build on x86-64 Linux into build-a64/
                         # (runs under qemu-user-static's binfmt)

Needs a C11 compiler and zlib; SDL2 (via `pkg-config`) adds a window for
graphics modes and is optional — without it the machine is headless.
The tests also use `nasm`, `python3`, `mtools`, `qemu-system-i386` and
`bochs`. The x87's arithmetic is Berkeley SoftFloat 3e (BSD licence,
`core/softfloat/COPYING.txt`).

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
  and V86 mode against QEMU and Bochs (`tools/pmoracle`: boot images that
  log their observations)
- `make test-fpu` — the x87: 6,600 cases (every instruction, the awkward
  operands, rounding and precision control, masked and unmasked
  exceptions) against Bochs's transcript, adjudicated where Bochs is the
  one that is off; the transcendentals against mpmath (`fpuacc.py`)
- `make test-dos` — DOS programs under the interpreter, the JIT and `-V`
- `make test-boot` — FreeDOS, MS-DOS and Windows booted from images and
  driven by `tools/expect.py`

## Layout

    core/   the x86: decoder, interpreter, paging, the x87 (and SoftFloat)
    dbt/    the translator (AArch64)
    pc/     the machine: BIOS, VGA, keyboard, timer, disks, CMOS, mouse
    dos/    the emulated DOS, MZ loader, DPMI host
    tools/  oracles, fuzzers, expect.py, the native BIOS routine
    tests/  DOS test programs, boot tests, test-suite runners

`CLAUDE.md` is the design document: scope, architecture, and the rules
learned the hard way.

## License

MIT — see `LICENSE`.
