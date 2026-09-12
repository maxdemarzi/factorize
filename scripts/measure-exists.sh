#!/usr/bin/env bash
# What EXISTS costs each engine on the CE corpus, per query.
#
#   scripts/measure-exists.sh              # writes tmp/exists.csv
#   CAP=60 OUT=tmp/x.csv scripts/...       # shorter cap, elsewhere
#
# Every corpus query is a count(*) over a join; this asks the same join whether
# it has any tuple at all. The two engines answer it differently: DuckDB builds
# the join's hash tables and stops at the first probe that matches, while the
# factorized path counts buckets of the join key and stops at the first that is
# non-empty -- so the buckets after it are never built.
#
# Needs tmp/ce_runnable_sql.psv (the corpus as plain SQL) and a release build:
# a relassert build carries the sanitizers, and a timing taken under those is
# not a timing.
#
# Each mode: one discarded warm-up, then two runs, the faster kept. The answers
# are recorded beside the times because a faster wrong answer is not a result.
#
# No set -e: a query that hits the cap is data, not a reason to stop.
cd "$(dirname "$0")/.."
D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-120}
OUT=${OUT:-tmp/exists.csv}
echo "query,off_answer,off_s,force_answer,force_s,fired" > "$OUT"
best() { grep "Run Time" | sed 's/.*real //' | awk '{print $1}' | sed -n "$1p" | sort -g | head -1; }
answer() { grep -E '^(true|false)$' | head -1; }
while IFS='|' read -r name expected q; do
  [ -n "$name" ] || continue
  q=$(echo "$q" | tr -d '\r')
  # The same join, asked for a witness rather than a count.
  e="SELECT EXISTS (SELECT 1 ${q#SELECT count(*) })"
  outoff=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}off${Q};" "$e;" "$e;" "$e;" |
           timeout $CAP "$D" -readonly "$DB" -noheader -list 2>&1)
  outf=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}force${Q};" "$e;" "$e;" "$e;" |
         timeout $CAP "$D" -readonly "$DB" -noheader -list 2>&1)
  off=$(echo "$outoff" | best '3,4')
  force=$(echo "$outf" | best '3,4')
  offa=$(echo "$outoff" | answer)
  fa=$(echo "$outf" | answer)
  fired=$(printf '%s\n' "SET factorize_mode=${Q}auto${Q};" "EXPLAIN $e;" |
          timeout 60 "$D" -readonly "$DB" -noheader -list 2>&1 | grep -c FACTORIZED)
  echo "${name},${offa:-?},${off:-to},${fa:-?},${force:-to},${fired}" >> "$OUT"
  printf '%-28s off %-7s %-8s force %-7s %-8s fired=%s%s\n' "$name" "${offa:-?}" "${off:-to}" \
      "${fa:-?}" "${force:-to}" "$fired" \
      "$([ -n "$offa" ] && [ -n "$fa" ] && [ "$offa" != "$fa" ] && echo '  <== DISAGREE')"
done < tmp/ce_runnable_sql.psv
echo "=== $(($(wc -l < "$OUT") - 1)) queries written to $OUT"
