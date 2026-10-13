#!/usr/bin/env python3
"""Does measured fan-out separate the queries the gate should fire on?

    scripts/collect-fanout.py && scripts/fanout-outcome.py

scripts/calibrate-outcome.py asked this of the gate's *predicted* speedup and
got a clear answer: the ordering is good (rank correlation +0.68) and the axis
still cannot be cut, because rescues and losses are interleaved along it. This
asks the same question of a different quantity -- fan-out, measured at runtime
rather than predicted at plan time, which is the axis CIDR 2025's adaptive
factorization found most predictive (their "average chain length", correlation
0.58 with the faster plan).

It reports three things, and refusing to look at only the first is the point:

  * Correlation of each fan-out summary with the achieved log speedup. A number
    near zero ends the line here.
  * Rank correlation, because D82's whole lesson is that a good *ordering* can
    sit under a scale too compressed to cut. Order and cut are separate
    questions and get separate answers.
  * What a bar on the best summary would actually cost, counted in outcomes
    rather than in accuracy -- the only form in which D71 through D82 were
    refutable, and the form in which this has to be refutable too.
"""

import csv
import math
import statistics
import sys


def num(value, cap=300.0):
    return cap if value in ("to", "", None) else float(value)


def load(path, key="query"):
    try:
        return {r[key]: r for r in csv.DictReader(open(path)) if r.get(key)}
    except FileNotFoundError:
        print(f"missing {path}", file=sys.stderr)
        return {}


def summarise(steps):
    """Per-query fan-out summaries, over the per-step rows for one query.

    `first` is the one a runtime gate could act on earliest and so the one
    worth the most; `min` is CIDR's choice, on the reasoning that a chain is
    only as factorized as its narrowest join.
    """
    mean = [float(s["fanout_mean"]) for s in steps]
    peak = [float(s["fanout_max"]) for s in steps]
    return {
        "first_mean": mean[0],
        "first_max": peak[0],
        "min_mean": min(mean),
        "max_max": max(peak),
        "last_mean": mean[-1],
    }


def pearson(xs, ys):
    n = len(xs)
    if n < 3:
        return 0.0
    mx, my = sum(xs) / n, sum(ys) / n
    num_ = sum((xs[i] - mx) * (ys[i] - my) for i in range(n))
    den = math.sqrt(sum((x - mx) ** 2 for x in xs) * sum((y - my) ** 2 for y in ys))
    return num_ / den if den else 0.0


def spearman(xs, ys):
    def ranked(values):
        order = sorted(range(len(values)), key=lambda i: values[i])
        ranks = [0.0] * len(values)
        for position, index in enumerate(order):
            ranks[index] = float(position)
        return ranks

    return pearson(ranked(xs), ranked(ys))


