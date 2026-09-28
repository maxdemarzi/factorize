#!/usr/bin/env python3
"""What firing in the >1e9-tuple regime is actually worth.

    scripts/excluded-verdict.py [baseline.csv] [ours.csv]

D42 wrote that "in the >1e9-tuple regime DuckDB does not finish, so firing more
is strictly better there", and every decision for months used the excluded
corpus's *fire count* as the benefit side of a trade. D47 measured nine of those
fires and found three wins and six losses, which rejected the claim without
replacing it, and said exactly what replacing it would take:

    The excluded corpus needs a stock baseline -- every query timed under
    factorize_mode='off' at a fixed cap -- so "fires" can be replaced by
    "answers stock does not produce, minus time lost on the ones it does".

This computes that. Two inputs, both a query per row:

    baseline.csv   query,expected,stock_s      from measure-excluded-baseline.sh
    ours.csv       query,off_s,on_s            from measure-parallel.sh, or any
                                               file whose last column is our time

A time of "to" means the cap was hit, which is the interesting value rather
than a missing one: it is the whole of what "answers stock does not produce"
means.

The verdict per query is one of four, and the counts of each are the result:

    rescued     stock capped, we answered           -- the reason this exists
    lost        we capped, stock answered           -- the cost of firing
    faster      both answered, we were quicker
    slower      both answered, we were not
    neither     both capped                         -- no information
"""

import csv
import sys

# "missing" is a query whose tables this database does not have; it is not a
# cap and not a time, and pairing it with anything would be inventing data.
CAP_MARKERS = {"to", "timeout", ""}
SKIP_MARKERS = {"missing"}


def read(path, time_column):
    out = {}
    for row in csv.DictReader(open(path)):
        value = (row.get(time_column) or "").strip()
        if value in SKIP_MARKERS:
            continue
        out[row["query"]] = None if value in CAP_MARKERS else float(value)
    return out


def main():
    baseline_path = sys.argv[1] if len(sys.argv) > 1 else "tmp/excluded_baseline.csv"
    ours_path = sys.argv[2] if len(sys.argv) > 2 else "tmp/excluded_ours.csv"
    stock = read(baseline_path, "stock_s")

    # Whatever the second file calls our column, take the last one that parses.
    header = next(csv.reader(open(ours_path)))
    ours_column = header[-1]
    ours = read(ours_path, ours_column)

    shared = [q for q in ours if q in stock]
    if not shared:
        print("no queries in common between the two files", file=sys.stderr)
        return 1

    buckets = {"rescued": [], "lost": [], "faster": [], "slower": [], "neither": []}
    for q in shared:
        s, o = stock[q], ours[q]
        if s is None and o is None:
            buckets["neither"].append((q, s, o))
        elif s is None:
            buckets["rescued"].append((q, s, o))
        elif o is None:
            buckets["lost"].append((q, s, o))
        elif o < s:
            buckets["faster"].append((q, s, o))
        else:
            buckets["slower"].append((q, s, o))

    print(f"{len(shared)} queries with both times\n")
    for name in ("rescued", "lost", "faster", "slower", "neither"):
        print(f"  {name:<9} {len(buckets[name]):>4}")

    # Time, counting a cap as the cap: an under-statement of what a query that
    # did not finish would have cost, and the only honest number available.
    both = buckets["faster"] + buckets["slower"]
    if both:
        saved = sum(s - o for _, s, o in both)
        print(f"\non the {len(both)} queries both engines answered: "
              f"{sum(s for _, s, _ in both):.1f}s stock against {sum(o for _, _, o in both):.1f}s ours "
              f"({saved:+.1f}s)")

    print(f"\nanswers stock does not produce: {len(buckets['rescued'])}")
    print(f"answers lost by firing:          {len(buckets['lost'])}")

    for name in ("rescued", "lost"):
        if not buckets[name]:
            continue
        print(f"\n{name}:")
        for q, s, o in sorted(buckets[name])[:20]:
            print(f"  {q:<28} stock {'cap' if s is None else f'{s:.1f}s':>8}   "
                  f"ours {'cap' if o is None else f'{o:.1f}s':>8}")
        if len(buckets[name]) > 20:
            print(f"  ... and {len(buckets[name]) - 20} more")

    # By dataset, because F18's whole point is that a corpus-wide count hides
    # which dataset it came from.
    print("\nby dataset:")
    datasets = {}
    for name in buckets:
        for q, _, _ in buckets[name]:
            d = datasets.setdefault(q.split("_")[0], dict.fromkeys(buckets, 0))
            d[name] += 1
    for d, counts in sorted(datasets.items()):
        line = "  ".join(f"{k} {v}" for k, v in counts.items() if v)
        print(f"  {d:<10} {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
