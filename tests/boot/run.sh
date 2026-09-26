#!/bin/sh
# Machine boots from disk images (FreeDOS 1.3, GPL, in disks/freedos/ —
# git-ignored; see tests/boot/README). Each boot runs on a scratch copy of
# the image and is driven by tools/expect.py from what is on the screen.
cd "$(dirname "$0")/../.."
DM=${DM:-./dos-monster}      # the binary under test (a cross build: build-a64/dos-monster)
fail=0
mkdir -p tmp
F=disks/freedos/floppy/144m
if [ -f $F/x86BOOT.img ]; then
    # the boot diskette: kernel, FDCONFIG menu, FreeCOM, the installer's first question
    cp $F/x86BOOT.img tmp/boot-a.img
    printf 'Select from Menu\t\\r\nproceed \\[Y,N\\]\\?\t\n' > tmp/boot-a.exp
    if python3 tools/expect.py -t 60 tmp/boot-a.exp -- $DM -m 386 -W -T 50 -fda tmp/boot-a.img -boot a >/dev/null 2>&1
    then echo "ok   freedos diskette boot"; else echo "FAIL freedos diskette boot"; fail=1; fi
    rm -f tmp/boot-a.img tmp/boot-a.exp
fi
if [ -f disks/freedos/c.img ]; then
    # the installed hard disk: MBR, partition boot sector, C:\> and a command
    cp disks/freedos/c.img tmp/boot-c.img
    printf 'C:.>$\tver\\r\nFreeCom version\t\n' > tmp/boot-c.exp
    if python3 tools/expect.py -t 90 tmp/boot-c.exp -- $DM -m 386 -W -T 80 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
    then echo "ok   freedos hard disk boot"; else echo "FAIL freedos hard disk boot"; fail=1; fi
    # 257 MB (-mem): HIMEMX finds all 256 MB above the first through INT
    # 15h E820h/E801h, and MEM says so
    cp disks/freedos/c.img tmp/boot-mem.img
    printf 'C:.>$\tmem > c:\\\\mem.txt\\r\nC:.>$\t\n' > tmp/boot-mem.exp
    python3 tools/expect.py -t 90 tmp/boot-mem.exp -- $DM -m 386 -W -T 80 -mem 257 -hda tmp/boot-mem.img -boot c >/dev/null 2>&1
    if mtype -i tmp/boot-mem.img@@32256 ::/MEM.TXT 2>/dev/null | grep -q "Extended (XMS)    262,144K"
    then echo "ok   freedos with 257 MB (himemx)"; else echo "FAIL freedos with 257 MB"; fail=1; fi
    rm -f tmp/boot-mem.img tmp/boot-mem.exp
    # the PS/2 mouse under a real driver: CuteMouse finds it through INT 15h
    # C2h, and a click (SGR reports on stdin) reaches mouse.com's INT 33h
    # event handler through IRQ 12, INT 74h and the driver
    mcopy -o -i tmp/boot-c.img@@32256 tests/dos/mouse.com ::/MOUSE.COM
    printf 'C:.>$\tctmouse\\r\nPS/2 port\tmouse\\r\nFFFF$\t\\x1b[<35;11;2M\ndelay 0.5\nsend \\x1b[<0;11;2M\ndelay 0.5\nsend \\x1b[<0;11;2m\n*E=0004 B=0000\t\\x1b\nC:.>$\t\n' > tmp/boot-c.exp
    if python3 tools/expect.py -t 90 tmp/boot-c.exp -- $DM -m 386 -W -T 80 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
    then echo "ok   ps/2 mouse under freedos + ctmouse"; else echo "FAIL ps/2 mouse under freedos + ctmouse"; fail=1; fi
    if mdir -i tmp/boot-c.img@@32256 ::/WP51/WP.EXE >/dev/null 2>&1; then
        printf 'C:.>$\tcd \\\\wp51\\rwp\\r\nDoc 1 Pg 1\tThe quick brown fox, under FreeDOS.\nFreeDOS\\.\t\n' > tmp/boot-wp.exp
        if python3 tools/expect.py -t 120 tmp/boot-wp.exp -- $DM -m 386 -W -T 110 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
        then echo "ok   wp51 under freedos"; else echo "FAIL wp51 under freedos"; fail=1; fi
        # the same under JEMMEX (menu 2): DOS in virtual-8086 mode with paging, UMBs
        cp disks/freedos/c.img tmp/boot-c.img            # a clean disk each: WP leaves its lock files
        printf 'Selection=\t2\nC:.>$\tmem\\r\nfree upper memory block\t\n' > tmp/boot-wp.exp
        if python3 tools/expect.py -t 120 tmp/boot-wp.exp -- $DM -m 386 -W -T 110 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
        then echo "ok   freedos under jemmex (v86)"; else echo "FAIL freedos under jemmex (v86)"; fail=1; fi
        cp disks/freedos/c.img tmp/boot-c.img
        printf 'Selection=\t2\nC:.>$\tcd \\\\wp51\\rwp\\r\nDoc 1 Pg 1\tHello from V86.\nV86\\.\t\n' > tmp/boot-wp.exp
        if python3 tools/expect.py -t 120 tmp/boot-wp.exp -- $DM -m 386 -W -T 110 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
        then echo "ok   wp51 under jemmex (v86)"; else echo "FAIL wp51 under jemmex (v86)"; fail=1; fi
        rm -f tmp/boot-wp.exp
    fi
    rm -f tmp/boot-c.img tmp/boot-c.exp
