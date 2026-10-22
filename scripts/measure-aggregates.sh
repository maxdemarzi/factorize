#!/usr/bin/env bash
# What the aggregates added since D76 are worth, as opposed to whether they are
# right.
#
#   scripts/measure-aggregates.sh            # writes tmp/aggregates.csv
#
# Five aggregates reached SQL in D77 through D80 -- min, max, avg, count(c),
# bool_and/bool_or and sum(x*y) -- and every one was verified against stock
# DuckDB and against brute force, and none was ever timed. The gate's
# coefficients were fitted on count(*) and sum (D19, O11), so there is no
# reason to assume the margin it applies is right for a fold that reads a
# column it did not read before, and the CE corpus cannot say: every one of its
# queries is a count(*).
#
# So the corpus is rewritten, one aggregate at a time, over the same join
# graphs: the shapes are the ones the gate was tuned on, and only the fold
# changes. One discarded warm-up then two timed runs, the faster kept, which is
# the methodology every other measurement here uses.
cd "$(dirname "$0")/.."
. "$(dirname "$0")/clean-spill.sh"
CleanSpill
trap CleanSpill EXIT
D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-120}
OUT=${OUT:-tmp/aggregates.csv}
KINDS=${KINDS:-"min max avg sumprod"}
echo "kind,query,off_s,auto_s,fired" > "$OUT"
best() { grep "Run Time" | sed 's/.*real //' | awk '{print $1}' | sed -n '3,4p' | sort -g | head -1; }
for kind in $KINDS; do
  list="tmp/agg_${kind}.psv"
  [ -f "$list" ] || { echo "no $list" >&2; continue; }
  n=0; fires=0
  while IFS='|' read -r name expected q; do
    [ -n "$name" ] || continue
    q=$(echo "$q" | tr -d '\r')
    off=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}off${Q};" "$q;" "$q;" "$q;" |
          timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | best)
    auto=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}auto${Q};" "$q;" "$q;" "$q;" |
           timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | best)
    f=$(printf '%s\n' "SET factorize_mode=${Q}auto${Q};" "EXPLAIN $q;" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | grep -ci Factorized)
    [ "$f" -gt 0 ] && fires=$((fires+1))
    n=$((n+1))
    echo "${kind},${name},${off:-to},${auto:-to},${f}" >> "$OUT"
  done < "$list"
  echo "=== $kind: $n queries, $fires fire"
done
