#!/usr/bin/env python3
"""Run the oracle kernel under both references and diff them.

Agreement is evidence, not proof — both are software. A disagreement is
the useful output: it names a case to settle against the Intel manual.
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pmrun, pmbochs
from expected import EXPECT

def key(r):
    if r["faulted"] is None: return ("truncated",)
    if r["faulted"]: return ("fault",)
    return ("ok", r["seg"], r["base"], r["limit"], r["ar"], r.get("ss"), r.get("esp"))

import pmcases
IMG = os.path.join(HERE, "pmtest.img")
spec = pmcases.cases(IMG)
q = pmrun.records()
under, after, fault, n = pmrun.footer(IMG)
b = pmbochs.run(IMG, under, fault, n, spec)

def verdict(r):
    if r["faulted"] is None: return "?"
    if r["faulted"]: return ("#%02X" % r["vec"]) if "vec" in r else "fault"
    return "ok"

print("QEMU %d cases, Bochs %d cases\n" % (len(q), len(b)))
print("  case               qemu   bochs  wanted")
agree = soft = hard = strict = unlisted = accessed = 0
for x, y in zip(q, b):
    if (x["target"], x["sel"]) != (y["target"], y["sel"]):
        print("  cases out of step (%s %04X vs %s %04X)" % (x["target"], x["sel"], y["target"], y["sel"])); break
    vq, vb = verdict(x), verdict(y)
    exp = EXPECT.get((x["cpl"], x["target"], x["sel"], x["aux"]))
    ve = exp[0] if exp else "-"
    # Bochs' harness reports only "fault", so compare fault-vs-not there.
    def same(a, c): return a == c or (a.startswith("#") and c == "fault") or (c.startswith("#") and a == "fault")
    note = ""
    if exp is None:
        unlisted += 1; note = "no expectation recorded"
    elif not same(ve, vq) or not same(ve, vb):
        if exp[1] == "neither":
            strict += 1; note = "deliberately stricter: " + exp[2]
        else:
            hard += 1; note = "UNEXPECTED: " + exp[2]
    elif not x["faulted"] and not y["faulted"] and \
         (x["seg"], x["base"], x["limit"]) == (y["seg"], y["base"], y["limit"]) and x["ar"] != y["ar"]:
        # The accessed bit is architectural — hardware sets it and writes it
        # back to the descriptor — so it does not belong with the encoding noise.
        if (x["ar"] ^ y["ar"]) == 1:
            accessed += 1
            note = ("ACCESSED BIT: qemu %s, bochs %s"
                    % ("set" if x["ar"] & 1 else "clear", "set" if y["ar"] & 1 else "clear"))
        else:
            soft += 1
            note = "cache-encoding difference only (ar %06X vs %06X)" % (x["ar"], y["ar"])
    else:
        agree += 1
    tag = "%04X" % x["sel"] + (("/%04X" % x["aux"]) if x["aux"] else "")
    print("  cpl%d %-5s <- %-9s %-6s %-6s %-6s %s" % (x["cpl"], x["target"], tag, vq, vb, ve, note))

print("\n%d as expected, %d encoding-only, %d where we are deliberately stricter than both references,"
      % (agree, soft, strict))
print("%d unexpected, %d with no expectation recorded, %d accessed-bit differences."
      % (hard, unlisted, accessed))
if strict:
    print("\nThe stricter cases are a decision, not a measurement: see expected.py for the")
    print("reasoning. They are the ones to revisit first if a real client ever misbehaves.")
