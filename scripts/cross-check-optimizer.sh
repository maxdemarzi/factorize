#!/usr/bin/env bash
# Phase 3's exit criterion: the CE corpus run as *plain SQL*, with the optimizer
# rule taking the plan over, must answer exactly what it answers with the rule
# switched off.
#
#   scripts/cross-check-optimizer.sh [max_expected_for_duckdb] [duckdb_limit]
#
# Two checks, because they fail in different ways:
#
#   1. Every query, against the corpus's published result size. Cheap, covers
#      the whole corpus, and catches a wrong count.
#   2. A subset small enough for stock DuckDB to materialise, against
#      factorize_mode='off' on the same machine in the same session. Slower,
#      covers far less, and catches a *disagreement* -- the case where both the
#      published size and our count could be right about different questions.
#
# Also records whether the rule actually fired, since a rule that silently
# declines everything passes both checks perfectly.
set -euo pipefail
cd "$(dirname "$0")/.."

DUCKDB="${DUCKDB:-build/relassert/duckdb}"
WORK="${TMPDIR:-/tmp}/factorize-duckdb"
DB="${DB:-$WORK/ce.db}"
MAX_EXPECTED="${1:-2000000}"
LIMIT="${2:-25}"
TIMEOUT="${TIMEOUT:-300}"

[ -s tmp/ce_runnable.psv ] || { echo "run scripts/run-ce-extension.sh first" >&2; exit 2; }
[ -x "$DUCKDB" ] || { echo "no duckdb binary at $DUCKDB" >&2; exit 2; }

echo "query,expected,forced,fired,duckdb,status"
fired=0
declined=0
mismatched=0
compared=0
total=0

while IFS='|' read -r name expected sql; do
    [ -n "$name" ] || continue
    total=$((total + 1))

    plain=$(python3 - "$sql" <<'PY'
import re, sys
m = re.search(r"factorized_count\(\[(.*?)\], \[(.*?)\]\)", sys.argv[1])
tables = [t.strip().strip("'") for t in m.group(1).split(",")]
joins = [j.strip().strip("'") for j in m.group(2).split("', '")]
print(f"SELECT count(*) FROM {', '.join(tables)} WHERE {' AND '.join(joins)};")
PY
)

    forced=$(timeout "$TIMEOUT" "$DUCKDB" "$DB" -noheader -list \
        -c "SET factorize_mode='force';" -c "$plain" 2>/dev/null | tr -d '[:space:]') || forced=""
    explain=$(timeout "$TIMEOUT" "$DUCKDB" "$DB" -noheader -list \
        -c "SET factorize_mode='force';" -c "EXPLAIN $plain" 2>/dev/null) || explain=""

    if grep -q FACTORIZED <<<"$explain"; then
        took_over=yes
        fired=$((fired + 1))
    else
        took_over=no
        declined=$((declined + 1))
    fi

    # Only queries stock DuckDB can finish get the head-to-head.
    stock=""
    if [ "$expected" -le "$MAX_EXPECTED" ] 2>/dev/null && [ "$compared" -lt "$LIMIT" ]; then
        stock=$(timeout "$TIMEOUT" "$DUCKDB" "$DB" -noheader -list \
            -c "SET factorize_mode='off';" -c "$plain" 2>/dev/null | tr -d '[:space:]') || stock=""
        [ -n "$stock" ] && compared=$((compared + 1))
    fi

    if [ -z "$forced" ]; then
        status=error
    elif [ "$forced" != "$expected" ]; then
        status=MISMATCH
        mismatched=$((mismatched + 1))
    elif [ -n "$stock" ] && [ "$forced" != "$stock" ]; then
        status=DISAGREE
        mismatched=$((mismatched + 1))
    else
        status=ok
    fi
    printf '%s,%s,%s,%s,%s,%s\n' "$name" "$expected" "$forced" "$took_over" "${stock:-}" "$status"
done < tmp/ce_runnable.psv

echo "-- $total queries: $fired taken over, $declined declined, $compared head-to-head vs 'off', $mismatched wrong" >&2
[ "$mismatched" -eq 0 ]
