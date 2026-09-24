#!/usr/bin/env python3
"""fpurun.py — the x87 oracle: fputest.img (fpugen.py) under dos-monster
-m 486, Bochs and QEMU -cpu 486, one line per case, diffed.

    python3 fpurun.py [--ours-only | --bochs-only] [--no-qemu] [--expect FILE] [-- extra dos-monster args]

Without --expect, prints every case where ours differs from Bochs, with
QEMU's line beside it, and a count by instruction. --expect compares ours
to a stored transcript (fputest.bochs: Bochs's, the reference test-fpu
checks against) and lists what differs; exit status 0 when nothing does.

fputest.expected is our transcript, adjudicated: it equals Bochs's
(fputest.bochs) but for transcendental lines where Bochs is the one off —
values (ours are correctly rounded to within half an ulp by mpmath,
fpuacc.py; Bochs's are off by up to ~1500 ulps, QEMU's more), C1 (which
follows the rounding), and Bochs's DE and UE on normal operands and on
results that are not tiny. `diff fputest.bochs fputest.expected` is that
list; rerun this without --expect after any change to see it again.
"""
import collections, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

def build():
    asm = os.path.join(HERE, "fputest.asm")
    with open(asm, "w") as f:
        subprocess.run([sys.executable, os.path.join(HERE, "fpugen.py")], stdout=f, check=True)
    img = os.path.join(HERE, "fputest.img")
    subprocess.run(["nasm", "-f", "bin", "-o", img, asm], check=True)
    com = {}
    for line in open(asm):
        m = re.search(r"; C([0-9A-F]{4}) (.*)$", line)
        if m: com[m.group(1)] = m.group(2)
    return img, com

def lines(text):
    return [l for l in text.splitlines() if l.startswith("C") or l in ("fputest", "done")]

def ours(img, extra):
    p = subprocess.run([os.path.join(ROOT, "dos-monster"), "-W", "-m", "486", "-T", "120", *extra, "-boot", img],
                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=180)
    return lines(p.stdout.decode("latin-1"))

def bochs(img):
    with tempfile.TemporaryDirectory() as d:
        rc = os.path.join(d, "bochsrc")
        src = open(os.path.join(HERE, "bochsrc")).read()
        src = re.sub(r"1_44=\S+,", "1_44=%s," % img, src).replace("/tmp/bochs.log", os.path.join(d, "bochs.log"))
        open(rc, "w").write(src + "port_e9_hack: enabled=1\n")
        p = subprocess.run(["bochs", "-q", "-f", rc], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=300)
        return lines(p.stdout.decode("latin-1"))

def qemu(img):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "e9.txt")
        subprocess.run(["qemu-system-i386", "-cpu", "486", "-drive", "file=%s,format=raw,if=floppy" % img,
                        "-display", "none", "-no-reboot", "-debugcon", "file:" + out,
                        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"],
                       timeout=300, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return lines(open(out, encoding="latin-1").read())

def main():
    args = sys.argv[1:]
    extra = []
    if "--" in args:
        i = args.index("--"); extra = args[i + 1:]; args = args[:i]
    expect = None
    if "--expect" in args:
        i = args.index("--expect"); expect = args[i + 1]; del args[i:i + 2]
    img, com = build()
    if "--bochs-only" in args:
        print("\n".join(bochs(img))); return 0
    o = ours(img, extra)
    if "--ours-only" in args:
        print("\n".join(o)); return 0
    if expect:
        b = open(os.path.join(HERE, expect), encoding="latin-1").read().splitlines()
        bad = [(x, y) for x, y in zip(o, b) if x != y] + [("(missing)", y) for y in b[len(o):]]
        for x, y in bad[:40]:
            k = (y if y.startswith("C") else x)[1:5]
            print("%s\n  ours %s\n  want %s" % (com.get(k, "?"), x, y))
        print("fpu oracle: %d of %d lines differ from %s" % (len(bad), len(b), expect))
        return 1 if bad else 0
    b = bochs(img)
    q = [] if "--no-qemu" in args else qemu(img)
    by = collections.Counter()
    n = 0
    for k in range(max(len(o), len(b))):
        x = o[k] if k < len(o) else "(missing)"
        y = b[k] if k < len(b) else "(missing)"
        if x == y: continue
        n += 1
        z = q[k] if k < len(q) else "(missing)"
        key = (y if y.startswith("C") else x)[1:5]
        c = com.get(key, "?")
        by[c.split()[0]] += 1
        print("%s\n  ours  %s\n  bochs %s\n  qemu  %s%s" % (c, x, y, z, "   (= ours)" if z == x else "   (= bochs)" if z == y else ""))
    print("%d of %d lines differ from Bochs:" % (n, len(b)), ", ".join("%s %d" % kv for kv in by.most_common()))
    return 1 if n else 0

if __name__ == "__main__":
    sys.exit(main())
