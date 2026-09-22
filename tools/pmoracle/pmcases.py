"""Read the case table straight out of the image, so the harnesses and the
expectations agree on what each case is without a second source of truth."""
import struct

LOAD_ADDR = 0x7C00
# The two-byte opcodes the kernel uses, and which register each targets.
TARGET = {b"\x8e\xd8": "ds", b"\x8e\xd0": "ss", b"\x8e\xc0": "es",
          b"\x8e\xe0": "fs", b"\x8e\xe8": "gs"}
# A far JMP/CALL carries its selector in the instruction, not in AX.
FAR = {0xEA: "jmpf", 0x9A: "callf"}

def footer(img):
    d = open(img, "rb").read()
    magic, slot, after, fault, n, cases, width = struct.unpack_from("<7H", d, 496)
    if magic != 0x5350:
        raise SystemExit("%s: no oracle footer (rebuild pmtest.img)" % img)
    return dict(slot=slot, after=after, fault=fault, n=n, cases=cases, width=width)

def cases(img):
    """[(key, selector, instruction-bytes, cpl)] in execution order."""
    d = open(img, "rb").read()
    f = footer(img)
    out = []
    for i in range(f["n"]):
        off = f["cases"] - LOAD_ADDR + i * f["width"]
        raw = d[off:off + 8]
        sel = struct.unpack_from("<H", d, off + 8)[0]
        ring = struct.unpack_from("<H", d, off + 10)[0]
        instr = raw.rstrip(b"\x90") or raw[:2]
        if raw[0] in FAR:
            sel = struct.unpack_from("<H", raw, 5)[0]
            out.append((FAR[raw[0]], sel, bytes(raw[:7]), ring))
        else:
            out.append((TARGET.get(bytes(instr[:2]), instr[:2].hex()), sel, bytes(instr), ring))
    return out
