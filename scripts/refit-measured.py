#!/usr/bin/env python3
"""Re-fit the gate's cost model on measured inputs, and score margin/floor
pairs on both corpora.

    scripts/refit-measured.py <calib.csv> <runnable_sweep.psv> [excluded_sweep.psv]

Supersedes refit-cost.py, which fitted our side against result tuples for a
model that charges per record, and scored candidates on the runnable corpus
alone (D48).

Our side is charged per *record* by the model, so it is fitted against records
the operator built, not result tuples. Candidates are scored two ways: corpus
time on the runnable queries, and -- per D47, which found fire counts there
meaningless -- the excluded queries whose outcome has actually been measured.
"""
import csv, os, re, sys


def fit(samples, quantile, with_intercept=True):
    def loss(p):
        total = 0.0
        for rows, out, ms in samples:
            predicted = max(p[0] + p[1] * rows + p[2] * out, 1e-6)
            r = ms - predicted
            total += r * quantile if r > 0 else -r * (1 - quantile)
        return total
    p = [0.0, 1e-5, 1e-5]
    step = [1.0, 1e-5, 1e-5]
    for _ in range(600):
        improved = False
        for i in range(3):
            if i == 0 and not with_intercept:
                continue
            for d in (1, -1):
                c = list(p)
                c[i] = max(0.0, c[i] + d * step[i])
                if loss(c) < loss(p):
                    p, improved = c, True
        if not improved:
            step = [s / 2 for s in step]
            if max(step) < 1e-13:
                break
    return p


def predicted_ms(rows, flat, recs, duckdb, ours):
    return (duckdb[0] + duckdb[1] * rows + duckdb[2] * flat,
            ours[0] + ours[1] * rows + ours[2] * recs)


def fires(rows, flat, recs, duckdb, ours, margin, floor):
    pd, po = predicted_ms(rows, flat, recs, duckdb, ours)
    return (pd - duckdb[0]) >= floor and pd >= margin * po


def load_sweep(path):
    out = {}
    for line in open(path, encoding="utf-8"):
        parts = line.rstrip("\n").rstrip("\r").split("|")
        if len(parts) < 7:
            continue
        try:
            out[parts[0]] = (float(parts[2]), float(parts[6]))
        except ValueError:
            continue
    return out


def table_sizes(path="tmp/table_rows.csv"):
    size = {}
    for line in open(path):
        name, _, rows = line.strip().partition(",")
        if rows.isdigit():
            size[name] = int(rows)
    return size


def input_rows(corpus, sizes):
    out = {}
    for line in open(corpus, encoding="utf-8"):
        parts = line.rstrip("\n").split("|", 2)
        if len(parts) < 3:
            continue
        m = re.search(r"from (.*?) where ", parts[2], re.IGNORECASE)
        if not m:
            continue
        out[parts[0]] = sum(sizes.get(e.strip().split()[0], 0) for e in m.group(1).split(","))
    return out


def measured_outcomes(path="tmp/excluded_measured.psv"):
    """name -> True when firing is right, False when it is wrong."""
    verdict = {}
    for line in open(path, encoding="utf-8"):
        parts = line.rstrip("\n").rstrip("\r").split("|")
        if len(parts) < 3:
            continue
        name, a, o = parts[0], parts[1].split(), parts[2].split()
        if len(a) < 2 or len(o) < 2:
            continue
        if a[0] == "ok" and (o[0] != "ok" or float(a[1]) < float(o[1])):
            verdict[name] = True
        elif o[0] == "ok" and (a[0] != "ok" or float(a[1]) > float(o[1])):
            verdict[name] = False
    return verdict


def main():
    calib = sys.argv[1] if len(sys.argv) > 1 else "tmp/calib_par.csv"
    sweep_path = sys.argv[2] if len(sys.argv) > 2 else "tmp/gs_runnable_recs.psv"
    ex_sweep_path = sys.argv[3] if len(sys.argv) > 3 else "tmp/gs_excluded_d48.psv"

    measured = {}
    for row in csv.DictReader(open(calib)):
        try:
            measured[row["query"]] = dict(rows=float(row["input_rows"]), recs=float(row["records"]),
                                          tuples=float(row["tuples"]), off=float(row["off_s"]),
                                          force=float(row["force_s"]))
        except (ValueError, KeyError):
            continue
    print(f"{len(measured)} runnable queries with both timings, from {calib}")

    duckdb = fit([(m["rows"], m["tuples"], m["off"] * 1000) for m in measured.values()], 0.25)
    ours = fit([(m["rows"], m["recs"], m["force"] * 1000) for m in measured.values()], 0.75)
    print(f"duckdb (25th): startup {duckdb[0]:.6g} ms, per input row {duckdb[1]:.6g}, per tuple {duckdb[2]:.6g}")
    print(f"ours   (75th): startup {ours[0]:.6g} ms, per input row {ours[1]:.6g}, per record {ours[2]:.6g}")
    print("shipped:       duckdb 0 / 2.324e-05 / 3.981e-06;  ours 0.108542 / 2.445e-05 / 3.961e-05")

    predicted = load_sweep(sweep_path)
    both = [q for q in measured if q in predicted]
    sizes = table_sizes()
    ex_pred = load_sweep(ex_sweep_path) if os.path.exists(ex_sweep_path) else {}
    ex_rows = input_rows("tmp/excluded_all.psv", sizes) if ex_pred else {}
    verdict = measured_outcomes() if os.path.exists("tmp/excluded_measured.psv") else {}
    should = [q for q, v in verdict.items() if v and q in ex_pred and q in ex_rows]
    should_not = [q for q, v in verdict.items() if not v and q in ex_pred and q in ex_rows]
    print(f"{len(both)} with predictions; excluded measured: {len(should)} should fire, "
          f"{len(should_not)} should not\n")

    print("  margin floor  fires  win loss   runnable   kept/should  fired/should-not")
    for margin in (1.2, 1.5, 2.0, 3.0, 5.0):
        for floor in (0.0, 5.0, 20.0):
            total = fires_n = wins = losses = 0.0
            for q in both:
                m = measured[q]
                flat, recs = predicted[q]
                on = fires(m["rows"], flat, recs, duckdb, ours, margin, floor)
                total += m["force"] if on else m["off"]
                if on:
                    fires_n += 1
                    wins += 1 if m["force"] <= m["off"] else 0
                    losses += 1 if m["force"] > m["off"] else 0
            kept = sum(1 for q in should
                       if fires(ex_rows[q], *ex_pred[q], duckdb, ours, margin, floor))
            bad = sum(1 for q in should_not
                      if fires(ex_rows[q], *ex_pred[q], duckdb, ours, margin, floor))
            print(f"  {margin:5.1f} {floor:5.0f}  {fires_n:5.0f} {wins:4.0f} {losses:4.0f}   "
                  f"{total:8.3f}s   {kept:5d}/{len(should)}      {bad:5d}/{len(should_not)}")

    print(f"\n  shipped gate: {sum(measured[q]['force'] if csv_fired(calib, q) else measured[q]['off'] for q in both):.3f}s"
          f"   all stock: {sum(measured[q]['off'] for q in both):.3f}s"
          f"   perfect: {sum(min(measured[q]['off'], measured[q]['force']) for q in both):.3f}s")


_fired_cache = {}


def csv_fired(path, query):
    if not _fired_cache:
        for row in csv.DictReader(open(path)):
            _fired_cache[row["query"]] = row.get("fired") == "1"
    return _fired_cache.get(query, False)


main()
