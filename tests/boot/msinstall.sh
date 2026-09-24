#!/bin/sh
# Install MS-DOS 6.22 from its three diskettes onto a blank 128 MB disk:
#   tests/boot/msinstall.sh [out.img]      (default disks/msdos622/c.img)
# The diskette images are your own (disks/msdos622/, git-ignored).
# Setup won't partition a blank disk ("upgrade" edition), so the script
# leaves Setup, runs FDISK (which reboots through the reset vector), runs
# FORMAT C: /S, and then lets Setup finish over the fresh system.
cd "$(dirname "$0")/../.."
D=disks/msdos622
OUT=${1:-$D/c.img}
mkdir -p tmp
for i in 1 2 3; do cp $D/msdos622-$i.img tmp/msi-$i.img; done
FDA=tmp/msi-1.img:tmp/msi-2.img:tmp/msi-3.img
dd if=/dev/zero of="$OUT" bs=1m count=128 2>/dev/null
run() {   # expect script on stdin, timeout
    cat > tmp/msi.exp
    python3 tools/expect.py -t "$1" tmp/msi.exp -- ./dos-monster -m 386 -W -T "$(($1 - 10))" \
        -fda $FDA -hda "$OUT" -boot a >/dev/null 2>&1 || { echo "FAIL msinstall: $2"; exit 1; }
    echo "ok   $2"
}
run 120 "fdisk" <<'X'
To continue Setup, press ENTER|ENTER=Continue	\x1bOR
*Exiting Setup	\x1bOR
A:.>$	fdisk\r
*Enter choice: .1.	\r
*Create Primary DOS Partition	\r
*maximum available size	y\r
*restart|any key	\r
*Welcome to Setup|A:.>$	
X
run 120 "format c: /s" <<'X'
To continue Setup, press ENTER|ENTER=Continue	\x1bOR
*Exiting Setup	\x1bOR
A:.>$	format c: /s\r
*Proceed with Format|.Y/N.	y\r
*Volume label	\r
A:.>$	
X
run 300 "setup" <<'X'
To continue Setup, press ENTER|ENTER=Continue	\r
*Setup has found DOS files	\x1b[B\r
*The settings are correct	\r
*place your MS-DOS files	\r
*Setup Disk #2	\x1b+\r
*Setup Disk #3	\x1b+\r
*Remove disks	\r
*MS-DOS Setup Complete|Setup is complete	
X
rm -f tmp/msi-?.img tmp/msi.exp
