#!/usr/bin/env python3
"""pngpix.py — print the RGB of pixels in a dos-monster PNG (8-bit RGB,
filter 0, as pc_video_png writes it):  pngpix.py FILE X,Y [X,Y ...]"""
import struct, sys, zlib

def load(path):
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n"
    i, w, h, idat = 8, 0, 0, b""
    while i < len(d):
        n = struct.unpack(">I", d[i:i + 4])[0]; t = d[i + 4:i + 8]; body = d[i + 8:i + 8 + n]
        if t == b"IHDR": w, h = struct.unpack(">II", body[:8])
        elif t == b"IDAT": idat += body
        i += 12 + n
    return w, h, zlib.decompress(idat)

w, h, raw = load(sys.argv[1])
for xy in sys.argv[2:]:
    x, y = map(int, xy.split(","))
    o = y * (1 + w * 3) + 1 + x * 3
    print("%d,%d %d %d %d" % (x, y, raw[o], raw[o + 1], raw[o + 2]))
