#!/bin/sh
# tools/x64enc.sh — check dbt/emit_x64.h's encodings against objdump.
# Builds tools/x64enc, emits the cases, disassembles, compares text.
cd "$(dirname "$0")/.." || exit 1
mkdir -p tmp
cc -Wall -Wextra -O0 -g -std=c11 -o tmp/x64enc tools/x64enc.c || exit 1
tmp/x64enc tmp/x64enc.bin tmp/x64enc.exp || exit 1
# objdump: one instruction per line; drop the address and hex columns,
# collapse spaces, and turn a branch's absolute target into "@next" when
# it is the next instruction's address.
objdump -D -b binary -m i386:x86-64 -M intel tmp/x64enc.bin | python3 -c '
import re, sys
lines, addrs = [], []
for l in sys.stdin:
    m = re.match(r"^ *([0-9a-f]+):\t(?:[0-9a-f]{2} )+ *(.*)$", l.rstrip("\n"))
    if not m or not m.group(2).strip(): continue        # header, or a continuation line of a long encoding
    lines.append(re.sub(r"\s+", " ", m.group(2)).strip()); addrs.append(int(m.group(1), 16))
for i, l in enumerate(lines):
    m = re.match(r"^(j[a-z]+|call) 0x([0-9a-f]+)$", l)
    if m and i + 1 < len(lines) and int(m.group(2), 16) == addrs[i + 1]: l = m.group(1) + " @next"
    print(l)
' > tmp/x64enc.got
if diff -u tmp/x64enc.exp tmp/x64enc.got > tmp/x64enc.diff; then
    echo "x64enc: $(wc -l < tmp/x64enc.exp) encodings match objdump"
else
    cat tmp/x64enc.diff; exit 1
fi
