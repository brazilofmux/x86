#!/bin/sh
# Fetch the SingleStepTests 80386 real-mode suite (v1_ex_real_mode, ~600 MB of MOO.gz)
# and the 80286 real-mode suite (~325 MB) into tests/sst386 and tests/sst286.
cd "$(dirname "$0")/.." || exit 1
fetch() { # repo dir dest
    curl -s "https://api.github.com/repos/SingleStepTests/$1/contents/$2?per_page=1000" |
      python3 -c 'import json,sys; [print(e["download_url"]) for e in json.load(sys.stdin)]' > "$3/urls.txt"
    (cd "$3" && xargs -P 8 -n 1 curl -sLO < urls.txt)
}
fetch 80386 v1_ex_real_mode sst386
curl -sL -o sst386/80386.csv https://raw.githubusercontent.com/SingleStepTests/80386/main/80386.csv
fetch 80286 v1_real_mode sst286
curl -sL -o sst286/README.md https://raw.githubusercontent.com/SingleStepTests/80286/main/README.md
