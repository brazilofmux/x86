#!/usr/bin/env python3
"""fpuacc.py — how far our transcendentals are from the true value, in
units of the last place, for every finite result fputest produces (needs
mpmath; fpurun.py builds the image and --ours-only gives the transcript).

    python3 fpurun.py --ours-only > ours.txt; python3 fpuacc.py ours.txt

FSIN/FCOS/FSINCOS/FPTAN are measured against the x87's own definition:
the argument reduced by pi rounded to 66 bits (what silicon does, and
what makes FSIN(M_PI) 1.2246063538223773e-16), then the true function.
"""
import os, re, sys
import mpmath
from mpmath import mpf

mpmath.mp.prec = 256
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

PI66 = (mpf(0xC90FDAA22168C234) + mpf(3) / 4) / mpf(2) ** 62

def val(se, sig):
    e = se & 0x7FFF
    if e == 0: e = 1
    v = mpf(sig) * mpf(2) ** (e - 16383 - 63)
    return -v if se & 0x8000 else v

def ulps(se, sig, true):
    got = val(se, sig)
    e = se & 0x7FFF or 1
    return float((got - true) / (mpf(2) ** (e - 16383 - 63)))

def reduce66(x):
    k = mpmath.nint(x / (PI66 / 2))
    return x - k * (PI66 / 2), int(k)

def trig(x):
    r, k = reduce66(x)
    s, c = mpmath.sin(r), mpmath.cos(r)
    return [(s, c), (c, -s), (-s, -c), (-c, s)][k & 3]

def main():
    src = open(os.path.join(HERE, "fpugen.py")).read()
    V = {}
    for m in re.finditer(r'\("([^"]+)",\s*0x([0-9A-F]{4}),\s*0x([0-9A-F]{16})\)', src):
        V[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    com = {}
    for line in open(os.path.join(HERE, "fputest.asm")):
        m = re.search(r"; C([0-9A-F]{4}) (.*)$", line)
        if m: com[m.group(1)] = m.group(2)
    worst = {}
    for line in open(sys.argv[1]):
        if not line.startswith("C"): continue
        k = line[1:5]
        c = com.get(k, "")
        op = c.split()[0] if c else ""
        if op not in ("fsin", "fcos", "fsincos", "fptan", "fpatan", "f2xm1", "fyl2x", "fyl2xp1"): continue
        if "unmasked" in c or "rc=" in c and "rc=near" not in c: continue
        regs = [(int(a, 16), int(b, 16)) for a, b in re.findall(r"([0-9A-F]{4}):([0-9A-F]{16})", line)]
        args = re.findall(r"st[01]=(\S+)", c) or [c.split()[1]]
        if op in ("fpatan", "fyl2x", "fyl2xp1") and len(args) < 2: continue
        # finite nonzero operands only: the special cases are tables, which
        # the comparison with Bochs and QEMU covers
        def plain(n):
            se, sig = V[n]
            return (se & 0x7FFF) != 0x7FFF and (sig or (se & 0x7FFF)) and ((sig >> 63) or not (se & 0x7FFF))
        if not all(n in V and plain(n) for n in args): continue
        try:
            if op in ("fpatan", "fyl2x", "fyl2xp1"):
                x, y = (val(*V[args[0]]), val(*V[args[1]]))
                if op == "fpatan": true = [mpmath.atan2(y, x)]
                elif op == "fyl2x": true = [y * mpmath.log(x, 2)] if x > 0 else None
                else: true = [y * mpmath.log(1 + x, 2)] if x > -1 else None
            else:
                x = val(*V[args[0]])
                if op == "f2xm1": true = [mpmath.power(2, x) - 1]
                else:
                    if abs(x) >= mpf(2) ** 63: continue
                    s, co = trig(x)
                    true = {"fsin": [s], "fcos": [co], "fsincos": [co, s], "fptan": [mpf(1), s / co]}[op]
        except (KeyError, ValueError, ZeroDivisionError):
            continue
        if not true or len(regs) < len(true): continue
        for (se, sig), t in zip(regs, true):
            if (se & 0x7FFF) == 0x7FFF or not mpmath.isfinite(t) or t == 0: continue
            if (sig >> 63) == 0 and (se & 0x7FFF): continue
            u = ulps(se, sig, t)
            if abs(u) > abs(worst.get(op, (0, ""))[0]): worst[op] = (u, c)
            if abs(u) > 1.0: print("%-40s %+.3f ulp" % (c, u))
    for op, (u, c) in sorted(worst.items()):
        print("worst %-8s %+.3f ulp  (%s)" % (op, u, c))

if __name__ == "__main__":
    main()
