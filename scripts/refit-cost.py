#!/usr/bin/env python3
"""Re-fit the gate's cost model against measurements from this machine.

    scripts/refit-cost.py <calibration.csv> <table_rows.csv>

DECISIONS O11 said the shipped coefficients were fitted on one machine and do
not transfer, and that they would have to be re-fitted rather than inherited.
This is that procedure, and the first thing it showed is that the error is not
a machine difference at all: the model charges DuckDB 3.9e-5 ms per result
tuple, and for a count(*) DuckDB carries no payload columns through the join, so
its real per-tuple cost is far below that. Over-charging DuckDB makes the gate
fire on queries DuckDB was about to win.

Quantile regression in log space, matching the asymmetry the gate depends on: a
gate must be pessimistic about the engine it picks and optimistic about the one
it rejects, or every estimation error turns into a regression.
"""

import csv
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def load_rows(path):
    rows = {}
    with open(path) as handle:
        for line in handle:
            name, _, size = line.strip().partition(",")
            if size.isdigit():
                rows[name] = int(size)
    return rows


def load_queries(rows):
    """input_rows and flat_tuples per query, from the corpus itself."""
    queries = {}
    with open(ROOT / "tmp" / "ce_runnable.psv") as handle:
        for line in handle:
            parts = line.rstrip("\n").split("|")
            if len(parts) < 3:
                continue
            name, expected, sql = parts[0], parts[1], parts[2]
            match = re.search(r"factorized_count\(\[(.*?)\], \[", sql)
            if not match:
                continue
            total = 0
            for entry in match.group(1).split(","):
                table = entry.strip().strip("'").split()[0]
                total += rows.get(table, 0)
            queries[name] = (total, float(expected))
    return queries


def fit(samples, quantile, with_intercept=True):
    """Least-absolute-deviation-ish fit by coordinate search on a log-space
    quantile loss. Small data and three parameters, so a direct search beats
    pulling in a solver."""

    def loss(params):
        total = 0.0
        for input_rows, output, millis in samples:
            predicted = params[0] + params[1] * input_rows + params[2] * output
            predicted = max(predicted, 1e-6)
            residual = millis - predicted
            # Asymmetric: overshooting is charged (1 - quantile), undershooting
            # `quantile`, which puts the fit above that share of the data.
            total += residual * quantile if residual > 0 else -residual * (1 - quantile)
        return total

    params = [0.0, 1e-5, 1e-5]
    if not with_intercept:
        params[0] = 0.0
    step = [1.0, 1e-5, 1e-5]
    for _ in range(400):
        improved = False
        for i in range(3):
            if i == 0 and not with_intercept:
                continue
            for direction in (1, -1):
                candidate = list(params)
                candidate[i] = max(0.0, candidate[i] + direction * step[i])
                if loss(candidate) < loss(params):
                    params = candidate
                    improved = True
        if not improved:
            step = [s / 2 for s in step]
            if max(step) < 1e-12:
                break
    return params


def main():
    calibration = sys.argv[1] if len(sys.argv) > 1 else "/tmp/calib.csv"
    table_rows = sys.argv[2] if len(sys.argv) > 2 else "/tmp/table_rows.csv"
    queries = load_queries(load_rows(table_rows))

    duckdb_samples = []
    ours_samples = []
    with open(calibration) as handle:
        for row in csv.DictReader(handle):
            sizes = queries.get(row["query"])
            if not sizes:
                continue
            input_rows, flat = sizes
            if row["off_s"]:
                duckdb_samples.append((input_rows, flat, float(row["off_s"]) * 1000.0))
            # Only the queries the gate actually took over measure *our* engine;
            # in the rest 'auto' is the stock plan wearing a different name.
            if row["auto_s"] and row["fired"] == "yes":
                ours_samples.append((input_rows, flat, float(row["auto_s"]) * 1000.0))

    print(f"duckdb: {len(duckdb_samples)} samples, ours: {len(ours_samples)} samples")
    duckdb = fit(duckdb_samples, 0.25)
    print(f"duckdb optimistic (25th): startup {duckdb[0]:.4g} ms, "
          f"per input row {duckdb[1]:.4g}, per flat tuple {duckdb[2]:.4g}")
    ours = fit(ours_samples, 0.75, with_intercept=False)
    print(f"ours pessimistic (75th):  startup {ours[0]:.4g} ms, "
          f"per input row {ours[1]:.4g}, per flat tuple {ours[2]:.4g}")

    # What the fits would have decided, replayed against the same measurements.
    fired = wins = losses = 0
    for row in csv.DictReader(open(calibration)):
        sizes = queries.get(row["query"])
        if not sizes or not row["off_s"] or not row["auto_s"]:
            continue
        input_rows, flat = sizes
        predicted_duckdb = duckdb[0] + duckdb[1] * input_rows + duckdb[2] * flat
        predicted_ours = ours[1] * input_rows
        if predicted_duckdb > 1.5 * predicted_ours:
            fired += 1
            if float(row["off_s"]) >= float(row["auto_s"]):
                wins += 1
            else:
                losses += 1
    print(f"replayed: would fire on {fired}, of which {wins} faster and {losses} slower")


if __name__ == "__main__":
    sys.exit(main())
