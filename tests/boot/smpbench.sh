#!/bin/sh
# SMP speed: Tiny Core (disks/linux/tc.img) on -smp 1, 2 and 4, a busy
# shell loop run one, two and four at a time, each timed on the host by
# expect.py -v from the prompt the command is typed at to its marker (spelled so the
# echoed command cannot match); then Windows 2000 MP (disks/win2k/c-mp.img,
# d-mp.img) to its desktop on one and on two processors.
# SMPBENCH_N sets the loop count (default 300000).
cd "$(dirname "$0")/../.."
DM=${DM:-./dos-monster}
N=${SMPBENCH_N:-300000}
mkdir -p tmp
loop="f(){ i=0; while [ \$i -lt $N ]; do i=\$((i+1)); done; }"
for cpus in ${SMPBENCH_CPUS:-1 2 4}; do
    cp disks/linux/tc.img tmp/smpbench.img
    {
        printf 'tc@box:~\\$\t%s; echo RE""ADY\\r\n*READY\t\n' "$loop"
        printf 'tc@box:~\\$\tf; echo O""NE-DONE\\r\n*ONE-DONE\t\n'
        printf 'tc@box:~\\$\tf & f & wait; echo T""WO-DONE\\r\n*TWO-DONE\t\n'
        printf 'tc@box:~\\$\tf & f & f & f & wait; echo F""OUR-DONE\\r\n*FOUR-DONE\t\n'
    } > tmp/smpbench.exp
    python3 tools/expect.py -v -t 600 tmp/smpbench.exp -- $DM -m 586 -mem 128 -smp $cpus -W -T 590 -hda tmp/smpbench.img -boot c 2>&1 >/dev/null \
        | awk -v c=$cpus '/expect: \[/ { gsub(/[][s]/, "", $2); t[++n] = $2 }
              END { printf "linux -smp %d: shell at %5.1f s   one loop %5.2f s   two %5.2f s   four %5.2f s\n", c, t[1], t[4]-t[3], t[6]-t[5], t[8]-t[7] }'
done
rm -f tmp/smpbench.img tmp/smpbench.exp
if [ -f disks/win2k/c-mp.img ] && [ -f disks/win2k/d-mp.img ]; then
    printf 'waitpix 400,300 85 170 170\nwaitpix 600,466 195 199 203\n' > tmp/smpbench.exp
    for cpus in 1 2; do
        python3 tools/expect.py -v -t 300 tmp/smpbench.exp -- $DM -m 586 -mem 64 -smp $cpus -W -T 290 -ro \
            -hda disks/win2k/c-mp.img -hdb disks/win2k/d-mp.img -boot c 2>&1 >/dev/null \
            | awk -v c=$cpus '/pixel 600,466 is/ { gsub(/[][s]/, "", $2); t = $2 } /^expect: (no|pixel)/ { why = $0 }
                  END { if (t) printf "windows 2000 mp -smp %d: desktop at %5.1f s\n", c, t; else printf "windows 2000 mp -smp %d: no desktop (%s)\n", c, why }'
    done
    rm -f tmp/smpbench.exp
fi