def main():
    base = load("tmp/headline.csv")
    fanout_rows = {}
    try:
        for row in csv.DictReader(open("tmp/fanout_runnable.csv")):
            fanout_rows.setdefault(row["query"], []).append(row)
    except FileNotFoundError:
        print("run scripts/collect-fanout.py first", file=sys.stderr)
        return 1

    # `force` rather than `auto`: what firing would achieve, not what the gate
    # allowed. Same choice calibrate-outcome.py makes, for the same reason --
    # a query the gate declined reports the gate's opinion, not the engine's.
    points = []
    for query, steps in fanout_rows.items():
        if query not in base:
            continue
        off, forced = num(base[query]["off_s"]), num(base[query].get("force_s"))
        if off <= 0.002 or forced <= 0.002:
            continue
        points.append((summarise(sorted(steps, key=lambda s: int(s["step"]))), math.log(off / forced), query))

    if len(points) < 5:
        print(f"only {len(points)} queries with both fan-out and a timing; nothing to say")
        return 1

    achieved = [p[1] for p in points]
    print(f"{len(points)} runnable queries with measured fan-out and a measured outcome")
    print(f"  {sum(1 for a in achieved if a > 0)} faster forced, {sum(1 for a in achieved if a <= 0)} not\n")
    print("  summary          pearson   spearman   (vs log achieved speedup)")
    best, best_key = 0.0, None
    for key in ("first_mean", "first_max", "min_mean", "max_max", "last_mean"):
        xs = [math.log(max(p[0][key], 1e-9)) for p in points]
        r, rho = pearson(xs, achieved), spearman(xs, achieved)
        print(f"  {key:<15} {r:>+7.3f}   {rho:>+7.3f}")
        if abs(rho) > abs(best):
            best, best_key = rho, key

    # The comparison that matters: D82 established the predicted axis gets
    # +0.68 here. A second axis earns its place by beating that, or by being
    # wrong where the first one is wrong.
    print(f"\n  best: {best_key} at {best:+.3f} rank correlation")
    print("  (the predicted-speedup axis gets +0.68; see calibrate-outcome.py)")

    # And then the only question that decided D71 through D82: what does a cut
    # on it give up? Counted on the queries themselves, not on an accuracy.
    print(f"\n  what a floor on {best_key} would cost, on this corpus:")
    print("    floor   declined   of which faster forced (given up) / slower (avoided)")
    values = sorted({round(p[0][best_key], 2) for p in points})
    for floor in values[:: max(1, len(values) // 8)]:
        declined = [p for p in points if p[0][best_key] < floor]
        given_up = sum(1 for p in declined if p[1] > 0)
        avoided = sum(1 for p in declined if p[1] <= 0)
        print(f"    {floor:>5.2f}   {len(declined):>8}   {given_up:>3} given up / {avoided:>3} avoided")

    # The table above is about every query. This one is about the only queries
    # a floor could change: the ones the gate fires on. A signal that declines
    # queries already being declined buys nothing, and D71 through D82 each
    # looked better on the first table than the second.
    print("\n  and on the queries the gate actually fires on:")
    fires = []
    for summary, _, query in points:
        row = base[query]
        if row.get("fired") != "1":
            continue
        off, auto = num(row["off_s"]), num(row["auto_s"])
        if off <= 0.002 or auto <= 0.002:
            continue
        fires.append((summary, math.log(off / auto), query))
    wins = sum(1 for f in fires if f[1] > 0)
    print(f"    {len(fires)} fires: {wins} faster, {len(fires) - wins} regressions")
    # Priced in seconds, which is the only unit the earlier attempts were ever
    # refuted in: a declined fire costs `off_s` instead of `auto_s`, so the
    # corpus total moves by exactly the sum of those swaps.
    current = sum(num(base[f[2]]["auto_s"]) for f in fires)
    others = sum(num(base[q]["auto_s"]) for q in base if base[q].get("fired") != "1")
    print("    floor   declined   wins lost / regressions cut   corpus total")
    for floor in (1.08, 1.18, 1.38, 1.89, 2.20):
        declined = [f for f in fires if f[0][best_key] < floor]
        lost = sum(1 for f in declined if f[1] > 0)
        total = current + sum(num(base[f[2]]["off_s"]) - num(base[f[2]]["auto_s"]) for f in declined)
        print(f"    {floor:>5.2f}   {len(declined):>8}   {lost:>4} / {len(declined) - lost:<21}"
              f"{total + others:>7.2f}s")
    print(f"    {'none':>5}   {0:>8}   {0:>4} / {0:<21}{current + others:>7.2f}s")

    for floor in (1.08, 1.18):
        declined = [f for f in fires if f[0][best_key] < floor]
        for f in sorted(declined, key=lambda f: -f[1]):
            if f[1] > 0:
                print(f"    floor {floor}: gives up {f[2]} at x{math.exp(f[1]):.2f} "
                      f"({num(base[f[2]]['off_s']):.3f}s -> {num(base[f[2]]['auto_s']):.3f}s), "
                      f"{best_key}={f[0][best_key]:.2f}")
        break

    worst = min(points, key=lambda p: p[1])
    print(f"\n  worst outcome: {worst[2]} at x{math.exp(worst[1]):.2f}, {best_key}={worst[0][best_key]:.2f}")
    print(f"  median {best_key} among faster: "
          f"{statistics.median([p[0][best_key] for p in points if p[1] > 0]):.2f}")
    print(f"  median {best_key} among slower: "
          f"{statistics.median([p[0][best_key] for p in points if p[1] <= 0]):.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
