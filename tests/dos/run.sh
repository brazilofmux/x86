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
for mode in -i -j -V "-m 86 -V"; do
    check "hello.com $mode" tests/dos/hello.out ./dos-monster $mode tests/dos/hello.com
    check "exe1.exe $mode"  tests/dos/exe1.out  ./dos-monster $mode tests/dos/exe1.exe
    check "exec.com $mode"  tests/dos/exec.out  ./dos-monster $mode tests/dos/exec.com
    check "a20.com $mode"   tests/dos/a20.out   ./dos-monster $mode tests/dos/a20.com
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
if [ -f disks/WP51/WP.EXE ]; then
    # WordPerfect 5.1 (installed by its own INSTALL.EXE under dos-monster): type a line
    rm -f 'disks/WP51/WP}WP{'* 2>/dev/null
    got=$( (sleep 2; printf 'The quick brown fox.'; sleep 2) | ./dos-monster -L 600000000 -C disks -D - disks/WP51/WP.EXE 2>/dev/null)
    case "$got" in *"The quick brown fox."*"Doc 1 Pg 1"*) echo "ok   wp51 typing";; *) echo "FAIL wp51 typing"; fail=1;; esac
    rm -f 'disks/WP51/WP}WP{'* 2>/dev/null
fi
exit $fail
