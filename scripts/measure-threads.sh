#!/usr/bin/env bash
# What the join-key partition is worth on this corpus, per query.
#
#   scripts/measure-threads.sh             # writes tmp/threads.csv
#   CAP=300 OUT=tmp/x.csv scripts/...      # longer cap, elsewhere
#
# The operator counts one bucket of a hash partition of the join key per thread,
# so `threads` is also the number of buckets. That is not free: every bucket has
# to look at every row to find its own, so N buckets is N filtering passes over
# the input, and the partition only pays when the joining costs more than the
# scanning (D39).
#
# Measured with the fallback carried, which is the default and was serial until
# the source became parallel again.
#
# Needs tmp/ce_runnable_sql.psv and a release build.
#
# No set -e: a query that hits the cap is data.
cd "$(dirname "$0")/.."
D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-300}
OUT=${OUT:-tmp/threads.csv}
ONLY=${ONLY:-}
echo "query,t1_s,t8_s" > "$OUT"
best() { grep "Run Time" | sed 's/.*real //' | awk '{print $1}' | sed -n "$1p" | sort -g | head -1; }
while IFS='|' read -r name expected q; do
  [ -n "$name" ] || continue
  [ -z "$ONLY" ] || echo "$name" | grep -q "$ONLY" || continue
  q=$(echo "$q" | tr -d '\r')
  # Forced, so the query being timed is the factorized one in both runs. One
  # discarded warm-up, then two runs, the faster kept.
  one=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}force${Q};" "SET threads=1;" "$q;" "$q;" "$q;" |
        timeout $CAP "$D" -readonly "$DB" -noheader -list 2>&1 | best '4,5')
  eight=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}force${Q};" "SET threads=8;" "$q;" "$q;" "$q;" |
          timeout $CAP "$D" -readonly "$DB" -noheader -list 2>&1 | best '4,5')
  echo "${name},${one:-to},${eight:-to}" >> "$OUT"
  printf '%-28s 1 thread %-9s 8 threads %-9s %s\n' "$name" "${one:-to}" "${eight:-to}" \
      "$(if [ -n "$one" ] && [ -n "$eight" ]; then awk -v a="$one" -v b="$eight" \
         'BEGIN{if(b>0) printf "%.2fx", a/b}'; fi)"
done < tmp/ce_runnable_sql.psv
echo "=== $(($(wc -l < "$OUT") - 1)) queries written to $OUT"