fi
if [ -f disks/freedos/c286.img ]; then
    # the same install made on a 286 (8086 kernel), booted on one
    cp disks/freedos/c286.img tmp/boot-c.img
    printf 'C:.>$\tver\\r\nFreeCom version\t\n' > tmp/boot-c.exp
    if python3 tools/expect.py -t 90 tmp/boot-c.exp -- $DM -m 286 -W -T 80 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
    then echo "ok   freedos 286 hard disk boot"; else echo "FAIL freedos 286 hard disk boot"; fail=1; fi
    rm -f tmp/boot-c.img tmp/boot-c.exp
fi
if [ -f disks/msdos622/c.img ]; then
    # MS-DOS 6.22 as its Setup left it (tests/boot/msinstall.sh): HIMEM, DOS=HIGH
    cp disks/msdos622/c.img tmp/boot-c.img
    printf 'C:.>$\tver\\r\nMS-DOS Version 6.22\t\n' > tmp/boot-c.exp
    if python3 tools/expect.py -t 90 tmp/boot-c.exp -- $DM -m 386 -W -T 80 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
    then echo "ok   ms-dos 6.22 hard disk boot"; else echo "FAIL ms-dos 6.22 hard disk boot"; fail=1; fi
    # EMM386 (V86 mode, paging, UMBs, EMS), SMARTDRV loaded high, then WP
    printf 'DEVICE=C:\\DOS\\SETVER.EXE\r\nDEVICE=C:\\DOS\\HIMEM.SYS\r\nDEVICE=C:\\DOS\\EMM386.EXE RAM\r\nDOS=HIGH,UMB\r\nFILES=30\r\n' > tmp/boot-cfg.sys
    printf 'LH C:\\DOS\\SMARTDRV.EXE /X\r\n@ECHO OFF\r\nPROMPT $p$g\r\nPATH C:\\DOS\r\nSET TEMP=C:\\DOS\r\n' > tmp/boot-auto.bat
    mcopy -o -i tmp/boot-c.img@@32256 tmp/boot-cfg.sys ::/CONFIG.SYS
    mcopy -o -i tmp/boot-c.img@@32256 tmp/boot-auto.bat ::/AUTOEXEC.BAT
    printf 'C:.>$\tmem /c\\r\n*Free Expanded \\(EMS\\)\t\n' > tmp/boot-c.exp
    if python3 tools/expect.py -t 90 tmp/boot-c.exp -- $DM -m 386 -W -T 80 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
    then echo "ok   ms-dos under emm386 (v86)"; else echo "FAIL ms-dos under emm386 (v86)"; fail=1; fi
    # IRQ 13 on a 486 under EMM386: FERR# to the slave 8259, the BIOS's
    # INT 75h run in V86 mode (its OUTs to F0h, A0h and 20h through the
    # monitor), the program's INT 2
    mcopy -o -i tmp/boot-c.img@@32256 tests/dos/irq13.com ::/IRQ13.COM
    printf 'C:.>$\tirq13\\r\nafter 02\t\n' > tmp/boot-c.exp
    if python3 tools/expect.py -t 90 tmp/boot-c.exp -- $DM -m 486 -W -T 80 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
    then echo "ok   irq 13 on a 486 under emm386"; else echo "FAIL irq 13 on a 486 under emm386"; fail=1; fi
    if [ -d disks/WP51 ]; then
        mcopy -s -i tmp/boot-c.img@@32256 disks/WP51 ::/
        printf 'C:.>$\tcd \\\\wp51\\rwp\\r\nDoc 1 Pg 1\tHello from MS-DOS.\nMS-DOS\\.\t\n' > tmp/boot-c.exp
        if python3 tools/expect.py -t 120 tmp/boot-c.exp -- $DM -m 386 -W -T 110 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
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
        if python3 tools/expect.py -t 90 tmp/boot-c.exp -- $DM -m 386 -W -T 80 -hda tmp/boot-c.img -boot c >tmp/boot-doom.out 2>&1 \
           && ! grep -aq "rror (" tmp/boot-doom.out
        then echo "ok   doom under ms-dos + emm386 (vcpi)"; else echo "FAIL doom under ms-dos + emm386 (vcpi)"; fail=1; fi
        # and without EMM386: DOS/4GW switches itself, memory from HIMEM
        cp disks/msdos622/c.img tmp/boot-c.img
        mmd -i tmp/boot-c.img@@32256 ::/DOOM
        mcopy -i tmp/boot-c.img@@32256 $D/DOOM.EXE $D/DOOM1.WAD ::/DOOM/
        if python3 tools/expect.py -t 90 tmp/boot-c.exp -- $DM -m 386 -W -T 80 -hda tmp/boot-c.img -boot c >tmp/boot-doom.out 2>&1 \
           && ! grep -aq "rror (" tmp/boot-doom.out
        then echo "ok   doom under ms-dos + himem (xms)"; else echo "FAIL doom under ms-dos + himem (xms)"; fail=1; fi
        rm -f tmp/boot-doom.out
    fi
    rm -f tmp/boot-c.img tmp/boot-c.exp tmp/boot-cfg.sys tmp/boot-auto.bat
