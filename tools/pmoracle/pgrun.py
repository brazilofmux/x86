#!/usr/bin/env python3
"""pgrun.py — run a transcript image (pgtest.asm, and the V86 ones that
follow it) under QEMU and under dos-monster and diff what each wrote to
port E9.

    python3 pgrun.py [image.asm] [--ours-only] [--m486] [--expect FILE] [-- extra dos-monster args]

--expect compares against a stored transcript instead of running QEMU:
c486test.bochs is Bochs's, since QEMU has no #AC and lets ring 3 run
INVD and WBINVD (the SDM, and Bochs, say #GP).

QEMU runs with -cpu 486 (CR0.WP off, as a 386 has none) and exits through
isa-debug-exit at port F4; dos-monster boots the image with -boot on a
386 (a 486 with --m486, for c486test.asm) and stops at the same port. Exit status 0 when the transcripts agree.
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

def build(asm):
    img = os.path.splitext(asm)[0] + ".img"
    subprocess.run(["nasm", "-f", "bin", "-o", img, asm], check=True)
    return img

def qemu(img):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "e9.txt")
        subprocess.run(["qemu-system-i386", "-cpu", "486",
                        "-drive", "file=%s,format=raw,if=floppy" % img,
                        "-display", "none", "-no-reboot",
                        "-debugcon", "file:" + out,
                        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"],
                       timeout=60, check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return open(out, encoding="latin-1").read()

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
    model = "486" if "--m486" in args else "386"
    expect = None
    if "--expect" in args:
        i = args.index("--expect"); expect = args[i + 1]; del args[i:i + 2]
    args = [a for a in args if a not in ("--ours-only", "--m486")]
    asm = args[0] if args else os.path.join(HERE, "pgtest.asm")
    img = build(asm)
    o = ours(img, extra, model)
    if only:
        sys.stdout.write(o); return 0
    q = open(os.path.join(HERE, expect), encoding="latin-1").read() if expect else qemu(img)
    ql, ol = q.splitlines(), o.splitlines()
    bad = 0
    for k in range(max(len(ql), len(ol))):
        a = ql[k] if k < len(ql) else "(missing)"
        b = ol[k] if k < len(ol) else "(missing)"
        mark = "  " if a == b else "!!"
        if a != b: bad += 1
        print("%s %s: %-44s ours: %s" % (mark, "want" if expect else "qemu", a, b))
    print("%d of %d lines differ" % (bad, max(len(ql), len(ol))))
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
