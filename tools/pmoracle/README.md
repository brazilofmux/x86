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

## Running it

    make -C ../.. test-pm            # QEMU alone
    python3 pmbochs.py               # Bochs alone
    python3 pmours.py                # our own interpreter
    make -C ../.. test-pm-compare    # all three, against what we require

The comparison prints four columns — `qemu`, `bochs`, `ours`, `wanted`.
`ours` is blank for cases our interpreter has not reached yet, which is
the point: it is the protected-mode work list, ordered by the same case
table, and it fills in as the implementation lands.

Our interpreter runs the same image through `dos-monster -boot`, which
loads sector one at 7C00 in real mode and serves the rest through
INT 13h, and emits machine state under `-pmtrace LO:HI` in the shape
QEMU's `-d cpu` uses, so one parser reads all three. The trace is
budgeted: a kernel that faults in a loop would otherwise write gigabytes
before anyone noticed it was stuck.

Bochs needs `brew install bochs` on macOS or `apt install bochs
bochsbios vgabios` on Ubuntu; both builds have the internal debugger,
which is what `pmbochs.py` drives (a physical breakpoint on the
instruction under test, then `r`/`s`/`r`/`sreg` per case, fed on stdin).
`bochsrc` here is a floppy-only machine with no display, written for
Homebrew; `bochscfg.py` fits it to the host (Ubuntu's 2.7 needs
BIOS-bochs-legacy, which boots this machine where its BIOS-bochs-latest
does not, an rfb display in place of the missing nogui, and no
`-debugger` flag). `pgrun.py --bochs` compares a transcript image against
Bochs instead of QEMU, and `--bochs-only` prints Bochs's transcript:
`c486test.bochs` is `python3 pgrun.py c486test.asm --bochs-only`.

## What it covers so far

61 cases: segment-register loads (DS, SS, ES) against every descriptor
shape, far JMP/CALL against code, data, gates and junk, the same
instructions again at **CPL 3**, an LDT, and IRET, reached by IRET to a ring-3 code
segment and returned from through a DPL 3 trap gate with a TSS supplying
the inbound stack.

52 match our expectations under both references, 2 differ only in cache
encoding, 2 are the LDTR case, 3 are accessed-bit differences, and none
are unexpected.

The SS rules differ from DS on every axis:

    case          DS     SS
    0020 absent   #0B    #0C     a not-present stack segment is #SS, not #NP
    0040 r/o data ok     #0D     SS must be writable
    0018 DPL 3    ok     #0D     SS requires DPL = CPL
    0000 null     ok     #0D     null is legal in DS, not in SS

Loading SS with `001B` faults with **error code `0018`**: the low three
bits of an error code are the EXT/IDT/TI flags, so RPL never appears
there. A gate whose *target* is bad reports the **target** selector, not
the gate's: `callf 0068` faults with `err=0010`.

At CPL 3 the returned CS says whether privilege actually changed,
because its RPL is the new CPL, and SS:ESP says what happened to the
stack:

    cpl0 callf 0008 -> CS=0008  ss:esp=0010:6FF8   same privilege, CS:EIP pushed
    cpl0 callf 0058 -> CS=0008  ss:esp=0010:6FF8   a gate at the same privilege pushes no more
    cpl3 jmpf  0048 -> CS=004B  ss:esp=001B:5000   conforming: CPL stays 3, stack untouched
    cpl3 jmpf  0070 -> CS=0073  ss:esp=001B:5000   DPL 3 code, still ring 3
    cpl3 callf 0080 -> CS=0008  ss:esp=0010:6FF0   gate inward: stack switched to the TSS
                                                   SS0:ESP0, sixteen bytes pushed
    cpl3 callf 0058 -> #GP                         DPL 0 gate is unusable from ring 3
    cpl3 jmpf  0008 -> #GP                         non-conforming DPL 0 is out of reach

The last inward case is the whole call-gate mechanism in one row: the
stack came from the TSS, and 0x7000 - 0x6FF0 is four dwords — SS, ESP,
CS, EIP of the interrupted ring-3 context.

### IRET

IRET is the only instruction here whose operand is a stack frame, so the
sweep builds one from the case: the CS to return through, and an SS if
the return is meant to change privilege. The processor decides that for
itself from CS.RPL, which is why a *mismatched* frame is worth testing.

The returned SS:ESP is the evidence of which kind of return happened:

    iret 0008      -> CS=0008  ss:esp=0010:7000   same privilege: EIP, CS, EFLAGS only
    iret 0048      -> CS=0048  ss:esp=0010:7000   conforming at RPL 0: still same privilege
    iret 0073/001B -> CS=0073  ss:esp=001B:5000   CS.RPL 3 > CPL: ESP and SS popped too
    iret 0010      -> #GP      a data segment as the return CS
    iret 0050      -> #NP      return CS not present
    iret 0000      -> #GP(0)   null return CS
    iret 0073/0010 -> #GP      SS.RPL must match CS.RPL

The last one is worth the measurement: the error code is **0010**, the
offending SS, not the CS the frame named. A fault during a return is
attributed to the selector that was actually wrong.

### The LDT

`LLDT` is itself a case, so everything before it in the table runs with
LDTR unloaded and everything after sees a real LDT. That keeps the
"LDTR never loaded" probes meaningful while still testing TI=1
selectors for real:

    lldt 0088 -> LDTR=0088 base=00007FB8 limit=1F
    ds   0004 -> ok     TI=1 index 0 is LDT entry 0, NOT null
    ds   0014 -> #0B    LDT entry not present, error code keeps the TI bit
    ds   00FC -> #0D    index past the LDT limit
    jmpf 001C -> ok     a far jump may target code in the LDT

That answers a question worth having measured: only a TI=0 selector with
index 0 is the null selector. With TI=1, index 0 is an ordinary LDT
entry. This matters for Phase B, because DPMI hands its clients LDT
descriptors.

### A trap the kernel itself fell into

An inter-privilege IRET nulls any data segment the new CPL cannot reach.
The sweep keeps its case table behind FS at DPL 0, so the first transfer
out to ring 3 nulled FS and every later read of the table faulted — an
endless fault loop that looked like the harness hanging. `next_case`
now reloads FS. Worth remembering when writing the interpreter: this is
a real rule that is easy to forget precisely because it costs nothing
until privilege changes.

### Divergence: the accessed bit

    jmpf <- 0048   ACCESSED BIT: qemu clear, bochs set

Both emulators set the accessed bit when CS is loaded with a
*non-conforming* code segment; QEMU does not set it for a *conforming*
one, nor for a DPL 3 one entered from ring 3. The bit is architectural — hardware sets it and writes it back to
the descriptor in memory — so `compare.py` reports it separately from
cosmetic encoding noise. Bochs looks right here.

### A limitation of the Bochs side

Both references now report the vector. The IDT has one stub per vector
(`mov ebp, <vector>; jmp fault`), so landing on stub *i* identifies the
exception by address alone — which is what lets an emulator with no
exception log report it. Reading the vector out of EBP instead would be
a step too early: the post-step dump is taken before the stub runs.

The error code still comes from QEMU's `-d int` only; Bochs corroborates
the vector but not the code.

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
