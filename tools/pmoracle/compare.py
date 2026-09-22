#!/usr/bin/env python3
"""Run the oracle kernel under both references and diff them.

Agreement is evidence, not proof — both are software. A disagreement is
the useful output: it names a case to settle against the Intel manual.
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pmrun, pmbochs

def key(r):
    if r["faulted"] is None: return ("truncated",)
    if r["faulted"]: return ("fault",)
    return ("ok", r["ds"], r["base"], r["limit"], r["ar"])

q = pmrun.records()
under, after, fault, n = pmrun.footer(os.path.join(HERE, "pmtest.img"))
b = pmbochs.run(os.path.join(HERE, "pmtest.img"), under, fault, n)

print("QEMU %d cases, Bochs %d cases" % (len(q), len(b)))
agree = soft = hard = 0
for i, (x, y) in enumerate(zip(q, b)):
    if x["sel"] != y["sel"]:
        print("  case %d: selectors out of step (%04X vs %04X)" % (i, x["sel"], y["sel"])); break
    if key(x) == key(y):
        agree += 1; continue
    def desc(r):
        return "fault" if r["faulted"] else ("ok DS=%04X base=%08X limit=%08X ar=%06X"
               % (r["ds"], r["base"], r["limit"], r["ar"]))
    # Both loaded, same visible selector and geometry, only the access-rights
    # word of an unusable cache differs: an internal encoding, not behaviour.
    same_shape = (not x["faulted"] and not y["faulted"]
                  and (x["ds"], x["base"], x["limit"]) == (y["ds"], y["base"], y["limit"]))
    if same_shape:
        soft += 1
        print("  sel %04X  differ in cache encoding only (ar %06X vs %06X)"
              % (x["sel"], x["ar"], y["ar"]))
        continue
    hard += 1
    print("  sel %04X  DISAGREE" % x["sel"])
    print("      qemu  %s" % desc(x))
    print("      bochs %s" % desc(y))
print("%d agree, %d encoding-only, %d behavioural" % (agree, soft, hard))
if hard == 0 and soft:
    print("note: agreement is evidence, not proof — both are software.")
