#!/usr/bin/env bash
# Would racing one bucket of each plan pick correctly, on the corpus where the
# gate actually goes wrong?
#
#   scripts/measure-runnable-race.sh          # tmp/runnable_race.csv
#
# D98 explains the gate's failure: it fires when predicted DuckDB time beats
# predicted ours, that prediction is proportional to the predicted result
# size, and the result size is over-estimated by a median 94.8x on the
# queries it fires on against 1.9x on the ones it declines. The decision rule
# and the error are the same quantity, so no correction to the estimate's
# centre can help -- only reducing its variance, or checking it against
# reality before committing.
#
# D89 proposed the check and D92 showed it pointed at the wrong corpus. On the
# excluded corpus, running one bucket of the *stock* plan cost 73s and 83s on
# the two tried, because an eighth of an explosive join is still explosive.
# The runnable corpus is the opposite case by definition: DuckDB finishes
# these whole, in well under a second, so an eighth of one is cheap. This is
# the same experiment where the arithmetic is affordable.
#
# Four numbers per query: each plan on bucket 0, and each plan whole. The
# question is whether the first two predict the sign of the last two.
set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/clean-spill.sh"
CleanSpill
trap CleanSpill EXIT

D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
N=${N:-8}
CAP=${CAP:-60}
OUT=${OUT:-tmp/runnable_race.csv}

echo "query,stock_bucket_s,ours_bucket_s,stock_full_s,ours_full_s" > "$OUT"
best() { grep "Run Time" | sed 's/.*real //' | awk '{print $1}' | tail -2 | sort -g | head -1; }

while IFS='|' read -r name expected q; do
    [ -n "$name" ] || continue
    q=$(printf '%s' "$q" | tr -d '\r')
    # Only the queries the gate fires on: where it declines there is nothing
    # for a race to change.
    fired=$(printf '%s\n' "SET factorize_mode=${Q}auto${Q};" "EXPLAIN ${q};" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | grep -ci Factorized || true)
    [ "$fired" -gt 0 ] || continue
    key=$(printf '%s' "$q" | sed -E "s/.* [Ww][Hh][Ee][Rr][Ee] ([A-Za-z0-9_]+\.[A-Za-z0-9_]+) =.*/\1/")
    case "$key" in *.*) ;; *) continue ;; esac

    sb=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}off${Q};" \
        "${q} AND (${key} % ${N}) = 0;" "${q} AND (${key} % ${N}) = 0;" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | best)
    ob=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}force${Q};" \
        "${q} AND (${key} % ${N}) = 0;" "${q} AND (${key} % ${N}) = 0;" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | best)
    sf=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}off${Q};" "${q};" "${q};" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | best)
    of=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}force${Q};" "${q};" "${q};" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | best)
    echo "${name},${sb:-to},${ob:-to},${sf:-to},${of:-to}" >> "$OUT"
    echo "  ${name} bucket: stock ${sb:-to} ours ${ob:-to} | whole: stock ${sf:-to} ours ${of:-to}" >&2
done < tmp/ce_runnable_sql.psv
echo "=== -> $OUT"
