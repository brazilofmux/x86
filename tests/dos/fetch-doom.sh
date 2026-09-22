#!/bin/sh
# tests/dos/fetch-doom.sh — shareware DOOM 1.9 into disks/doom/inst/DOOMS.
#
# doom19s.zip is id's freely redistributable shareware release. It is an
# installer, not a directory: DEICE.EXE copies DOOMS_19.1/.2 into an LHA
# self-extractor, which then unpacks the game. Both run under dos-monster
# itself — the install is a real-mode test of its own — and DOOM.EXE then
# needs the DPMI host (DOS/4GW is bound into it). disks/ is git-ignored.
set -e
cd "$(dirname "$0")/../.."
[ -x ./dos-monster ] || make -s
mkdir -p disks/doom
[ -f disks/doom/doom19s.zip ] ||
    curl -fSL -o disks/doom/doom19s.zip https://www.gamers.org/pub/idgames/idstuff/doom/doom19s.zip
rm -rf disks/doom/inst
mkdir -p disks/doom/inst
unzip -o -q disks/doom/doom19s.zip -d disks/doom/inst
# DEICE asks for a drive letter and a confirmation, then copies.
(sleep 2; printf 'C'; sleep 2; printf '\r'; sleep 2; printf 'Y'; sleep 20) |
    ./dos-monster -j -C disks/doom/inst -L 200000000000 disks/doom/inst/DEICE.EXE >/dev/null 2>&1 || true
./dos-monster -j -C disks/doom/inst/DOOMS -L 200000000000 disks/doom/inst/DOOMS/DOOMS_19.EXE </dev/null >/dev/null
rm -f disks/doom/inst/DOOMS/DOOMS_19.EXE
test -f disks/doom/inst/DOOMS/DOOM.EXE && test -f disks/doom/inst/DOOMS/DOOM1.WAD
echo "doom: installed in $(pwd)/disks/doom/inst/DOOMS"
