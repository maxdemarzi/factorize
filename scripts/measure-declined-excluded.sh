#!/usr/bin/env bash
# What is the gate declining on the excluded corpus, and is it right to?
#
#   scripts/measure-declined-excluded.sh            # tmp/excluded_agg/declined.csv
#   FLOOR=2 CAP=90 scripts/measure-declined-excluded.sh
#
# D103 measured the excluded corpus and found 94% of our time (342s of 365s)
# in 24 queries running at 1.02x against stock. That was reported as "both
# plans doing the same dominant work", which is true in the uninteresting
# sense: the gate DECLINED on 23 of the 24, so `auto` *is* stock there and
# those timings say nothing about this engine at all.
#
# D100 closed the gate line on the runnable corpus with the finding that
# declining is the wrong move. This asks the same question where the stakes
# are an order of magnitude higher: 340s sits behind these declines, and
# nobody has ever run them forced.
#
# off and force are timed adjacent in one session, because D103 also showed
# this machine drifts 12-36% between runs and only within-run ratios survive
# it. Answers are compared, not just clocks -- a fast wrong answer is not a
# win, and `force` bypasses the gate rather than the correctness path.
set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/clean-spill.sh"
CleanSpill

D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-90}
FLOOR=${FLOOR:-2}
DIR=${DIR:-tmp/excluded_agg}
SRC="$DIR/min.csv"
LIST="$DIR/agg_min.psv"
OUT="$DIR/declined.csv"

[ -f "$SRC" ] || { echo "no $SRC -- run measure-excluded-aggregates.sh first" >&2; exit 1; }
trap 'CleanSpill' EXIT

# The population: gate declined (fired = 0) and auto took at least FLOOR
# seconds. Everything below the floor is startup noise and cannot carry 340s.
NAMES=$(awk -F, -v f="$FLOOR" 'NR>1 && $2==0 && $4+0 >= f {print $1}' "$SRC" | sort)
echo "=== $(printf '%s\n' "$NAMES" | grep -c .) declined queries at or above ${FLOOR}s, cap ${CAP}s"

run_mode() {
    # Wall clock, and the answer, from one invocation: a plan that is faster
    # because it is wrong is the failure mode this whole line of work risks.
    local mode="$1" query="$2" start out rc
    start=$(date +%s.%N)
    if out=$(printf '%s\n' "SET factorize_mode=${Q}${mode}${Q};" "${query};" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1); then
        printf '%s|%s' "$(echo "$(date +%s.%N) - ${start}" | bc)" "$(printf '%s' "$out" | tail -1)"
    else
        printf 'to|'
    fi
}

echo "query,off_s,force_s,agree" >"$OUT"
while IFS='|' read -r name expected q; do
    [ -n "$name" ] || continue
    printf '%s\n' "$NAMES" | grep -qxF "$name" || continue
    q=$(printf '%s' "$q" | tr -d '\r')
    IFS='|' read -r off_s off_v <<<"$(run_mode off "$q")"
    IFS='|' read -r frc_s frc_v <<<"$(run_mode force "$q")"
    if [ "$off_s" = to ] || [ "$frc_s" = to ]; then
        agree=na
    elif [ "$off_v" = "$frc_v" ]; then
        agree=yes
    else
        agree="NO(${off_v}/${frc_v})"
    fi
    echo "${name},${off_s},${frc_s},${agree}" >>"$OUT"
    printf '  %-34s off %-12s force %-12s %s\n' "$name" "$off_s" "$frc_s" "$agree"
done <"$LIST"
echo "=== -> $OUT"
