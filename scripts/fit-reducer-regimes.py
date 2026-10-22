#!/usr/bin/env python3
"""Fit the reducer's two per-row rates separately, one per regime.

    scripts/fit-reducer-regimes.py tmp/shape/reducer_cost_threaded.csv

Since D118 a relation at or above PARALLEL_ROW_FLOOR is reduced on every core
and one below it is not, so a single `per_input_row_ms` cannot describe both.
This splits each query's input rows by that floor and does a least squares
through the origin on the two totals -- no intercept, because the fitted one
is DuckDB's process start and not a cost this engine pays (D109).

**The sample `fit-reducer-cost.sh` produces cannot support this fit**, and the
script says so rather than being quietly wrong: 98.5% of its rows are above the
floor, so the sequential rate comes back as roughly the per-query fixed cost
divided by the few sequential rows there are -- 8.96e-4 ms/row against the
3.99e-5 the same corpus measured before threading. A trustworthy sequential
rate needs the reducer timed over the runnable corpus, whose relations are
smaller. See D119.
"""
import csv
import re
import sys

FLOOR = 1 << 16
ROWS = "tmp/shape/table_rows.psv"
QUERIES = "tmp/excluded_agg/agg_min.psv"


def table_rows():
    rows = {}
    for line in open(ROWS):
        name, _, count = line.strip().partition("|")
        if count:
            rows[name] = int(count)
    return rows


def split(query, rows):
    """Input rows of one query, divided by the floor the engine divides on."""
    named = re.search(r"\sFROM\s+(.*?)\s+WHERE\s", query, re.I)
    if not named:
        return None
    sequential = parallel = 0
    for table in named.group(1).split(","):
        table = table.strip().split()[0]
        if table not in rows:
            return None
        if rows[table] >= FLOOR:
            parallel += rows[table]
        else:
            sequential += rows[table]
    return sequential, parallel


def main():
    rows = table_rows()
    sql = {}
    for line in open(QUERIES):
        parts = line.rstrip("\n").split("|")
        if len(parts) >= 3:
            sql[parts[0]] = parts[2]

    points = []
    for measured in csv.DictReader(open(sys.argv[1])):
        divided = split(sql.get(measured["query"], ""), rows)
        if divided is None:
            continue
        points.append((divided[0], divided[1], float(measured["reduced_s"]) * 1000.0))
    if len(points) < 2:
        print("not enough measurements to fit")
        return 1

    sxx = sum(a * a for a, _, _ in points)
    sxz = sum(a * b for a, b, _ in points)
    szz = sum(b * b for _, b, _ in points)
    sxy = sum(a * y for a, _, y in points)
    szy = sum(b * y for _, b, y in points)
    determinant = sxx * szz - sxz * sxz
    sequential_rate = (sxy * szz - szy * sxz) / determinant
    parallel_rate = (szy * sxx - sxy * sxz) / determinant

    total_sequential = sum(a for a, _, _ in points)
    total_parallel = sum(b for _, b, _ in points)
    share = 100.0 * total_parallel / (total_sequential + total_parallel)
    errors = sorted(
        abs(sequential_rate * a + parallel_rate * b - y) / y for a, b, y in points if y > 0
    )
    print("%d queries, %.3g sequential rows and %.3g parallel (%.1f%% above the floor)"
          % (len(points), total_sequential, total_parallel, share))
    print("sequential %.6g ms/row, parallel %.6g ms/row, median relative error %.1f%%"
          % (sequential_rate, parallel_rate, 100 * errors[len(errors) // 2]))
    if share > 90:
        print("the sequential rate here is the per-query fixed cost, not a rate: "
              "fit it on a corpus with relations under the floor (D119)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
