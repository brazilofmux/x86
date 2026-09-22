#!/usr/bin/env python3
"""Run the oracle kernel under Bochs and return the same per-case records
that pmrun.py gets from QEMU, so the two can be diffed."""
import re, subprocess, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))

RE_RAX = re.compile(r"^rax: [0-9a-f]{8}_([0-9a-f]{8})")
RE_RIP = re.compile(r"^rip: [0-9a-f]{8}_([0-9a-f]{8})")
RE_SEG = re.compile(r"^(ds|ss|es|fs|gs|cs):0x([0-9a-f]{4}), dh=0x([0-9a-f]{8}), dl=0x([0-9a-f]{8}), valid=(\d+)")
RE_DEC = re.compile(r"base=0x([0-9a-f]+), limit=0x([0-9a-f]+)")

def run(img, under, fault, ncases, spec=None):
    cmds = ["pb 0x%x" % under]
    for _ in range(ncases):
        cmds += ["c", "r", "s", "r", "sreg"]
    cmds.append("q")
    out = subprocess.run(["bochs", "-q", "-f", os.path.join(HERE, "bochsrc"), "-debugger"],
                         input="\n".join(cmds) + "\n", capture_output=True, text=True,
                         timeout=300, cwd=HERE).stdout

    # Each case emits: r (pre), s, r (post), sreg. Both r blocks print rax
    # and rip, so the two have to be told apart explicitly.
    recs, sel, rip_post = [], None, None
    segs, last = {}, None
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
            m = RE_SEG.match(line)
            if m:
                segs[m.group(1)] = [int(m.group(2), 16), int(m.group(3), 16), 0, 0]
                last = m.group(1); continue
            if last:
                m = RE_DEC.search(line)
                if m:
                    segs[last][2] = int(m.group(1), 16); segs[last][3] = int(m.group(2), 16)
                last = None
            if line.startswith("gdtr:"):                 # end of the sreg block
                i = len(recs)
                tgt = spec[i][0] if spec and i < len(spec) else "ds"
                v = segs.get(tgt, [0, 0, 0, 0])
                recs.append({"target": tgt, "sel": sel, "faulted": rip_post == fault,
                             "seg": v[0], "base": v[2], "limit": v[3],
                             "ar": (v[1] >> 8) & 0xFFFFFF})
                segs, state = {}, "pre_rax"
    return recs

if __name__ == "__main__":
    sys.path.insert(0, HERE)
    from pmrun import footer
    img = os.path.join(HERE, "pmtest.img")
    under, after, fault, n = footer(img)
    import pmcases
    for r in run(img, under, fault, n, pmcases.cases(img)):
        if r["faulted"]:
            print("  %s <- %04X  fault" % (r["target"], r["sel"]))
        else:
            print("  %s <- %04X  ok  %04X base=%08X limit=%08X ar=%06X"
                  % (r["target"], r["sel"], r["seg"], r["base"], r["limit"], r["ar"]))
