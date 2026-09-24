#!/bin/sh
# Wall-clock milestones on the booted Windows 3.11 disk (disks/win311/c.img,
# tests/boot/wininstall.sh): power-on to C:\> (MS-DOS, HIMEM's memory test),
# then "win" to a drawn Program Manager — standard mode and 386 enhanced
# mode. expect.py -v stamps each step; the desktop is its title bar pixel
# and the desktop grey. The numbers the enhanced-mode JIT campaign tracks.
cd "$(dirname "$0")/../.."
DM=${DM:-./dos-monster}      # the binary under test (a cross build: build-a64/dos-monster)
mkdir -p tmp
for mode in "win /s" "win"; do
    cp disks/win311/c.img tmp/bench.img
    printf 'C:.>$\t%s\\r\nwaitpix 10,10 195 199 203\nwaitpix 150,61 0 0 170\n' "$mode" > tmp/bench.exp
    python3 tools/expect.py -v -t 120 tmp/bench.exp -- $DM -m 386 -W -T 110 -hda tmp/bench.img -boot c 2>&1 >/dev/null \
        | awk -v m="$mode" '/C:\.>/ { gsub(/[][s]/, "", $2); dos = $2 } /150,61/ { gsub(/[][s]/, "", $2); printf "%-7s  C:\\> at %5.2f s   Program Manager %5.2f s later\n", m, dos, $2 - dos }'
done
rm -f tmp/bench.img tmp/bench.exp
