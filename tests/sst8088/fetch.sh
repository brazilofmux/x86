#!/bin/sh
# Fetch the SingleStepTests 8088 suite (v2, MOO binary format, ~500MB).
# https://github.com/SingleStepTests/8088 — MIT, by Daniel Balsom.
# Files land next to this script and are git-ignored.
set -e
cd "$(dirname "$0")"
RAW=https://raw.githubusercontent.com/SingleStepTests/8088/main
API=https://api.github.com/repos/SingleStepTests/8088/contents/v2_binary
curl -sLo metadata.json $RAW/v2/metadata.json
curl -sL "$API" | grep '"name"' | sed 's/.*"name": "\(.*\)".*/\1/' | grep MOO.gz | while read f; do
    [ -f "$f" ] || { echo "$f"; curl -sLo "$f" "$RAW/v2_binary/$f"; }
done
echo "done: $(ls *.MOO.gz | wc -l) files"
