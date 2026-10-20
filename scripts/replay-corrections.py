#!/usr/bin/env python3
"""Does a shape-keyed correction change the gate's decisions for the better?

Variance explained is not the question. D75's test was the right one: replay
the decisions with the correction applied, leave-one-out, and count the
outcomes that change. A model can halve the residual and still move nothing,
because the decision turns on whether one predicted time clears another by
1.5x and most queries are nowhere near that line.

The gate fires when predicted DuckDB time beats ours by the margin. DuckDB's
predicted time is proportional to the predicted flat result, so multiplying
that estimate by exp(correction) scales `duckdb_ms` by the same factor -- an
approximation of re-running the gate, and the same one D75 replayed with.

Outcomes come from the measured corpus: firing is good when `force` beat
`off` and bad when it did not, so every changed decision can be scored
against what actually happened rather than against the estimate.

    scripts/replay-corrections.py tmp/shape/predicted.csv \
        tmp/shape/acyclic_runnable.psv tmp/headline_acyclicfix.csv
"""

import argparse
import collections
import csv
import math
import sys

sys.path.insert(0, "scripts")
shape_model = __import__("shape-error-model")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("predicted")
    parser.add_argument("psv")
    parser.add_argument("outcomes")
    parser.add_argument("--margin", type=float, default=1.5)
    parser.add_argument("--min-obs", type=int, default=3)
    parser.add_argument("--key", default="shape", choices=["shape", "magnitude", "table"],
                        help="which correction to replay; all three score about the same on "
                             "variance explained, which is exactly why variance explained is "
                             "not the test")
    args = parser.parse_args()

    actual, sql_of = {}, {}
    for line in open(args.psv, encoding="utf-8"):
        parts = line.rstrip("\n").split("|")
        if len(parts) == 3:
            actual[parts[0]], sql_of[parts[0]] = int(parts[1]), parts[2].strip()

    rows = {}
    for row in csv.DictReader(open(args.predicted)):
        try:
            flat = float(row["pred_flat"])
            rows[row["query"]] = (flat, float(row["pred_ours_ms"]), float(row["pred_duckdb_ms"]),
                                  row["fired"] == "1")
        except (ValueError, KeyError):
            continue

    # Corrections are learned over every query that has ground truth, and
    # applied leave-one-out, so a query never informs its own decision.
    totals, counts = collections.defaultdict(float), collections.Counter()
    error_of, key_of = {}, {}
    for name, (flat, _, _, _) in rows.items():
        truth = actual.get(name)
        if not truth or flat <= 0:
            continue
        shape = shape_model.parse_shape(sql_of[name])
        if shape is None:
            continue
        dataset = name.split("_")[0]
        if args.key == "shape":
            key = (dataset, shape[1], shape[2])
        elif args.key == "magnitude":
            key = (dataset, int(math.log10(max(flat, 1))))
        else:
            key = tuple(sorted({t.strip().split()[0] for t in
                                shape_model.re.search(r"\sfrom\s(.*?)\swhere\s",
                                                      sql_of[name], shape_model.re.I).group(1).split(",")}))
        error_of[name] = math.log(truth / flat)
        key_of[name] = key
        totals[key] += error_of[name]
        counts[key] += 1

    outcomes = {r["query"]: r for r in csv.DictReader(open(args.outcomes))}
    tally = collections.Counter()
    changes = []
    for name, row in outcomes.items():
        if name not in rows or name not in key_of:
            continue
        flat, ours, duckdb, fired_now = rows[name]
        key = key_of[name]
        if counts[key] - 1 < args.min_obs:
            continue
        correction = (totals[key] - error_of[name]) / (counts[key] - 1)
        would_fire = duckdb * math.exp(correction) > args.margin * ours
        if would_fire == fired_now:
            continue
        off, force = float(row["off_s"]), float(row["force_s"])
        helps = force < off
        verdict = ("gained" if helps else "started a loss") if would_fire else \
                  ("stopped a loss" if not helps else "lost")
        tally[verdict] += 1
        changes.append((name, fired_now, would_fire, off, force, verdict))

    print(f"{len(outcomes)} measured queries, {sum(tally.values())} decisions change\n")
    for name, was, now, off, force, verdict in sorted(changes, key=lambda c: c[5]):
        print(f"  {name:30} {'fire' if was else 'decline'} -> {'fire' if now else 'decline':8}"
              f"  off {off:7.3f}  force {force:7.3f}   {verdict}")
    print()
    for verdict, n in sorted(tally.items()):
        print(f"  {verdict:18} {n}")
    good = tally["gained"] + tally["stopped a loss"]
    bad = tally["started a loss"] + tally["lost"]
    print(f"\n  better {good}, worse {bad}, net {good - bad:+d}")
    seconds = sum((c[3] - c[4]) if c[2] else (c[4] - c[3]) for c in changes)
    print(f"  time those changes are worth: {seconds:+.3f}s")


if __name__ == "__main__":
    main()
