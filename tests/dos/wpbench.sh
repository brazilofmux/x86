#!/bin/sh
# tests/dos/wpbench.sh — the WordPerfect 5.1 workload: retrieve a 50-page
# DOS text document, replace every "monster" with "MONSTER" across it.
# Needs disks/WP51 (installed by WP's own INSTALL.EXE under dos-monster).
# Prints the run-loop rate every half second and the fallback histogram;
# the replace phase is the number to watch.
cd "$(dirname "$0")/../.." || exit 1
[ -f disks/WP51/WP.EXE ] || { echo "no disks/WP51/WP.EXE"; exit 1; }
[ -f disks/WP51/BIGDOC.TXT ] || python3 - <<'PY'
import random
random.seed(1)
words = ("the quick brown fox jumps over the lazy dog while wordperfect reveals codes and "
         "the monster translates every block of the document into native code at absurd speed").split()
lines = []
for page in range(50):
    for l in range(54):
        lines.append(' '.join(random.choice(words) for _ in range(random.randint(8, 13))).capitalize() + '.')
    lines.append('')
open('disks/WP51/BIGDOC.TXT', 'w', newline='\r\n').write('\n'.join(lines) + '\n')
PY
rm -f 'disks/WP51/WP}WP{'* 2>/dev/null
( sleep 2; printf '\033[21;2~BIGDOC.TXT\r'; sleep 5; printf '\033[12;3~nmonster\033[12~MONSTER\033[12~'; sleep 8 ) |
  X86_RATE=1 ./dos-monster "$@" -s -L 30000000000 -C disks -D /dev/null disks/WP51/WP.EXE
rm -f 'disks/WP51/WP}WP{'* 2>/dev/null
