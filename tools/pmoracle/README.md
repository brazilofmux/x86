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

### Known divergence

QEMU 11.0.0 does not fault on a selector with TI=1 when LDTR has never
been loaded. Its power-on LDTR cache is `base 0, limit FFFF, ar 8200`,
which has the present bit set, so it happily reads a "descriptor" from
linear address 0 and loads the garbage:

    sel 0084  ->  ok  DS=0084 base=F053F000 limit=0000FF53 ar=0000FF
    sel 000C  ->  ok  DS=000C base=F053F000 limit=0000E2C3 ar=0000FF

Real hardware holds LDTR null out of reset and should raise `#GP` with
the selector as the error code, as it does for a GDT index past the
limit (`sel 0088 -> #0D err=0088`, which QEMU gets right). Do not copy
this behaviour into the interpreter; confirm against Bochs first.
