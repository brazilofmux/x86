#!/bin/sh
# tcimage.sh — the Tiny Core Linux disk image the boot tests use, from
# Tiny Core's own ISO: a 64 MB disk (130 x 16 x 63) with one FAT16
# partition at 1 MB holding the kernel and initramfs, GRUB 2's boot.img
# in the MBR (the partition table kept) and its core.img in the gap
# before the partition. core.img carries its own script: load the kernel
# with the console on the VGA and COM1, the initramfs, boot.
#
#   tests/boot/tcimage.sh [Core-16.2.iso]      -> disks/linux/tc.img
#
# Tiny Core 16.x's 32-bit build still runs on a Pentium (no CMOV). The ISO
# comes from https://distro.ibiblio.org/tinycorelinux/16.x/x86/release/
# (MD5 26a1b47200d4820d46f161b783f5e6eb for Core-16.2.iso). Needs 7zz,
# mtools and GRUB for i386-pc (Homebrew: i686-elf-grub).
set -e
cd "$(dirname "$0")/../.."
ISO=${1:-disks/linux/Core-16.2.iso}
OUT=disks/linux/tc.img
G=${GRUB_I386_PC:-$(brew --prefix i686-elf-grub 2>/dev/null)/lib/i686-elf/grub/i386-pc}
MKIMAGE=${GRUB_MKIMAGE:-i686-elf-grub-mkimage}
T=$(mktemp -d "${TMPDIR:-/tmp}/tcimage.XXXXXX")
7zz x -o"$T" "$ISO" boot/vmlinuz boot/core.gz -r > /dev/null
python3 - "$OUT" <<'E'
import struct, sys
H, S, C = 16, 63, 130
total, start = C * H * S, 2048
def chs(l):
    c = l // (H * S); r = l % (H * S); h = r // S; s = r % S + 1
    return bytes([h, s | ((c >> 2) & 0xC0), c & 0xFF])
m = bytearray(512)
m[446:462] = bytes([0x80]) + chs(start) + bytes([0x06]) + chs(total - 1) + struct.pack('<II', start, total - start)
m[510:512] = b'\x55\xaa'
with open(sys.argv[1], 'wb') as f:
    f.write(m); f.truncate(total * 512)
E
mformat -i "$OUT@@1048576" -t 128 -h 16 -s 63 -H 2048 -v TINYCORE ::
mcopy -i "$OUT@@1048576" "$T/boot/vmlinuz" "$T/boot/core.gz" ::
printf 'set root=(hd0,msdos1)\nlinux /vmlinuz console=ttyS0 console=tty0 loglevel=7 noapic\ninitrd /core.gz\nboot\n' > "$T/early.cfg"
"$MKIMAGE" -O i386-pc -d "$G" -o "$T/core.img" -c "$T/early.cfg" -p '(hd0,msdos1)/boot/grub' biosdisk part_msdos fat linux boot
python3 - "$OUT" "$G/boot.img" "$T/core.img" <<'E'
import sys
out, bootimg, coreimg = sys.argv[1:]
b = open(bootimg, 'rb').read(); c = open(coreimg, 'rb').read()
assert len(c) <= 2047 * 512, len(c)
with open(out, 'r+b') as f:
    mbr = bytearray(f.read(512))
    mbr[0:0x1B8] = b[0:0x1B8]          # GRUB's boot code; the partition table stays
    f.seek(0); f.write(mbr)
    f.seek(512); f.write(c)            # core.img from LBA 1, where its blocklist says
E
rm -rf "$T"
echo "$OUT"
