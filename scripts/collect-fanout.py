#!/usr/bin/env python3
"""Measured fan-out per join, over a corpus.

    scripts/collect-fanout.py                      # tmp/fanout_runnable.csv
    scripts/collect-fanout.py --list tmp/x.psv --out tmp/y.csv

Why this exists. Five attempts to stop the gate firing where it loses have now
failed the same way (D71, D73, D75, D81, D82), and the capstone finding was
that the axis they all moved a threshold along *orders queries correctly* and
still cannot be cut, because rescues and losses are interleaved on it. The
conclusion was that progress needs a second axis rather than a better cut.

CIDR 2025's adaptive factorization names a candidate, and it is not another
plan-time feature: average chain length, read off the hash build at *runtime*,
was the single most predictive feature in their table (correlation 0.58 with
the faster plan). The equivalent here is fan-out -- live children per populated
parent slot -- which `factorized_steps` now reports per join.

This collects it. It decides nothing; it produces the file
scripts/fanout-outcome.py then tests for signal, so that the decision to wire a
runtime gate or to record a sixth refuted axis is made on measurement.

One honest limitation, stated here because the analysis cannot see it:
`factorized_steps` runs unsliced, uncapped and in BOTTOM_INSERT mode, which is
not exactly the path `auto` takes. Fan-out is a property of the representation
rather than of the schedule, so the numbers should carry, but "should" is doing
work and a disagreement here would be worth chasing rather than explaining.
"""

import argparse
import csv
import os
import re
import subprocess
import sys


def parse_query(sql):
    """(tables, predicates) from a CE-shaped `select count(*) from ... where ...`.

    The same mechanical rewrite scripts/ce-to-factorized-count.py does, and
    deliberately as strict: a query whose predicates are not all plain
    equalities is skipped rather than silently mangled into a different one.
    """
    line = sql.strip().rstrip(";")
    lowered = line.lower()
    try:
        frm = lowered.index(" from ")
        where = lowered.index(" where ")
    except ValueError:
        return None, None
    tables = [t.strip() for t in line[frm + 6 : where].split(",") if t.strip()]
    # Case-insensitively: ce-to-factorized-count.py reads the raw CE files,
    # which are lowercase, while tmp/ce_runnable_sql.psv has been uppercased
    # along the way. Splitting on a lowercase " and " there yields one giant
    # predicate that binds as a single malformed equality, and every query in
    # the corpus fails identically -- which is how this was found.
    predicates = [p.strip() for p in re.split(r"\s+and\s+", line[where + 7 :], flags=re.IGNORECASE)]
    if not tables or not predicates:
        return None, None
    if any("=" not in p or ">" in p or "<" in p for p in predicates):
        return None, None
    return tables, predicates


def quote(values):
    return "[" + ", ".join("'" + v.replace("'", "''") + "'" for v in values) + "]"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--list", default="tmp/ce_runnable_sql.psv")
    parser.add_argument("--out", default="tmp/fanout_runnable.csv")
    parser.add_argument("--bin", default=os.environ.get("BIN", "build/release/duckdb"))
    parser.add_argument("--db", default=os.environ.get("DB", "/tmp/factorize-duckdb/ce.db"))
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    rows, skipped, failed = [], [], []
    with open(args.list, errors="replace") as handle:
        queries = [line.rstrip("\n").split("|") for line in handle if line.strip()]

    for index, fields in enumerate(queries, 1):
        if len(fields) < 3:
            continue
        name, sql = fields[0], fields[2]
        tables, predicates = parse_query(sql)
        if not tables:
            skipped.append(name)
            continue
        statement = (
            f"SELECT step, live, tuples, compression, fanout_mean, fanout_max "
            f"FROM factorized_steps({quote(tables)}, {quote(predicates)}) ORDER BY step;"
        )
        try:
            done = subprocess.run(
                [args.bin, "-readonly", args.db, "-noheader", "-csv", "-c", statement],
                capture_output=True,
                text=True,
                timeout=args.timeout,
            )
        except subprocess.TimeoutExpired:
            failed.append((name, "timeout"))
            print(f"[{index}/{len(queries)}] {name}: timeout", file=sys.stderr)
            continue
        if done.returncode != 0:
            reason = (done.stderr or "").strip().splitlines()
            failed.append((name, reason[-1] if reason else "error"))
            print(f"[{index}/{len(queries)}] {name}: {failed[-1][1][:90]}", file=sys.stderr)
            continue
        steps = 0
        for line in done.stdout.splitlines():
            parts = line.split(",")
            if len(parts) != 6:
                continue
            rows.append([name] + parts)
            steps += 1
        print(f"[{index}/{len(queries)}] {name}: {steps} steps", file=sys.stderr)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["query", "step", "live", "tuples", "compression", "fanout_mean", "fanout_max"])
        writer.writerows(rows)

    measured = len({row[0] for row in rows})
    print(
        f"=== {measured} queries measured, {len(skipped)} not rewritable, "
        f"{len(failed)} failed -> {args.out}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