fi
if [ -f disks/win311/c.img ] && mdir -i disks/win311/c.img@@32256 ::/WINDOWS/WIN.COM >/dev/null 2>&1; then
    # Windows 3.11 (tests/boot/wininstall.sh) in standard mode: DOSX, the
    # 16-bit protected-mode kernel, VGA mode 12h. Program Manager is up
    # when its title bar (active: 0,0,170) is where it draws it.
    cp disks/win311/c.img tmp/boot-c.img
    printf 'C:.>$\twin /s\\r\ndelay 40\nshot tmp/boot-win.png\n' > tmp/boot-c.exp
    python3 tools/expect.py -t 90 tmp/boot-c.exp -- $DM -m 386 -W -T 80 -hda tmp/boot-c.img -boot c >/dev/null 2>&1
    if [ "$(python3 tools/pngpix.py tmp/boot-win.png 150,61 2>/dev/null)" = "150,61 0 0 170" ]
    then echo "ok   windows 3.11 standard mode (program manager)"; else echo "FAIL windows 3.11 standard mode"; fail=1; fi
    # 386 enhanced mode (plain "win": WIN386, its VxDs, DOS in V86 under
    # paging), and an MS-DOS Prompt in it: a second VM, full screen, whose
    # prompt and VER come out as text; EXIT returns to Program Manager.
    cp disks/win311/c.img tmp/boot-c.img
    printf 'C:.>$\twin\\r\ndelay 60\nsend \\x1b[C\\x1b[C\\x1b[C\\x1b[C\ndelay 3\nsend \\r\n*C:.WINDOWS>\tver\\r\n*MS-DOS Version 6.22\texit\\r\ndelay 30\nshot tmp/boot-win.png\n' > tmp/boot-c.exp
    python3 tools/expect.py -t 200 tmp/boot-c.exp -- $DM -m 386 -W -T 190 -hda tmp/boot-c.img -boot c >/dev/null 2>&1 && ok=1 || ok=0
    if [ $ok = 1 ] && [ "$(python3 tools/pngpix.py tmp/boot-win.png 150,61 2>/dev/null)" = "150,61 0 0 170" ]
    then echo "ok   windows 3.11 enhanced mode, ms-dos prompt vm"; else echo "FAIL windows 3.11 enhanced mode"; fail=1; fi
    # ... and with 32-bit disk access: WDCTRL validates the BIOS driving the
    # IDE controller (the AT's sequence, the diagnostic cylinder), then
    # drives it itself
    cp disks/win311/c.img tmp/boot-c.img
    mtype -i tmp/boot-c.img@@32256 ::/WINDOWS/SYSTEM.INI | python3 -c "import sys; s=sys.stdin.buffer.read().decode('latin-1'); sys.stdout.buffer.write(s.replace('[386Enh]\r\n','[386Enh]\r\n32BitDiskAccess=on\r\ndevice=*int13\r\ndevice=*wdctrl\r\n',1).encode('latin-1'))" > tmp/boot-system.ini
    mcopy -o -i tmp/boot-c.img@@32256 tmp/boot-system.ini ::/WINDOWS/SYSTEM.INI
    python3 tools/expect.py -t 200 tmp/boot-c.exp -- $DM -m 386 -W -T 190 -hda tmp/boot-c.img -boot c >/dev/null 2>&1 && ok=1 || ok=0
    if [ $ok = 1 ] && [ "$(python3 tools/pngpix.py tmp/boot-win.png 150,61 2>/dev/null)" = "150,61 0 0 170" ]
    then echo "ok   windows 3.11 enhanced mode, 32-bit disk access (wdctrl)"; else echo "FAIL windows 3.11 32-bit disk access"; fail=1; fi
    rm -f tmp/boot-c.img tmp/boot-c.exp tmp/boot-win.png tmp/boot-system.ini
