#!/usr/bin/env bash
# Is a prefix of a join cheaper through the representation than through DuckDB?
#
#   scripts/measure-tuples.sh               # writes tmp/tuples.csv
#   K=100 CAP=120 OUT=tmp/x.csv scripts/...
#
# The README has claimed since the table functions went in that "a hundred rows
# out of a join with a trillion" is where factorization wins outright, because
# "stock DuckDB materialises the hash-join intermediates regardless of the
# LIMIT". Half of that is true: DuckDB's probe side streams and stops at k, so
# the top join materializes nothing -- but the *build* side of every join is
# blocking, and in a multi-way plan those build sides are themselves join
# results.
#
# Which half dominates is a measurement, and it is the measurement that decides
# whether §10.3's tuple output is worth wiring into the optimizer rule. Doing it
# through the table function that already exists costs nothing and answers the
# same question, which is the lesson of D53: the EXISTS shape was built first
# and measured second, and it was slower on 93 of 119 queries.
#
# Both sides emit the same k rows of the same join. No warm-up is discarded:
# each side is run three times and the fastest kept.
#
# No set -e: hitting the cap is a result.
cd "$(dirname "$0")/.."
D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-120}
K=${K:-100}
OUT=${OUT:-tmp/tuples.csv}
STRIDE=${STRIDE:-4}
echo "query,k,stock_s,ours_s" > "$OUT"
best() { grep "Run Time" | sed 's/.*real //' | awk '{print $1}' | sed -n "$1p" | sort -g | head -1; }

n=0
while IFS='|' read -r name expected q; do
  [ -n "$name" ] || continue
  n=$((n + 1))
  [ $((n % STRIDE)) -eq 0 ] || continue
  q=$(echo "$q" | tr -d '\r')
  from=${q#*FROM }; from=${from%% WHERE*}
  where=${q#*WHERE }
  # The same join, asked for k rows instead of a count.
  stock="SELECT * FROM ${from} WHERE ${where} LIMIT ${K}"
  # ['a', 'b', ...] and ['a.x = b.x', ...] for the table function. Built in
  # python rather than sed: the quoting needed to emit single quotes from a
  # shell already inside single quotes is how the first version of this silently
  # produced an empty list.
  ours=$(python3 -c '
import sys
frm, where, k = sys.argv[1], sys.argv[2], sys.argv[3]
tables = [t.strip() for t in frm.split(",") if t.strip()]
preds = [p.strip() for p in where.split(" AND ") if p.strip()]
quote = lambda xs: "[" + ", ".join("'"'"'" + x + "'"'"'" for x in xs) + "]"
print("SELECT * FROM factorized_tuples(%s, %s, %s)" % (quote(tables), quote(preds), k))
' "$from" "$where" "$K")

  o=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}off${Q};" "$stock;" "$stock;" "$stock;" |
      timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | best '2,4')
  m=$(printf '%s\n' ".timer on" "$ours;" "$ours;" "$ours;" |
      timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | best '1,3')
  echo "${name},${K},${o:-to},${m:-to}" >> "$OUT"
  printf '%-28s k=%-5s stock %-9s ours %-9s %s\n' "$name" "$K" "${o:-to}" "${m:-to}" \
      "$(if [ -n "$o" ] && [ -n "$m" ]; then awk -v a="$o" -v b="$m" \
         'BEGIN{if(b>0) printf "%.2fx", a/b}'; fi)"
done < tmp/ce_runnable_sql.psv
echo "=== $(($(wc -l < "$OUT") - 1)) queries written to $OUT"
