#!/usr/bin/env python3
"""Evaluate the Phase 1.5 criterion from the harness output.

Two ratios are reported, and they answer different questions:

  factorized vs our own flat baseline
      Same arena, same hash table, same plan, same machine, same thread count.
      This isolates *factorization* from everything else, and it is how the
      paper reports its own numbers.

  factorized vs stock DuckDB
      What a user would actually experience, and the bar the plan's Phase 1.5
      exit/kill criterion is written against (DECISIONS.md D11).

Queries where the flat baseline could not complete are reported separately
rather than dropped: those are the high-duplication cases factorization exists
for, so silently excluding them biases the ratio against it -- which is exactly
what happened on the first run of this benchmark.

Usage:
    python3 scripts/analyze-ce.py tmp/ce_batch1.csv [--duckdb tmp/ce_duckdb.csv]
"""

import argparse
import csv
import math
import os
import sys


def geomean(values):
    values = [v for v in values if v and v > 0]
    if not values:
        return float("nan")
    return math.exp(sum(math.log(v) for v in values) / len(values))


def percentile(values, p):
    if not values:
        return float("nan")
    values = sorted(values)
    return values[min(len(values) - 1, int(p / 100.0 * len(values)))]


def load(path):
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def num(row, key):
    value = row.get(key, "")
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("results", nargs="+", help="harness CSV(s)")
    parser.add_argument("--duckdb", help="CSV from scripts/run-duckdb-ce.sh")
    args = parser.parse_args()

    rows = []
    for path in args.results:
        rows.extend(load(path))

    duckdb = {}
    if args.duckdb and os.path.exists(args.duckdb):
        for row in load(args.duckdb):
            duckdb[row["query"]] = row

    # --- correctness ---------------------------------------------------
    #
    # A decline and a wrong answer are not the same failure and must never be
    # counted together. Declining is safe -- the extension would fall back to
    # the stock plan and the user gets a correct, slower result. Returning the
    # wrong count is catastrophic and would sink the project.
    ok = [r for r in rows if r["status"].startswith("ok")]
    unsupported = [r for r in rows if r["status"].startswith("unsupported")]
    declined = [r for r in rows
                if "error:" in r["status"] and not r["status"].startswith("ok")]
    wrong = [r for r in rows if "MISMATCH" in r["status"]]

    print("== correctness ==")
    print("  queries run          %d" % len(rows))
    print("  matched the oracle   %d" % len(ok))
    print("  WRONG ANSWERS        %d%s" % (len(wrong), "" if wrong else "   <- the number that matters"))
    for r in wrong[:10]:
        print("      %-34s expected %-14s got %-14s %s" %
              (r["query"], r["expected"], r["factorized"], r["status"]))
    if declined:
        print("  declined (no answer) %d" % len(declined))
        reasons = {}
        for r in declined:
            reasons[r["status"].split(":")[-1].strip()] = reasons.get(r["status"].split(":")[-1].strip(), 0) + 1
        for reason, count in sorted(reasons.items(), key=lambda kv: -kv[1]):
            print("      %-52s %d" % (reason, count))
    if unsupported:
        print("  unsupported          %d" % len(unsupported))

    # Best of the two insert modes: v1 has no cost model yet, so this is the
    # ceiling an optimizer choosing per-join could reach, not what it achieves.
    def best_ms(r):
        top, bottom = num(r, "fact_top_ms"), num(r, "fact_bottom_ms")
        candidates = [v for v in (top, bottom) if v and v > 0]
        return min(candidates) if candidates else None

    # --- vs our own flat baseline ---------------------------------------
    with_flat = [r for r in ok if (num(r, "flat_ms") or 0) > 0]
    flat_failed = [r for r in ok if (num(r, "flat_ms") or 0) <= 0]

    print()
    print("== factorized vs our own flat baseline (identical infrastructure) ==")
    print("  both engines completed: %d      flat could not complete: %d" %
          (len(with_flat), len(flat_failed)))
    if flat_failed:
        biggest = sorted(flat_failed, key=lambda r: -int(r["factorized"] or 0))[:3]
        print("      (the flat-failure set is the high-duplication one, e.g. %s)" %
              ", ".join("%s tuples" % f"{int(r['factorized']):,}" for r in biggest))

    def report(label, subset):
        if not subset:
            return
        top = [num(r, "flat_ms") / num(r, "fact_top_ms") for r in subset if num(r, "fact_top_ms")]
        bottom = [num(r, "flat_ms") / num(r, "fact_bottom_ms") for r in subset if num(r, "fact_bottom_ms")]
        best = [num(r, "flat_ms") / best_ms(r) for r in subset if best_ms(r)]
        wins = sum(1 for v in best if v > 1.0)
        print("  %-24s n=%3d   top %6.2fx  bottom %6.2fx  best %6.2fx   "
              "p10 %5.2f  p90 %6.2f   wins %d/%d" %
              (label, len(subset), geomean(top), geomean(bottom), geomean(best),
               percentile(best, 10), percentile(best, 90), wins, len(subset)))

    report("all", with_flat)
    for threshold in (10, 100, 1000):
        report("flat > %d ms" % threshold,
               [r for r in with_flat if num(r, "flat_ms") > threshold])

    # --- vs stock DuckDB -------------------------------------------------
    if duckdb:
        print()
        print("== factorized vs stock DuckDB (this machine) ==")
        for column, label in (("duckdb_1t_ms", "DuckDB 1 thread"),
                              ("duckdb_default_ms", "DuckDB default threads")):
            paired = []
            for r in ok:
                entry = duckdb.get(r["query"])
                if not entry:
                    continue
                try:
                    reference = float(entry[column])
                except (KeyError, TypeError, ValueError):
                    continue
                if reference > 0 and best_ms(r):
                    paired.append((r, reference))
            if not paired:
                continue
            print("  -- %s --" % label)
            for threshold in (0, 100):
                subset = [(r, d) for r, d in paired if d > threshold]
                if not subset:
                    continue
                ratios = [d / best_ms(r) for r, d in subset]
                wins = sum(1 for v in ratios if v > 1.0)
                name = "all" if threshold == 0 else "DuckDB > %d ms" % threshold
                print("     %-22s n=%3d   %6.2fx   p10 %5.2f  median %6.2f  p90 %7.2f   wins %d/%d" %
                      (name, len(subset), geomean(ratios), percentile(ratios, 10),
                       percentile(ratios, 50), percentile(ratios, 90), wins, len(subset)))
    else:
        print()
        print("== factorized vs stock DuckDB ==")
        print("  no DuckDB timings supplied; run scripts/run-duckdb-ce.sh first.")
        print("  The published numbers in the artifact are from a 64-core Xeon and")
        print("  are NOT comparable with this machine.")

    # --- representation quality -----------------------------------------
    print()
    print("== representation ==")
    compression = []
    for r in ok:
        try:
            tuples, records = int(r["factorized"]), int(r["records"])
        except (TypeError, ValueError):
            continue
        if tuples > 0 and records > 0:
            compression.append(tuples / records)
    if compression:
        print("  flat tuples per f-rep record:  median %.1fx   p90 %.1fx   max %.1fx" %
              (percentile(compression, 50), percentile(compression, 90), max(compression)))
        print("  queries where the f-rep is smaller than the flat result: %d/%d" %
              (sum(1 for c in compression if c > 1), len(compression)))
    return 0 if not wrong else 1


if __name__ == "__main__":
    sys.exit(main())
