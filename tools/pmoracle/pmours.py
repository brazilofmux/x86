#!/usr/bin/env python3
"""Run the oracle kernel on our own interpreter and return the same per-case
records the QEMU and Bochs harnesses return.

dos-monster boots the image with -boot and emits machine state in QEMU's
-d cpu shape under -pmtrace, so the parser in pmrun.py reads all three.
Until protected mode is implemented the kernel dies early and this returns
nothing, which is the point: the gap is the work list.
"""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pmrun, pmcases

MONSTER = os.path.join(HERE, "..", "..", "dos-monster")
LOG = "/tmp/pmours.log"

def run(img=None, insn_limit=2_000_000):
    img = img or os.path.join(HERE, "pmtest.img")
    f = pmcases.footer(img)
    with open(LOG, "wb") as log:
        subprocess.run([MONSTER, "-boot", img, "-i", "-m", "386", "-L", str(insn_limit),
                        "-pmtrace", "0x7c00:0x8fff"],
                       stdout=subprocess.DEVNULL, stderr=log,
                       timeout=300, check=False)
    spec = pmcases.cases(img)
    out = []
    for i, (pre, post) in enumerate(pmrun.cases(pmrun.parse(LOG), f["slot"])):
        if i >= len(spec):
            break
        tgt, sel, _, cpl, aux = spec[i]
        r = {"target": tgt, "sel": sel, "cpl": cpl, "aux": aux}
        if pre["exc"]:
            r.update(faulted=True, vec=pre["exc"][0], err=pre["exc"][1])
        elif post is None:
            r.update(faulted=None)
        else:
            reg = {"jmpf": "CS", "callf": "CS", "iret": "CS",
                   "lldt": "LDT", "ltr": "TR"}.get(tgt, tgt.upper())
            d = post["segs"].get(reg, (0, 0, 0, 0))
            r.update(faulted=False, seg=d[0], base=d[1], limit=d[2], ar=d[3] >> 8,
                     esp=post["regs"].get("ESP", 0), ss=post["segs"].get("SS", (0,))[0])
        out.append(r)
    return out

if __name__ == "__main__":
    recs = run()
    print("ours: %d of %d cases reached" % (len(recs), pmcases.footer(os.path.join(HERE, "pmtest.img"))["n"]))
    for r in recs:
        print("  cpl%d %s <- %04X  %s" % (r["cpl"], r["target"], r["sel"],
              ("#%02X" % r["vec"]) if r["faulted"] else "ok"))
