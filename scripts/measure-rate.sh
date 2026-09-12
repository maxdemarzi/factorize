#!/usr/bin/env bash
# The rate each query actually delivers at its last materialized join.
#
#   scripts/measure-rate.sh                 # writes tmp/rate.csv
#   CAP=300 OUT=tmp/x.csv scripts/...
#
# D52 measured this across six wins and four losses and found the two
# populations separated -- wins 1.11e5 to 6.28e7 tuples/ms, losses 17 to 4.59e4
# -- which is what `factorize_min_rate` was built on. A floor is only worth
# having if that separation is a property of the queries rather than of the
# afternoon it was measured on, and the way to find out is to measure the same
# ten queries again.
#
# At the default thread count, which is what a shipped floor would actually see:
# the check runs inside a slice, so the rate it divides is one bucket's tuples
# over one bucket's milliseconds. That is a different number from the whole
# query's, and it is the number that matters. Measuring at threads=1 instead was
# tried and several of these queries do not finish inside 200s that way.
#
# Reads the per-step line `factorize_explain` prints for slice 0 and takes the
# last step's tuples and cumulative milliseconds, which is exactly what
# CheckRate divides.
cd "$(dirname "$0")/.."
D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-300}
OUT=${OUT:-tmp/rate.csv}
NAMES=${NAMES:-"hetio_acyclic_205_03 hetio_acyclic_211_07 hetio_acyclic_211_10 hetio_acyclic_216_14
hetio_acyclic_210_05 hetio_acyclic_225_02 watdiv_acyclic_217_05 watdiv_acyclic_217_10
watdiv_acyclic_217_15 watdiv_acyclic_218_15"}

echo "query,tuples,elapsed_ms,rate" > "$OUT"
for name in $NAMES; do
  q=$(grep -h "^${name}|" tmp/ce_runnable_sql.psv tmp/excluded_all.psv 2>/dev/null |
      head -1 | cut -d'|' -f3- | tr -d '\r')
  [ -n "$q" ] || { echo "no sql for $name"; continue; }
  line=$(printf '%s\n' "SET factorize_mode=${Q}force${Q};" "SET factorize_explain=true;" "$q;" |
         timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | grep -o 'slice 0 .*')
  # Last `[relation: R recs L live T tuples Cx Mms]` group on the line.
  read -r tuples ms <<EOF
$(echo "$line" | grep -o '[0-9-]* tuples [0-9.]*x [0-9]*ms' | tail -1 |
  sed 's/ tuples [0-9.]*x / /; s/ms$//')
EOF
  if [ -z "$tuples" ] || [ -z "$ms" ] || [ "$ms" = "0" ]; then
    echo "${name},,,"; echo "$name: no step measured (capped, or the only join was fused)" >&2
    echo "${name},,," >> "$OUT"
    continue
  fi
  rate=$(awk -v t="$tuples" -v m="$ms" 'BEGIN{printf "%.0f", t/m}')
  echo "${name},${tuples},${ms},${rate}" >> "$OUT"
  printf '%-28s %16s tuples in %8sms  %12s tuples/ms\n' "$name" "$tuples" "$ms" "$rate"
done
echo "=== written to $OUT"
