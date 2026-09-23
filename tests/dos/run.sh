#!/bin/sh
# tests/dos/run.sh — DOS-level smoke tests: the assembled programs in this
# directory under interpreter, JIT and -V, and (if disks/tp55 is present)
# a Turbo Pascal 5.5 compile + run + a scripted IDE session.
cd "$(dirname "$0")/../.." || exit 1
fail=0
check() { # name expected-output-file command...
    name=$1; want=$2; shift 2
    got=$("$@" 2>/dev/null)
    if [ "$got" = "$(cat "$want")" ]; then echo "ok   $name"; else echo "FAIL $name"; echo "$got" | head -5; fail=1; fi
}
for mode in -i -j -V "-m 86 -V" "-m 386 -V"; do
    check "hello.com $mode" tests/dos/hello.out ./dos-monster $mode tests/dos/hello.com
    check "exe1.exe $mode"  tests/dos/exe1.out  ./dos-monster $mode tests/dos/exe1.exe
    check "exec.com $mode"  tests/dos/exec.out  ./dos-monster $mode tests/dos/exec.com
    check "a20.com $mode"   tests/dos/a20.out   ./dos-monster $mode tests/dos/a20.com
    check "smcpush $mode"   tests/dos/smcpush.out ./dos-monster $mode tests/dos/smcpush.com
    check "smcfar $mode"    tests/dos/smcfar.out  ./dos-monster $mode tests/dos/smcfar.com
    check "fileio $mode"    tests/dos/fileio.out  ./dos-monster $mode -L 50000000 tests/dos/fileio.com
done
# unreal mode (HIMEMX's): PE set with a real-mode CS, a 4 GB DS kept across real-mode loads
for mode in -i -j -V; do
    check "unreal $mode"    tests/dos/unreal.out  ./dos-monster -m 386 $mode -L 1000000 tests/dos/unreal.com
done
# The VGA text renderer (-G on a text screen: attributes, blink off, line
# drawing, a block cursor), compared as decoded pixels; and the BIOS's
# INT 9 translation of modified keys fed as xterm sequences; the INT 33h
# driver's event handler calls, fed xterm SGR mouse reports.
mkdir -p tmp
for mode in -j -V; do
    rm -f tmp/text.png
    ./dos-monster -W $mode -G tmp/text.png tests/dos/text.com </dev/null >/dev/null 2>&1
    if [ "$(python3 tests/dos/pnghash.py tmp/text.png 2>/dev/null)" = "$(cat tests/dos/text.out)" ]; then echo "ok   text render $mode"; else echo "FAIL text render $mode"; fail=1; fi
    got=$( (printf '\033[1;5D\033[1;5C\033[1;5H\033[1;5F\033[5;5~\033[6;5~\033[1;2P\033[1;3R\033[12;5~\033[23~\033[24;2~a'; sleep 2; printf '\033') | ./dos-monster -W $mode -L 200000000 tests/dos/kbd.com 2>/dev/null | tr -d '\r')
    if [ "$got" = "$(cat tests/dos/kbd.out)" ]; then echo "ok   kbd codes $mode"; else echo "FAIL kbd codes $mode"; echo "$got" | tr '\n' ' '; echo; fail=1; fi
    got=$( (sleep 1; printf '\033[<35;11;2M'; sleep 0.5; printf '\033[<0;11;2M'; sleep 0.5; printf '\033[<2;21;4M'; sleep 0.5; printf '\033[<2;21;4m'; sleep 0.5; printf '\033[<0;21;4m'; sleep 0.5; printf '\033') | X86_MOUSE=1 ./dos-monster -W $mode -L 400000000 tests/dos/mouse.com 2>/dev/null | tr -d '\r')
    if [ "$got" = "$(cat tests/dos/mouse.out)" ]; then echo "ok   mouse events $mode"; else echo "FAIL mouse events $mode"; echo "$got" | tr '\n' ' '; echo; fail=1; fi
done
rm -f tmp/text.png
# The DPMI clients only make sense on a 386: one source, assembled as a
# 16-bit client (dpmi.com) and a 32-bit one (dpmi32.com), since the host
# shapes every gate and frame by the client's width. Each prints a line per
# stage, so the expected output also says which stage broke.
for mode in -i -j -V; do
    check "dpmi $mode"   tests/dos/dpmi.out ./dos-monster $mode -m 386 -L 20000000 tests/dos/dpmi.com
    check "dpmi32 $mode" tests/dos/dpmi.out ./dos-monster $mode -m 386 -L 20000000 tests/dos/dpmi32.com
done

if [ -f disks/tp55/TPC.EXE ]; then
    rm -f disks/tp55/HELLO.EXE
    check "tpc -V" tests/dos/tpc.out ./dos-monster -V disks/tp55/TPC.EXE hello.pas
    check "tp hello -V" tests/dos/tphello.out ./dos-monster -V disks/tp55/HELLO.EXE
    for mode in -j -V; do
        got=$(printf '\033flhello.pas\r\033rr\033x\033x' | ./dos-monster $mode disks/tp55/TURBO.EXE 2>&1)
        case "$got" in *"sum = 705082704"*) ;; *) echo "FAIL ide $mode"; fail=1;; esac
        case "$got" in *exhausted*) echo "FAIL ide $mode (did not quit)"; fail=1;; *) echo "ok   ide $mode";; esac
    done
