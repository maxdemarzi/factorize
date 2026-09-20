#!/usr/bin/env python3
"""Would perfect statistics make the gate decide better?

    scripts/oracle-stats.py [tmp/calib_new.csv]

F19 closes on O12 by naming what the gate would need: "a statistic that
captures *joint* presence across columns (a sketch, not an MCV list)". Six
estimator fixes have been measured and rejected since (D44a, D45, D46, D48,
D49, D50), and a sketch is more work than any of them, so the question worth
answering first is not "how accurate could the estimate be" but "how much would
accuracy buy".

That has a ceiling and the ceiling is computable from measurements already
taken. `calibrate-measured.sh` records, per query, the input rows, the records
the operator actually built, the exact result tuples, and both engines' warm
times. Feeding the *measured* records and tuples into the cost model is a gate
with perfect statistics -- better than any sketch can be, since a sketch only
estimates them -- and scoring its decisions against the measured times says
what perfect statistics are worth.

Three gates are scored the same way:

  shipped    what the `fired` column records the gate actually doing
  oracle     the same cost model and margin, given measured records and tuples
  perfect    fires exactly when the forced run was faster, which no estimator
             can beat because it is the answer

A gate is scored as the total time the corpus takes under it: the forced time
where it fires, the stock time where it does not.
"""

import csv
import sys

# As shipped in src/core/cost.hpp.
DUCKDB = (0.0, 2.324e-5, 3.981e-6)   # startup ms, per input row, per result tuple
OURS = (0.108542, 2.445e-5, 3.961e-5)  # startup ms, per input row, per record
MARGIN = 1.5
FLOOR_MS = 5.0  # min_duckdb_work_ms


def fires(rows, tuples, records):
    """The gate's rule, exactly as cost.cpp applies it."""
    predicted_duckdb = DUCKDB[0] + DUCKDB[1] * rows + DUCKDB[2] * tuples
    predicted_ours = OURS[0] + OURS[1] * rows + OURS[2] * records
    return (predicted_duckdb - DUCKDB[0]) >= FLOOR_MS and predicted_duckdb >= MARGIN * predicted_ours


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "tmp/calib_new.csv"
    rows = []
    for r in csv.DictReader(open(path)):
        try:
            rows.append(dict(query=r["query"], rows=float(r["input_rows"]), recs=float(r["records"]),
                             tuples=float(r["tuples"]), off=float(r["off_s"]), force=float(r["force_s"]),
                             fired=r["fired"] == "1"))
        except ValueError:
            continue  # a query that hit the cap has no time to score

    def score(decide):
        total = 0.0
        wrong_fires = wrong_declines = 0
        for m in rows:
            on = decide(m)
            total += m["force"] if on else m["off"]
            if on and m["force"] > m["off"]:
                wrong_fires += 1
            if not on and m["force"] < m["off"]:
                wrong_declines += 1
        return total, wrong_fires, wrong_declines

    # The gate's own predictions, if they have been collected. Substituting one
    # measured quantity at a time says which statistic the headroom is in --
    # which matters because a joint-presence sketch improves the *join size*,
    # and the model charges our side per record rather than per tuple.
    predicted = {}
    try:
        for r in csv.DictReader(open("tmp/predicted.csv")):
            if r["pred_records"] and r["pred_flat"]:
                predicted[r["query"]] = (float(r["pred_records"]), float(r["pred_flat"]))
    except IOError:
        pass
    rows = [m for m in rows if not predicted or m["query"] in predicted]

    def modelled(m, recs_from, flat_from):
        pr, pf = predicted.get(m["query"], (m["recs"], m["tuples"]))
        recs = m["recs"] if recs_from == "measured" else pr
        flat = m["tuples"] if flat_from == "measured" else pf
        return fires(m["rows"], flat, recs)

    gates = [
        ("never fire", lambda m: False),
        ("shipped gate", lambda m: m["fired"]),
        ("perfect knowledge", lambda m: m["force"] < m["off"]),
    ]
    # The same gate, given statistics from a full scan instead of a 16,384-row
    # sample. Not a sketch and not a better estimator -- the same estimator,
    # told the truth about the columns.
    exact = {}
    try:
        for r in csv.DictReader(open("tmp/predicted_exact.csv")):
            if r["pred_records"] and r["pred_flat"]:
                exact[r["query"]] = (float(r["pred_records"]), float(r["pred_flat"]))
    except IOError:
        pass

    if predicted:
        gates[2:2] = [
            ("modelled, as shipped", lambda m: modelled(m, "predicted", "predicted")),
            ("perfect join size", lambda m: modelled(m, "predicted", "measured")),
            ("perfect records", lambda m: modelled(m, "measured", "predicted")),
        ]
    if exact:
        gates[2:2] = [("exact statistics",
                       lambda m: fires(m["rows"], *reversed(exact.get(m["query"], (m["recs"], m["tuples"])))))]
    gates[-1:-1] = [("oracle statistics", lambda m: fires(m["rows"], m["tuples"], m["recs"]))]
    print(f"{len(rows)} queries with both times measured\n")
    print(f"{'gate':<20} {'corpus':>9} {'vs stock':>9} {'fires':>6} {'wrong fires':>12} {'wrong declines':>15}")
    stock = sum(m["off"] for m in rows)
    for name, decide in gates:
        total, wf, wd = score(decide)
        n = sum(1 for m in rows if decide(m))
        print(f"{name:<20} {total:>8.2f}s {stock / total:>8.2f}x {n:>6} {wf:>12} {wd:>15}")

    # The margin is the one knob that moves a gate without changing what it
    # knows, so sweeping it separates "the statistics are wrong" from "the
    # threshold is wrong". D48 found the coefficients had been fitted on top of
    # the estimator's errors and could not be refitted alone; if that holds, the
    # best margin for true inputs is not the shipped one.
    global MARGIN
    shipped_margin = MARGIN
    print("\nmargin sweep (corpus seconds; lower is better):")
    print(f"  {'margin':>7} {'oracle stats':>14}")
    best = None
    for m in [0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 5.0, 10.0]:
        MARGIN = m
        total, wf, wd = score(lambda q: fires(q["rows"], q["tuples"], q["recs"]))
        mark = "   <- shipped margin" if m == shipped_margin else ""
        print(f"  {m:>7.2f} {total:>13.2f}s{mark}")
        if best is None or total < best[1]:
            best = (m, total)
    MARGIN = shipped_margin
    print(f"  best margin on oracle statistics: {best[0]} at {best[1]:.2f}s "
          f"(perfect knowledge is {score(lambda q: q['force'] < q['off'])[0]:.2f}s)")

    # Where the two gates disagree is the whole of what better statistics buy.
    print("\nqueries where perfect statistics would change the decision:")
    changed = [m for m in rows if fires(m["rows"], m["tuples"], m["recs"]) != m["fired"]]
    for m in sorted(changed, key=lambda m: -abs(m["off"] - m["force"]))[:12]:
        want = "fire" if fires(m["rows"], m["tuples"], m["recs"]) else "decline"
        good = "better" if ((m["force"] < m["off"]) == (want == "fire")) else "WORSE"
        print(f"  {m['query']:<28} -> {want:<8} stock {m['off']:>8.3f}s ours {m['force']:>8.3f}s  {good}")
    if not changed:
        print("  none")


if __name__ == "__main__":
    main()
