# CLAUDE.md — x86 Real/Protected-Mode DOS Dynamic Binary Translator

## What This Is

A high-performance execution environment for **8086 through 80386** guest
code running **DOS** (and DPMI clients) on modern AArch64 and x86-64
hosts, with the explicit goal of **multiple billions of guest
instructions per second**.

It is the third in the line: `~/riscv/dbt` (RV32IMFD), `~/slow-32`, and
`~/z80` (Z80 + CP/M, 4.3 BIPS on Apple Silicon). Every technique that
worked there — pinned guest registers, two-phase translate with static
dead-flag elimination, block chaining, RAS-backed CALL/RET, span-gated
SMC bitmap, lockstep `-V` verification — applies here. The low byte of
x86 FLAGS is literally the 8080 F register (CF=0, PF=2, AF=4, ZF=6,
SF=7), so even the flag tables carry over.

## Why This Exists (The Vibe)

- Because WordPerfect 5.1 was perfect and nobody runs it at 5 BIPS.
- Because DOSBox's dynamic core has been the state of the art since
  2004 and tops out in the low hundreds of MIPS.
- Because on Apple Silicon there is no hardware path for real mode at
  all — DBT is the only game in town.
- Because DOOM at 3000 fps is funny.

## Scope (decided 2026-09-21)

**In:**

- **Real mode**, 8086/186/286 instruction set, 1 MB + HMA, A20. This is
  Phase A and the bulk of the retro value: WordPerfect 5.1, Turbo
  Pascal, Lotus 1-2-3, dBASE III, pre-1992 games.
- **32-bit protected mode, flat model**, via a **host-side DPMI server**
  (we are the DPMI host, like NTVDM). DOS/4GW, PMODE/W and CWSDPMI
  clients detect us via INT 2Fh/1687h and never raw-switch, never touch
  CR0/CR3, never set up paging. The JIT sees "base 0, limit 4G" code:
  address = offset. Phase B: DOOM, Descent, Duke3D, Quake.
- **Real DOS booted from disk images** (FreeDOS, MS-DOS) with the
  machine underneath it: INT 13h over images, CMOS, extended memory
  reported through INT 15h, and the XMS drivers that use it — HIMEM.SYS
  and FreeDOS's HIMEMX. So **unreal mode** is in (added 2026-09-23):
  real-mode segment loads keep a cached limit above 64K, and
  32-bit-addressed real-mode code runs in the interpreter.

**Out (do not build toward these):**

- 64-bit long mode. Never.
- V86 mode, EMM386, Windows enhanced mode.
- Paging. The DPMI host owns memory; guests never see a page table.
- 16-bit PM (Windows standard mode) — maybe later.
- **Cycle counting.** No per-instruction cycle accounting, ever. The
  pinned pending-instruction counter (already needed for interrupt
  delivery) is the one pacing knob; if a 1988 game needs throttling,
  pace *that* against wall clock. Default is unthrottled.
- Sound. Forever.

## Architecture

### Guest Environment

