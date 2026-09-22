#!/usr/bin/env python3
"""pmrun.py — drive the protected-mode oracle kernel under an emulator and
report, per test case, the state going into the instruction under test and
what came out: the loaded descriptor, or the exception and error code.

The emulator is a reference, not silicon. Run the same image under more
than one and treat a disagreement as the interesting output rather than
as a failure of the tool.
"""
import re, struct, subprocess, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pmcases

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = "/tmp/pmoracle.log"

def footer(img):
    d = open(img, "rb").read()
    magic, under, after, fault, n = struct.unpack_from("<5H", d, 496)
    if magic != 0x5350:
        sys.exit("%s: no oracle footer (rebuild pmtest.img)" % img)
    return under, after, fault, n

def run_qemu(img):
    # -dfilter keeps the log to our own code: without it a boot is gigabytes.
    subprocess.run(["qemu-system-i386",
                    "-drive", "file=%s,format=raw,if=floppy" % img,
                    "-display", "none", "-no-reboot",
                    "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
                    "-accel", "tcg,one-insn-per-tb=on",
                    "-dfilter", "0x7c00..0x8fff",
                    "-d", "cpu,int", "-D", LOG],
                   timeout=180, check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

RE_EXC  = re.compile(r"^\s*\d+:\s+v=([0-9a-f]{2}) e=([0-9a-f]{4})")
RE_GPR  = re.compile(r"\b(E[A-Z][A-Z])=([0-9a-f]{8})")
RE_EIP  = re.compile(r"^EIP=([0-9a-f]{8}) EFL=([0-9a-f]{8})")
RE_SEG  = re.compile(r"^(ES|CS|SS|DS|FS|GS|LDT|TR) =([0-9a-f]{4}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})")

def parse(path):
    """One record per CPU dump, in order, each carrying any exception
    reported immediately before it."""
    out, cur, exc = [], None, None
    for line in open(path, errors="replace"):
        m = RE_EXC.match(line)
        if m:
            exc = (int(m.group(1), 16), int(m.group(2), 16))
            continue
        if line.startswith("EAX="):
            if cur is not None: out.append(cur)
            cur = {"regs": {}, "segs": {}, "exc": exc, "eip": None}
            exc = None
        if cur is None:
            continue
        m = RE_EIP.match(line)
        if m:
            cur["eip"], cur["efl"] = int(m.group(1), 16), int(m.group(2), 16)
            continue
        m = RE_SEG.match(line)
        if m:
            cur["segs"][m.group(1)] = tuple(int(x, 16) for x in m.groups()[1:])
            continue
        if line[0].isupper() and "=" in line:
            for k, v in RE_GPR.findall(line):
                cur["regs"][k] = int(v, 16)
    if cur is not None: out.append(cur)
    return out

def cases(states, under):
    """(pre, outcome) per execution. QEMU dumps a faulting instruction
    twice — once before it runs, once as the exception is taken — so a
    run of dumps at `under` is one case, and what follows is its result."""
    i, res = 0, []
    while i < len(states):
        if states[i].get("eip") != under:
            i += 1; continue
        pre, j = states[i], i + 1
        while j < len(states) and states[j].get("eip") == under:
            if states[j]["exc"]: pre = dict(pre, exc=states[j]["exc"])
            j += 1
        res.append((pre, states[j] if j < len(states) else None))
        i = j
    return res

def records(img=None):
    """Per-case results from QEMU, in the shape pmbochs.py also returns."""
    img = img or os.path.join(HERE, "pmtest.img")
    under, after, fault, ncases = footer(img)
    spec = pmcases.cases(img)
    run_qemu(img)
    out = []
    for i, (pre, post) in enumerate(cases(parse(LOG), under)):
        tgt, sel = (spec[i][0], spec[i][1]) if i < len(spec) else ("?", 0)
        r = {"target": tgt, "sel": sel}
        if pre["exc"]:
            r.update(faulted=True, vec=pre["exc"][0], err=pre["exc"][1])
        elif post is None:
            r.update(faulted=None)
        else:
            d = post["segs"].get(tgt.upper(), (0, 0, 0, 0))
            r.update(faulted=False, seg=d[0], base=d[1], limit=d[2], ar=d[3] >> 8)
        out.append(r)
    return out

def show(recs, title):
    print(title)
    for r in recs:
        if r["faulted"]:
            extra = "  #%02X err=%04X" % (r["vec"], r["err"]) if "vec" in r else ""
            print("  %s <- %04X  fault%s" % (r["target"], r["sel"], extra))
        else:
            print("  %s <- %04X  ok  %04X base=%08X limit=%08X ar=%06X"
                  % (r["target"], r["sel"], r["seg"], r["base"], r["limit"], r["ar"]))

if __name__ == "__main__":
    show(records(), "QEMU:")
