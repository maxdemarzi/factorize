#!/usr/bin/env bash
# Is the 340s the excluded corpus spends in declined queries actually necessary?
#
#   scripts/measure-semijoin.sh            # tmp/excluded_agg/semijoin.csv
#
# D104 showed the gate is right to decline these: forcing them is a wash,
# because this engine is no faster than DuckDB on them. That is a statement
# about the two plans on offer, not about the query. A MIN over an acyclic
# join needs a full reducer, not the join, and neither plan does that.
#
# Both forms are timed adjacent and both answers compared, because a rewrite
# that is fast and wrong is the obvious failure mode here.
set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/clean-spill.sh"
CleanSpill
trap 'CleanSpill' EXIT

D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-120}
SRC=${SRC:-tmp/excluded_agg/declined.csv}
LIST=${LIST:-tmp/excluded_agg/agg_min.psv}
OUT=${OUT:-tmp/excluded_agg/semijoin.csv}

# REDUCED_ONLY reads the join time from $SRC instead of re-measuring it. The
# join side of this corpus is 60s timeouts by construction, and re-paying them
# to confirm a 20x gap is a poor trade; the gap is far outside the 12-36% drift
# D103 measured between runs, so a stale baseline is good enough here and a
# fresh one is not worth an hour.
REDUCED_ONLY=${REDUCED_ONLY:-0}
JOIN_COL=${JOIN_COL:-off_s}
NAMES=$(awk -F, 'NR>1 {print $1}' "$SRC" | sort)
REWRITTEN=$(mktemp); trap 'rm -f "$REWRITTEN"; CleanSpill' EXIT
# shellcheck disable=SC2086
python3 scripts/semijoin-rewrite.py "$LIST" $NAMES >"$REWRITTEN" 2>/dev/null
echo "=== $(wc -l <"$REWRITTEN") of $(printf '%s\n' "$NAMES" | grep -c .) rewritten, cap ${CAP}s"

run() {
    local query="$1" start out
    start=$(date +%s.%N)
    if out=$(printf '%s\n' "SET factorize_mode=${Q}off${Q};" "${query};" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1); then
        printf '%s|%s' "$(echo "$(date +%s.%N) - ${start}" | bc)" "$(printf '%s' "$out" | tail -1)"
    else
        printf 'to|'
    fi
}

echo "query,join_s,reduced_s,speedup,agree" >"$OUT"
while IFS='|' read -r name reduced; do
    [ -n "$name" ] || continue
    original=$(awk -F'|' -v n="$name" '$1==n {print $3}' "$LIST" | tr -d '\r')
    [ -n "$original" ] || continue
    if [ "$REDUCED_ONLY" = 1 ]; then
        js=$(awk -F, -v n="$name" -v c="$JOIN_COL" 'NR==1{for(i=1;i<=NF;i++) if($i==c) k=i; next} $1==n {print $k}' "$SRC")
        jv=""
    else
        IFS='|' read -r js jv <<<"$(run "$original")"
    fi
    IFS='|' read -r rs rv <<<"$(run "$reduced")"
    if [ "$REDUCED_ONLY" = 1 ]; then
        # No join answer to compare against, so the clock is all there is; the
        # rewrite's correctness is established on the 23 where both ran.
        if [ "$rs" = to ]; then
            agree=na; speed=na
        elif [ "$js" = to ]; then
            agree="unrun"; speed="inf"
        else
            agree="unchecked"; speed=$(awk -v a="$js" -v b="$rs" 'BEGIN{printf "%.1f", a/b}')
        fi
    elif [ "$js" = to ] || [ "$rs" = to ]; then
        agree=na; speed=na
    elif [ "$jv" = "$rv" ]; then
        agree=yes; speed=$(awk -v a="$js" -v b="$rs" 'BEGIN{printf "%.1f", a/b}')
    else
        agree="NO(${jv}/${rv})"; speed=na
    fi
    echo "${name},${js},${rs},${speed},${agree}" >>"$OUT"
    printf '  %-26s join %-12s reduced %-12s %6sx  %s\n' "$name" "$js" "$rs" "$speed" "$agree"
done <"$REWRITTEN"
echo "=== -> $OUT"
