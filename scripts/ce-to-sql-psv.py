#!/usr/bin/env python3
"""Emit `name|expected|sql` for CE benchmark queries, one per line.

`tmp/ce_runnable_sql.psv` -- the list almost every measurement script reads --
had no generator in the repo; it was made by hand and the cyclic half of the
corpus was never converted at all. D96 recorded C (cyclic queries) as
unmeasurable because "the FactDB query files are not checked out here". They
are: ../FactDB/bench/ce/queries holds 88 acyclic files and 39 cyclic ones,
760 cyclic queries between them (2590 with --include-disabled). Only the data is a download (D12).

Unlike ce-to-factorized-count.py, which rewrites each query as a
factorized_count() table function, this keeps the query as ordinary SQL. That
is the difference between testing the engine and testing the product: an
`auto` vs `off` comparison exercises the optimizer rule, the gate and the
fallback, which is what a user actually meets.

    scripts/ce-to-sql-psv.py ../FactDB/bench/ce/queries --pattern _cyclic
"""

import argparse
import glob
import os
import re
import sys

NAME = re.compile(r"^(?:--)?[\\]set queryname (.+)$")
SIZE = re.compile(r"^--\s*Result size:\s*(-?\d+)")


def parse(path, include_disabled=False):
    """Yields (name, expected, sql) per live query in a CE query file."""
    name = None
    expected = None
    for raw in open(path, errors="replace"):
        line = raw.rstrip()
        match = NAME.match(line)
        if match:
            name = match.group(1).strip()
            continue
        match = SIZE.match(line)
        if match:
            expected = int(match.group(1))
            continue
        if line.startswith("--select ") and include_disabled:
            line = line[2:]
        if not line.startswith("select "):
            continue
        if name and expected is not None:
            yield name, expected, line.rstrip(";")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("queries", help="directory of CE .sql files")
    parser.add_argument("--pattern", default="_acyclic", help="substring a filename must contain")
    parser.add_argument("--agg", default="", help="replace count(*) with this aggregate")
    parser.add_argument(
        "--include-disabled",
        action="store_true",
        help="also take queries the benchmark comments out -- 601 acyclic and 417 cyclic. They are "
        "commented out because they are too large to run, which makes them the part of the range "
        "where the estimator is least tested and D104's 1,449x underestimate lives. EXPLAIN still "
        "prices them and the published size is still the truth, so they can be measured without "
        "being run.",
    )
    args = parser.parse_args()

    emitted = 0
    for path in sorted(glob.glob(os.path.join(args.queries, "*.sql"))):
        if args.pattern not in os.path.basename(path):
            continue
        for name, expected, sql in parse(path, args.include_disabled):
            # A pipe inside a query would split the record and silently
            # truncate it downstream; CE has none, so assert rather than quote.
            assert "|" not in sql, f"{name}: query contains a pipe"
            if args.agg:
                sql = sql.replace("count(*)", args.agg, 1)
            print(f"{name}|{expected}|{sql}")
            emitted += 1
    print(f"-- {emitted} queries emitted", file=sys.stderr)


if __name__ == "__main__":
    main()
