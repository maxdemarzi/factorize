#!/usr/bin/env bash
# Time the reducer against the rows it reads, so its cost can be fitted
# instead of inherited.
#
#   scripts/fit-reducer-cost.sh            # tmp/shape/reducer_cost.csv
#
# The gate charges a reducer `ours.per_input_row_ms`, a coefficient fitted for
# building an f-representation. D109 measured the result: predictions 3 to 10x
# low, 429ms against an actual 3470ms before the key set was replaced. A
# reducer reads rows and probes a hash set; a fold builds records. There is no
# reason the same number should describe both, and nobody had checked.
#
# Only the queries the reducer actually runs on, and a short cap. The first
# version of this forced every query in the sample at a 60s cap, three times
# each: where the reducer does not apply, `force` runs the fold, which on this
# corpus means paying three full timeouts to learn nothing about a reducer. It
# produced two rows in several minutes and was abandoned.
#
# `force`, not `auto`, because the point is to time the path whatever the gate
# thinks of it. Two timed runs, faster kept.
set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/clean-spill.sh"
CleanSpill
trap 'CleanSpill' EXIT

D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-20}
LIST=${LIST:-tmp/excluded_agg/agg_min.psv}
SAMPLE=${SAMPLE:-tmp/excluded_agg/min.csv}
ROWS=${ROWS:-tmp/shape/table_rows.psv}
OUT=${OUT:-tmp/shape/reducer_cost.csv}

# Reducible shapes only: `fired` is 1 where the rule took the query over,
# and only those run the path being fitted.
names=$(awk -F, 'NR>1 && $2!=0 {print $1}' "$SAMPLE" | sort)
echo "query,relations,input_rows,reduced_s" >"$OUT"
while IFS='|' read -r name expected q; do
    [ -n "$name" ] || continue
    printf '%s\n' "$names" | grep -qxF "$name" || continue
    q=$(printf '%s' "$q" | tr -d '\r')
    # Total rows the reducer must read: every relation of the query, once.
    # Aliases name the same table twice and are counted twice, which is what
    # the reducer actually does.
    tables=$(printf '%s' "$q" | sed -n 's/.* [Ff][Rr][Oo][Mm] \(.*\) [Ww][Hh][Ee][Rr][Ee] .*/\1/p' | tr ',' '\n' | awk '{print $1}')
    total=0; n=0
    for t in $tables; do
        r=$(awk -F'|' -v t="$t" '$1==t {print $2}' "$ROWS")
        [ -n "$r" ] || { total=; break; }
        total=$((total + r)); n=$((n + 1))
    done
    [ -n "$total" ] || continue
    best=
    for i in 1 2; do
        s=$(date +%s.%N)
        if printf '%s\n' "SET factorize_mode=${Q}force${Q};" "${q};" |
            timeout "$CAP" "$D" -readonly "$DB" -noheader -list >/dev/null 2>&1; then
            t=$(echo "$(date +%s.%N) - ${s}" | bc)
            best=$(printf '%s\n%s\n' "$best" "$t" | grep . | sort -g | head -1)
        fi
    done
    [ -n "$best" ] || continue
    echo "${name},${n},${total},${best}" >>"$OUT"
done <"$LIST"
echo "=== $(($(wc -l <"$OUT") - 1)) timed -> $OUT"
