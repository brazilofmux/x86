#!/bin/sh
# Machine boots from disk images (FreeDOS 1.3, GPL, in disks/freedos/ —
# git-ignored; see tests/boot/README). Each boot runs on a scratch copy of
# the image and is driven by tools/expect.py from what is on the screen.
cd "$(dirname "$0")/../.."
fail=0
mkdir -p tmp
F=disks/freedos/floppy/144m
if [ -f $F/x86BOOT.img ]; then
    # the boot diskette: kernel, FDCONFIG menu, FreeCOM, the installer's first question
    cp $F/x86BOOT.img tmp/boot-a.img
    printf 'Select from Menu\t\\r\nproceed \\[Y,N\\]\\?\t\n' > tmp/boot-a.exp
    if python3 tools/expect.py -t 60 tmp/boot-a.exp -- ./dos-monster -m 386 -W -T 50 -fda tmp/boot-a.img -boot a >/dev/null 2>&1
    then echo "ok   freedos diskette boot"; else echo "FAIL freedos diskette boot"; fail=1; fi
    rm -f tmp/boot-a.img tmp/boot-a.exp
fi
if [ -f disks/freedos/c.img ]; then
    # the installed hard disk: MBR, partition boot sector, C:\> and a command
    cp disks/freedos/c.img tmp/boot-c.img
    printf 'C:.>$\tver\\r\nFreeCom version\t\n' > tmp/boot-c.exp
    if python3 tools/expect.py -t 90 tmp/boot-c.exp -- ./dos-monster -m 386 -W -T 80 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
    then echo "ok   freedos hard disk boot"; else echo "FAIL freedos hard disk boot"; fail=1; fi
    if mdir -i tmp/boot-c.img@@32256 ::/WP51/WP.EXE >/dev/null 2>&1; then
        printf 'C:.>$\tcd \\\\wp51\\rwp\\r\nDoc 1 Pg 1\tThe quick brown fox, under FreeDOS.\nFreeDOS\\.\t\n' > tmp/boot-wp.exp
        if python3 tools/expect.py -t 120 tmp/boot-wp.exp -- ./dos-monster -m 386 -W -T 110 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
        then echo "ok   wp51 under freedos"; else echo "FAIL wp51 under freedos"; fail=1; fi
        rm -f tmp/boot-wp.exp
    fi
    rm -f tmp/boot-c.img tmp/boot-c.exp
fi
exit $fail
