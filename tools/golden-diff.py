#!/usr/bin/env python3
"""golden-diff.py A B — compare two X86_GOLDEN translation logs.

Each log has one line per translation: key, code length, hash of the
emitted bytes. A key translated more than once (SMC invalidations, cache
flushes) may legitimately differ from itself only if the guest bytes or
the segment state changed, so the comparison is per key, in order of
translation: the n-th translation of a key in A must match the n-th in B.
Keys that one run reached and the other did not are counted, not failed:
the interpreter's clock is virtual but the host's is not (a DOS time
call, a keyboard poll), so the two runs may take slightly different
paths. Exit status 1 on any mismatching block.
"""
import sys
from collections import defaultdict

def load(path):
    d = defaultdict(list)
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 3: continue          # a run cut off mid-line (-T, a kill)
            key, n, h = parts
            d[key].append((int(n), h))
    return d

a, b = load(sys.argv[1]), load(sys.argv[2])
only_a = [k for k in a if k not in b]
only_b = [k for k in b if k not in a]
bad = 0
for k in a:
    if k not in b: continue
    for i, (x, y) in enumerate(zip(a[k], b[k])):
        if x != y:
            if bad < 20:
                print(f"key {k} translation {i}: A {x[0]} bytes {x[1]}, B {y[0]} bytes {y[1]}")
            bad += 1
common = sum(min(len(a[k]), len(b[k])) for k in a if k in b)
# Order-free view for runs whose timing differs (a boot, keys typed on a
# clock): per key, translations of A that B never produced and vice versa.
ua = sum(len(set(a[k]) - set(b[k])) for k in a if k in b)
ub = sum(len(set(b[k]) - set(a[k])) for k in a if k in b)
print(f"{common} translations compared, {bad} differ in sequence, {ua}+{ub} unmatched as sets; keys only in A: {len(only_a)}, only in B: {len(only_b)}")
sys.exit(1 if bad else 0)
