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
# An explicit limit, not DuckDB's default of 80% of RAM: a cap only protects the
# session if it leaves room for the rest of the machine.
MEMORY_LIMIT="${MEMORY_LIMIT:-4GB}"
TIMEOUT="${TIMEOUT:-300}"

[ -x "$DUCKDB" ] || { echo "no duckdb binary at $DUCKDB -- run scripts/wsl-build.sh" >&2; exit 2; }
[ -s "$DB" ] || { echo "no database at $DB -- run scripts/run-duckdb-ce.sh first" >&2; exit 2; }

mkdir -p tmp
python3 scripts/ce-to-factorized-count.py "$QUERIES" --pattern "$PATTERN" --stride "$STRIDE" \
    > tmp/ce_factorized_count.sql

"$DUCKDB" "$DB" -csv -c "select table_name from duckdb_tables();" | tail -n +2 > tmp/ce_loaded_tables.txt

# Keep only queries every one of whose tables exists, and split each into
# name/expected/sql so the runner can report per query. Table names are the
# entries of the *first* list argument -- matching quoted strings anywhere would
# also catch the query name, which looks like an identifier but is never a table.
python3 - <<'PY' > tmp/ce_runnable.psv
import re, sys

loaded = {line.strip() for line in open("tmp/ce_loaded_tables.txt") if line.strip()}
header = re.compile(r"^SELECT '([^']+)' AS query, (-?\d+) AS expected, \((.*)\) AS got;$")
tables_list = re.compile(r"factorized_count\(\[(.*?)\], \[")
kept = dropped = 0
for line in open("tmp/ce_factorized_count.sql"):
    match = header.match(line.strip())
    if not match:
        continue
    name, expected, inner = match.groups()
    tables = tables_list.search(inner)
    if not tables:
        dropped += 1
        continue
    # "'a', 'b x'" -> ["a", "b"]: an alias is dropped, the table name is first.
    names = [entry.strip().strip("'").split()[0] for entry in tables.group(1).split(",")]
    if names and all(n in loaded for n in names):
        print(f"{name}|{expected}|{inner};")
        kept += 1
    else:
        dropped += 1
print(f"{kept} runnable, {dropped} skipped (tables not loaded)", file=sys.stderr)
PY

# One query per invocation. `.read` aborts the whole script at the first error,
# and an error here is an outcome to record -- a query declining because its
# f-representation will not fit is expected and safe, and must not stop the run.
echo "query,expected,got,status" > tmp/ce_extension_results.csv
while IFS='|' read -r name expected sql; do
    [ -n "$name" ] || continue
    if out=$(timeout "$TIMEOUT" "$DUCKDB" "$DB" -noheader -list \
                 -c "SET memory_limit = '${MEMORY_LIMIT}';" -c "$sql" 2>tmp/ce_one_error.txt); then
        got=$(echo "$out" | tr -d '[:space:]')
        status=$([ "$got" = "$expected" ] && echo ok || echo MISMATCH)
        printf '%s,%s,%s,%s\n' "$name" "$expected" "$got" "$status" >> tmp/ce_extension_results.csv
    else
        reason=$(tr '\n' ' ' < tmp/ce_one_error.txt | sed 's/,/;/g' | cut -c1-90)
        printf '%s,%s,,%s\n' "$name" "$expected" "declined: ${reason:-timeout}" >> tmp/ce_extension_results.csv
    fi
done < tmp/ce_runnable.psv

python3 - <<'PY'
import csv

rows = [r for r in csv.reader(open("tmp/ce_extension_results.csv")) if len(r) == 4 and r[0] != "query"]
ok = [r for r in rows if r[3] == "ok"]
bad = [r for r in rows if r[3] == "MISMATCH"]
declined = [r for r in rows if r[3].startswith("declined")]
print(f"{len(rows)} queries: {len(ok)} match the published size, {len(bad)} mismatch, {len(declined)} declined")
for r in bad[:10]:
    print(f"  MISMATCH {r[0]}: expected {r[1]}, got {r[2]}")
for r in declined[:5]:
    print(f"  declined {r[0]}: {r[3][10:]}")
raise SystemExit(1 if bad else 0)
PY
