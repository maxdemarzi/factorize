#!/usr/bin/env python3
"""How the gate's predicted speedup relates to the one it actually gets.

    scripts/calibrate-outcome.py

Five attempts have now been made to stop the gate firing where it loses -- D71's
coefficient, D73's four plan-time features, D75's per-table calibration and its
join-shape follow-up, and D81's per-fold cost -- and every one failed the same
way: a global scalar moves a threshold, and moving the threshold surrenders far
more good outcomes than bad ones. This computes why, from measurements already
taken.

Three things come out, and the third is the answer:

  * The ordering is good. Rank correlation between predicted and achieved
    speedup is about +0.68 over the queries the gate fires on: its most
    confident predictions really do win biggest.

  * The scale is not. Predictions span 1.4x to 451,884x while outcomes span
    0.05x to 73.6x, compressed by a power law -- achieved is roughly
    0.146 * predicted^0.516, which halves the error against taking the
    prediction at face value (x4.9 against x10.8). On that curve, actually
    achieving 1.5x needs a raw prediction near 91x, which is why raising the
    margin from 1.5x to 2.55x moved two decisions out of thirty-five.

  * And calibrating the scale still does not help, because rescues and losses
    are interleaved along the axis rather than separated by it. A bar at 20x
    makes the runnable corpus faster (11.78s to 11.18s, 13 regressions down to
    4) and gives up 36 rescues to avoid 4 slower queries and 1 lost one.

That last line is the finding. The axis orders queries correctly and still
cannot be cut, because what the gate most wants to keep and what it most wants
to drop sit next to each other on it.
"""

import csv
import math
import statistics
import sys


def num(value, cap=300.0):
    return cap if value in ("to", "", None) else float(value)


def load(path, key="query"):
    return {r[key]: r for r in csv.DictReader(open(path)) if r.get(key)}


def ratio(row):
    ours = float(row.get("pred_ours_ms") or 0)
    duckdb = float(row.get("pred_duckdb_ms") or 0)
    return duckdb / ours if ours > 0 and duckdb > 0 else None


def read_times(path):
    reader = csv.reader(open(path))
    next(reader)
    return {row[0]: row[-1].strip() for row in reader if row}


def main():
    base = load("tmp/headline.csv")
    runnable = load("tmp/pred_times_runnable.csv")

    # The power law, over every runnable query, using `force` so each one
    # reports what firing would actually achieve rather than what it was
    # allowed to.
    predicted, achieved = [], []
    for query, row in runnable.items():
        if query not in base:
            continue
        x = ratio(row)
        off, forced = num(base[query]["off_s"]), num(base[query].get("force_s"))
        if x is None or off <= 0.002 or forced <= 0.002:
            continue
        predicted.append(math.log(x))
        achieved.append(math.log(off / forced))
    n = len(predicted)
    mx, my = sum(predicted) / n, sum(achieved) / n
    slope = sum((predicted[i] - mx) * (achieved[i] - my) for i in range(n)) / sum(
        (predicted[i] - mx) ** 2 for i in range(n)
    )
    scale = math.exp(my - slope * mx)
    fitted = [achieved[i] - (math.log(scale) + slope * predicted[i]) for i in range(n)]
    face = [achieved[i] - predicted[i] for i in range(n)]
    print(f"{n} runnable queries")
    print(f"  achieved ~ {scale:.3f} * predicted^{slope:.3f}")
    print(f"  residual after the fit x{math.exp(statistics.pstdev(fitted)):.1f}, "
          f"at face value x{math.exp(statistics.pstdev(face)):.1f}")
    print(f"  a real 1.5x needs a raw prediction near "
          f"{math.exp((math.log(1.5) - math.log(scale)) / slope):,.0f}x")

    # What a bar on that axis costs, against the excluded corpus's outcomes.
    stock, ours = {}, {}
    for a, b in (("tmp/d72_stock.csv", "tmp/d72_auto.csv"),
                 ("tmp/d73_stock.csv", "tmp/d73_auto.csv"),
                 ("tmp/d76_stock.csv", "tmp/d76_auto.csv")):
        stock.update(read_times(a))
        ours.update(read_times(b))
    excluded = {}
    for path in ("tmp/pred_times.csv", "tmp/pred_holdout.csv"):
        excluded.update(load(path))

    def verdict(query):
        s, o = stock.get(query), ours.get(query)
        if not s or not o or "missing" in (s, o):
            return None
        S = None if s == "to" else float(s)
        O = None if o == "to" else float(o)
        if S is None and O is None:
            return "neither"
        if S is None:
            return "RESCUE"
        if O is None:
            return "lost"
        return "faster" if O < S else "slower"

    print("\n  what a bar on the predicted axis gives up, by outcome:")
    for bar in (20, 50, 91):
        given_up = {}
        for query, row in excluded.items():
            x = ratio(row)
            if x is None or row.get("fired") != "1" or x >= bar:
                continue
            v = verdict(query)
            if v:
                given_up[v] = given_up.get(v, 0) + 1
        print(f"    {bar:>3}x  {given_up}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
