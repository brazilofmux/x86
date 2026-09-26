#!/bin/sh
# Install Slackware 3.1 onto the disk slackimage.sh made:
#   tests/boot/slackinstall.sh [out.img]      (default disks/slack31/slack-installed.img)
# The root disk's own setup, driven by its dialogs' titles, answering as
# a 1996 owner of this machine would: swap on hda3 (mkswap'd), hda2
# formatted as /, the DOS partition as /dosc; source the hard drive
# partition hda1, directory /slakware; the disk sets A AP D E F N X XAP
# Y, installed without prompting; no bootdisk (no floppy drive), a PS/2
# mouse, no modem or CD-ROM, LILO in the MBR (5 s, "Linux" = hda2),
# loopback networking as darkstar.frop.org, no gpm (X wants the mouse),
# sendmail without a nameserver, US/Mountain. About 45 minutes, most of
# it gzip and tar in 16 MB. The console blanks after ten idle minutes,
# which would hide the prompts after the packages: blanking goes off
# first.
cd "$(dirname "$0")/../.."
S=disks/slack31
OUT=${1:-$S/slack-installed.img}
[ -f $S/slack.img ] || { echo "slackinstall: needs $S/slack.img (tests/boot/slackimage.sh)"; exit 1; }
mkdir -p tmp
cp $S/slack.img "$OUT"
D='\x1b[B'
{
cat <<'X'
login:	root\r
[#]\s*$	setterm -blank 0; echo -e '\\033[9;0]'; setup\r
*Slackware96 Linux Setup	a\r
*SWAP SPACE DETECTED	\r
*MKSWAP WARNING	\r
*USE MKSWAP	\r
*ACTIVATE SWAP SPACE	\r
*SWAP SPACE CONFIGURED	\r
*CONTINUE WITH INSTALLATION	\r
*Select Linux installation partition	\r
*FORMAT PARTITION	\r
*SELECT INODE DENSITY	\r
*DOS AND OS/2 PARTITION SETUP	\r
*CHOOSE PARTITION	/dev/hda1\r
*SELECT MOUNT POINT	/dosc\r
*CURRENT DOS/HPFS PARTITION STATUS	\r
*have already been added	q\r
*go on to the SOURCE	\r
*SOURCE MEDIA SELECTION	1\r
*INSTALLING FROM HARD DISK	/dev/hda1\r
*SELECT SOURCE DIRECTORY	/slakware\r
*DISK SETS section	\r
X
# CUS A AP D E F K N T TCL X XAP XD XV Y, from CUS: AP D E F N X XAP Y
printf '*SERIES SELECTION\t%s\n' "$D$D $D $D $D $D$D $D$D$D $D $D$D$D \\r"
printf '*go on to the INSTALL section\t\\r\n'
printf '*SELECT PROMPTING MODE\t%s\n' "$D$D$D$D$D\\r"
cat <<'X'
*installing \*everything\*	\r
*INSTALL LINUX KERNEL	s\r
*CONFIGURE YOUR SYSTEM	\r
*MAKE BOOTDISK	c\r
*MODEM CONFIGURATION	n
*MOUSE CONFIGURATION	\r
*SELECT MOUSE TYPE	2\r
*CONFIGURE CD-ROM	n
*SCREEN FONT CONFIGURATION	n
*SET YOUR MODEM SPEED	\r
*LILO INSTALLATION	\r
*OPTIONAL append	\r
*SELECT LILO TARGET LOCATION	\r
*CHOOSE LILO DELAY	5\r
*LILO INSTALLATION	l\r
*SELECT LINUX PARTITION	/dev/hda2\r
*SELECT PARTITION NAME	Linux\r
*LILO INSTALLATION	i\r
*CONFIGURE NETWORK\?	\r
*NETWORK CONFIGURATION	\r
*ENTER HOSTNAME	darkstar\r
*ENTER DOMAINNAME	frop.org\r
*LOOPBACK ONLY	\r
*NETWORK SETUP COMPLETE	\r
*GPM CONFIGURATION	n
*SENDMAIL CONFIGURATION	\x1b[B\r
X
# US/Mountain is the 432nd zone in setup.timeconfig's list, and only the
# arrows move in it
printf '*TIMEZONE CONFIGURATION\t%s\\r\n' "$(python3 -c "print(r'\x1b[B' * 431, end='')")"
cat <<'X'
*SETUP COMPLETE	\r
*Slackware96 Linux Setup	e\r
[#]\s*$	umount /mnt; sync\r
[#]\s*$
X
} > tmp/slacki.exp
python3 tools/expect.py -v -t 4800 tmp/slacki.exp -- ./dos-monster -m 586 -mem 17 -W -T 4790 -hda "$OUT" -boot c >/dev/null \
    && echo "$OUT" || { echo "slackinstall: failed"; exit 1; }