fi
if [ -f disks/cobol50/COBOL.EXE ]; then
    # MS COBOL 5.0 (Micro Focus underneath), from the user's diskettes expanded
    # into disks/cobol50: compile, link and run a sample, all under -V. The
    # compiler is a large real-mode program; its run-time rearranges the PSP's
    # job file table, which is what gave the DOS layer a real one.
    rm -rf tmp/cobtest && mkdir -p tmp && cp -r disks/cobol50 tmp/cobtest
    got=$(./dos-monster -V -C tmp/cobtest -L 4000000000 tmp/cobtest/COBOL.EXE "DIOPHANT;" </dev/null 2>/dev/null | tr -d '\r')
    case "$got" in *"no errors"*) echo "ok   cobol compile -V";; *) echo "FAIL cobol compile -V"; echo "$got" | tail -3; fail=1;; esac
    ./dos-monster -V -C tmp/cobtest -L 4000000000 tmp/cobtest/LINK.EXE "diophant,,,lcobol+cobapi/nod/st:8192;" </dev/null >/dev/null 2>&1
    if [ -f tmp/cobtest/DIOPHANT.EXE ]; then echo "ok   cobol link -V"; else echo "FAIL cobol link -V"; fail=1; fi
    got=$(printf '3\r5\r1\r' | ./dos-monster -V -C tmp/cobtest -L 4000000000 tmp/cobtest/DIOPHANT.EXE 2>/dev/null | tr -d '\r')
    if [ "$got" = "$(cat tests/dos/cobol.out)" ]; then echo "ok   cobol run -V"; else echo "FAIL cobol run -V"; echo "$got" | tail -3; fail=1; fi
    rm -rf tmp/cobtest
fi
if [ -f disks/WP51/WP.EXE ]; then
    # WordPerfect 5.1 (installed by its own INSTALL.EXE under dos-monster): type a line.
    # -L only bounds a runaway: WP idles near 1 BIPS, so it must not double as a timer.
    rm -f 'disks/WP51/WP}WP{'* 2>/dev/null
    got=$( (sleep 2; printf 'The quick brown fox.'; sleep 2) | ./dos-monster -L 20000000000 -C disks -D - disks/WP51/WP.EXE 2>/dev/null)
    case "$got" in *"The quick brown fox."*"Doc 1 Pg 1"*) echo "ok   wp51 typing";; *) echo "FAIL wp51 typing"; fail=1;; esac
    rm -f 'disks/WP51/WP}WP{'* 2>/dev/null
fi
if [ -f disks/djgpp/bin/djecho.exe ]; then
    # DJGPP v2.05 (tests/dos/fetch-djgpp.sh): real DPMI clients. djecho is the
    # whole host in one breath — mode switch, LDT, extended and DOS memory,
    # real-mode excursions, a callback, exception handlers and the 387
    # emulator's #NM path. stubedit reads a file through the transfer buffer.
    # The JIT refuses protected mode, so -j and -V exercise the fallback.
    for mode in -i -j -V; do
        got=$(./dos-monster $mode -m 386 -L 20000000000 disks/djgpp/bin/djecho.exe hello from djgpp 2>&1)
        [ "$got" = "hello from djgpp" ] && echo "ok   djecho $mode" || { echo "FAIL djecho $mode"; echo "$got" | head -5; fail=1; }
    done
    got=$(./dos-monster -i -m 386 -L 20000000000 -C disks/djgpp/bin disks/djgpp/bin/stubedit.exe -v djecho.exe 2>&1)
    case "$got" in *"CWSDPMI.EXE"*"Program to load"*) echo "ok   stubedit -v";; *) echo "FAIL stubedit -v"; echo "$got" | head -5; fail=1;; esac
fi
if [ -f disks/doom/inst/DOOMS/DOOM.EXE ]; then
    # Shareware DOOM 1.9 (tests/dos/fetch-doom.sh): DOS/4GW bound into the
    # game, a raw-switching DPMI client at ring 3. 150M instructions takes it
    # through its whole startup — WAD, refresh, DPMI, keyboard, timer, sound
    # init — and into mode 13h, drawing through the planar VGA; -G proves a
    # frame exists. It must also be told the truth about free memory (0500).
    D=disks/doom/inst/DOOMS
    for mode in -i -j -V; do
        rm -f disks/doom/check.png
        got=$(./dos-monster $mode -m 386 -W -C $D -L 150000000 -G disks/doom/check.png $D/DOOM.EXE </dev/null 2>&1)
        case "$got" in
        *"DPMI memory: 0x0,"*) echo "FAIL doom $mode (host reported no free memory)"; fail=1;;
        *"ST_Init: Init status bar."*) [ -s disks/doom/check.png ] && echo "ok   doom $mode" || { echo "FAIL doom $mode (never reached mode 13h)"; fail=1; };;
        *) echo "FAIL doom $mode"; echo "$got" | tail -5; fail=1;;
        esac
    done
fi
exit $fail
