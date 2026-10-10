#!/usr/bin/env python3
"""hivewalk.py — list keys of an NT registry hive file (REGF) whose path
matches a pattern, with their subkeys and values:

    tools/hivewalk.py SYSTEM 'Enum\\Root'

Enough to see what a guest's setup left in a hive on an image (mcopy the
file out of the FAT partition first) without loading it anywhere: cells
are found by their nk/vk signatures, so a damaged hive still yields what
it has. Names print compressed (ASCII) or UTF-16 as stored; binary data
as hex, strings decoded; REG_MULTI_SZ with | between its strings.
"""
import sys, struct, re
d = open(sys.argv[1], 'rb').read()
B = 0x1000
nks = {}
for m in re.finditer(b'nk', d):
    o = m.start()
    if o < B: continue
    try:
        flags = struct.unpack_from('<H', d, o + 2)[0]
        parent = struct.unpack_from('<I', d, o + 0x10)[0]
        nsub = struct.unpack_from('<I', d, o + 0x14)[0]
        sublist = struct.unpack_from('<I', d, o + 0x1C)[0]
        nval = struct.unpack_from('<I', d, o + 0x24)[0]
        vallist = struct.unpack_from('<I', d, o + 0x28)[0]
        namelen = struct.unpack_from('<H', d, o + 0x48)[0]
        if namelen == 0 or namelen > 200: continue
        name = d[o + 0x4C:o + 0x4C + namelen]
        if not (flags & 0x20): name = name.decode('utf-16-le', 'replace').encode()
        nks[o] = (name, parent, nsub, sublist, nval, vallist)
    except Exception: pass
def path(o):
    parts = []
    for _ in range(40):
        if o not in nks: break
        name, parent = nks[o][0], nks[o][1]
        parts.append(name.decode('latin-1'))
        o = B + parent + 4
    return '\\'.join(reversed(parts))
def values(v):
    nval, vallist = v[4], v[5]
    lo = B + vallist + 4
    out = []
    for i in range(nval):
        vo = struct.unpack_from('<I', d, lo + 4 * i)[0]
        c = B + vo + 4
        if d[c:c+2] != b'vk': continue
        namelen, datalen, dataoff, typ, vflags = struct.unpack_from('<HIIIH', d, c + 2)
        name = d[c + 20:c + 20 + namelen]
        n = datalen & 0x7FFFFFFF
        data = struct.pack('<I', dataoff)[:n] if datalen & 0x80000000 else d[B + dataoff + 4: B + dataoff + 4 + n]
        shown = data.decode('utf-16-le', 'replace') if typ in (1, 2, 7) else data.hex()
        out.append('%s=%s' % (name.decode('latin-1') or '(default)', shown[:80].replace('\x00', '|')))
    return out
def subkeys(v):
    if not v[2]: return []
    lo = B + v[3] + 4
    sig = d[lo:lo+2]; cnt = struct.unpack_from('<H', d, lo + 2)[0]
    step = 8 if sig in (b'lf', b'lh') else 4
    return [nks.get(B + struct.unpack_from('<I', d, lo + 4 + step * i)[0] + 4, (b'?',))[0].decode('latin-1') for i in range(cnt)]
pat = sys.argv[2] if len(sys.argv) > 2 else ''
for o, v in sorted(nks.items(), key=lambda kv: path(kv[0])):
    p = path(o)
    if re.search(pat, p):
        print(p, '| sub:', subkeys(v), '| val:', values(v))
