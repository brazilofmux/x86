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
        # the same under JEMMEX (menu 2): DOS in virtual-8086 mode with paging, UMBs
        cp disks/freedos/c.img tmp/boot-c.img            # a clean disk each: WP leaves its lock files
        printf 'Selection=\t2\nC:.>$\tmem\\r\nfree upper memory block\t\n' > tmp/boot-wp.exp
        if python3 tools/expect.py -t 120 tmp/boot-wp.exp -- ./dos-monster -m 386 -W -T 110 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
        then echo "ok   freedos under jemmex (v86)"; else echo "FAIL freedos under jemmex (v86)"; fail=1; fi
        cp disks/freedos/c.img tmp/boot-c.img
        printf 'Selection=\t2\nC:.>$\tcd \\\\wp51\\rwp\\r\nDoc 1 Pg 1\tHello from V86.\nV86\\.\t\n' > tmp/boot-wp.exp
        if python3 tools/expect.py -t 120 tmp/boot-wp.exp -- ./dos-monster -m 386 -W -T 110 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
        then echo "ok   wp51 under jemmex (v86)"; else echo "FAIL wp51 under jemmex (v86)"; fail=1; fi
        rm -f tmp/boot-wp.exp
    fi
    rm -f tmp/boot-c.img tmp/boot-c.exp
fi
if [ -f disks/freedos/c286.img ]; then
    # the same install made on a 286 (8086 kernel), booted on one
    cp disks/freedos/c286.img tmp/boot-c.img
    printf 'C:.>$\tver\\r\nFreeCom version\t\n' > tmp/boot-c.exp
    if python3 tools/expect.py -t 90 tmp/boot-c.exp -- ./dos-monster -m 286 -W -T 80 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
    then echo "ok   freedos 286 hard disk boot"; else echo "FAIL freedos 286 hard disk boot"; fail=1; fi
    rm -f tmp/boot-c.img tmp/boot-c.exp
fi
if [ -f disks/msdos622/c.img ]; then
    # MS-DOS 6.22 as its Setup left it (tests/boot/msinstall.sh): HIMEM, DOS=HIGH
    cp disks/msdos622/c.img tmp/boot-c.img
    printf 'C:.>$\tver\\r\nMS-DOS Version 6.22\t\n' > tmp/boot-c.exp
    if python3 tools/expect.py -t 90 tmp/boot-c.exp -- ./dos-monster -m 386 -W -T 80 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
    then echo "ok   ms-dos 6.22 hard disk boot"; else echo "FAIL ms-dos 6.22 hard disk boot"; fail=1; fi
    # EMM386 (V86 mode, paging, UMBs, EMS), SMARTDRV loaded high, then WP
    printf 'DEVICE=C:\\DOS\\SETVER.EXE\r\nDEVICE=C:\\DOS\\HIMEM.SYS\r\nDEVICE=C:\\DOS\\EMM386.EXE RAM\r\nDOS=HIGH,UMB\r\nFILES=30\r\n' > tmp/boot-cfg.sys
    printf 'LH C:\\DOS\\SMARTDRV.EXE /X\r\n@ECHO OFF\r\nPROMPT $p$g\r\nPATH C:\\DOS\r\nSET TEMP=C:\\DOS\r\n' > tmp/boot-auto.bat
    mcopy -o -i tmp/boot-c.img@@32256 tmp/boot-cfg.sys ::/CONFIG.SYS
    mcopy -o -i tmp/boot-c.img@@32256 tmp/boot-auto.bat ::/AUTOEXEC.BAT
    printf 'C:.>$\tmem /c\\r\n*Free Expanded \\(EMS\\)\t\n' > tmp/boot-c.exp
    if python3 tools/expect.py -t 90 tmp/boot-c.exp -- ./dos-monster -m 386 -W -T 80 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
    then echo "ok   ms-dos under emm386 (v86)"; else echo "FAIL ms-dos under emm386 (v86)"; fail=1; fi
    if [ -d disks/WP51 ]; then
        mcopy -s -i tmp/boot-c.img@@32256 disks/WP51 ::/
        printf 'C:.>$\tcd \\\\wp51\\rwp\\r\nDoc 1 Pg 1\tHello from MS-DOS.\nMS-DOS\\.\t\n' > tmp/boot-c.exp
        if python3 tools/expect.py -t 120 tmp/boot-c.exp -- ./dos-monster -m 386 -W -T 110 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
        then echo "ok   wp51 under ms-dos + emm386"; else echo "FAIL wp51 under ms-dos + emm386"; fail=1; fi
    fi
    D=disks/doom/inst/DOOMS
    if [ -f $D/DOOM.EXE ]; then
        # DOOM (DOS/4GW) under EMM386: into protected mode through VCPI, its
        # own page tables, the JIT on code pages mapped below their linear
        # addresses. I_StartupMouse comes after zone, WAD, refresh and DPMI.
        mmd -i tmp/boot-c.img@@32256 ::/DOOM
        mcopy -i tmp/boot-c.img@@32256 $D/DOOM.EXE $D/DOOM1.WAD ::/DOOM/
        printf 'C:.>$\tcd \\\\doom\\rdoom\\r\n*I_StartupMouse|rror \\(|xception\t\n' > tmp/boot-c.exp
        if python3 tools/expect.py -t 90 tmp/boot-c.exp -- ./dos-monster -m 386 -W -T 80 -hda tmp/boot-c.img -boot c >tmp/boot-doom.out 2>&1 \
           && ! grep -aq "rror (" tmp/boot-doom.out
        then echo "ok   doom under ms-dos + emm386 (vcpi)"; else echo "FAIL doom under ms-dos + emm386 (vcpi)"; fail=1; fi
        # and without EMM386: DOS/4GW switches itself, memory from HIMEM
        cp disks/msdos622/c.img tmp/boot-c.img
        mmd -i tmp/boot-c.img@@32256 ::/DOOM
        mcopy -i tmp/boot-c.img@@32256 $D/DOOM.EXE $D/DOOM1.WAD ::/DOOM/
        if python3 tools/expect.py -t 90 tmp/boot-c.exp -- ./dos-monster -m 386 -W -T 80 -hda tmp/boot-c.img -boot c >tmp/boot-doom.out 2>&1 \
           && ! grep -aq "rror (" tmp/boot-doom.out
        then echo "ok   doom under ms-dos + himem (xms)"; else echo "FAIL doom under ms-dos + himem (xms)"; fail=1; fi
        rm -f tmp/boot-doom.out
    fi
    rm -f tmp/boot-c.img tmp/boot-c.exp tmp/boot-cfg.sys tmp/boot-auto.bat
fi
exit $fail
