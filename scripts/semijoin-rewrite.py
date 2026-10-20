#!/usr/bin/env python3
"""Rewrite `min(T.c)` over an acyclic join as a full reducer, for measurement.

D104 established that the gate is right to decline the 23 slowest queries of
the excluded corpus: forcing them is a wash, because this engine is no faster
than DuckDB there. Both are slow for the same reason, and it is not I/O --
scanning and counting distinct values over all five inputs of
job_acyclic_109_08 takes 0.5s against the query's 45s.

What neither does is exploit acyclicity. A MIN or MAX over an acyclic join
does not need the join: a full reducer (Yannakakis) leaves exactly the tuples
that participate in some full result, and the extreme over the reduced
relation is the extreme over the join. That is O(input), not O(output).

Rooting the join tree at the relation holding the aggregated column, the
reducer is a nest of EXISTS -- a tuple of the root survives exactly when it
extends to a full join tuple. This emits that nest so the opportunity can be
timed against the original before anything is built into the engine.

    scripts/semijoin-rewrite.py tmp/excluded_agg/agg_min.psv job_acyclic_109_08
"""

import argparse
import re
import sys


def parse(sql):
    agg = re.search(r"(min|max)\((\w+)\.(\w+)\)", sql, re.I)
    body = re.search(r"\sFROM\s(.*?)\sWHERE\s", sql, re.I)
    if not agg or not body:
        return None
    relations = []
    for item in body.group(1).split(","):
        parts = item.strip().split()
        relations.append((parts[0], parts[-1]))  # (table, alias)
    predicates = []
    for term in re.split(r"\sAND\s", sql[body.end():], flags=re.I):
        match = re.match(r"\s*(\w+)\.(\w+)\s*=\s*(\w+)\.(\w+)\s*$", term.strip().rstrip(";"))
        if not match:
            return None
        predicates.append(match.groups())
    return agg.group(1), agg.group(2), agg.group(3), relations, predicates


def rewrite(sql):
    parsed = parse(sql)
    if not parsed:
        return None
    fold, root_alias, column, relations, predicates = parsed
    table_of = dict((alias, table) for table, alias in relations)

    # Spanning tree by breadth-first attachment from the root. A predicate not
    # consumed by the tree closes a cycle, and the rewrite is only sound on an
    # acyclic query, so that case is refused rather than approximated.
    joined = {root_alias}
    used = [False] * len(predicates)
    children = dict((alias, []) for alias in table_of)
    order = [root_alias]
    while len(joined) < len(table_of):
        progressed = False
        for i, (la, lc, ra, rc) in enumerate(predicates):
            if used[i]:
                continue
            if (la in joined) != (ra in joined):
                parent, pcol, child, ccol = (la, lc, ra, rc) if la in joined else (ra, rc, la, lc)
                children[parent].append((child, ccol, pcol))
                joined.add(child)
                order.append(child)
                used[i] = True
                progressed = True
        if not progressed:
            return None
    if not all(used):
        return None  # a predicate left over means a cycle

    def nest(alias, depth):
        clauses = []
        for child, ccol, pcol in children[alias]:
            inner = nest(child, depth + 1)
            pad = "  " * depth
            clauses.append(
                f"EXISTS (SELECT 1 FROM {table_of[child]} {child} "
                f"WHERE {child}.{ccol} = {alias}.{pcol}{inner})"
            )
        if not clauses:
            return ""
        return " AND " + " AND ".join(clauses)

    where = nest(root_alias, 1)
    return (f"SELECT {fold}({root_alias}.{column}) FROM {table_of[root_alias]} {root_alias}"
            f" WHERE true{where}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("psv")
    parser.add_argument("names", nargs="*")
    args = parser.parse_args()
    wanted = set(args.names)
    for line in open(args.psv, encoding="utf-8", errors="replace"):
        parts = line.rstrip("\n").split("|")
        if len(parts) != 3:
            continue
        name, _, sql = parts
        if wanted and name not in wanted:
            continue
        out = rewrite(sql.strip())
        if out is None:
            print(f"{name}|SKIP|", file=sys.stderr)
            continue
        print(f"{name}|{out}")


if __name__ == "__main__":
    main()
