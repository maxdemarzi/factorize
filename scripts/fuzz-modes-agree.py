#!/usr/bin/env python3
"""Assert that factorize_mode changes speed and nothing else.

    scripts/fuzz-modes-agree.py [iterations] [seed]

Generates random join graphs over random small tables and checks that 'off',
'force' and 'auto' all return the same count. That is the property users
actually care about, and it is the one a matcher bug breaks: every bug found in
this rule so far has been a shape that was accepted when it should have been
declined, or declined when it should have been accepted, and both show up here
as a number that disagrees with stock DuckDB.

Small tables on purpose. The interesting cases are structural -- an empty
relation, a single row, a self-join, a duplicate predicate, a cycle, a NULL key,
a filter that removes everything -- and none of them need volume to be wrong.
The plan (§7.2) asks for exactly these degenerate shapes.
"""

import random
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DUCKDB = ROOT / "build" / "release" / "duckdb"
if not DUCKDB.exists():
    DUCKDB = ROOT / "build" / "relassert" / "duckdb"

TYPES = ["INTEGER", "BIGINT", "SMALLINT", "TINYINT"]


def make_table(rng, name):
    """A table of one or two integer columns, sometimes empty, sometimes NULL-ridden."""
    rows = rng.choice([0, 1, 2, 3, 5, 8, 13])
    columns = rng.choice([1, 2])
    # A narrow domain makes joins actually match, and makes duplicates common,
    # which is where multiplicity bugs live.
    domain = rng.choice([1, 2, 3, 5])
    column_type = rng.choice(TYPES)
    names = ["c0", "c1"][:columns]
    if rows == 0:
        return f"CREATE TABLE {name} ({', '.join(f'{c} {column_type}' for c in names)});", names
    values = []
    for _ in range(rows):
        cells = []
        for _ in range(columns):
            # NULLs are the case that desynchronised a relation's columns once.
            cells.append("NULL" if rng.random() < 0.15 else str(rng.randrange(domain)))
        values.append("(" + ", ".join(cells) + ")")
    # Name the VALUES columns rather than relying on the generated names, which
    # differ between DuckDB versions and silently made an earlier version of
    # this script generate tables that never got created -- a fuzzer that tests
    # nothing reports no failures, which reads exactly like success.
    sources = [f"v{i}" for i in range(columns)]
    casts = ", ".join(f"CAST({s} AS {column_type}) AS {c}" for s, c in zip(sources, names))
    return (
        f"CREATE TABLE {name} AS SELECT {casts} "
        f"FROM (VALUES {', '.join(values)}) AS src({', '.join(sources)});",
        names,
    )


def make_query(rng, tables):
    """A join over a random subset, with random edges and maybe a filter."""
    count = rng.randrange(1, min(4, len(tables)) + 1)
    chosen = rng.sample(tables, count)
    # A self-join is two aliases over one table, and is worth generating often:
    # relation identity is per-scan, not per-table, and getting that wrong is
    # invisible until two scans of one table disagree.
    if rng.random() < 0.3:
        chosen.append(rng.choice(chosen))
    aliases = [(f"t{i}", name, cols) for i, (name, cols) in enumerate(chosen)]
    froms = ", ".join(f"{name} {alias}" for alias, name, _ in aliases)

    predicates = []
    for i in range(1, len(aliases)):
        # Attach to any earlier relation, which produces stars, chains and
        # everything between; occasionally attach twice, which produces cycles.
        left_alias, _, left_cols = aliases[rng.randrange(i)]
        right_alias, _, right_cols = aliases[i]
        predicates.append(f"{left_alias}.{rng.choice(left_cols)} = {right_alias}.{rng.choice(right_cols)}")
    if len(aliases) > 2 and rng.random() < 0.35:
        a, _, a_cols = rng.choice(aliases)
        b, _, b_cols = rng.choice(aliases)
        if a != b:
            predicates.append(f"{a}.{rng.choice(a_cols)} = {b}.{rng.choice(b_cols)}")
    if not predicates:
        # A single relation, or a product: both must be declined, and both must
        # still answer.
        predicates.append("1 = 1")

    if rng.random() < 0.4:
        alias, _, cols = rng.choice(aliases)
        column = f"{alias}.{rng.choice(cols)}"
        predicates.append(
            rng.choice(
                [
                    f"{column} = {rng.randrange(3)}",
                    f"{column} > {rng.randrange(3)}",
                    f"{column} BETWEEN 0 AND {rng.randrange(3)}",
                    f"{column} IS NOT NULL",
                    f"{column} IS NULL",
                    f"({column} = 0 OR {column} = 1)",
                ]
            )
        )
    return f"SELECT count(*) FROM {froms} WHERE {' AND '.join(predicates)};"


def run(script):
    result = subprocess.run(
        [str(DUCKDB), "-noheader", "-list", "-c", script],
        capture_output=True,
        text=True,
        timeout=120,
    )
    return result.stdout.strip().splitlines(), result.stderr.strip()


def main():
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    rng = random.Random(seed)
    if not DUCKDB.exists():
        print(f"no duckdb binary at {DUCKDB}", file=sys.stderr)
        return 2

    failures = 0
    for iteration in range(iterations):
        setup = []
        tables = []
        for t in range(rng.randrange(2, 5)):
            name = f"f{t}"
            ddl, cols = make_table(rng, name)
            setup.append(ddl)
            tables.append((name, cols))
        query = make_query(rng, tables)

        script = "\n".join(setup) + "\n"
        for mode in ("off", "force", "auto"):
            script += f"SET factorize_mode='{mode}';\n{query}\n"

        lines, error = run(script)
        if len(lines) != 3:
            failures += 1
            print(f"-- iteration {iteration}: expected three answers, got {lines} {error}")
            print(script)
            continue
        if lines[0] != lines[1] or lines[0] != lines[2]:
            failures += 1
            print(f"-- iteration {iteration}: off={lines[0]} force={lines[1]} auto={lines[2]}")
            print(script)

    print(f"{iterations} random queries, {failures} disagreements", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
