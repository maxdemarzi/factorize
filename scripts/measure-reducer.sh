#!/usr/bin/env bash
# What the reducer does to the corpus it was built for.
#
#   scripts/measure-reducer.sh                  # tmp/excluded_agg/reducer.csv
#   KIND=max scripts/measure-reducer.sh
#
# Two questions at once. The clock: `auto` now reduces where it used to fold,
# and the excluded corpus is where that should matter. The answer: the fold is
# still there behind `factorize_reduce_extremes=false`, so on every query the
# fold can finish there are two implementations of this engine to disagree,
# and they must not.
#
# The fold is only run where the previous measurement says it finished, since
# re-paying a 60s timeout to rediscover that it times out buys nothing.
set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/clean-spill.sh"
CleanSpill
trap 'CleanSpill' EXIT

D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-60}
KIND=${KIND:-min}
LIST=${LIST:-tmp/excluded_agg/agg_${KIND}.psv}
SRC=${SRC:-tmp/excluded_agg/${KIND}.csv}
OUT=${OUT:-tmp/excluded_agg/reducer_${KIND}.csv}

run() {
    local query="$1" extra="$2" start out
    start=$(date +%s.%N)
    if out=$(printf '%s\n' "${extra}" "${query};" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1); then
        printf '%s|%s' "$(echo "$(date +%s.%N) - ${start}" | bc)" "$(printf '%s' "$out" | tail -1)"
    else
        printf 'to|'
    fi
}

echo "query,was_off,was_auto,now_auto_s,now_v,fold_s,fold_v,agree" >"$OUT"
n=0 answered=0 agree=0 compared=0
while IFS=, read -r name fired was_off was_auto; do
    [ "$name" = query ] && continue
    sql=$(awk -F'|' -v q="$name" '$1==q {print $3}' "$LIST" | tr -d '\r')
    [ -n "$sql" ] || continue
    n=$((n + 1))
    IFS='|' read -r now_s now_v <<<"$(run "$sql" "SET factorize_mode=${Q}auto${Q};")"
    [ "$now_s" != to ] && answered=$((answered + 1))
    # The fold, on the same binary, wherever it previously finished.
    if [ "$was_auto" != to ]; then
        IFS='|' read -r fold_s fold_v <<<"$(run "$sql" "SET factorize_reduce_extremes=false; SET factorize_mode=${Q}force${Q};")"
    else
        fold_s=skipped; fold_v=""
    fi
    if [ "$fold_s" = skipped ] || [ "$fold_s" = to ] || [ "$now_s" = to ]; then
        verdict=na
    elif [ "$now_v" = "$fold_v" ]; then
        verdict=yes; agree=$((agree + 1)); compared=$((compared + 1))
    else
        verdict="NO(${now_v}/${fold_v})"; compared=$((compared + 1))
        echo "  !! $name reducer=$now_v fold=$fold_v" >&2
    fi
    echo "${name},${was_off},${was_auto},${now_s},${now_v},${fold_s},${fold_v},${verdict}" >>"$OUT"
done <"$SRC"
echo "=== ${KIND}: $n queries, $answered answered by auto, $agree of $compared agree with the fold -> $OUT"
