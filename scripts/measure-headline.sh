#!/usr/bin/env bash
# The three numbers the README leads with, re-measured.
#
#   scripts/measure-headline.sh             # writes tmp/headline.csv
#   CAP=300 OUT=tmp/x.csv scripts/...
#
# "1.71x on the corpus", "faster on 23, fires on 35" and "firing on every match
# would be 0.04x" were all measured before the parallel fallback became the
# default (D54d), which is worth 3.00x on this corpus. A headline that predates
# a 3x change is wrong in the project's own favour, which is the direction a
# README is least entitled to be wrong in.
#
# Three modes per query: 'off' is the baseline, 'auto' is what a user gets, and
# 'force' is what firing on every match costs. One discarded warm-up then two
# timed runs, the faster kept, which is the methodology calibrate-measured.sh
# established and the only one these numbers can be compared against.
#
# No set -e: a query that hits the cap is data.
cd "$(dirname "$0")/.."
# Clear any spill a previous capped run left behind, and again at the end:
# `timeout` kills DuckDB before it cleans up after itself (scripts/clean-spill.sh).
. "$(dirname "$0")/clean-spill.sh"
CleanSpill
trap CleanSpill EXIT
D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-300}
OUT=${OUT:-tmp/headline.csv}
LIST=${LIST:-tmp/ce_runnable_sql.psv}
echo "query,off_s,auto_s,force_s,fired" > "$OUT"
best() { grep "Run Time" | sed 's/.*real //' | awk '{print $1}' | sed -n "$1p" | sort -g | head -1; }
run() {  # $1 = sql, $2 = mode
  local out status
  out=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}$2${Q};" "$1;" "$1;" "$1;" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1)
  status=$?
  if [ "$status" -eq 124 ]; then echo "to"; return; fi
  echo "$out" | best '3,4'
}

while IFS='|' read -r name expected q; do
  [ -n "$name" ] || continue
  q=$(echo "$q" | tr -d '\r')
  off=$(run "$q" off)
  auto=$(run "$q" auto)
  force=$(run "$q" force)
  fired=$(printf '%s\n' "SET factorize_mode=${Q}auto${Q};" "EXPLAIN $q;" |
          timeout 120 "$D" -readonly "$DB" -noheader -list 2>&1 | grep -c FACTORIZED)
  echo "${name},${off:-to},${auto:-to},${force:-to},${fired}" >> "$OUT"
  printf '%-28s off %-9s auto %-9s force %-9s fired=%s\n' "$name" "${off:-to}" "${auto:-to}" "${force:-to}" "$fired"
done < "$LIST"
echo "=== $(($(wc -l < "$OUT") - 1)) queries written to $OUT"
