#!/usr/bin/env bash
# Scripted run of the Windows package under Wine (real GPU through winevulkan), like d3drive.sh.
# usage: tools/winedrive.sh PORT_DIR LOG SCRIPT
#   PORT_DIR: an extracted SanctuaryPort-windows folder; SCRIPT: as in d3drive.sh.
# Game folders: SANCTUARY_GAME_DIR / SANCTUARY_PATCH_DIR (Linux paths, default ~/Games/PS4/CUSA00243[-UPDATE]);
# SANCTUARY_DATA_DIR (Linux path) for out/ and user/. Prefix: WINEPREFIX (default out/win/wineprefix).
set -u
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
port=$(cd -- "$1" && pwd) log=$2 script=$3
winpath() { printf 'Z:%s' "$(realpath -m -- "$1" | tr / '\\')"; }
export WINEPREFIX=${WINEPREFIX:-$root/out/win/wineprefix} WINEDLLOVERRIDES="mscoree=;mshtml=" WINEDEBUG=${WINEDEBUG:--all}
pad=$(mktemp)
: > "$pad"
data=${SANCTUARY_DATA_DIR:-$root/out/winedrive}
mkdir -p "$data"
(cd "$port" && SANCTUARY_PAD_FILE=$(winpath "$pad") \
    SANCTUARY_GAME_DIR=$(winpath "${SANCTUARY_GAME_DIR:-$HOME/Games/PS4/CUSA00243}") \
    SANCTUARY_PATCH_DIR=$(winpath "${SANCTUARY_PATCH_DIR-$HOME/Games/PS4/CUSA00243-UPDATE}") \
    SANCTUARY_DATA_DIR=$(winpath "$data") \
    wine python/python.exe -u run.py) > "$log" 2>&1 &
game=$!
start=$(date +%s.%N)
while read -r at tokens; do
    [[ -z ${at:-} || $at == \#* ]] && continue
    while :; do
        now=$(date +%s.%N)
        awk -v n="$now" -v s="$start" -v a="$at" 'BEGIN{exit !(n-s>=a)}' && break
        kill -0 "$game" 2>/dev/null || break 2
        read -r -t 0.1 <> <(:) || true
    done
    [[ $tokens == end ]] && break
    if [[ $tokens == - ]]; then : > "$pad"; else printf '%s\n' "$tokens" > "$pad"; fi
    echo "drive: t=$at $tokens" >> "$log"
done < "$script"
kill "$game" 2>/dev/null; sleep 1
# Only the game: a launcher open in the same prefix stays.
pkill -f 'sanctuary-probe\.exe' 2>/dev/null; pkill -f 'python/python\.exe -u run\.py' 2>/dev/null
wait "$game" 2>/dev/null
rm -f "$pad"
grep -ao 'Screenshot: [^ ]*' "$log"
