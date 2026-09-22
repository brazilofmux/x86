#!/usr/bin/env python3
"""Run the oracle kernel under Bochs and return the same per-case records
that pmrun.py gets from QEMU, so the two can be diffed."""
import re, subprocess, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))

RE_RAX = re.compile(r"^rax: [0-9a-f]{8}_([0-9a-f]{8})")
RE_RIP = re.compile(r"^rip: [0-9a-f]{8}_([0-9a-f]{8})")
RE_DS  = re.compile(r"^ds:0x([0-9a-f]{4}), dh=0x([0-9a-f]{8}), dl=0x([0-9a-f]{8}), valid=(\d+)")
RE_DEC = re.compile(r"base=0x([0-9a-f]+), limit=0x([0-9a-f]+)")

def run(img, under, fault, ncases):
    cmds = ["pb 0x%x" % under]
    for _ in range(ncases):
        cmds += ["c", "r", "s", "r", "sreg"]
    cmds.append("q")
    out = subprocess.run(["bochs", "-q", "-f", os.path.join(HERE, "bochsrc"), "-debugger"],
                         input="\n".join(cmds) + "\n", capture_output=True, text=True,
                         timeout=300, cwd=HERE).stdout

    # Each case emits: r (pre), s, r (post), sreg. Both r blocks print rax
    # and rip, so the two have to be told apart explicitly.
    recs, sel, rip_post, ds = [], None, None, None
    state = "pre_rax"
    for line in out.splitlines():
        m = RE_RAX.match(line)
        if m:
            if state == "pre_rax":
                sel = int(m.group(1), 16) & 0xFFFF; state = "pre_rip"
            elif state == "post_rax":
                state = "post_rip"
            continue
        m = RE_RIP.match(line)
        if m:
            if state == "pre_rip":
                state = "post_rax"                     # this rip is the pre one
            elif state == "post_rip":
                rip_post = int(m.group(1), 16); state = "want_ds"
            continue
        if state == "want_ds":
            m = RE_DS.match(line)
            if m:
                ds = (int(m.group(1), 16), int(m.group(2), 16)); continue
            if ds is not None:
                m = RE_DEC.search(line)
                base, limit = (int(m.group(1), 16), int(m.group(2), 16)) if m else (0, 0)
                recs.append({"sel": sel, "faulted": rip_post == fault,
                             "ds": ds[0], "base": base, "limit": limit,
                             "ar": (ds[1] >> 8) & 0xFFFFFF})
                ds, state = None, "pre_rax"
    return recs

if __name__ == "__main__":
    sys.path.insert(0, HERE)
    from pmrun import footer
    img = os.path.join(HERE, "pmtest.img")
    under, after, fault, n = footer(img)
    for r in run(img, under, fault, n):
        if r["faulted"]:
            print("  sel %04X  ->  fault" % r["sel"])
        else:
            print("  sel %04X  ->  ok  DS=%04X base=%08X limit=%08X ar=%06X"
                  % (r["sel"], r["ds"], r["base"], r["limit"], r["ar"]))
