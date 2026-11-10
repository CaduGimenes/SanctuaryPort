#!/usr/bin/env bash
# Scripted Diablo III run (development): starts the game, plays a button script through
# GOW3_PAD_FILE and takes screenshots, so the bring-up can be tested without a person.
#
# usage: tools/d3drive.sh LOG SCRIPT
# SCRIPT lines: "<seconds> <tokens...>"  (tokens: cross circle square triangle options up down
#               left right l1 r1 ... lx=0..255 ly=.. ; "shot" = screenshot; "-" = release all;
#               "end" = stop the game). Times are absolute from the start.
set -u
cd -- "$(dirname -- "$0")/.."
log=$1 script=$2
pad=$(mktemp)
: > "$pad"
GOW3_PAD_FILE=$pad GOW3_GAME_DIR=${GOW3_GAME_DIR:-$HOME/Games/PS4/CUSA00243} GOW3_PATCH_DIR=${GOW3_PATCH_DIR-$HOME/Games/PS4/CUSA00243-UPDATE} bash run.sh > "$log" 2>&1 &
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
kill "$game" 2>/dev/null; sleep 1; pkill -x gow3-probe 2>/dev/null
wait "$game" 2>/dev/null
rm -f "$pad"
grep -o 'Screenshot: [^ ]*' "$log"