- **CPU**: 8086 → 386 (no FPU emulation initially; 387 is a Phase C
  question — Turbo Pascal's software floating point works without one).
- **Memory**: flat host buffer. Real mode `seg<<4 + off` with 20-bit
  wrap and an A20 gate; PM flat selectors are just offsets. Mirror page
  trick from `z80_mem_alloc` for wrap-exact 16-bit stack ops.
- **Interrupts**: unlike CP/M, DOS is interrupt-driven. INT 8 (18.2 Hz
  timer), INT 9 (keyboard), INT 1Ch, and programs hook all of them.
  Delivery happens when the pinned instruction counter overflows at a
  chained-block edge — no extra checks on the hot path.

### Host Services Model

Two layers, same journey as z80's shim → real DRI CP/M:

1. **HLE DOS** (`dos/`): INT 21h implemented in the host, host directory
   mapped as C:, INT 10h/16h/1Ah BIOS services, PSP/MCB/FCB emulation.
   MZ loader. This is where we start.
2. **Real DOS behind our BIOS** (later): boot FreeDOS (GPL) or the
   MIT-licensed MS-DOS 4.0 from a disk image.
3. **DPMI host** (`dos/dos_dpmi.c`): INT 31h services, LDT management,
   real-mode callbacks, INT 21h translation from PM.

### PC Personality (`pc/`)

- 80×25 text mode with CGA attributes → the Kaypro cell-buffer renderer,
  nearly unchanged. HUD too.
- Mode 13h (320×200×256) for Phase B: a framebuffer window (minimal SDL2,
  or Kitty-protocol terminal graphics to stay a terminal app).
- Keyboard: scancodes via INT 9 + INT 16h buffer.

### DBT Pipeline (`dbt/`)

1. Decoder: table-driven, ModRM/SIB, prefixes (seg override, 66/67,
   REP, LOCK), 0F map. Shared by interpreter and translator.
2. Two-phase translate: decode whole block → backward FLAGS liveness →
   emit. Dead-flag elimination kills AF/PF almost everywhere.
3. Block cache keyed on linear address + mode/size bits. Chaining.
4. RAS for near CALL/RET (far CALL/RET too, once segments are pinned).
5. SMC bitmap — SMC is *more* common in DOS (overlays, Borland runtime
   patching, copy protection) than in CP/M.

### Register Pinning

AArch64 (primary — it's the machine we sit at): AX BX CX DX SI DI BP SP
+ DS/ES/SS/CS bases + FLAGS + mem base + aux + insn count, all pinned.
x86-64 (secondary): same-ISA case; map guest AX→host EAX etc. so the
common instructions re-emit nearly verbatim with a segment-base add,
and native flags are free.

## Verification

- **Interpreter first**, validated against the SingleStepTests 8088 JSON
  suite (exhaustive, per-instruction, includes undefined-flag behavior)
  before a single byte of JIT exists. We did not have this luxury on
  Z80; use it.
- Barotto's `test386.asm` for the 386 phase.
- `-V` lockstep JIT-vs-interpreter, as in z80. Block exits mark all
  flags live so it stays exact.
- Real apps: WP51, Turbo Pascal 5.5 (free from Borland museum),
  DOOM shareware.

## Phases

- **Phase A**: 8086/286 real mode, HLE DOS, text mode, AArch64 backend.
  Done when: WP51 runs, `-V` clean, HUD says N BIPS. Own campaign; clean
  stopping point.
- **Phase B**: 386 32-bit PM, DPMI host, mode 13h, DOOM.
- **Phase C**: x64 backend, real DOS from a disk image, 387, whatever
  is fun.

## Where the Bodies Are Buried (borrow from `~/z80`, don't reinvent)

The z80 tree is the template. Lift these, rename, adapt; the design is
already debugged:

| Need | Steal from |
|---|---|
| JIT buffer: `MAP_JIT`, `pthread_jit_write_protect_np` bracketing, I-cache flush | `dbt/dbt.h` (`DBT_JIT_MMAP_FLAGS`, `dbt_jit_writable_begin/end`) |
| Block cache, direct-mapped entries with `span`, link records + repatching on insert | `dbt/block_cache.c` (`dbt_cache_lookup/insert`, `dbt_link_record`, `dbt_links_repatch`) |
| SMC: code bitmap marked per block span, invalidate-on-store, host-write hook | `dbt/block_cache.c` (`dbt_mark_block_bytes`, `dbt_invalidate_for_store`, `z80_mem_host_wrote`) |
| Trampoline entry, run loop, `-V` lockstep with first-diff dump, stats | `dbt/dbt_common.c` (`dbt_run`, `verify_first_diff`, `dbt_print_stats`) |
| Result-indexed flag tables, parity, helper fallbacks for the JIT | `dbt/dbt_flags.c` / `dbt_flags.h` — the low FLAGS byte is the same layout |
| Two-phase decode → backward flag liveness → emit; the pinned-register convention | `dbt/dbt_a64.c` (convention comment at the top; the liveness pass is the model for ours) |
| AArch64 / x86-64 instruction encoders | `dbt/emit_a64.h`, `dbt/emit_x64.h` — copy wholesale, add what x86 needs (32-bit ops, SIB-shaped addressing) |
| Mirror-page guest memory for wrap-exact 16-bit accesses | `core/z80.h` (`z80_mem_alloc`), used via `z80_mem_mirrored` |
| Interpreter shape and decoder tables as a starting structure | `core/z80_decode.c`, `core/z80_interp.c` |
| Cell-buffer video + tty renderer + keyboard translation + HUD | `kaypro/kaypro_video.c`, `kaypro_render_tty.c`, `kaypro_kbd.c` |
| Host directory as a drive, file-handle mapping, image flushing | `cpm/cpm_host.c`, `cpm/cpm_disk.c` |
| CLI, `-s` stats, `-V`, `-i`, headless/script modes | `main.c` |
| App smoke tests as shell scripts driving the binary | `tests/*.sh`, `tools/wskaypro.sh` |
| Small assembler-built guest test programs | `tools/mk*.c` pattern (generate a binary, run it, diff output) |

Rules that were paid for in blood over there and still hold here:

- fmask is the LIVE-OUT mask, not live∩write; pass-through ops must see
  live bits outside their own write set.
- Block exits mark all flags live so `-V` stays exact.
- Never leave a code-bitmap byte set after its covering blocks are gone
  (a43a84c).
- Flush disk images at exit, not per write (6e47d11).

## Development Workflow

Same as z80: interpreter until it runs COMMAND-less .COM/.EXE programs
→ DBT skeleton from the z80/riscv template → `-V` early → real apps →
profile → specialize.

## File Naming & Layout (strict)

- `core/x86_*.c` — ISA: decoder, interpreter, state
- `dbt/dbt_*.c` — the translator (`dbt_a64.c`, `dbt_x64.c`, common)
- `dos/dos_*.c` — DOS layer: INT 21h, loader, DPMI, host files
- `pc/pc_*.c` — machine personality: video, keyboard, timer, BIOS
- `tools/`, `tests/`, `docs/`
- Binary: `dos-monster`.

## Success Metric

`C:\WP51> wp` opens a 50-page document, search/replace across it,
Reveal Codes flickers by, and the HUD says **5 BIPS**. Then DOOM.

---

*Now go make 1990 cry.*
