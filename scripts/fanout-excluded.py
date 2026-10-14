#!/usr/bin/env python3
"""What a fan-out floor would cost on the excluded corpus.

    scripts/collect-fanout.py --list tmp/excluded_all.psv \
        --out tmp/fanout_excluded.csv
    scripts/fanout-excluded.py

The runnable corpus is where a gate change looks good and the excluded corpus
is where it dies. Every one of D71, D73, D75, D81 and D82 improved the first
and gave up rescues on the second, and the rescues are the thing this engine
exists for: 244 queries whose answers do not otherwise exist, because DuckDB
cannot materialise a result of more than a billion tuples.

So this is the veto, and it is deliberately scored in outcomes rather than in
seconds. A rescue is not worth some number of milliseconds elsewhere; it is an
answer against no answer, and the only honest accounting counts them.
"""

import argparse
import collections
import csv
import sys


def read_times(path):
    """{query: last column}, by position. The harness has written this column
    under three different names across runs (off_s, stock_s, auto_s), and
    reading it by name is how excluded-verdict.py came to score 30 rescues
    where there were 6 -- so it is read by position, as that fix did."""
    try:
        reader = csv.reader(open(path))
        next(reader)
        return {row[0]: row[-1].strip() for row in reader if row}
    except FileNotFoundError:
        print(f"missing {path}", file=sys.stderr)
        return {}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fanout", default="tmp/fanout_excluded.csv")
    parser.add_argument("--floor", type=float, default=1.0)
    # The predicted file has one row per *edge* and a `partners` column; the
    # measured one has a row per *step* and `fanout_mean`. Both reduce to a
    # minimum per query, which is the only thing scored here.
    parser.add_argument("--column", default="fanout_mean",
                        help="fanout_mean for measured, partners for predicted")
    args = parser.parse_args()

    stock, ours = {}, {}
    for a, b in (("tmp/d72_stock.csv", "tmp/d72_auto.csv"),
                 ("tmp/d73_stock.csv", "tmp/d73_auto.csv"),
                 ("tmp/d76_stock.csv", "tmp/d76_auto.csv")):
        stock.update(read_times(a))
        ours.update(read_times(b))

    def verdict(query):
        s, o = stock.get(query), ours.get(query)
        if not s or not o or "missing" in (s, o):
            return None
        timed_out_stock, timed_out_ours = s == "to", o == "to"
        if timed_out_stock and timed_out_ours:
            return "neither"
        if timed_out_stock:
            return "RESCUE"
        if timed_out_ours:
            return "lost"
        return "faster" if float(o) < float(s) else "slower"

    fanout = {}
    try:
        for row in csv.DictReader(open(args.fanout)):
            try:
                fanout.setdefault(row["query"], []).append(float(row[args.column]))
            except (ValueError, KeyError):
                continue
    except FileNotFoundError:
        print(f"run collect-fanout.py over tmp/excluded_all.psv first", file=sys.stderr)
        return 1

    # Only queries with both a measured fan-out and a measured outcome can say
    # anything; the rest are counted and named rather than quietly dropped.
    scored, unmeasured = {}, 0
    for query, values in fanout.items():
        v = verdict(query)
        if v is None:
            unmeasured += 1
            continue
        scored[query] = (min(values), v)

    print(f"{len(fanout)} queries with a {args.column} value, {len(scored)} of them with a measured outcome "
          f"({unmeasured} without)")
    overall = collections.Counter(v for _, v in scored.values())
    print(f"  outcomes now: {dict(overall)}\n")

    for floor in (args.floor, 1.05, 1.2, 1.5):
        declined = {q: v for q, (mn, v) in scored.items() if mn <= floor}
        counts = collections.Counter(declined.values())
        rescues = counts.get("RESCUE", 0)
        # Judged on rescues alone, not on rescues netted against slower
        # queries. An earlier version of this line printed "GOOD" whenever the
        # slower queries avoided outnumbered the rescues surrendered, which
        # stamped approval on the predicted-fan-out run that gave up 13 of
        # them. They do not trade: a rescue is an answer against no answer,
        # and D60 onwards has refused that exchange every time it was offered.
        verdict_label = "safe" if rescues == 0 else f"COSTS {rescues} RESCUES"
        print(f"  floor {floor:<5} declines {len(declined):>3}: "
              f"{rescues} rescues and {counts.get('faster', 0)} faster given up, "
              f"{counts.get('slower', 0)} slower and {counts.get('lost', 0)} lost avoided"
              f"   [{verdict_label}]")

    # And the number that actually decides it: a rule that gives up even a few
    # rescues has to be paying for them with something, and on this corpus the
    # only currency is the slower queries it prevents.
    at_floor = {q: v for q, (mn, v) in scored.items() if mn <= args.floor}
    if at_floor:
        print(f"\n  at floor {args.floor}, the rescues it would give up:")
        given = [q for q, v in at_floor.items() if v == "RESCUE"]
        for q in sorted(given)[:20]:
            print(f"    {q}  (ours {ours.get(q)}s, stock timed out)")
        if len(given) > 20:
            print(f"    ... and {len(given) - 20} more")
        if not given:
            print("    none")
    return 0


if __name__ == "__main__":
    sys.exit(main())
