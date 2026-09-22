#!/bin/sh
# jitdis.sh <dump.bin> <key-suffix> — disassemble one translated block from an
# X86_JIT_DUMP file (e.g. tools/jitdis.sh /tmp/jit.bin 10109). Apple's
# objdump has no raw-binary mode, so the bytes are wrapped in a Mach-O
# object with `as` first.
bin=$1; key=$2
line=$(grep -v '^#' "$bin.idx" | grep ":0*$key " | head -1)
[ -z "$line" ] && { echo "no block $key"; exit 1; }
off=$(echo "$line" | awk '{print $2}')
next=$(grep -v '^#' "$bin.idx" | awk -v o="$off" '$2 > o {print $2}' | sort -n | head -1)
[ -z "$next" ] && next=$(stat -f %z "$bin")
dd if="$bin" bs=1 skip="$off" count=$((next - off)) 2>/dev/null | od -An -v -tx1 | \
  awk 'BEGIN{print ".text"} {for(i=1;i<=NF;i++) printf ".byte 0x%s\n", $i}' > /tmp/jitdis.s
as -arch arm64 -o /tmp/jitdis.o /tmp/jitdis.s && objdump -d --no-show-raw-insn /tmp/jitdis.o | tail -n +5
