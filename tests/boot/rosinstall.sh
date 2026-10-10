#!/bin/sh
# Install ReactOS 0.4.16 onto a 2 GB disk, from its hybrid ISO as the
# second hard disk:
#   tests/boot/rosinstall.sh [out.img]      (default disks/reactos/reactos-installed.img)
# Needs disks/reactos/ReactOS-0.4.16-i386.iso (ReactOS-0.4.16-i386.zip,
# sha256 e5851510cf5b79ef51a76b8155dd5ef7faca1155f44d0a1d41c35b77ece4ce2a,
# from sourceforge.net/projects/reactos) and the PCI machine (-pci: the
# IDE channel as a PCI function, which ReactOS's ATA driver wants) with
# the VESA display (-vbe: the display as a PCI function, for its VESA
# driver).
#
# Stage 1, the text-mode setup, is driven from its screens: English,
# a partition over the whole first disk, FAT, quick format, the boot
# loader into the MBR, \ReactOS; it copies its files (about a minute)
# and reboots — into the medium's "press any key" prompt, since the
# first disk is bootable now, which is where the stage ends. Stage 2, the
# wizard on the desktop, is driven blind (a graphics mode: no text to
# match): Next through its pages, "dos-monster" as the owner, the
# defaults otherwise; it registers components for ten minutes or so and
# reboots to the desktop, whose icon labels and wallpaper are the end.
#
# ReactOS's own flakiness: its FAT driver asserts now and then during the
# copy (CORE-19741, "ClustersFree == FreeClusterBitMapClear"), and a
# stuck setup is the result — run it again. The ISO's menu defaults to
# the live desktop after three seconds; a copy with the setup as its
# default and a longer timeout is made in tmp/ for the run.
cd "$(dirname "$0")/../.."
S=disks/reactos
ISO=$S/ReactOS-0.4.16-i386.iso
OUT=${1:-$S/reactos-installed.img}
[ -f $ISO ] || { echo "rosinstall: needs $ISO"; exit 1; }
mkdir -p tmp
python3 - "$ISO" tmp/ros-setup.iso <<'E'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
a, b = b'DefaultOS=LiveImg\r\nTimeOut=3\r\n', b'DefaultOS=Setup\r\n\r\nTimeOut=9\r\n'
assert d.count(a) == 1 and len(a) == len(b)
i = d.find(a); d[i:i + len(a)] = b
open(sys.argv[2], 'wb').write(d)
E
rm -f "$OUT"; truncate -s 2G "$OUT"
cat > tmp/ros1.exp <<'X'
*Language Selection	\r
*Welcome to ReactOS Setup	\r
*ReactOS Version Status	\r
*Accept these device settings	\r
*Harddisk 1 \(Port=0, Bus=0, Id=1\)	c
*ENTER = Create Partition	\r
*ENTER = Install	\r
*This Partition will be formatted next	\r
*Setup will now format the partition	\r
*select where Setup should install the bootloader	\r
*want ReactOS to be installed	\r
*Press any key to boot from the ReactOS medium	
X
python3 tools/expect.py -t 1200 tmp/ros1.exp -- ./dos-monster -m 586 -mem 128 -vbe -pci -W -T 1190 -hda "$OUT" -hdb tmp/ros-setup.iso -boot d >/dev/null 2>&1 \
    || { echo "rosinstall: stage 1 (text-mode setup) did not finish"; exit 1; }
# stage 2: the wizard — welcome, acknowledgements, installation type, the
# owner's name, computer name, regional settings, date and time, the
# workgroup; then the desktop: a white stroke of "My Documents" and the
# wallpaper's teal
printf 'delay 150\nsend \\r\ndelay 8\nsend \\r\ndelay 8\nsend \\r\ndelay 8\nsend dos-monster\\r\ndelay 8\nsend \\r\ndelay 8\nsend \\r\ndelay 8\nsend \\r\ndelay 8\nsend \\r\nwaitpix 12,54 255 255 255\nwaitpix 20,100 85 170 170\n' > tmp/ros2.exp
python3 tools/expect.py -t 1500 tmp/ros2.exp -- ./dos-monster -m 586 -mem 128 -vbe -pci -W -T 1490 -hda "$OUT" -boot c >/dev/null 2>&1 \
    && echo "$OUT" || { echo "rosinstall: stage 2 (the wizard) did not reach the desktop"; exit 1; }
