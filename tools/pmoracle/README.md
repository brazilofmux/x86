# Protected-mode oracle

There is no SingleStepTests suite for protected mode — the 80386 repo
ships `v1_ex_real_mode` and nothing else — so for descriptors, gates and
privilege we have to make our own reference. This is the generator.

## How it works

`pmtest.asm` is a two-stage boot image, not a test program. It builds an IDT whose
every gate abandons the frame and resumes the sweep, enters 32-bit
protected mode with a GDT holding one descriptor of each interesting
shape, and then runs a table of cases. Each case carries the **bytes** of the
instruction under test, which are copied into a fixed slot and executed
there — so the harness always finds the instruction at one address, and
the case decides what the instruction is. It asserts nothing: what the
machine does is whatever the log says it did.

The image is self-describing. A footer at offset 496 carries the magic
`PS`, the slot address, the address after it, the fault handler, the
case count, and the address and stride of the case table, so the
harnesses and `expected.py` all read the cases from one source.

`pmrun.py` runs the image under QEMU with `-d cpu,int` and
`-accel tcg,one-insn-per-tb=on`, and pairs up the dumps: the one at the
instruction's address is the state going in, what follows is the result.
A faulting instruction is dumped twice — once before it runs, once as the
exception is taken — and the `v=..` line carries the vector and error
code. `-dfilter` keeps the log to our own code; without it a single boot
writes about 5 GB.

    make -C ../.. test-pm        # or: nasm -f bin -o pmtest.img pmtest.asm && python3 pmrun.py

The footer at offset 496 carries the magic `PS`, the addresses of the
instruction under test, of the instruction after it, and of the fault
handler, plus the case count — so the harness needs no symbol file.

## Why an emulator is not silicon

QEMU and Bochs are references, not hardware. Run the same image under
both: where they agree, that is decent evidence; where they disagree,
that is the interesting list, and it gets adjudicated against the Intel
manual rather than by majority vote. The kernel is deliberately written
as a plain boot sector so it also runs on a real 386 if one ever turns up.

## Running both

    make -C ../.. test-pm            # QEMU
    python3 pmbochs.py               # Bochs
    python3 compare.py               # both, diffed case by case

Bochs needs `brew install bochs`; the build Homebrew ships has the
internal debugger, which is what `pmbochs.py` drives (a physical
breakpoint on the instruction under test, then `r`/`s`/`r`/`sreg` per
case, fed on stdin). `bochsrc` here is a floppy-only machine with no
display.

## What it covers so far

Segment-register loads against every descriptor shape, for DS, SS and
ES: 24 cases, of which 20 match our expectations under both references,
two differ only in cache encoding, and two are the LDTR case below.

The SS rules are the interesting part, because they differ from DS on
every axis and the oracle shows it plainly:

    case          DS     SS
    0020 absent   #0B    #0C     a not-present stack segment is #SS, not #NP
    0040 r/o data ok     #0D     SS must be writable
    0018 DPL 3    ok     #0D     SS requires DPL = CPL
    0000 null     ok     #0D     null is legal in DS, not in SS

and one detail worth having measured rather than remembered: loading SS
with `001B` faults with **error code `0018`**. The low three bits of an
exception error code are the EXT/IDT/TI flags, so the RPL does not
appear in it.

### A limitation of the Bochs side

`pmbochs.py` reports fault-versus-not, not the vector: it infers a fault
from landing on the handler. QEMU's `-d int` gives the vector and error
code, so those columns come from QEMU alone and Bochs corroborates only
the decision. Recording the vector guest-side would need per-vector IDT
stubs, which is worth doing when the case set grows.

### Where we are deliberately stricter

`expected.py` records what the *architecture* requires, per case, with a
rationale and a note on whether the references corroborate it.
`compare.py` prints it as a third column, so a case where we knowingly
differ from both emulators is a tracked decision rather than something
to rediscover:

      sel   qemu   bochs  wanted
      0084  ok     ok     #0D    deliberately stricter: TI=1 with LDTR never loaded -> #GP
      0088  #0D    fault  #0D

Neither emulator faults on a selector with TI=1 when LDTR has never been
loaded. Both hold a reset LDTR whose cache has the present bit set, so
both read a "descriptor" from linear address 0 and load whatever is
there — and they disagree about *which* garbage, because it is IVT
contents and their BIOSes differ.

We take the manual's reading and fault, for three reasons:

* the permissive behaviour is **not reproducible** — it depends on BIOS
  contents, and lockstep verification needs determinism;
* permissiveness is **unfalsifiable in the bad direction**: if we are lax
  and wrong, nothing ever tells us, whereas if we are strict and wrong a
  real program breaks loudly and we learn something;
* it **masks our own bugs**: when the DPMI host hands out a stale LDT
  selector, a lax CPU turns that into a wrong answer ten thousand
  instructions later, and a strict one faults at the instruction that
  caused it.

"No real software does that" is not a reason to be lax. The value of an
oracle is in the cases nobody intended.

These are decisions, not measurements. They are the first things to
revisit if a real client ever misbehaves, and the distinction from a
silicon-measured contract should stay visible in the interpreter too.
