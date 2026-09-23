#!/bin/sh
# Install Windows 3.11 onto a copy of the MS-DOS 6.22 disk:
#   tests/boot/wininstall.sh [out.img]      (default disks/win311/c.img)
# From the six OEM 3.5" diskettes (disks/win311/oem/DISK1-6.IMG, the
# user's download, git-ignored), flattened into C:\WININST — the machine
# has no CD-ROM, and Setup finds every disk's files in one directory by
# the DISKn marker files. Setup's DOS half is driven by its text; its
# Windows half (standard mode, VGA mode 12h) blind, by keys and delays:
# name, printer (none), the applications it finds, the tutorial, reboot.
cd "$(dirname "$0")/../.."
W=disks/win311
OUT=${1:-$W/c.img}
[ -f disks/msdos622/c.img ] || { echo "wininstall: needs disks/msdos622/c.img (msinstall.sh)"; exit 1; }
mkdir -p tmp tmp/winflat
rm -f tmp/winflat/*
for i in 1 2 3 4 5 6; do mcopy -n -i $W/oem/DISK$i.IMG '::/*' tmp/winflat/ 2>/dev/null; done
cp disks/msdos622/c.img "$OUT"
mmd -i "$OUT"@@32256 ::/WININST
mcopy -i "$OUT"@@32256 tmp/winflat/* ::/WININST/
cat > tmp/wini.exp <<'X'
C:.>$	cd \\wininst\rsetup\r
*Welcome to Setup	\r
*Express Setup .Recommended	\r
delay 25
send DOS Monster\r
delay 5
send \r
delay 60
send \r
delay 30
send \r
delay 15
send \r
delay 15
send \r
delay 15
send \r
C:.>$	
X
python3 tools/expect.py -t 400 tmp/wini.exp -- ./dos-monster -m 386 -W -T 390 -hda "$OUT" -boot c >/dev/null 2>&1 \
    || { echo "FAIL wininstall"; exit 1; }
mdir -i "$OUT"@@32256 ::/WINDOWS/WIN.COM >/dev/null 2>&1 || { echo "FAIL wininstall: no WIN.COM"; exit 1; }
mdeltree -i "$OUT"@@32256 ::/WININST >/dev/null 2>&1
echo "ok   windows 3.11 installed"
rm -rf tmp/winflat tmp/wini.exp
