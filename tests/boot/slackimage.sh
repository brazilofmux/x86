#!/bin/sh
# slackimage.sh — a disk to install Slackware 3.1 ("Slackware 96", Linux
# 2.0.0, 1996) on, the way INSTALL.TXT's section 3.4.2.2 has it: from an
# MS-DOS partition holding the disk sets as directories.
#
#   tests/boot/slackimage.sh [SLACKDIR]      -> disks/slack31/slack.img
#
# SLACKDIR (default disks/slack31) holds a copy of the release's
# slakware/ tree, kernels/bare.i/zImage and rootdsks/color.gz, from
# https://mirrors.slackware.com/slackware/slackware-3.1/ (the mirror no
# longer has procmail, seyon, xfileman or xv; setup does without).
# tests/boot/slackinstall.sh then installs from it.
#
# A 504 MB disk (1015 x 16 x 63: under 528 MB, so no translation for a
# 1996 kernel or LILO to worry about), partitioned on cylinder boundaries
# as DOS's FDISK would:
#   hda1  FAT16, 120 MB (cylinders 2-245): \SLAKWARE\A1 ..., and \LINUX
#         with the install kernel (bare.i: IDE) and root disk (color.gz)
#   hda2  Linux, cylinders 246-946 — setup's to format
#   hda3  Linux swap, cylinders 947-1014, 33 MB
# There is no floppy controller on this machine, so there are no boot and
# root diskettes: GRUB 2, in the two cylinders before hda1, does what
# LOADLIN would have from DOS — loads bare.i with color.gz as its initrd
# and root=/dev/ram, and the root disk's setup runs from memory. Needs
# mtools and GRUB for i386-pc (Homebrew: i686-elf-grub).
set -e
cd "$(dirname "$0")/../.."
S=${1:-disks/slack31}
OUT=disks/slack31/slack.img
G=${GRUB_I386_PC:-$(brew --prefix i686-elf-grub 2>/dev/null)/lib/i686-elf/grub/i386-pc}
MKIMAGE=${GRUB_MKIMAGE:-i686-elf-grub-mkimage}
T=$(mktemp -d "${TMPDIR:-/tmp}/slackimage.XXXXXX")
H=16; SPT=63; C=1015; CYL=$((H * SPT))
P1=$((2 * CYL)); P1N=$((244 * CYL))
P2=$((246 * CYL)); P2N=$((701 * CYL))
P3=$((947 * CYL)); P3N=$((68 * CYL))
python3 - "$OUT" $H $SPT $C $P1 $P1N $P2 $P2N $P3 $P3N <<'E'
import struct, sys
out, H, S, C, *p = sys.argv[1], *map(int, sys.argv[2:])
def chs(l):
    c = l // (H * S); r = l % (H * S); h = r // S; s = r % S + 1
    return bytes([h, s | ((c >> 2) & 0xC0), c & 0xFF])
m = bytearray(512)
for i, (boot, typ, start, n) in enumerate([(0x80, 0x06, p[0], p[1]), (0, 0x83, p[2], p[3]), (0, 0x82, p[4], p[5])]):
    m[446 + 16 * i:462 + 16 * i] = bytes([boot]) + chs(start) + bytes([typ]) + chs(start + n - 1) + struct.pack('<II', start, n)
m[510:512] = b'\x55\xaa'
with open(out, 'wb') as f:
    f.write(m); f.truncate(C * H * S * 512)
E
I="$OUT@@$((P1 * 512))"
mformat -i "$I" -T $P1N -h $H -s $SPT -H $P1 -v SLACKWARE ::
mmd -i "$I" ::/LINUX ::/SLAKWARE
mcopy -i "$I" "$S/kernels/bare.i/zImage" ::/LINUX/ZIMAGE
mcopy -i "$I" "$S/rootdsks/color.gz" ::/LINUX/COLOR.GZ
# A disk the mirror has emptied (xap4 held only xv) is left out, and its
# install.end — the end of its series — goes on the disk before it: an
# empty one has setup take the unmatched "*.tgz" for a package, and stop
# to ask about it.
prev=
for d in "$S"/slakware/*/; do
    n=$(basename "$d")
    if ! ls "$d"*.tgz >/dev/null 2>&1; then
        [ -f "$d/install.end" ] && [ -n "$prev" ] && mcopy -o -i "$I" "$d/install.end" "::/SLAKWARE/$prev/"
        continue
    fi
    mmd -i "$I" "::/SLAKWARE/$n"
    mcopy -i "$I" "$d"* "::/SLAKWARE/$n/"
    prev=$n
done
printf 'set root=(hd0,msdos1)\nlinux16 /linux/zimage root=/dev/ram\ninitrd16 /linux/color.gz\nboot\n' > "$T/early.cfg"
"$MKIMAGE" -O i386-pc -d "$G" -o "$T/core.img" -c "$T/early.cfg" -p '(hd0,msdos1)/boot/grub' biosdisk part_msdos fat linux16 boot
python3 - "$OUT" "$G/boot.img" "$T/core.img" $P1 <<'E'
import sys
out, bootimg, coreimg, p1 = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
b = open(bootimg, 'rb').read(); c = open(coreimg, 'rb').read()
assert len(c) <= (p1 - 1) * 512, len(c)
with open(out, 'r+b') as f:
    mbr = bytearray(f.read(512))
    mbr[0:0x1B8] = b[0:0x1B8]          # GRUB's boot code; the partition table stays
    f.seek(0); f.write(mbr)
    f.seek(512); f.write(c)            # core.img from LBA 1, where its blocklist says
E
rm -rf "$T"
echo "$OUT"
