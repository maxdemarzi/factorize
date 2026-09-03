#!/usr/bin/env python3
"""Rewrite CE benchmark queries as factorized_count() calls.

Phase 2's exit criterion is that factorized_count() agrees with both the
corpus's published result size and stock DuckDB's count(*), on 200+ queries.
This emits a SQL script that checks the first of those and prints a row per
query, so a diff against the published sizes is the whole test.

The rewrite is mechanical because CE queries are all the same shape:

    select count(*) from a, b where a.s = b.s;
        ->
    select 'name', <expected>, (select * from factorized_count(['a','b'],
                                                               ['a.s = b.s']));

Aliases are carried through verbatim -- "yago2 yago2_2" stays one list entry --
because factorized_count parses them the same way SQL does, and self-joins are
the only thing distinguishing two relations over one table.
"""

import argparse
import glob
import os
import re
import sys

NAME = re.compile(r"^(?:--)?\\set queryname (.+)$")
SIZE = re.compile(r"^--\s*Result size:\s*(-?\d+)")


def parse(path):
    """Yields (name, expected, tables, predicates) per live query in a file."""
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
        if not line.startswith("select "):
            continue
        try:
            frm = line.index(" from ")
            where = line.index(" where ")
        except ValueError:
            continue
        tables = [t.strip() for t in line[frm + 6 : where].split(",") if t.strip()]
        predicates = [p.strip() for p in line[where + 7 :].rstrip(";").split(" and ")]
        if name and tables and predicates:
            yield name, expected, tables, predicates


def quote(values):
    return "[" + ", ".join("'" + v.replace("'", "''") + "'" for v in values) + "]"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("queries", help="directory of CE .sql files")
    parser.add_argument("--pattern", default="_acyclic", help="substring a filename must contain")
    parser.add_argument("--limit", type=int, default=0, help="stop after N queries")
    parser.add_argument("--stride", type=int, default=1, help="keep every Nth query")
    args = parser.parse_args()

    emitted = 0
    seen = 0
    for path in sorted(glob.glob(os.path.join(args.queries, "*.sql"))):
        if args.pattern not in os.path.basename(path):
            continue
        for name, expected, tables, predicates in parse(path):
            seen += 1
            if (seen - 1) % args.stride:
                continue
            if args.limit and emitted >= args.limit:
                print(f"-- {emitted} queries emitted", file=sys.stderr)
                return
            # Every predicate must be a plain equality for the rewrite to be
            # faithful; anything else is skipped rather than silently mangled.
            if any("=" not in p or ">" in p or "<" in p for p in predicates):
                continue
            print(
                f"SELECT '{name}' AS query, {expected} AS expected, "
                f"(SELECT * FROM factorized_count({quote(tables)}, {quote(predicates)})) AS got;"
            )
            emitted += 1
    print(f"-- {emitted} queries emitted", file=sys.stderr)


if __name__ == "__main__":
    main()
