#!/usr/bin/env bash
# Predicted per-edge fan-out from the gate's cost model (EdgeFactor.partners),
# for comparison against the fan-out the engine then actually measures.
#
#   scripts/collect-predicted-fanout.sh                      # /tmp/predfan.csv
#   LIST=tmp/excluded_all.psv OUT=/tmp/predfan_excluded.csv scripts/collect-predicted-fanout.sh
#
# EXPLAIN only, so this costs seconds per query where measuring the real thing
# costs minutes -- which is exactly why the plan-time version was worth testing
# before the runtime one, and D84 records how that came out.
#
# The diagnostics are behind `factorize_explain`, not behind EXPLAIN. Asking
# for the plan alone prints the operator and none of the gate's reasoning, and
# a first run of this collected 0 rows from 119 queries without erroring.
set -euo pipefail
cd "$(dirname "$0")/.."
D=build/release/duckdb; DB=/tmp/factorize-duckdb/ce.db
LIST=${LIST:-tmp/ce_runnable_sql.psv}
OUT=${OUT:-/tmp/predfan.csv}
echo "query,partners" > "$OUT"
while IFS='|' read -r name expected q; do
  [ -n "$name" ] || continue
  q=$(echo "$q" | tr -d '\r')
  {
    echo "SET factorize_mode='auto';"
    echo "SET factorize_explain=true;"
    echo "EXPLAIN $q;"
  } | timeout 120 "$D" -readonly "$DB" -noheader -list 2>&1 |
    grep -o 'partners [0-9.e+-]*' | awk -v n="$name" '{print n "," $2}' >> "$OUT"
done < "$LIST"
echo "=== $(($(wc -l < "$OUT") - 1)) edge rows, $(tail -n +2 "$OUT" | cut -d, -f1 | sort -u | wc -l) queries -> $OUT"
