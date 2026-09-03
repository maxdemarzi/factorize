#!/usr/bin/env bash
# Phase 2 exit check: run CE queries through factorized_count() inside DuckDB
# and compare against the corpus's published result sizes.
#
#   scripts/run-ce-extension.sh [stride] [pattern]
#
# Expects a database already loaded by scripts/run-duckdb-ce.sh. Only queries
# whose tables are present in it are run; the rest are skipped rather than
# counted as failures.
set -euo pipefail
cd "$(dirname "$0")/.."

FACTDB="${FACTDB:-$(cd ../FactDB && pwd)}"
QUERIES="${QUERIES:-${FACTDB}/bench/ce/queries}"
DUCKDB="${DUCKDB:-build/relassert/duckdb}"
WORK="${TMPDIR:-/tmp}/factorize-duckdb"
DB="${DB:-$WORK/ce.db}"
STRIDE="${1:-20}"
PATTERN="${2:-_acyclic}"

[ -x "$DUCKDB" ] || { echo "no duckdb binary at $DUCKDB -- run scripts/wsl-build.sh" >&2; exit 2; }
[ -s "$DB" ] || { echo "no database at $DB -- run scripts/run-duckdb-ce.sh first" >&2; exit 2; }

mkdir -p tmp
python3 scripts/ce-to-factorized-count.py "$QUERIES" --pattern "$PATTERN" --stride "$STRIDE" \
    > tmp/ce_factorized_count.sql

"$DUCKDB" "$DB" -csv -c "select table_name from duckdb_tables();" | tail -n +2 > tmp/ce_loaded_tables.txt

# Keep only queries every one of whose tables exists. The table names are the
# entries of the *first* list argument -- matching quoted strings anywhere in the
# line would also catch the query name, which looks like an identifier and is
# never a table, and would drop every query.
python3 - <<'PY' > tmp/ce_factorized_count_filtered.sql
import re, sys

loaded = {line.strip() for line in open("tmp/ce_loaded_tables.txt") if line.strip()}
tables_list = re.compile(r"factorized_count\(\[(.*?)\], \[")
kept = dropped = 0
for line in open("tmp/ce_factorized_count.sql"):
    if not line.startswith("SELECT '"):
        continue
    match = tables_list.search(line)
    if not match:
        dropped += 1
        continue
    # "'a', 'b x'" -> ["a", "b"]: an alias is dropped, the table name is first.
    names = [entry.strip().strip("'").split()[0] for entry in match.group(1).split(",")]
    if names and all(name in loaded for name in names):
        print(line, end="")
        kept += 1
    else:
        dropped += 1
print(f"-- {kept} runnable, {dropped} skipped (tables not loaded)", file=sys.stderr)
PY

echo "== running factorized_count over the CE corpus ==" >&2
# The local build links factorize into the binary, so there is nothing to LOAD.
"$DUCKDB" "$DB" -csv -c ".read tmp/ce_factorized_count_filtered.sql" \
    > tmp/ce_extension_results.csv 2>tmp/ce_extension_errors.txt || {
    echo "duckdb failed; see tmp/ce_extension_errors.txt" >&2
    tail -5 tmp/ce_extension_errors.txt >&2
    exit 1
}

python3 - <<'PY'
import csv

rows = [r for r in csv.reader(open("tmp/ce_extension_results.csv")) if len(r) == 3 and r[0] != "query"]
ok = [r for r in rows if r[1] == r[2]]
bad = [r for r in rows if r[1] != r[2]]
print(f"{len(rows)} queries: {len(ok)} match the published result size, {len(bad)} do not")
for r in bad[:10]:
    print(f"  MISMATCH {r[0]}: expected {r[1]}, got {r[2]}")
raise SystemExit(1 if bad else 0)
PY
