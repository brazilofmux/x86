#!/bin/sh
# tests/dos/fetch-djgpp.sh — DJGPP v2.05 into disks/djgpp, as a real DPMI client.
#
# The .exe files are DJGPP-stubbed COFF programs: a 16-bit real-mode stub
# finds a DPMI host, switches to protected mode and loads a 32-bit payload.
# Between them they exercise nearly the whole host — LDT descriptors, extended
# and DOS memory, real-mode excursions, callbacks, exception handlers and the
# 387 emulator's #NM path — which is why tests/dos/run.sh uses them when they
# are present. disks/ is git-ignored; bring your own copy with this.
set -e
cd "$(dirname "$0")/../.."
mkdir -p disks/djgpp
cd disks/djgpp
[ -f djdev205.zip ] || curl -fSL -o djdev205.zip https://www.delorie.com/pub/djgpp/current/v2/djdev205.zip
unzip -o -q djdev205.zip 'bin/*'
echo "djgpp: $(ls bin | wc -l | tr -d ' ') programs in $(pwd)/bin"