fi
if [ -f disks/linux/tc.img ]; then
    # Linux 6.12 (Tiny Core 16.2, tests/boot/tcimage.sh) on the Pentium:
    # GRUB 2 from the disk, the kernel and its initramfs, the shell, a command
    cp disks/linux/tc.img tmp/boot-tc.img
    printf 'tc@box:~\\$\tuname -a\\r\ni586 GNU/Linux\t\n' > tmp/boot-tc.exp
    if python3 tools/expect.py -t 280 tmp/boot-tc.exp -- $DM -m 586 -mem 128 -W -T 270 -hda tmp/boot-tc.img -boot c >/dev/null 2>&1
    then echo "ok   linux 6.12 (tiny core) to a shell on the pentium"; else echo "FAIL linux 6.12 (tiny core)"; fail=1; fi
    rm -f tmp/boot-tc.img tmp/boot-tc.exp
fi
if [ -f disks/linux/tc.img ] && $DM -nic e1000,user -h >/dev/null 2>&1; then
    # ... with the e1000 on a slirp network (a build with libslirp): Linux's
    # DHCP client gets slirp's 10.0.2.15, and slirp's gateway answers two
    # pings a second apart (a clock that runs on through HLT). Nothing
    # outside the host is reached.
    cp disks/linux/tc.img tmp/boot-tcn.img
    printf '%s\t%s\\r\n%s\t\n' 'tc@box:~\$' "ifconfig eth0 | grep -q addr:10.0.2.15 && [ \$(ping -c 2 10.0.2.2 | grep -c 'bytes from') = 2 ] && echo NET-O''K" '*NET-OK' > tmp/boot-tcn.exp
    if python3 tools/expect.py -t 280 tmp/boot-tcn.exp -- $DM -m 586 -mem 128 -W -T 270 -nic e1000,user -hda tmp/boot-tcn.img -boot c >/dev/null 2>&1
    then echo "ok   linux 6.12 on the network: an e1000, dhcp and ping through slirp"; else echo "FAIL linux on the network (e1000, slirp)"; fail=1; fi
    rm -f tmp/boot-tcn.img tmp/boot-tcn.exp
fi
if [ -f disks/linux/tcroot.img ] && [ -x "$(brew --prefix e2fsprogs 2>/dev/null)/sbin/debugfs" ]; then
    # ... and with its root filesystem on our IDE disk (tests/boot/tcroot.sh:
    # ext2, no initramfs): Linux writes a file there as root, and it is in
    # the image afterwards
    cp disks/linux/tcroot.img tmp/boot-tcr.img
    printf 'tc@box:~\\$\tsudo sh -c "echo written by linux > /hello.txt"; sync\\r\ndelay 15\n' > tmp/boot-tcr.exp
    python3 tools/expect.py -t 280 tmp/boot-tcr.exp -- $DM -m 586 -mem 128 -W -T 270 -hda tmp/boot-tcr.img -boot c >/dev/null 2>&1
    dd if=tmp/boot-tcr.img of=tmp/boot-tcr-p2.img bs=512 skip=67584 2>/dev/null
    if [ "$("$(brew --prefix e2fsprogs)/sbin/debugfs" -R "cat /hello.txt" tmp/boot-tcr-p2.img 2>/dev/null)" = "written by linux" ]
    then echo "ok   linux 6.12 with its root on the ide disk (ext2, read and written)"; else echo "FAIL linux root on the ide disk"; fail=1; fi
    rm -f tmp/boot-tcr.img tmp/boot-tcr-p2.img tmp/boot-tcr.exp
fi
if [ -f disks/slack31/slack-installed.img ]; then
    # Slackware 3.1 (1996: Linux 2.0.0, gcc 2.7.2), installed by its own
    # setup (tests/boot/slackimage.sh, slackinstall.sh): the BIOS, LILO
    # from the MBR, the kernel from hda2, a login as root, and a C program
    # compiled and run with the gcc it came with
    printf '%s\t%s\n' 'darkstar login:' 'root\r' '[#] *$' "echo 'main(){printf(\"%d\\\\n\",1995+1);}' > /tmp/y.c; gcc -o /tmp/y /tmp/y.c; /tmp/y\\r" '*(?m)^1996$' '' > tmp/boot-slack.exp
    if python3 tools/expect.py -t 200 tmp/boot-slack.exp -- $DM -m 586 -mem 17 -W -T 190 -ro -hda disks/slack31/slack-installed.img -boot c >/dev/null 2>&1
    then echo "ok   slackware 3.1 (linux 2.0.0) from lilo, gcc 2.7.2 compiles"; else echo "FAIL slackware 3.1"; fail=1; fi
    rm -f tmp/boot-slack.exp
fi
exit $fail
