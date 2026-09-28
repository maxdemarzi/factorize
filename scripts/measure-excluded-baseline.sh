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
# has this" from "stock does not". Measured, the answers run right up to the cap
# (hetio_acyclic_203_15 at 59.0s), so a shorter one would misclassify -- and a
# cap that truncates real answers is the one way this measurement can lie in the
# direction that flatters us.
#
# A query whose tables are not in the database is recorded as `missing`, not as
# a time. The first version of this took the Run Time that DuckDB prints after a
# *binder error* as an answer, and so reported that dblp answered 52 of 52
# queries in a millisecond -- dblp is not loaded here at all. A harness that
# records a non-answer as a fast answer is the same bug as one that records a
# timeout as a fast answer, and this file has now had both.
#
# No set -e: the caps are the result.
cd "$(dirname "$0")/.."
D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-60}
MODE=${MODE:-off}
OUT=${OUT:-tmp/excluded_baseline.csv}
LIST=${LIST:-tmp/excluded_all.psv}

# The tables this database actually has, so a query naming anything else is
# skipped rather than timed.
have=$("$D" -readonly "$DB" -noheader -list -c "select table_name from duckdb_tables();" 2>/dev/null)

echo "query,expected,${MODE}_s" > "$OUT"
n=0; answered=0; capped=0; missing=0
while IFS='|' read -r name expected q; do
  [ -n "$name" ] || continue
  q=$(echo "$q" | tr -d '\r')
  from=${q#*from }; from=${from#*FROM }; from=${from%% where*}; from=${from%% WHERE*}
  absent=0
  for t in $(echo "$from" | tr ',' ' '); do
    echo "$have" | grep -qx "$t" || { absent=1; break; }
  done
  n=$((n + 1))
  if [ "$absent" -eq 1 ]; then
    missing=$((missing + 1))
    echo "${name},${expected},missing" >> "$OUT"
    printf '%-28s %16s  not loaded\n' "$name" "$expected"
    continue
  fi
  out=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}${MODE}${Q};" "$q;" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1)
  status=$?
  secs=""
  if [ "$status" -ne 124 ] && ! echo "$out" | grep -qi "error"; then
    secs=$(echo "$out" | grep "Run Time" | sed 's/.*real //' | awk '{print $1}' | tail -1)
  fi
  if [ -n "$secs" ]; then
    answered=$((answered + 1))
  else
    secs=to; capped=$((capped + 1))
  fi
  echo "${name},${expected},${secs}" >> "$OUT"
  printf '%-28s %16s  stock %s\n' "$name" "$expected" "$secs"
done < "$LIST"
echo "=== $n queries: $answered answered within ${CAP}s, $capped capped, $missing not loaded -> $OUT"
