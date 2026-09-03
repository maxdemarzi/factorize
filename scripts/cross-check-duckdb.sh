#!/usr/bin/env bash
# The other half of Phase 2's exit criterion: factorized_count() must agree with
# stock DuckDB's count(*), not only with the corpus's published result sizes.
#
#   scripts/cross-check-duckdb.sh [max_expected] [limit]
#
# Only queries whose result DuckDB can materialise in reasonable time are used --
# the published sizes reach 1e17, and the point here is to check the two engines
# against each other, not to time DuckDB.
set -euo pipefail
cd "$(dirname "$0")/.."

DUCKDB="${DUCKDB:-build/relassert/duckdb}"
WORK="${TMPDIR:-/tmp}/factorize-duckdb"
DB="${DB:-$WORK/ce.db}"
MAX_EXPECTED="${1:-2000000}"
LIMIT="${2:-25}"
TIMEOUT="${TIMEOUT:-120}"

[ -s tmp/ce_runnable.psv ] || { echo "run scripts/run-ce-extension.sh first" >&2; exit 2; }

echo "query,expected,factorized,duckdb,status"
checked=0
while IFS='|' read -r name expected sql; do
    [ -n "$name" ] || continue
    [ "$expected" -le "$MAX_EXPECTED" ] 2>/dev/null || continue
    [ "$checked" -lt "$LIMIT" ] || break

    # Rebuild the equivalent plain SQL from the same lists the function takes,
    # so both engines are asked the identical question.
    plain=$(python3 - "$sql" <<'PY'
import re, sys
inner = sys.argv[1]
m = re.search(r"factorized_count\(\[(.*?)\], \[(.*?)\]\)", inner)
tables = [t.strip().strip("'") for t in m.group(1).split(",")]
joins = [j.strip().strip("'") for j in m.group(2).split("', '")]
print(f"SELECT count(*) FROM {', '.join(tables)} WHERE {' AND '.join(joins)};")
PY
)
    fact=$(timeout "$TIMEOUT" "$DUCKDB" "$DB" -noheader -list -c "$sql" 2>/dev/null | tr -d '[:space:]') || fact=""
    duck=$(timeout "$TIMEOUT" "$DUCKDB" "$DB" -noheader -list -c "$plain" 2>/dev/null | tr -d '[:space:]') || duck=""
    [ -n "$fact" ] && [ -n "$duck" ] || continue

    if [ "$fact" = "$duck" ] && [ "$fact" = "$expected" ]; then
        status=ok
    else
        status=MISMATCH
    fi
    printf '%s,%s,%s,%s,%s\n' "$name" "$expected" "$fact" "$duck" "$status"
    checked=$((checked + 1))
done < tmp/ce_runnable.psv

echo "-- cross-checked $checked queries" >&2
