#!/usr/bin/env bash
# Does charging a column-reading fold more make the gate's decisions better?
#
#   scripts/sweep-fold-cost.sh                 # tmp/shape/fold_sweep.csv
#   KINDS="sum avg" VALUES="1 2 4" scripts/sweep-fold-cost.sh
#
# D81 built `factorize_fold_cost` for exactly this and measured it doing
# nothing, so it ships at 1.0. That measurement is no longer the right one:
# it was taken over a corpus where `min` and `max` were fold-path queries,
# and D108's reducer took both off the fold entirely. What remains on the
# fold -- avg, sum, sum(x*y) -- is where the corpus's largest avoidable loss
# now sits, 3.65s to 4.37s each against a total win of 8 to 9s (D114).
#
# Settings are swept per query, interleaved, because the machine drifts
# through a run by more than the effect being looked for (D113).
set -uo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/clean-spill.sh"
CleanSpill
trap 'CleanSpill' EXIT

D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-60}
KINDS=${KINDS:-"sum avg sumprod"}
VALUES=${VALUES:-"1 2 4 8"}
OUT=${OUT:-tmp/shape/fold_sweep.csv}

echo "kind,query,fold_cost,off_s,auto_s,fired" >"$OUT"
for kind in $KINDS; do
    list="tmp/agg_${kind}.psv"
    [ -f "$list" ] || continue
    while IFS='|' read -r name expected q; do
        [ -n "$name" ] || continue
        q=$(printf '%s' "$q" | tr -d '\r')
        s=$(date +%s.%N)
        printf '%s\n' "SET factorize_mode=${Q}off${Q};" "${q};" |
            timeout "$CAP" "$D" -readonly "$DB" -noheader -list >/dev/null 2>&1
        off=$(echo "$(date +%s.%N) - ${s}" | bc)
        for v in $VALUES; do
            fired=$(printf '%s\n' "SET factorize_fold_cost=${v};" "SET factorize_mode=${Q}auto${Q};" "EXPLAIN ${q};" |
                timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | grep -c FACTORIZED)
            s=$(date +%s.%N)
            printf '%s\n' "SET factorize_fold_cost=${v};" "SET factorize_mode=${Q}auto${Q};" "${q};" |
                timeout "$CAP" "$D" -readonly "$DB" -noheader -list >/dev/null 2>&1
            auto=$(echo "$(date +%s.%N) - ${s}" | bc)
            echo "${kind},${name},${v},${off},${auto},${fired}" >>"$OUT"
        done
    done <"$list"
    echo "=== $kind swept"
done
