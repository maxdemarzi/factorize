#!/usr/bin/env python3
"""Decide alpha-acyclicity of CE queries by GYO reduction.

The corpus labels queries cyclic or acyclic in their filenames, and this
project has trusted that label without ever checking it. D28 already found it
is not reliable in one direction -- "25 of the 132 are acyclic, a doubled edge
between two relations is not a cycle" -- so a measurement that turns on the
label needs the label verified.

The join hypergraph: union-find over (relation, column) pairs merged by each
equality predicate gives the variables; each relation is a hyperedge over the
variables it mentions. GYO then repeatedly removes an ear -- a vertex in only
one hyperedge, or a hyperedge contained in another -- and the query is
alpha-acyclic exactly when nothing is left.

    scripts/classify-acyclicity.py tmp/cyclic/cyclic_sql_runnable.psv
    scripts/classify-acyclicity.py --only dblp_cyclic_q2_07 ... < list
"""

import argparse
import re
import sys

FROM = re.compile(r"\sfrom\s(.*?)\swhere\s", re.I)


def parse(sql):
    """-> (list of relation aliases, list of (alias, col, alias, col))."""
    body = FROM.search(sql)
    if not body:
        return None, None
    aliases = []
    for item in body.group(1).split(","):
        parts = item.strip().split()
        aliases.append(parts[-1])  # "dblp26 dblp26_0" -> alias; "dblp1" -> itself
    where = sql[body.end():]
    predicates = []
    for term in re.split(r"\sand\s", where, flags=re.I):
        match = re.match(r"\s*(\w+)\.(\w+)\s*=\s*(\w+)\.(\w+)\s*$", term.strip().rstrip(";"))
        if not match:
            return aliases, None  # anything that is not a plain equijoin
        predicates.append(match.groups())
    return aliases, predicates


def acyclic(aliases, predicates):
    parent = {}

    def find(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    for la, lc, ra, rc in predicates:
        union((la, lc), (ra, rc))

    # Hyperedges over join variables only. A column touched by no predicate
    # sits in one edge and GYO would strip it on the first pass regardless.
    edges = {a: set() for a in aliases}
    for la, lc, ra, rc in predicates:
        edges[la].add(find((la, lc)))
        edges[ra].add(find((ra, rc)))
    edges = [e for e in edges.values() if e]

    changed = True
    while changed and edges:
        changed = False
        # Ear 1: a vertex in exactly one hyperedge is removed from it.
        counts = {}
        for e in edges:
            for v in e:
                counts[v] = counts.get(v, 0) + 1
        for e in edges:
            lone = {v for v in e if counts[v] == 1}
            if lone:
                e -= lone
                changed = True
        # Ear 2: a hyperedge contained in another is dropped.
        edges = [e for e in edges if e]
        for i, e in enumerate(edges):
            if any(i != j and e <= f for j, f in enumerate(edges)):
                del edges[i]
                changed = True
                break
    return not edges


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("psv", help="name|expected|sql per line")
    args = parser.parse_args()
    counts = {"acyclic": 0, "cyclic": 0, "skipped": 0}
    for line in open(args.psv, encoding="utf-8"):
        parts = line.rstrip("\n").split("|")
        if len(parts) != 3:
            continue
        name, _, sql = parts
        aliases, predicates = parse(sql)
        if not aliases or not predicates:
            counts["skipped"] += 1
            print(f"{name}\tskipped")
            continue
        verdict = "acyclic" if acyclic(aliases, predicates) else "cyclic"
        counts[verdict] += 1
        print(f"{name}\t{verdict}")
    print(f"-- {counts}", file=sys.stderr)


if __name__ == "__main__":
    main()
