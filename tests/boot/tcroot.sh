#!/bin/sh
# tcroot.sh — Tiny Core's userland on an ext2 root partition of our IDE
# disk, for Linux to boot from with no initramfs: every file after the
# kernel comes through the IDE controller (Linux's pata_legacy).
#
#   tests/boot/tcroot.sh [Core-16.2.iso]      -> disks/linux/tcroot.img
#
# A 128 MB disk (260 x 16 x 63): partition 1, FAT16 from 1 MB, the kernel;
# partition 2, ext2 filling the rest, core.gz unpacked (device nodes left
# out: the kernel mounts devtmpfs on /dev itself, devtmpfs.mount=1). GRUB
# 2 as in tcimage.sh, its script booting root=/dev/sda2. Needs 7zz,
# mtools, e2fsprogs and GRUB for i386-pc (Homebrew: e2fsprogs,
# i686-elf-grub).
set -e
cd "$(dirname "$0")/../.."
ISO=${1:-disks/linux/Core-16.2.iso}
OUT=disks/linux/tcroot.img
G=${GRUB_I386_PC:-$(brew --prefix i686-elf-grub 2>/dev/null)/lib/i686-elf/grub/i386-pc}
MKIMAGE=${GRUB_MKIMAGE:-i686-elf-grub-mkimage}
MKE2FS=${MKE2FS:-$(brew --prefix e2fsprogs 2>/dev/null)/sbin/mke2fs}
T=$(mktemp -d "${TMPDIR:-/tmp}/tcroot.XXXXXX")
7zz x -o"$T" "$ISO" boot/vmlinuz boot/core.gz -r > /dev/null
mkdir "$T/root"
(cd "$T/root" && gzip -dc ../boot/core.gz | cpio -idm --quiet 2>/dev/null || true)
rm -rf "$T/root/dev" && mkdir "$T/root/dev"
chmod -R u+rw "$T/root"                               # (sudo comes as 4711: mke2fs must read it)
H=16; S=63; C=260; P1=2048; P1N=65536                 # 32 MB for the kernel
TOTAL=$((C * H * S)); P2=$((P1 + P1N)); P2N=$((TOTAL - P2))
python3 - "$OUT" $H $S $C $P1 $P1N $P2 $P2N <<'E'
import struct, sys
out, H, S, C, p1, p1n, p2, p2n = sys.argv[1], *map(int, sys.argv[2:])
total = C * H * S
def chs(l):
    c = l // (H * S); r = l % (H * S); h = r // S; s = r % S + 1
    c = min(c, 1023)
    return bytes([h, s | ((c >> 2) & 0xC0), c & 0xFF])
m = bytearray(512)
m[446:462] = bytes([0x80]) + chs(p1) + bytes([0x06]) + chs(p1 + p1n - 1) + struct.pack('<II', p1, p1n)
m[462:478] = bytes([0x00]) + chs(p2) + bytes([0x83]) + chs(p2 + p2n - 1) + struct.pack('<II', p2, p2n)
m[510:512] = b'\x55\xaa'
with open(out, 'wb') as f:
    f.write(m); f.truncate(total * 512)
E
mformat -i "$OUT@@$((P1 * 512))" -T $P1N -h $H -s $S -H $P1 -v TCBOOT ::
mcopy -i "$OUT@@$((P1 * 512))" "$T/boot/vmlinuz" ::
"$MKE2FS" -q -t ext2 -L TCROOT -E root_owner=0:0 -d "$T/root" "$T/p2.img" $((P2N / 2))k
# every file root's, as in the initramfs (mke2fs -d keeps the unpacker's uid)
python3 - "$T/root" > "$T/chown.cmds" <<'OWNERS'
import os, sys
top = sys.argv[1]
for d, dirs, files in os.walk(top):
    for n in [d] + [os.path.join(d, f) for f in dirs + files]:
        p = '/' + os.path.relpath(n, top)
        p = '/' if p == '/.' else p
        print('sif "%s" uid 0' % p); print('sif "%s" gid 0' % p)
OWNERS
"$(dirname "$MKE2FS")/debugfs" -w -f "$T/chown.cmds" "$T/p2.img" > /dev/null 2>&1
dd if="$T/p2.img" of="$OUT" bs=512 seek=$P2 conv=notrunc 2>/dev/null
printf 'set root=(hd0,msdos1)\nlinux /vmlinuz root=/dev/sda2 rw devtmpfs.mount=1 console=ttyS0 console=tty0 loglevel=7 noapic\nboot\n' > "$T/early.cfg"
"$MKIMAGE" -O i386-pc -d "$G" -o "$T/core.img" -c "$T/early.cfg" -p '(hd0,msdos1)/boot/grub' biosdisk part_msdos fat linux boot
python3 - "$OUT" "$G/boot.img" "$T/core.img" <<'E'
import sys
out, bootimg, coreimg = sys.argv[1:]
b = open(bootimg, 'rb').read(); c = open(coreimg, 'rb').read()
assert len(c) <= 2047 * 512, len(c)
with open(out, 'r+b') as f:
    mbr = bytearray(f.read(512))
    mbr[0:0x1B8] = b[0:0x1B8]
    f.seek(0); f.write(mbr)
    f.seek(512); f.write(c)
E
rm -rf "$T"
echo "$OUT"
