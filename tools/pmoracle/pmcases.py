"""Read the case table straight out of the image, so the harnesses and the
expectations agree on what each case is without a second source of truth."""
import struct

LOAD_ADDR = 0x7C00
# The two-byte opcodes the kernel uses, and which register each targets.
TARGET = {b"\x8e\xd8": "ds", b"\x8e\xd0": "ss", b"\x8e\xc0": "es",
          b"\x8e\xc8": "cs", b"\x8e\xe0": "fs", b"\x8e\xe8": "gs"}

def footer(img):
    d = open(img, "rb").read()
    magic, slot, after, fault, n, cases, width = struct.unpack_from("<7H", d, 496)
    if magic != 0x5350:
        raise SystemExit("%s: no oracle footer (rebuild pmtest.img)" % img)
    return dict(slot=slot, after=after, fault=fault, n=n, cases=cases, width=width)

def cases(img):
    """[(key, selector, instruction-bytes)] in execution order."""
    d = open(img, "rb").read()
    f = footer(img)
    out = []
    for i in range(f["n"]):
        off = f["cases"] - LOAD_ADDR + i * f["width"]
        raw = d[off:off + 8]
        sel = struct.unpack_from("<H", d, off + 8)[0]
        instr = raw.rstrip(b"\x90") or raw[:2]
        out.append((TARGET.get(bytes(instr[:2]), instr[:2].hex()), sel, bytes(instr)))
    return out
