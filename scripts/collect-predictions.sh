#!/usr/bin/env bash
# What the gate predicts, per query, beside what was measured.
#
#   scripts/collect-predictions.sh          # writes tmp/predicted.csv
#
# `factorize_explain` prints the gate's own numbers -- the records it expects
# standing after each step, and the flat tuple count it expects DuckDB to
# produce. Summing the step records gives the quantity `calibrate-measured.sh`
# records as measured, so the two are directly comparable and
# `scripts/oracle-stats.py` can substitute one for the other.
#
# An EXPLAIN, not a run: the gate decides without executing, which is the whole
# point of it, and that is all this needs.
cd "$(dirname "$0")/.."
D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
OUT=${OUT:-tmp/predicted.csv}
echo "query,pred_records,pred_flat" > "$OUT"
while IFS='|' read -r name expected q; do
  [ -n "$name" ] || continue
  q=$(echo "$q" | tr -d '\r')
  # EXTRA lets a caller ask the same question of a differently-configured gate,
  # e.g. EXTRA="SET factorize_gate_exact_stats=true;" to separate what the
  # sample gets wrong from what the estimator does.
  line=$(printf '%s\n' "SET factorize_mode=${Q}auto${Q};" "${EXTRA:-}" "SET factorize_explain=true;" "EXPLAIN $q;" |
         timeout 300 "$D" -readonly "$DB" -noheader -list 2>&1 | grep -o 'gate predicted:.*' | head -1)
  [ -n "$line" ] || { echo "$name: gate printed nothing (declined before costing)" >&2; continue; }
  recs=$(echo "$line" | grep -o '[0-9.e+-]* recs' | awk '{s += $1} END {printf "%.6g", s}')
  flat=$(echo "$line" | grep -o 'flat [0-9.e+-]*' | awk '{print $2}')
  echo "${name},${recs:-},${flat:-}" >> "$OUT"
  printf '%-28s predicted recs %-12s flat %s\n' "$name" "${recs:-?}" "${flat:-?}"
done < tmp/ce_runnable_sql.psv
echo "=== $(($(wc -l < "$OUT") - 1)) queries written to $OUT"
