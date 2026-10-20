#!/usr/bin/env python3
"""Is the estimator's error a property of the join shape rather than the tables?

D75 built runtime calibration keyed on tables and measured it doing nothing:
a per-table mean removes 20-33% of the variance in log(actual/predicted), the
residual spread is still x7, and a leave-one-out replay over the excluded
corpus changes four decisions that cancel exactly. It named the next attempt
in its own closing paragraph -- "keyed on the join shape rather than the
tables" -- and left the plumbing in place for it.

This tests that, before anything is built. Two things differ from D75's setup:

  * Ground truth comes from the corpus's published result sizes, not from
    queries this engine finished. D75 could only learn from what completed,
    which is the small end of the range; the published sizes cover queries
    nobody can run, including the 592 over 1e9 where D104 found the estimator
    1,449x low. That is the part of the range the gate's mistakes live in and
    the part a finished-query store can never see.
  * Every candidate key is scored leave-one-out, so a query never benefits
    from its own observation, which is the only way the number means anything.

    scripts/shape-error-model.py tmp/shape/predicted.csv tmp/shape/acyclic_runnable.psv
"""

import argparse
import collections
import csv
import math
import re
import statistics
import sys


def parse_shape(sql):
    """-> (relation count, class degrees, relation degrees) for one query."""
    body = re.search(r"\sfrom\s(.*?)\swhere\s", sql, re.I)
    if not body:
        return None
    aliases = [t.strip().split()[-1] for t in body.group(1).split(",")]
    predicates = []
    for term in re.split(r"\sand\s", sql[body.end():], flags=re.I):
        match = re.match(r"\s*(\w+)\.(\w+)\s*=\s*(\w+)\.(\w+)\s*$", term.strip().rstrip(";"))
        if not match:
            return None
        predicates.append(match.groups())

    parent = {}

    def find(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for la, lc, ra, rc in predicates:
        a, b = find((la, lc)), find((ra, rc))
        if a != b:
            parent[a] = b

    members = collections.defaultdict(set)
    for key in list(parent):
        members[find(key)].add(key)
    # A class's degree is how many distinct relations it touches; a relation's
    # degree is how many classes it sits in. Between them they separate a star
    # from a chain from a bushy tree without naming a single table.
    class_degrees = sorted(len({alias for alias, _ in group}) for group in members.values())
    relation_degrees = collections.Counter()
    for root, group in members.items():
        for alias, _ in group:
            relation_degrees[alias] += 1
    return len(aliases), tuple(class_degrees), tuple(sorted(relation_degrees.values()))


def keys_for(name, sql, shape, magnitude):
    """Every candidate grouping, coarsest comparison first."""
    dataset = name.split("_")[0]
    tables = tuple(sorted({t.strip().split()[0] for t in
                           re.search(r"\sfrom\s(.*?)\swhere\s", sql, re.I).group(1).split(",")}))
    rels, class_degrees, relation_degrees = shape
    return {
        "global (one correction for everything)": "*",
        "dataset": dataset,
        "table set (D75's key)": tables,
        "relation count": rels,
        "class degrees": class_degrees,
        "class + relation degrees": (class_degrees, relation_degrees),
        "shape within a dataset": (dataset, class_degrees, relation_degrees),
        # Is the error a function of how big the estimate already is? If the
        # estimator degrades with magnitude, that is a feature it has at plan
        # time and need learn nothing to use.
        "predicted magnitude (decade)": ("mag", magnitude),
        "dataset x magnitude": (dataset, magnitude),
        "dataset x relations": (dataset, rels),
        "dataset x shape x magnitude": (dataset, class_degrees, relation_degrees, magnitude),
    }


def leave_one_out_per_table(points, min_obs):
    """D75's actual model, reproduced: a running mean of log(actual/predicted)
    per *table*, combined across the tables a query touches by their mean --
    the geometric mean of the corrections, since these are already logs.

    Grouping on the whole table set is not the same thing and is far weaker
    here: 2021 CE queries have almost 2021 distinct table sets, so nearly
    every group holds one query and leave-one-out can correct nothing. Per
    table, a table seen in fifty queries informs the fifty-first.
    """
    totals = collections.defaultdict(float)
    counts = collections.Counter()
    for _, error, _, tables in points:
        for table in tables:
            totals[table] += error
            counts[table] += 1
    residuals = []
    for _, error, _, tables in points:
        corrections = []
        for table in tables:
            if counts[table] - 1 < min_obs:
                continue
            corrections.append((totals[table] - error) / (counts[table] - 1))
        residuals.append(error - (sum(corrections) / len(corrections) if corrections else 0.0))
    return residuals


def leave_one_out(observations, min_obs):
    """Variance of the residual after each point is corrected by its group,
    with that point excluded from its own group's mean."""
    groups = collections.defaultdict(list)
    for key, error in observations:
        groups[key].append(error)
    residuals = []
    for key, error in observations:
        others = groups[key]
        if len(others) - 1 < min_obs:
            residuals.append(error)  # not enough history: no correction applied
            continue
        mean_without = (sum(others) - error) / (len(others) - 1)
        residuals.append(error - mean_without)
    return residuals


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("predicted")
    parser.add_argument("psv")
    parser.add_argument("--min-obs", type=int, default=3)
    args = parser.parse_args()

    actual, sql_of = {}, {}
    for line in open(args.psv, encoding="utf-8"):
        parts = line.rstrip("\n").split("|")
        if len(parts) == 3:
            actual[parts[0]] = int(parts[1])
            sql_of[parts[0]] = parts[2].strip()

    points = []
    skipped = 0
    for row in csv.DictReader(open(args.predicted)):
        name = row["query"]
        try:
            predicted = float(row["pred_flat"])
        except (ValueError, KeyError):
            skipped += 1
            continue
        truth = actual.get(name)
        if not truth or predicted <= 0:
            skipped += 1
            continue
        shape = parse_shape(sql_of[name])
        if shape is None:
            skipped += 1
            continue
        tables = sorted({t.strip().split()[0] for t in
                         re.search(r"\sfrom\s(.*?)\swhere\s", sql_of[name], re.I).group(1).split(",")})
        points.append((name, math.log(truth / predicted), keys_for(name, sql_of[name], shape, int(math.log10(max(predicted, 1)))), tables))

    if not points:
        print("no usable points", file=sys.stderr)
        return 1
    errors = [p[1] for p in points]
    base = statistics.pvariance(errors)
    print(f"{len(points)} queries ({skipped} skipped)\n")
    print(f"raw error log(actual/predicted): mean {statistics.mean(errors):+.2f} "
          f"(x{math.exp(statistics.mean(errors)):.2f}), sd {math.sqrt(base):.2f} "
          f"(x{math.exp(math.sqrt(base)):.1f})\n")
    print(f"{'key':40} {'groups':>7} {'LOO var explained':>18} {'residual spread':>16}")
    residuals = leave_one_out_per_table(points, args.min_obs)
    explained = 1 - statistics.pvariance(residuals) / base
    spread = math.exp(math.sqrt(statistics.pvariance(residuals)))
    print(f"{'per table (D75 reproduced)':40} {len({t for p in points for t in p[3]}):7} "
          f"{100 * explained:17.0f}% {'x' + format(spread, '.1f'):>16}")
    for label in next(iter(points))[2]:
        observations = [(p[2][label], p[1]) for p in points]
        residuals = leave_one_out(observations, args.min_obs)
        explained = 1 - statistics.pvariance(residuals) / base
        spread = math.exp(math.sqrt(statistics.pvariance(residuals)))
        groups = len({o[0] for o in observations})
        print(f"{label:40} {groups:7} {100 * explained:17.0f}% {'x' + format(spread, '.1f'):>16}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
