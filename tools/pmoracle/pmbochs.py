#!/usr/bin/env python3
"""Run the oracle kernel under Bochs and return the same per-case records
that pmrun.py gets from QEMU, so the two can be diffed."""
import re, subprocess, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
STUB_BASE, STUB_STRIDE, NVEC = 0x6400, 16, 0x31   # must match pmtest.asm
RET_VEC = 0x30

RE_RAX = re.compile(r"^rax: [0-9a-f]{8}_([0-9a-f]{8})")
RE_RIP = re.compile(r"^rip: [0-9a-f]{8}_([0-9a-f]{8})")
RE_RBP = re.compile(r"^rbp: [0-9a-f]{8}_([0-9a-f]{8})")
RE_RSP = re.compile(r"^rsp: [0-9a-f]{8}_([0-9a-f]{8})")
RE_SEG = re.compile(r"^(ds|ss|es|fs|gs|cs|ldtr|tr):0x([0-9a-f]{4}), dh=0x([0-9a-f]{8}), dl=0x([0-9a-f]{8}), valid=(\d+)")
RE_DEC = re.compile(r"base=0x([0-9a-f]+), limit=0x([0-9a-f]+)")

def run(img, under, fault, ncases, spec=None):
    cmds = ["pb 0x%x" % under]
    for _ in range(ncases + 2):        # a couple spare: the kernel wraps, so extra is safe
        cmds += ["c", "r", "s", "r", "sreg"]
    cmds.append("q")
    out = subprocess.run(["bochs", "-q", "-f", os.path.join(HERE, "bochsrc"), "-debugger"],
                         input="\n".join(cmds) + "\n", capture_output=True, text=True,
                         timeout=300, cwd=HERE).stdout

    # Each case emits: r (pre), s, r (post), sreg. Both r blocks print rax
    # and rip, so the two have to be told apart explicitly.
    recs, sel, rip_post, ebp, esp = [], None, None, None, None
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
        m = RE_RSP.match(line)
        if m and state == "post_rip":
            esp = int(m.group(1), 16)
            continue
        m = RE_RBP.match(line)
        if m and state == "post_rip":
            ebp = int(m.group(1), 16)          # the stub left its vector here
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
                cpl = spec[i][3] if spec and i < len(spec) else 0
                aux = spec[i][4] if spec and i < len(spec) else 0
                if spec and i < len(spec):
                    sel = spec[i][1]          # a far transfer carries it in the instruction
                reg = {"jmpf": "cs", "callf": "cs", "iret": "cs", "retf": "cs", "lldt": "ldtr", "ltr": "tr"}.get(tgt, tgt)
                v = segs.get(reg, [0, 0, 0, 0])
                # Landing on stub i *is* the vector: one stub per vector, fixed
                # stride. Reading it from a register would be a step too early,
                # since the post-step dump is taken before the stub runs.
                faulted = STUB_BASE <= rip_post < STUB_BASE + NVEC * STUB_STRIDE
                r = {"target": tgt, "sel": sel, "cpl": cpl, "aux": aux, "faulted": faulted,
                     "seg": v[0], "base": v[2], "limit": v[3],
                     "ar": (v[1] >> 8) & 0xFFFFFF,
                     "esp": esp or 0, "ss": segs.get("ss", [0])[0]}
                if faulted:
                    v = (rip_post - STUB_BASE) // STUB_STRIDE
                    if v == RET_VEC:            # the deliberate return, not a fault
                        r["faulted"] = False
                    else:
                        r["vec"] = v
                recs.append(r)
                segs, ebp, esp, state = {}, None, None, "pre_rax"
    return recs[:ncases]

if __name__ == "__main__":
    sys.path.insert(0, HERE)
    from pmrun import footer
    img = os.path.join(HERE, "pmtest.img")
    under, after, fault, n = footer(img)
    import pmcases
    for r in run(img, under, fault, n, pmcases.cases(img)):
        if r["faulted"]:
            print("  %s <- %04X  #%02X" % (r["target"], r["sel"], r.get("vec", 0xFF)))
        else:
            print("  %s <- %04X  ok  %04X ar=%06X  ss:esp=%04X:%08X"
                  % (r["target"], r["sel"], r["seg"], r["ar"], r["ss"], r["esp"]))
