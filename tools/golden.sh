#!/bin/sh
# tools/golden.sh — the translator's golden set (X86_GOLDEN, see dbt.h).
#
#   tools/golden.sh run DIR      run the workloads, one translation log each
#   tools/golden.sh diff A B     compare two such directories, log by log
#
# The workloads are the test suites' programs and boots under the
# interpreter, every block translated as it is reached and hashed. A
# refactor of the translator is held to an identical set: run it before
# and after, then diff. Big workloads run in the background; the whole
# set takes a few minutes on a fast host. DM names the binary.
cd "$(dirname "$0")/.." || exit 1
DM=${DM:-./dos-monster}
mode=$1; shift
# GOLDEN_SERIAL=1: one workload at a time (a shared or loaded host)
bg() { if [ -n "$GOLDEN_SERIAL" ]; then "$@"; else "$@" & fi; }

run() { # name command...   (the log is DIR/name.txt; stdout and stderr to DIR/name.log)
    name=$1; shift
    X86_GOLDEN="$DIR/$name.txt" "$@" > "$DIR/$name.log" 2>&1
    grep -h '^golden:' "$DIR/$name.log" | sed "s/^/$name: /"
}

case "$mode" in
run)
    DIR=$1; mkdir -p "$DIR" tmp
    D=disks/doom/inst/DOOMS
    if [ -z "$GOLDEN_ONLY_BOOT" ]; then    # GOLDEN_ONLY_BOOT=1: just the booted machines
    # the assembled DOS programs, each model
    for m in 286 86 386; do
        for p in hello exe1 exec a20 smcpush smcfar; do run $p-$m $DM -m $m -W tests/dos/$p.com; done
        run fileio-$m $DM -m $m -W -L 50000000 tests/dos/fileio.com
    done
    run exe1-286 $DM -W tests/dos/exe1.exe
    run unreal $DM -m 386 -W -L 1000000 tests/dos/unreal.com
    run text $DM -W -G tmp/golden-text.png tests/dos/text.com </dev/null
    run dpmi $DM -m 386 -W -L 20000000 tests/dos/dpmi.com
    run dpmi32 $DM -m 386 -W -L 20000000 tests/dos/dpmi32.com
    if [ -f disks/tp55/TPC.EXE ]; then
        rm -f disks/tp55/HELLO.EXE
        run tpc $DM -W disks/tp55/TPC.EXE hello.pas
        run tphello $DM -W disks/tp55/HELLO.EXE
        printf '\033flhello.pas\r\033rr\033x\033x' > tmp/golden-ide.keys
        run ide $DM -W -L 300000000 disks/tp55/TURBO.EXE < tmp/golden-ide.keys
    fi
    if [ -f disks/djgpp/bin/djecho.exe ]; then
        run djecho $DM -m 386 -W -L 200000000 disks/djgpp/bin/djecho.exe hello from djgpp
    fi
    # the big ones, three at a time
    if [ -f disks/cobol50/COBOL.EXE ]; then
        rm -rf tmp/golden-cob && cp -r disks/cobol50 tmp/golden-cob
        cobol_group() {
            run cobol $DM -W -C tmp/golden-cob -L 1500000000 tmp/golden-cob/COBOL.EXE "DIOPHANT;" </dev/null
            run coblink $DM -W -C tmp/golden-cob -L 1500000000 tmp/golden-cob/LINK.EXE "diophant,,,lcobol+cobapi/nod/st:8192;" </dev/null
            printf '3\r5\r1\r' > tmp/golden-cob.keys
            run cobrun $DM -W -C tmp/golden-cob -L 1500000000 tmp/golden-cob/DIOPHANT.EXE < tmp/golden-cob.keys
        }
        bg cobol_group
    fi
    if [ -f $D/DOOM.EXE ]; then
        doom_group() { run doom $DM -m 386 -W -C $D -L 150000000 -G tmp/golden-doom.png $D/DOOM.EXE </dev/null; }
        bg doom_group
    fi
    if [ -f disks/WP51/WP.EXE ]; then
        rm -f 'disks/WP51/WP}WP{'* 2>/dev/null
        # keys fed at once, not on a wall clock: WP takes them when it asks, at the same instruction every run
        wp_group() { printf 'The quick brown fox.' | run wp51 $DM -L 300000000 -C disks -D - disks/WP51/WP.EXE; }
        bg wp_group
    fi
    wait
    rm -rf tmp/golden-cob 'disks/WP51/WP}WP{'*
    fi
    [ -n "$GOLDEN_NO_BOOT" ] && exit 0          # GOLDEN_NO_BOOT=1: the exact workloads only
    # booted machines: real DOS, V86 and paging under memory managers, Windows
    if [ -f disks/freedos/c.img ]; then
        cp disks/freedos/c.img tmp/golden-fd.img
        printf 'Selection=\t2\nC:.>$\tmem\\r\nfree upper memory block\t\n' > tmp/golden-fd.exp
        fd_group() { run freedos-jemmex python3 tools/expect.py -t 600 tmp/golden-fd.exp -- $DM -m 386 -W -T 590 -L 600000000 -hda tmp/golden-fd.img -boot c; }
        bg fd_group
    fi
    if [ -f disks/msdos622/c.img ] && [ -f $D/DOOM.EXE ]; then
        cp disks/msdos622/c.img tmp/golden-ms.img
        printf 'DEVICE=C:\\DOS\\SETVER.EXE\r\nDEVICE=C:\\DOS\\HIMEM.SYS\r\nDEVICE=C:\\DOS\\EMM386.EXE RAM\r\nDOS=HIGH,UMB\r\nFILES=30\r\n' > tmp/golden-cfg.sys
        printf 'LH C:\\DOS\\SMARTDRV.EXE /X\r\n@ECHO OFF\r\nPROMPT $p$g\r\nPATH C:\\DOS\r\nSET TEMP=C:\\DOS\r\n' > tmp/golden-auto.bat
        mcopy -o -i tmp/golden-ms.img@@32256 tmp/golden-cfg.sys ::/CONFIG.SYS
        mcopy -o -i tmp/golden-ms.img@@32256 tmp/golden-auto.bat ::/AUTOEXEC.BAT
        mmd -i tmp/golden-ms.img@@32256 ::/DOOM
        mcopy -i tmp/golden-ms.img@@32256 $D/DOOM.EXE $D/DOOM1.WAD ::/DOOM/
        printf 'C:.>$\tcd \\\\doom\\rdoom\\r\n*I_StartupMouse|rror \\(|xception\t\n' > tmp/golden-ms.exp
        ms_group() { run msdos-emm386-doom python3 tools/expect.py -t 900 tmp/golden-ms.exp -- $DM -m 386 -W -T 890 -L 1000000000 -hda tmp/golden-ms.img -boot c; }
        bg ms_group
    fi
    if [ -f disks/win311/c.img ]; then
        cp disks/win311/c.img tmp/golden-win.img
        printf 'C:.>$\twin\\r\ndelay 600\n' > tmp/golden-win.exp
        win_group() { run win311-enhanced python3 tools/expect.py -t 900 tmp/golden-win.exp -- $DM -m 386 -W -T 890 -L 2500000000 -hda tmp/golden-win.img -boot c; }
        bg win_group
    fi
    wait
    rm -f tmp/golden-*.img tmp/golden-*.exp tmp/golden-*.keys tmp/golden-cfg.sys tmp/golden-auto.bat tmp/golden-*.png
    ;;
diff)
    A=$1; B=$2; fail=0
    for f in "$A"/*.txt; do
        n=$(basename "$f")
        [ -f "$B/$n" ] || { echo "$n: missing in $B"; fail=1; continue; }
        printf '%-22s ' "$n"
        out=$(python3 tools/golden-diff.py "$f" "$B/$n"); rc=$?
        echo "$out" | tail -1
        [ $rc = 0 ] || fail=1
    done
    exit $fail
    ;;
*)  echo "usage: $0 run DIR | diff A B" >&2; exit 2;;
esac
