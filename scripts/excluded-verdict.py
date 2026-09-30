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


def read(path):
    """Times by query, keyed off the file's LAST column whatever it is called.

    measure-excluded-baseline.sh names its column after the mode it ran, so the
    same script writes `stock_s`, `off_s` or `auto_s` depending on how it was
    invoked. Looking a fixed name up with .get() returned None for every row of
    an `off_s` file, which this then read as a cap -- turning 26 stock answers
    into 26 queries stock could not answer, and a run of 30 rescued that was
    really 4. A header the reader does not recognise must not be able to look
    like data, which is the same defect as the binder errors in D60.
    """
    out = {}
    reader = csv.reader(open(path))
    header = next(reader)
    query_at, time_at = header.index("query"), len(header) - 1
    for row in reader:
        if not row:
            continue
        value = row[time_at].strip()
        if value in SKIP_MARKERS:
            continue
        out[row[query_at]] = None if value in CAP_MARKERS else float(value)
    return out


def verdicts(stock, ours):
    """The four-way classification, over the queries both files time."""
    buckets = {"rescued": [], "lost": [], "faster": [], "slower": [], "neither": []}
    for q in [q for q in ours if q in stock]:
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
    return buckets


def main():
    baseline_path = sys.argv[1] if len(sys.argv) > 1 else "tmp/excluded_baseline.csv"
    ours_path = sys.argv[2] if len(sys.argv) > 2 else "tmp/excluded_ours.csv"
    stock = read(baseline_path)
    ours = read(ours_path)

    buckets = verdicts(stock, ours)
    shared = [q for b in buckets.values() for q, _, _ in b]
    if not shared:
        print("no queries in common between the two files", file=sys.stderr)
        return 1

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


def selftest():
    """The two ways this file has read a measurement wrongly, pinned.

        scripts/excluded-verdict.py --selftest

    Both were found in D61 and both moved the answer our way. The baseline is
    written with an `off_s` column, not `stock_s`, and looking the old fixed
    name up returned None for every row -- which this read as a cap, turning
    every stock answer into a query stock could not answer. On the real data
    that printed 30 rescued and 0 lost where the truth was 6 and 2.
    """
    import tempfile, os

    d = tempfile.mkdtemp()
    stock = os.path.join(d, "stock.csv")
    ours = os.path.join(d, "ours.csv")
    # Column named for the mode, not "stock_s" -- the whole point.
    eol = chr(10)
    open(stock, "w").write(eol.join(["query,expected,off_s", "a,1,10.0", "b,1,to", "c,1,missing", "d,1,3.0", "e,1,to", ""]))
    open(ours, "w").write(eol.join(["query,expected,auto_s", "a,1,2.0", "b,1,4.0", "c,1,1.0", "d,1,to", "e,1,to", ""]))

    got = read(stock)
    assert got == {"a": 10.0, "b": None, "d": 3.0, "e": None}, got
    assert "c" not in got, "a `missing` row is not a cap and not a time"

    ours_read = read(ours)
    assert ours_read["a"] == 2.0 and ours_read["d"] is None, ours_read

    # a faster, b rescued, c skipped, d lost, e neither.
    counts = verdicts(got, ours_read)
    expected = {"rescued": 1, "lost": 1, "faster": 1, "slower": 0, "neither": 1}
    assert {k: len(v) for k, v in counts.items()} == expected, counts
    print("selftest ok")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    sys.exit(main())
