#!/usr/bin/env bash
# Which queries the gate fires on, by EXPLAIN alone.
#
#   scripts/collect-fires.sh                 # writes tmp/fires.csv
#   LIST=... OUT=... scripts/collect-fires.sh
#
# collect-predictions.sh records a row whenever the gate printed its numbers,
# which it does whether it fires or declines -- so that file cannot answer
# "did this decision change". This can: it asks for the plan and looks for the
# operator. No execution, so a whole corpus costs seconds.
cd "$(dirname "$0")/.."
D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
OUT=${OUT:-tmp/fires.csv}
LIST=${LIST:-tmp/ce_runnable_sql.psv}
echo "query,fired" > "$OUT"
while IFS='|' read -r name expected q; do
  [ -n "$name" ] || continue
  q=$(echo "$q" | tr -d '\r')
  n=$(printf '%s\n' "SET factorize_mode=${Q}auto${Q};" "EXPLAIN $q;" |
      timeout 300 "$D" -readonly "$DB" -noheader -list 2>&1 | grep -c FACTORIZED)
  echo "${name},${n}" >> "$OUT"
done < "$LIST"
echo "=== $(($(wc -l < "$OUT") - 1)) queries, $(awk -F, 'NR>1 && $2>0' "$OUT" | wc -l) fire -> $OUT"
