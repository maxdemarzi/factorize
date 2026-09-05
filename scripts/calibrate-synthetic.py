#!/usr/bin/env python3
"""Fit the gate's cost model against shapes this machine can generate itself.

    scripts/calibrate-synthetic.py [--fit] [--json out.json]

WHY THIS EXISTS. The two calibration scripts that came before it
(refit-cost.py, calibrate-gate.sh) both read tmp/ce_runnable.psv and need the
CE corpus -- 5.3 GB nobody had downloaded. So on a fresh checkout the shipped
coefficients could not be re-fitted, only inherited, which is the one thing
DECISIONS O11 says not to do with them. This needs nothing but `range()`.

WHAT IT MEASURES. A grid of join shapes -- stars and chains, varying relation
count, key domain and table size -- timed under factorize_mode='off' and
='force', with the f-representation's actual record count read out of
factorized_stats(). That is the (input_rows, records, milliseconds) triple the
model is defined over, and the (input_rows, flat_tuples, milliseconds) triple
DuckDB's half is defined over.

WHAT IT CANNOT TELL YOU, and this is not a footnote. The data is uniform. Real
corpora are skewed, and F18 records that our flat estimate is *already* weakest
on uniform data, so a fit from here describes the friendly case. It is a way to
find a coefficient that is wrong by an order of magnitude; it is not a
substitute for calibrating against the corpus a user's queries resemble. Treat
a fit from this as a floor on the error, never as a ceiling.

Build with `make release` first: a relassert build carries ASAN and UBSAN, and
a timing taken under those is not a timing -- measured here, assertions inflate
DuckDB by 7-11x, which is enough on its own to invert a conclusion.
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DUCKDB = Path(os.environ.get("FACTORIZE_DUCKDB", ROOT / "build" / "release" / "duckdb"))

RUN_TIME = re.compile(r"Run Time \(s\): real ([0-9.]+)")

#: The second entry is what the third column of the report is measured under.
#: 'force' answers "is the engine faster"; 'auto' answers "does a user get it",
#: and those are different questions -- the gate sits between them and this
#: whole script exists because it was answering no.
MODES = ("off", os.environ.get("FACTORIZE_COMPARE", "force"))


def star(relations, domain, rows):
    """A hub of `domain` keys with `relations - 1` arms of `rows` rows each."""
    setup = [f"CREATE OR REPLACE TABLE hub AS SELECT i AS k FROM range({domain}) t(i);"]
    names = ["hub"]
    for i in range(relations - 1):
        setup.append(
            f"CREATE OR REPLACE TABLE arm{i} AS "
            f"SELECT i % {domain} AS k, i AS v FROM range({rows}) t(i);"
        )
        names.append(f"arm{i}")
    predicates = " AND ".join(f"hub.k = arm{i}.k" for i in range(relations - 1))
    return setup, names, predicates


def unique(relations, domain, rows):
    """A join on a unique key: no fan-out anywhere, so nothing to factor out.

    The case where factorization has nothing to exploit and pays for structure
    it never uses -- measured 3-4x SLOWER than stock. A calibration grid made
    only of fan-out shapes cannot see this, and a fit validated on such a grid
    is validated against the cases it was always going to win.
    """
    setup = []
    names = []
    for i in range(relations):
        setup.append(f"CREATE OR REPLACE TABLE u{i} AS SELECT i AS k, i AS v FROM range({rows}) t(i);")
        names.append(f"u{i}")
    predicates = " AND ".join(f"u0.k = u{i}.k" for i in range(1, relations))
    return setup, names, predicates


def chain(relations, domain, rows):
    """r0 - r1 - r2 ...: each relation joins only its neighbour, on a second column."""
    setup = []
    names = []
    for i in range(relations):
        setup.append(
            f"CREATE OR REPLACE TABLE r{i} AS "
            f"SELECT i % {domain} AS a, (i * 7) % {domain} AS b FROM range({rows}) t(i);"
        )
        names.append(f"r{i}")
    predicates = " AND ".join(f"r{i}.b = r{i + 1}.a" for i in range(relations - 1))
    return setup, names, predicates


#: Above this the *stock* side stops being measurable in reasonable time, and a
#: calibration run that hangs on its widest shape produces no fit at all. It is
#: also the honest boundary of what this can calibrate: shapes whose flat result
#: DuckDB cannot enumerate are exactly the ones the gate exists to catch, and no
#: paired timing can be taken for them because one side never finishes.
MAX_FLAT_TUPLES = 3e7


BUILDERS = {"star": star, "chain": chain, "unique": unique}


def predicted_flat(kind, relations, domain, rows):
    """Analytic join size for a uniform shape, to skip what cannot be timed."""
    if kind == "unique":
        return rows
    per_key = rows / float(domain)
    if kind == "star":
        return domain * per_key ** (relations - 1)
    return rows ** relations / float(domain) ** (relations - 1)


SHAPES = []
for kind, counts, domains in (("star", (3, 4, 5), (100, 1000, 10000)),
                              ("chain", (3, 4), (1000, 10000)),
                              # Shapes the engine LOSES on, and they are here on
                              # purpose. A grid of fan-out shapes is a grid the
                              # engine was always going to win, so a fit checked
                              # against it is checked against nothing: every
                              # error that makes the gate fire too eagerly is
                              # invisible. `unique` has no fan-out at all, and
                              # `star` at a tiny domain is below the floor.
                              ("unique", (2, 3), (0,))):
    for relations in counts:
        for domain in domains:
            for rows in (10000, 100000, 200000):
                if predicted_flat(kind, relations, domain, rows) <= MAX_FLAT_TUPLES:
                    SHAPES.append((kind, relations, domain, rows))


def run(script):
    # On stdin rather than -c: dot commands (.timer) are shell directives the
    # -c path does not interpret, so a script passed that way silently produces
    # no timings at all -- which reads exactly like a query that was too fast.
    result = subprocess.run(
        [str(DUCKDB), "-noheader", "-list", "-init", "/dev/null"],
        input=script,
        capture_output=True,
        text=True,
        timeout=600,
    )
    return result.stdout, result.stderr


def measure(kind, relations, domain, rows, repeats=3):
    setup, names, predicates = BUILDERS[kind](relations, domain, rows)
    froms = ", ".join(names)
    query = f"SELECT count(*) FROM {froms} WHERE {predicates};"
    tables = ", ".join(f"'{n}'" for n in names)
    joins = ", ".join(f"'{p.strip()}'" for p in predicates.split(" AND "))

    lines = list(setup)
    lines.append("SET disabled_optimizers='compressed_materialization';")
    # The sizes first, without the timer, so the stats call is not itself timed.
    lines.append(f"SELECT 'STATS', * FROM factorized_stats([{tables}], [{joins}]);")
    for mode in MODES:
        # The timer goes on *after* the SET, because a SET emits a Run Time line
        # of its own and it is ~0. Taken as a minimum, those zeros become the
        # whole measurement: the first version of this reported 1.0ms for both
        # modes on a query that takes 94ms and 3.5ms.
        lines.append(f"SET factorize_mode='{mode}';")
        lines.append(".timer on")
        # One warm-up run per mode, discarded: the first touch of a page is not
        # the cost of the query, and comparing a cold mode against a warm one
        # measures the order they were run in.
        for _ in range(repeats + 1):
            lines.append(query)
        lines.append(".timer off")

    out, err = run("\n".join(lines))
    if "STATS" not in out:
        return None
    stats_line = [l for l in out.splitlines() if l.startswith("STATS")][0].split("|")
    times = [float(m) for m in RUN_TIME.findall(err + out)]
    if len(times) != len(MODES) * (repeats + 1):
        # Exactly two blocks of repeats+1 are expected. Anything else means the
        # script did not run the way it was written, and a fit from a misaligned
        # split is worse than no fit at all.
        return None
    blocks = [times[i * (repeats + 1) + 1:(i + 1) * (repeats + 1)] for i in range(len(MODES))]
    off, force = blocks[0], blocks[1]
    if kind == "star":
        input_rows = domain + (relations - 1) * rows
    else:
        input_rows = relations * rows
    return {
        "kind": kind,
        "relations": relations,
        "domain": domain,
        "rows": rows,
        "input_rows": input_rows,
        "flat_tuples": int(stats_line[1]),
        "records": int(stats_line[2]),
        "bytes": int(stats_line[3]),
        "off_ms": min(off) * 1000.0,
        "force_ms": min(force) * 1000.0,
    }


def fit(samples, x1, x2, y, quantile):
    """Least-squares with a free intercept, then shifted to the wanted quantile.

    The shipped `ours` model pins startup_ms to 0, which is why its per-input-row
    term reads 1.225e-3: with no intercept to hold the fixed cost, the per-row
    slope absorbs it, and then over-predicts by that same fixed cost times every
    row of a large input. A free intercept is the difference between a model
    that is merely imprecise and one that cannot represent the shape at all.
    """
    n = len(samples)
    if n < 4:
        return None
    # Normal equations for y = c + a*x1 + b*x2.
    sums = [[0.0] * 4 for _ in range(3)]
    for s in samples:
        row = [1.0, s[x1], s[x2]]
        for i in range(3):
            for j in range(3):
                sums[i][j] += row[i] * row[j]
            sums[i][3] += row[i] * s[y]
    for col in range(3):
        pivot = max(range(col, 3), key=lambda r: abs(sums[r][col]))
        if abs(sums[pivot][col]) < 1e-18:
            return None
        sums[col], sums[pivot] = sums[pivot], sums[col]
        scale = sums[col][col]
        sums[col] = [v / scale for v in sums[col]]
        for r in range(3):
            if r != col:
                factor = sums[r][col]
                sums[r] = [v - factor * w for v, w in zip(sums[r], sums[col])]
    c, a, b = sums[0][3], sums[1][3], sums[2][3]
    a = max(a, 0.0)
    b = max(b, 0.0)
    # Shift the intercept until the wanted share of the data sits below the line.
    residuals = sorted(s[y] - (a * s[x1] + b * s[x2]) for s in samples)
    index = min(len(residuals) - 1, max(0, int(round(quantile * (len(residuals) - 1)))))
    return {"startup_ms": max(residuals[index], 0.0), "per_input_row_ms": a, "per_output_ms": b}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fit", action="store_true", help="report fitted coefficients")
    parser.add_argument("--json", help="write the raw measurements here")
    args = parser.parse_args()

    if not DUCKDB.exists():
        print(f"no duckdb binary at {DUCKDB} (make release)", file=sys.stderr)
        return 2

    samples = []
    for shape in SHAPES:
        got = measure(*shape)
        if not got:
            print(f"-- {shape}: no measurement", file=sys.stderr)
            continue
        samples.append(got)
        print(
            f"{got['kind']:5s} n={got['relations']} dom={got['domain']:<6d} rows={got['rows']:<7d} "
            f"in={got['input_rows']:<8d} tuples={got['flat_tuples']:<12d} recs={got['records']:<9d} "
            f"off={got['off_ms']:8.2f}ms force={got['force_ms']:8.2f}ms "
            f"speedup={got['off_ms'] / max(got['force_ms'], 1e-9):6.2f}x",
            flush=True,
        )

    if args.json:
        Path(args.json).write_text(json.dumps(samples, indent=2))

    if not args.fit or len(samples) < 8:
        return 0

    # Half the shapes fit, half held out, split by index so both halves span the
    # grid rather than one getting all the stars. A fit that is only checked on
    # the points it was fitted to is not checked.
    train = [s for i, s in enumerate(samples) if i % 2 == 0]
    test = [s for i, s in enumerate(samples) if i % 2 == 1]

    ours = fit(train, "input_rows", "records", "force_ms", 0.75)
    theirs = fit(train, "input_rows", "flat_tuples", "off_ms", 0.25)
    print()
    for name, model, x2, y in (("ours", ours, "records", "force_ms"),
                               ("duckdb", theirs, "flat_tuples", "off_ms")):
        if not model:
            print(f"{name}: no fit")
            continue
        print(f"{name} {{{model['startup_ms']:.6g}, {model['per_input_row_ms']:.4g}, "
              f"{model['per_output_ms']:.4g}}}")
        errors = []
        for s in test:
            predicted = (model["startup_ms"] + model["per_input_row_ms"] * s["input_rows"] +
                         model["per_output_ms"] * s[x2])
            errors.append(predicted / max(s[y], 1e-6))
        errors.sort()
        print(f"  held out (n={len(test)}): predicted/actual median {statistics.median(errors):.2f}x, "
              f"range {errors[0]:.2f}x..{errors[-1]:.2f}x")
    return 0


if __name__ == "__main__":
    sys.exit(main())
