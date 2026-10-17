#!/bin/sh
# SMP soak: each guest booted many times on two and four processors, and
# a pass rate per case — for SMP changes, whose bugs hide at one boot in
# ten (the one 78dafe5 left did), not for every commit.
#
#   linuxN  Tiny Core (disks/linux/tc.img) on N processors: a fork storm,
#           tools/litmus.asm (message passing, no forbidden outcome),
#           tools/qlock.asm (NT's queued spinlock, a count of exactly 2N),
#           and a kernel log free of BUG/lockup/stall/Oops; the guest checks
#           and prints SOAK=OK or SOAK=BAD with what it saw
#   w2kN    Windows 2000 MP (disks/win2k/c-mp.img, d-mp.img) to its desktop,
#           KeBugCheckEx trapped (X86_INTERP_FROM) so a failure's log holds
#           both processors' state at the bugcheck
#   haikuN  Haiku R1/beta6 (its anyboot image, -acpi) to the welcome window
#
# SOAK_N runs per case (30), SOAK_CASES which ("linux2 linux4 w2k2 w2k4
# haiku2 haiku4"), SOAK_PAR runs at once (6 / processors, at least 1: the
# host's performance cores). A failure keeps tmp/soak/CASE-I.log (expect's
# post-mortem and the emulator's stderr) and CASE-I.png (the last screen).
# Exit 1 if anything failed.
cd "$(dirname "$0")/../.."
DM=${DM:-./dos-monster}
N=${SOAK_N:-30}
CASES=${SOAK_CASES:-"linux2 linux4 w2k2 w2k4 haiku2 haiku4"}
OUT=tmp/soak
rm -rf $OUT; mkdir -p $OUT

# Tiny Core with the two test programs on its FAT partition
nasm -f bin -o $OUT/litmus tools/litmus.asm && nasm -f bin -o $OUT/qlock tools/qlock.asm || exit 1
if [ -f disks/linux/tc.img ]; then
    cp disks/linux/tc.img $OUT/tc-soak.img
    mcopy -o -i $OUT/tc-soak.img@@1048576 $OUT/litmus ::/LITMUS && mcopy -o -i $OUT/tc-soak.img@@1048576 $OUT/qlock ::/QLOCK || exit 1
fi
cat > $OUT/linux.exp.in <<'EOF'
tc@box:~\$	clear; sudo mount -t vfat /dev/sda1 /mnt/sda1; cp /mnt/sda1/LITMUS /tmp/l; cp /mnt/sda1/QLOCK /tmp/q; chmod +x /tmp/l /tmp/q; for i in 1 2 3 4 5 6; do sh -c 'for j in $(seq 1 60); do /bin/true; done' & done; wait; c=$(grep -c ^processor /proc/cpuinfo); l=$(/tmp/l); q=$(/tmp/q | tr '\n' ,); k=$(dmesg | grep -ciE 'BUG:|lockup|stall|Oops|general protection'); [ "$c $l $q $k" = "@CPUS@ 0 2000000, 0" ] && echo SO""AK=OK || echo SO""AK=BAD cpus=$c litmus=$l qlock=$q kmsg=$k\r
*SOAK=OK
EOF
printf 'waitpix 400,300 85 170 170\nwaitpix 600,466 195 199 203\n' > $OUT/w2k.exp
printf 'waitpix 60,60 51 102 152\nwaitpix 345,147 255 233 148\n' > $OUT/haiku.exp

# one run: CASE CPUS I -> $OUT/CASE-I.res holds ok or FAIL
run_one() {
    c=$1; cpus=$2; i=$3; base=$OUT/$c-$i
    case $c in
    linux*)
        cp $OUT/tc-soak.img $base.img
        sed "s/@CPUS@/$cpus/" $OUT/linux.exp.in > $base.exp
        EXPECT_SHOT=$base.png python3 tools/expect.py -t 240 $base.exp -- $DM -m 586 -mem 128 -smp $cpus -W -T 230 \
            -hda $base.img -boot c > /dev/null 2> $base.log
        r=$?; rm -f $base.img $base.exp ;;
    w2k*)
        X86_INTERP_FROM=8042A288 EXPECT_SHOT=$base.png python3 tools/expect.py -t 240 $OUT/w2k.exp -- $DM -m 586 -mem 64 -smp $cpus -W -T 230 -ro \
            -hda disks/win2k/c-mp.img -hdb disks/win2k/d-mp.img -boot c > /dev/null 2> $base.log
        r=$? ;;
    haiku*)
        EXPECT_SHOT=$base.png python3 tools/expect.py -t 420 $OUT/haiku.exp -- $DM -m 586 -mem 257 -vbe -smp $cpus -acpi -W -T 410 -ro \
            -hda disks/haiku/haiku-r1beta6-x86_gcc2h-anyboot.iso -boot c > /dev/null 2> $base.log
        r=$? ;;
    esac
    if [ $r = 0 ]; then echo ok > $base.res; rm -f $base.log $base.png; else echo FAIL > $base.res; fi
}

fail=0
for c in $CASES; do
    cpus=${c##*[a-z]}
    case $c in
    linux*) [ -f $OUT/tc-soak.img ] || { echo "skip $c (no disks/linux/tc.img)"; continue; } ;;
    w2k*)   [ -f disks/win2k/c-mp.img ] && [ -f disks/win2k/d-mp.img ] || { echo "skip $c (no disks/win2k/c-mp.img, d-mp.img)"; continue; } ;;
    haiku*) [ -f disks/haiku/haiku-r1beta6-x86_gcc2h-anyboot.iso ] || { echo "skip $c (no Haiku image)"; continue; } ;;
    *)      echo "unknown case $c"; continue ;;
    esac
    par=${SOAK_PAR:-$((6 / cpus))}; [ "$par" -ge 1 ] || par=1
    t0=$(date +%s)
    i=1
    while [ $i -le $N ]; do
        j=0
        while [ $j -lt $par ] && [ $((i + j)) -le $N ]; do run_one $c $cpus $((i + j)) & j=$((j + 1)); done
        wait
        i=$((i + par))
    done
    ok=$(cat $OUT/$c-*.res | grep -c '^ok')
    bad=$(grep -l FAIL $OUT/$c-*.res 2>/dev/null | sed 's|.*/||; s|\.res$||' | tr '\n' ' ')
    printf '%-7s %d/%d ok  (%d s)%s\n' $c $ok $N $(( $(date +%s) - t0 )) "${bad:+   failed: $bad}"
    [ $ok = $N ] || fail=1
done
rm -f $OUT/tc-soak.img $OUT/*.res
exit $fail
