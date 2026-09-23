#!/usr/bin/env python3
"""pnghash.py FILE — width, height and the SHA-1 of the decoded RGB pixels
of an 8-bit RGB PNG (what dos-monster -G writes), independent of how zlib
compressed them."""
import hashlib, struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
i, idat, w, h = 8, b'', 0, 0
while i < len(d):
    n = struct.unpack('>I', d[i:i + 4])[0]; t = d[i + 4:i + 8]
    if t == b'IHDR': w, h = struct.unpack('>II', d[i + 8:i + 16])
    if t == b'IDAT': idat += d[i + 8:i + 8 + n]
    i += 12 + n
raw = zlib.decompress(idat)
px = b''.join(raw[y * (1 + w * 3) + 1:(y + 1) * (1 + w * 3)] for y in range(h))
print(w, h, hashlib.sha1(px).hexdigest())
