#!/usr/bin/env bash
# What the join-key partition is worth, with the fallback carried.
#
#   scripts/measure-parallel.sh             # writes tmp/parallel.csv
#   CAP=300 STRIDE=1 OUT=tmp/x.csv scripts/...
#
# `factorize_parallel_fallback` decides whether the operator counts one bucket
# per thread while carrying the §7.5 fallback, and it is off: D54a measured it
# 5.4x the wrong way on a query that exceeds the memory budget, because every
# bucket exceeded it separately. Two things have changed since -- the buckets
# now stop each other (D54b) and the slice key is chosen to partition what the
# plan builds first (D54c) -- so the question is open again.
#
# It is the *setting* that is compared here, not `threads`. Comparing thread
# counts measures nothing while the setting is off, because one bucket is all
# the operator will use either way -- which is how the first version of this
# measurement came to say 1.49x about a configuration nobody runs.
#
# Forced, so the query being timed is the factorized one in both runs. One
# discarded warm-up, then two runs, the faster kept.
#
# No set -e: a query that hits the cap is data.
cd "$(dirname "$0")/.."
D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-300}
OUT=${OUT:-tmp/parallel.csv}
STRIDE=${STRIDE:-1}
LIST=${LIST:-tmp/ce_runnable_sql.psv}
echo "query,off_s,on_s" > "$OUT"
best() { grep "Run Time" | sed 's/.*real //' | awk '{print $1}' | sed -n "$1p" | sort -g | head -1; }
run() {  # $1 = sql, $2 = setting
  local out status
  out=$(printf '%s\n' ".timer on" "SET factorize_parallel_fallback=$2;" "SET factorize_mode=${Q}force${Q};" \
        "$1;" "$1;" "$1;" | timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1)
  status=$?
  if [ "$status" -eq 124 ]; then echo "to"; return; fi
  echo "$out" | best '4,5'
}

n=0
while IFS='|' read -r name expected q; do
  [ -n "$name" ] || continue
  n=$((n + 1))
  [ $((n % STRIDE)) -eq 0 ] || continue
  q=$(echo "$q" | tr -d '\r')
  off=$(run "$q" false)
  on=$(run "$q" true)
  echo "${name},${off:-to},${on:-to}" >> "$OUT"
  printf '%-28s serial %-9s buckets %-9s %s\n' "$name" "${off:-to}" "${on:-to}" \
      "$(if [ -n "$off" ] && [ -n "$on" ] && [ "$off" != to ] && [ "$on" != to ]; then \
         awk -v a="$off" -v b="$on" 'BEGIN{if(b>0) printf "%.2fx", a/b}'; fi)"
done < "$LIST"
echo "=== $(($(wc -l < "$OUT") - 1)) queries written to $OUT"
