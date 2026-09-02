#!/usr/bin/env bash
# Time stock DuckDB on the same CE queries, on this machine.
#
# The artifact publishes DuckDB numbers, but they come from a 64-core Xeon Gold
# 6430. Comparing this laptop's factorized engine against those would measure
# the hardware, not the idea, so stock DuckDB is re-run here.
#
# DuckDB is measured at threads=1 as well as at its default. The factorized
# engine is single-threaded until Phase 4, so threads=1 is the like-for-like
# comparison; the default is what a user would actually experience. Reporting
# only one of them would be picking the flattering number.
#
#   scripts/run-duckdb-ce.sh <output.csv> <shape> <dataset> [<dataset>...]
set -euo pipefail
cd "$(dirname "$0")/.."

FACTDB="${FACTDB:-$(cd ../FactDB && pwd)}"
DATA="${FACTDB}/bench/data/ce"
QUERIES="${FACTDB}/bench/ce/queries"
DUCKDB="${DUCKDB:-build/relassert/duckdb}"
OUT="${1:?output csv}"
SHAPE="${2:?acyclic|cyclic}"
shift 2
DATASETS=("$@")
WARMUPS="${WARMUPS:-1}"
RUNS="${RUNS:-3}"
# Timing every CE query through DuckDB costs hours, and a go/no-go decision does
# not need it: SAMPLE=n keeps every n-th query, which is a systematic sample
# across each file rather than a prefix, so it stays representative of the
# runtime spread instead of favouring whichever queries happen to come first.
SAMPLE="${SAMPLE:-1}"

[ -x "$DUCKDB" ] || { echo "no duckdb binary at $DUCKDB" >&2; exit 2; }

WORK="${TMPDIR:-/tmp}/factorize-duckdb"
mkdir -p "$WORK"
DB="$WORK/ce.db"

# Load only the tables the selected datasets need; the full corpus is 5.3 GB.
if [ ! -s "$DB" ]; then
    echo "loading datasets: ${DATASETS[*]}" >&2
    {
        for d in "${DATASETS[@]}"; do
            for csv in "$DATA/$d"*.csv; do
                t=$(basename "$csv" .csv)
                echo "create table $t (s int not null, d int not null);"
                echo "copy $t from '$csv' with (delimiter ',');"
            done
        done
    } > "$WORK/load.sql"
    "$DUCKDB" "$DB" < "$WORK/load.sql" > /dev/null
    echo "loaded $(grep -c '^create table' "$WORK/load.sql") tables" >&2
fi

# Emit one timed statement per query, at each thread setting.
python3 - "$QUERIES" "$SHAPE" "$WARMUPS" "$RUNS" "$SAMPLE" "${DATASETS[@]}" > "$WORK/bench.sql" <<'PY'
import os, sys, re
queries, shape, warmups, runs = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
sample = max(1, int(sys.argv[5]))
datasets = sys.argv[6:]
print(".timer on")
for threads in (1, 0):
    # Reset per pass, so both thread settings time the *same* sampled queries.
    # Letting it run on would sample disjoint sets and make the two columns
    # incomparable.
    seen = 0
    if threads:
        print(f"SET threads={threads};")
    else:
        print("RESET threads;")
    for name in sorted(os.listdir(queries)):
        if not any(name.startswith(d + "_" + shape) for d in datasets):
            continue
        pending = None
        for line in open(os.path.join(queries, name), errors="replace"):
            line = line.rstrip("\r\n")
            m = re.match(r"^\\set queryname (\S+)", line)
            if m:
                pending = m.group(1)
                continue
            if not line.startswith("select ") or pending is None:
                continue
            seen += 1
            if (seen - 1) % sample != 0:
                pending = None
                continue
            for i in range(warmups + runs):
                tag = "warmup" if i < warmups else "run"
                # A dot-command, so the marker itself is not timed and
                # cannot be mistaken for the query's own Run Time.
                print(f".print MARK {pending} threads={threads} {tag}")
                print(line)
            pending = None
PY

echo "timing $(grep -c '^\.print MARK' "$WORK/bench.sql") statements" >&2
"$DUCKDB" "$DB" < "$WORK/bench.sql" > "$WORK/bench.out" 2>&1 || true

# The CLI prints "Run Time (s): real X ..." after each statement.
python3 - "$WORK/bench.out" > "$OUT" <<'PY'
import re, sys, statistics
from collections import defaultdict
times = defaultdict(list)
pending = None
for line in open(sys.argv[1], errors="replace"):
    m = re.match(r"^MARK (\S+) threads=(\d+) (\w+)", line)
    if m:
        pending = m.groups()
        continue
    m = re.search(r"Run Time \(s\): real ([0-9.]+)", line)
    if m and pending:
        name, threads, tag = pending
        if tag == "run":
            times[(name, threads)].append(float(m.group(1)) * 1000.0)
        pending = None
print("query,duckdb_1t_ms,duckdb_default_ms")
names = sorted({n for n, _ in times})
for name in names:
    one = times.get((name, "1"), [])
    dflt = times.get((name, "0"), [])
    f = lambda xs: "%.3f" % statistics.median(xs) if xs else ""
    print(f"{name},{f(one)},{f(dflt)}")
PY
echo "wrote $OUT" >&2
