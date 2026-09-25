#!/usr/bin/env python3
"""pgrun.py — run a transcript image (pgtest.asm, and the V86 ones that
follow it) under QEMU and under dos-monster and diff what each wrote to
port E9.

    python3 pgrun.py [image.asm] [--ours-only] [--m486 | --m586] [--expect FILE | --bochs | --bochs-only] [-- extra dos-monster args]

--bochs runs Bochs as the reference instead of QEMU (bochscfg.py fits the
checked-in bochsrc to the host); --bochs-only prints its transcript, which
is how c486test.bochs is made:  python3 pgrun.py c486test.asm --bochs-only > c486test.bochs

--expect compares against a stored transcript instead of running QEMU.
c486test.expected is Bochs's (c486test.bochs, verbatim) but for four
lines: QEMU has no #AC and lets ring 3 run INVD and WBINVD (the SDM, and
Bochs, say #GP), and its x87 pops on unmasked exceptions and misses #MF
on waiting loads; Bochs's default CPU stores FCS and FDS as 0, as Intel's
since Haswell do, where a 486 stores the selectors (QEMU agrees).
c586test.expected is Bochs's pentium model verbatim (c586test.bochs); QEMU
differs on two lines, both leniencies of its TCG: a WRMSR to the TSC does
not change what RDMSR/RDTSC read (T13), and an MSR that does not exist
reads 0 instead of #GP (T15).

QEMU runs with -cpu 486 (CR0.WP off, as a 386 has none) and exits through
isa-debug-exit at port F4; dos-monster boots the image with -boot on a
386 (a 486 with --m486, for c486test.asm) and stops at the same port.
--m586 (c586test.asm) makes all three a Pentium: QEMU's -cpu pentium,
Bochs's pentium model (with a bad MSR a #GP, not ignored), and ours. Exit status 0 when the transcripts agree.
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

def build(asm):
    img = os.path.splitext(asm)[0] + ".img"
    subprocess.run(["nasm", "-f", "bin", "-o", img, asm], check=True)
    return img

def qemu(img, model="386"):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "e9.txt")
        subprocess.run(["qemu-system-i386", "-cpu", "pentium" if model == "586" else "486",
                        "-drive", "file=%s,format=raw,if=floppy" % img,
                        "-display", "none", "-no-reboot",
                        "-debugcon", "file:" + out,
                        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"],
                       timeout=60, check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return open(out, encoding="latin-1").read()

def bochs(img, model="386"):
    """Bochs's port-E9 transcript: the debugger is told to continue, and the
    run is stopped once the image has written its closing "done" (Bochs has
    no isa-debug-exit). Lines between the image's name and "done"."""
    import bochscfg
    extra = ["port_e9_hack: enabled=1"]
    if model == "586": extra.append("cpu: model=pentium, ignore_bad_msrs=0")
    rc = bochscfg.write(os.path.abspath(img), extra)
    name = os.path.splitext(os.path.basename(img))[0]
    lines, on = [], False
    p = subprocess.Popen(["bochs", "-q", "-f", rc], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, encoding="latin-1", cwd=HERE)
    try:
        p.stdin.write("c\n"); p.stdin.flush()
        for line in p.stdout:
            line = line.rstrip("\n")
            if line.endswith(name): on = True; line = name   # (the debugger prompt may share its line)
            if on: lines.append(line)
            if on and line == "done": break
    finally:
        p.kill(); p.wait(); os.unlink(rc)
    return "\n".join(lines) + "\n"

def ours(img, extra, model="386"):
    # stdin stays an open, empty pipe: end of input would feed the guest a
    # Ctrl-Z, and its keyboard interrupt would land in a slow -V run
    p = subprocess.Popen([os.path.join(ROOT, "dos-monster"), "-W", "-m", model, "-T", "60",
                          *extra, "-boot", img],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        out = p.stdout.read()
        p.wait(timeout=90)
    except subprocess.TimeoutExpired:
        p.kill(); out = b""
    p.stdin.close()
    return out.decode("latin-1")

def main():
    args = sys.argv[1:]
    extra = []
    if "--" in args:
        i = args.index("--"); extra = args[i + 1:]; args = args[:i]
    only = "--ours-only" in args
    model = "486" if "--m486" in args else "586" if "--m586" in args else "386"
    expect = None
    if "--expect" in args:
        i = args.index("--expect"); expect = args[i + 1]; del args[i:i + 2]
    use_bochs = "--bochs" in args or "--bochs-only" in args
    args = [a for a in args if a not in ("--ours-only", "--m486", "--m586", "--bochs", "--bochs-only")]
    asm = args[0] if args else os.path.join(HERE, "pgtest.asm")
    img = build(asm)
    if "--bochs-only" in sys.argv:
        sys.stdout.write(bochs(img, model)); return 0
    o = ours(img, extra, model)
    if only:
        sys.stdout.write(o); return 0
    q = open(os.path.join(HERE, expect), encoding="latin-1").read() if expect else bochs(img, model) if use_bochs else qemu(img, model)
    ql, ol = q.splitlines(), o.splitlines()
    bad = 0
    for k in range(max(len(ql), len(ol))):
        a = ql[k] if k < len(ql) else "(missing)"
        b = ol[k] if k < len(ol) else "(missing)"
        mark = "  " if a == b else "!!"
        if a != b: bad += 1
        print("%s %s: %-44s ours: %s" % (mark, "want" if expect else "bochs" if use_bochs else "qemu", a, b))
    print("%d of %d lines differ" % (bad, max(len(ql), len(ol))))
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
