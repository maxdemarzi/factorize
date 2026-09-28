#!/usr/bin/env bash
# What the stock plan does with the >1e9-tuple regime.
#
#   scripts/measure-excluded-baseline.sh      # writes tmp/excluded_baseline.csv
#   CAP=60 OUT=tmp/x.csv scripts/...
#
# D47's open item, and the reason every claim about this regime has been
# unfalsifiable. The CE benchmark disables any query whose result exceeds 1e9
# tuples, so 481 queries have no published comparison and no stock timing here
# either -- which is why D42 could write "in the >1e9-tuple regime DuckDB does
# not finish, so firing more is strictly better there" and be wrong, and why
# D47 could only reject it rather than replace it:
#
#   "The excluded corpus needs a stock baseline -- every query timed under
#    factorize_mode='off' at a fixed cap -- so 'fires' can be replaced by
#    'answers stock does not produce, minus time lost on the ones it does'.
#    Until that exists, this corpus can say a setting is *safe* but not that it
#    is *better*."
#
# This is that. One run per query, because the question is a classification --
# does the stock plan answer at all, and if so how fast -- and not a precision
# timing. A query that answers in 3s and one that answers in 4s are the same
# answer to it.
#
# The cap is 60s by default rather than 300s: what matters is separating "stock
# has this" from "stock does not", and the known cases sit far from the line
# (5.1s, 0.02s, 35.7s on one side; no answer in 300s on the other). A shorter
# cap makes the run finish, which is the difference between a baseline that
# exists and one that is described in a decision entry.
#
# No set -e: the caps are the result.
cd "$(dirname "$0")/.."
D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-60}
OUT=${OUT:-tmp/excluded_baseline.csv}
LIST=${LIST:-tmp/excluded_all.psv}
echo "query,expected,stock_s" > "$OUT"
n=0; answered=0; capped=0
while IFS='|' read -r name expected q; do
  [ -n "$name" ] || continue
  q=$(echo "$q" | tr -d '\r')
  n=$((n + 1))
  out=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}off${Q};" "$q;" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1)
  if [ $? -eq 124 ]; then
    secs=to; capped=$((capped + 1))
  else
    secs=$(echo "$out" | grep "Run Time" | sed 's/.*real //' | awk '{print $1}' | tail -1)
    [ -n "$secs" ] && answered=$((answered + 1)) || { secs=to; capped=$((capped + 1)); }
  fi
  echo "${name},${expected},${secs}" >> "$OUT"
  printf '%-28s %16s  stock %s\n' "$name" "$expected" "$secs"
done < "$LIST"
echo "=== $n queries: $answered answered within ${CAP}s, $capped capped -> $OUT"
