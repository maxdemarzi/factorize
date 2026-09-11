#!/usr/bin/env bash
# The inputs a refit of the gate's cost model needs, measured on this machine.
#
#   scripts/calibrate-measured.sh          # writes tmp/calib_new.csv
#   OUT=tmp/x.csv CAP=300 scripts/...      # elsewhere, longer cap
#
# Feed the result to scripts/refit-measured.py. Needs tmp/ce_runnable_sql.psv
# (the corpus as plain SQL) and tmp/table_rows.csv (SELECT table_name,
# estimated_size FROM duckdb_tables()), and a release build: a relassert build
# carries the sanitizers, and a timing taken under those is not a timing.
#
# Per query it records input rows, the records the operator actually built,
# the exact result size, a warm stock time and a warm forced time -- measured
# rather than predicted, which is what the previous fit got wrong.
#
# Each mode: one discarded warm-up, then two runs, the faster kept.
# No set -e: a query that hits the cap, or a grep that finds no Run Time
# because of it, is data -- not a reason to abandon the corpus.
cd "$(dirname "$0")/.."
D=${BIN:-build/release/duckdb}; DB=/tmp/factorize-duckdb/ce.db; Q="'"
CAP=${CAP:-120}
OUT=${OUT:-tmp/calib_new.csv}
echo "query,input_rows,records,tuples,off_s,force_s,fired" > "$OUT"
best() { grep "Run Time" | sed 's/.*real //' | awk '{print $1}' | sed -n "$1p" | sort -g | head -1; }
while IFS='|' read -r name expected q; do
  [ -n "$name" ] || continue
  q=$(echo "$q" | tr -d '\r')
  rows=$(echo "$q" | sed 's/.*FROM //; s/ WHERE.*//' | tr ',' '\n' | awk '{print $1}' | while read -r t; do
           grep -i "^${t}," tmp/table_rows.csv | head -1 | cut -d, -f2; done | paste -sd+ | bc)
  outoff=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}off${Q};" "$q;" "$q;" "$q;" | timeout $CAP "$D" -readonly "$DB" -noheader -list 2>&1)
  off=$(echo "$outoff" | best '3,4')
  outf=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}force${Q};" "SET factorize_explain=true;" "$q;" "$q;" "$q;" | timeout $CAP "$D" -readonly "$DB" -noheader -list 2>&1)
  force=$(echo "$outf" | best '5,6')
  # Records the operator actually built, summed over its materialised steps.
  recs=$(echo "$outf" | grep -o '\[[0-9]*: [0-9]* recs' | grep -o '[0-9]* recs' | awk '{s+=$1} END {print s+0}')
  fired=$(printf '%s\n' "SET factorize_mode=${Q}auto${Q};" "EXPLAIN $q;" | timeout 60 "$D" -readonly "$DB" -noheader -list 2>&1 | grep -c FACTORIZED)
  echo "${name},${rows:-0},${recs:-0},${expected},${off},${force},${fired}" >> "$OUT"
  printf '%-28s rows %-9s recs %-11s off %-8s force %-8s fired=%s\n' "$name" "${rows:-?}" "${recs:-?}" "${off:-to}" "${force:-to}" "$fired"
done < tmp/ce_runnable_sql.psv
echo "=== $(wc -l < "$OUT") rows written to $OUT"
