#!/bin/sh
# doom-timedemo.sh — DOOM's `-timedemo demo1` (5026 gametics) on dos-monster,
# Bochs, DOSBox-X and, with -q, QEMU, one after another on this host; each
# prints DOOM's own "timed 5026 gametics in N realtics" (a realtic is 1/35 s
# of the guest's clock, which every one of these keeps to the host's).
#
#   docs/bench/doom-timedemo.sh [-q] [dos-monster|bochs|dosbox-x|qemu ...]
#
# Needs (git-ignored, bring your own):
#   disks/msdos622/c.img        MS-DOS 6.22 as tests/boot/msinstall.sh leaves it
#                               (HIMEM, DOS=HIGH, SMARTDRV; see README.md here)
#   disks/doom/inst/DOOMS/      DOOM.EXE and DOOM1.WAD, shareware v1.9
# Override with DOS= and DOOMDIR=. Each run starts from a fresh copy of the
# image with a \DOOM folder holding just those two files, so DOOM runs on its
# built-in defaults (no DEFAULT.CFG).
cd "$(dirname "$0")/../.." || exit 1
DOS=${DOS:-disks/msdos622/c.img}
DOOMDIR=${DOOMDIR:-disks/doom/inst/DOOMS}
DM=${DM:-./dos-monster}
BOCHS_SHARE=${BOCHS_SHARE:-$(brew --prefix 2>/dev/null)/share/bochs}
T=$(mktemp -d "${TMPDIR:-/tmp}/doom-bench.XXXXXX")
export MTOOLS_SKIP_CHECK=1

which="dos-monster bochs dosbox-x"
[ "$1" = "-q" ] && { which="$which qemu"; shift; }
[ $# -gt 0 ] && which="$*"

image() {   # a fresh MS-DOS disk with \DOOM; $1: an AUTOEXEC.BAT to install, or none
    cp "$DOS" "$T/c.img"
    mmd -i "$T/c.img@@32256" ::/DOOM
    mcopy -i "$T/c.img@@32256" "$DOOMDIR/DOOM.EXE" "$DOOMDIR/DOOM1.WAD" ::/DOOM/
    [ -n "$1" ] && mcopy -o -i "$T/c.img@@32256" "$1" ::/AUTOEXEC.BAT
    rm -f "$T/td.txt"
}

# $1 seconds at most: poll the image for C:\TD.TXT (DOS writes it when DOOM
# exits), print its last line, stop the emulator (pid $2)
await() {
    n=0
    while [ $n -lt "$1" ]; do
        sleep 5; n=$((n + 5))
        if mtype -i "$T/c.img@@32256" ::/TD.TXT 2>/dev/null | grep -a "timed" > "$T/td.txt"; then break; fi
        kill -0 "$2" 2>/dev/null || break
    done
    kill "$2" 2>/dev/null; wait "$2" 2>/dev/null
    if [ -s "$T/td.txt" ]; then tr -d '\r' < "$T/td.txt" | tail -1; else echo "(no result in $1 s)"; fi
}

for e in $which; do
    printf '%-12s ' "$e"
    case $e in
    dos-monster)
        # MS-DOS's own AUTOEXEC, then the timedemo typed at the prompt; no window
        image ""
        printf '%s\n' 'C:.>$	cd \\doom\rdoom -timedemo demo1\r' '*timed [0-9]+ gametics	' > "$T/td.exp"
        python3 tools/expect.py -t 300 "$T/td.exp" -- "$DM" -m 386 -W -T 290 -hda "$T/c.img" -boot c 2>&1 \
            | grep -a "^timed" | tail -1
        ;;
    bochs)
        image docs/bench/timedemo.bat
        sed -e "s|@BOCHS_SHARE@|$BOCHS_SHARE|" -e "s|@IMG@|$T/c.img|" -e "s|@LOG@|$T/bochs.log|" \
            docs/bench/bochsrc > "$T/bochsrc"
        bochs -q -f "$T/bochsrc" > "$T/bochs.out" 2>&1 < /dev/null &
        await 1200 $!
        ;;
    dosbox-x)
        # its built-in DOS; the two files in a host folder it mounts as C:
        mkdir -p "$T/dbx" && cp "$DOOMDIR/DOOM.EXE" "$DOOMDIR/DOOM1.WAD" "$T/dbx/"
        sed "s|@DOOMDIR@|$T/dbx|" docs/bench/dosbox-x.conf > "$T/dosbox-x.conf"
        dosbox-x -conf "$T/dosbox-x.conf" -nomenu -fastlaunch > "$T/dosbox-x.out" 2>&1
        tr -d '\r' < "$T/dbx/TD.TXT" 2>/dev/null | grep -a "timed" | tail -1 || echo "(no result)"
        ;;
    qemu)
        # never finished on the Mac (2026-09-24): still mid-demo at 90 s
        image docs/bench/timedemo.bat
        qemu-system-i386 -m 16 -drive "file=$T/c.img,format=raw" -display none -no-reboot > "$T/qemu.out" 2>&1 &
        await 600 $!
        ;;
    *) echo "unknown: $e" ;;
    esac
done
rm -rf "$T"
