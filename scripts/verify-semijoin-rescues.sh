#!/usr/bin/env bash
# Cross-check the reduced form where stock DuckDB cannot answer at all.
#
#   scripts/verify-semijoin-rescues.sh
#
# The rewrite agrees with stock on all 23 queries where both finish, which is
# the only comparison available there. On the queries stock times out on, the
# reduced form produces an answer nothing else in the project has produced,
# and "fast" is worthless without "right".
#
# This engine is the second opinion. min.csv records 46 queries it rescues --
# stock times out, `auto` finishes -- so on those there are two independent
# implementations to disagree: a Yannakakis reducer run by DuckDB, and an
# f-representation built by us.
set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/clean-spill.sh"
CleanSpill
trap 'CleanSpill' EXIT

D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-90}
SRC=${SRC:-tmp/excluded_agg/min.csv}
LIST=${LIST:-tmp/excluded_agg/agg_min.psv}
OUT=${OUT:-tmp/excluded_agg/semijoin_verify.csv}
SAMPLE=${SAMPLE:-20}

# Rescues only: off timed out, auto did not.
RESCUES=$(awk -F, 'NR>1 && $3=="to" && $4!="to" {print $1}' "$SRC" | head -"$SAMPLE")
echo "=== cross-checking $(printf '%s\n' "$RESCUES" | grep -c .) rescued queries"

REWRITTEN=$(mktemp); trap 'rm -f "$REWRITTEN"; CleanSpill' EXIT
# shellcheck disable=SC2086
python3 scripts/semijoin-rewrite.py "$LIST" $RESCUES >"$REWRITTEN" 2>/dev/null

echo "query,engine_s,engine_v,reduced_s,reduced_v,agree" >"$OUT"
agree=0 n=0
while IFS='|' read -r name reduced; do
    [ -n "$name" ] || continue
    original=$(awk -F'|' -v q="$name" '$1==q {print $3}' "$LIST" | tr -d '\r')
    n=$((n + 1))
    s=$(date +%s.%N)
    ev=$(printf '%s\n' "SET factorize_mode=${Q}force${Q};" "${original};" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | tail -1) || ev=to
    es=$(echo "$(date +%s.%N) - ${s}" | bc)
    s=$(date +%s.%N)
    rv=$(printf '%s\n' "SET factorize_mode=${Q}off${Q};" "${reduced};" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | tail -1) || rv=to
    rs=$(echo "$(date +%s.%N) - ${s}" | bc)
    if [ "$ev" = "$rv" ]; then verdict=yes; agree=$((agree + 1)); else verdict="NO"; fi
    echo "${name},${es},${ev},${rs},${rv},${verdict}" >>"$OUT"
    printf '  %-28s engine %-10s = %-14s reduced %-10s = %-14s %s\n' "$name" "$es" "$ev" "$rs" "$rv" "$verdict"
done <"$REWRITTEN"
echo "=== $agree of $n agree -> $OUT"
