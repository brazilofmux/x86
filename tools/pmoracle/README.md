# Protected-mode oracle

There is no SingleStepTests suite for protected mode — the 80386 repo
ships `v1_ex_real_mode` and nothing else — so for descriptors, gates and
privilege we have to make our own reference. This is the generator.

## How it works

`pmtest.asm` is a boot sector, not a test program. It builds an IDT whose
every gate abandons the frame and resumes the sweep, enters 32-bit
protected mode with a GDT holding one descriptor of each interesting
shape, and then executes **the instruction under test at a fixed address**
once per case. It asserts nothing: what the machine does is whatever the
log says it did.

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

## First results

Ten of fourteen cases agree exactly. A not-present descriptor gives #NP
with the selector as the error code, execute-only code loaded into DS
gives #GP, a GDT index past the limit gives #GP, and the loaded
descriptor caches match down to the byte-granular `limit=0FFF`.

Of the four that differ, three differ only in the access-rights word of
a cache that is unusable anyway (a null selector, or the garbage below),
which is an internal encoding rather than behaviour. `compare.py`
reports those separately from behavioural differences.

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
