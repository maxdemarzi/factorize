# factorize

A DuckDB extension that answers `SELECT count(*)` over many-to-many equi-join
graphs without materialising the join, by keeping intermediates in factorised
(f-)representations.

Clean-room implementation of Lehner & Neumann, *The Data World Is Not Flat:
Efficient Factorized Execution for Relational Systems* (PVLDB 19(11):3006–3019,
2026). Derived from the paper only — no GPLv3 source was read or ported
([DECISIONS](DECISIONS.md) D1). MIT licensed.

## Status: plain SQL is accelerated on request, not yet by default

The engine, the cost model, an explicit `factorized_count()` table function and
the optimizer rule all work against real DuckDB storage. Ordinary `count(*)`
queries are taken over and answered by the factorized engine — but only with
`SET factorize_mode='force'`. **The rule does not fire on its own yet**: that
needs the cost gate wired into the optimizer, and a query whose
f-representation runs out of memory needs to fall back to the stock plan
instead of failing.

| | state |
|---|---|
| f-representation engine (`src/core/`) | working, measured |
| MCV statistics and cost model | working, measured |
| gate (fire/decline decision) | working, measured (standalone harness) |
| `factorized_count()` table function | working — 238 CE queries, 0 mismatches against published sizes and stock DuckDB (DECISIONS D16) |
| optimizer rule under `factorize_mode='force'` | working — matches inner equi-join `count(*)`, carries the plan's filters across, and answers identically to `'off'` (DECISIONS D18) |
| `factorize_mode='auto'` | **not built** — behaves as `'off'`. Needs the cost gate, and fallback-on-overflow rather than an error |
| parallelism | **not built** — single-threaded |

CI builds the extension on Linux, macOS, Windows and Wasm against DuckDB
v1.5.5.

```sql
SET factorize_mode = 'force';        -- 'auto' is not wired to a gate yet
SET factorize_explain = true;        -- say what was taken over, or why not
SELECT count(*) FROM a, b, c WHERE a.x = b.x AND b.y = c.y;
```

Shapes the rule takes over: an ungrouped `count(*)` over inner equi-joins of
stored tables on integer columns, with filters DuckDB pushed into or left above
the scans. Everything else — outer joins, `GROUP BY`, non-integer keys, cyclic
join graphs, computed join keys — is declined silently and answered by the
stock plan.

## What it does

An f-representation stores a join result as a tree of independent subtrees
rather than a flat list of tuples, so a result of *n* tuples can often be held
in far fewer records. Counting then walks the tree instead of the tuples.

On the CE benchmark's hetio dataset, a query returning **10,091,982,222,905**
tuples is answered in **1.3 seconds** from 5.06M records — a compression of
1,993,452×. Stock DuckDB does not finish it inside 180 seconds, and at its
measured rate of 2.0e7 result tuples/second it would need roughly five days.

Correctness has held throughout: **0 wrong answers across ~2,200 query
executions**. Every failure is a decline, which is safe.

## The gate is the interesting part

Factorization is not a general accelerator. Fired unconditionally on the CE
corpus the engine is *slower* than DuckDB. The value is entirely in predicting
which queries it helps, and the shape of that prediction turned out to be the
hard problem:

- Compression is the wrong criterion. Speedup is compression × K, and K — the
  per-record speed advantage — spans **84×** across datasets. No threshold on
  compression is right for all of them.
- So the gate estimates **time for both engines** and compares them.
- Estimating the factorized side needs skew: the textbook formula
  `|R⋈S| = |R||S|/max(V_R,V_S)` under-predicts a skewed join by up to **2814×**,
  because five key values can carry 90% of a 1.9e8-tuple join. `src/core/stats`
  keeps a 128-entry most-common-value list per column.

Measured on 194 CE queries with both engines timed:

| gate | fires | speedup | geomean | regressions |
|---|---|---|---|---|
| compression ≥ 50× | 30 | 1.65× | 2.28× | 2 |
| **predicted time** | **47** | **2.26×** | **3.37×** | 4 |
| oracle (perfect knowledge) | 84 | 5.87× | 5.78× | 0 |

Five-fold cross-validation reproduces this exactly, so it is not overfit.

## Read FINDINGS.md

[FINDINGS.md](FINDINGS.md) is the substance of the project — what was measured,
including several things that did not work and are deliberately absent from the
code. The one that reframes everything:

**The CE benchmark disables every query whose result exceeds 1e9 tuples**,
removing 344 of 360 hetio queries. The live corpus is, by construction, the
regime a flat engine already handles. In the excluded regime the comparison is
not close — 182 of 344 answered, 0 wrong, and DuckDB times out on eight of nine
at a 180-second cap. [DECISIONS](DECISIONS.md) D15 repoints the project there.

Also worth knowing before trusting any number here:

- **O11** — the cost model's coefficients are fitted on one machine and do not
  transfer. `FitEngineCost` and the harness's `--calibrate` mode are the
  supported way to replace them.
- **O12 / F19** — flat estimation over-predicts on uniform data by up to 84×.
  Three fixes were measured and rejected; it needs a joint-presence sketch or a
  runtime bail-out, not a better decision rule.
- Benchmarks come from one laptop, not the paper's 64-core Xeon.

## Building and testing

The core has no DuckDB dependency and builds on its own:

```sh
scripts/core-test.sh          # builds and runs the unit tests, asan and -O2
```

The extension builds through the standard DuckDB template flow:

```sh
make                          # ./build/release/duckdb and the extension
```

The benchmark harness needs the CE corpus (~5.3 GB, downloaded from TUM):

```sh
scripts/fetch-ce-data.sh
scripts/run-ce.sh tmp/out.csv acyclic hetio watdiv yago
scripts/run-duckdb-ce.sh tmp/duckdb.csv acyclic hetio watdiv yago
```

`--gate-only` decides without running, which is what an optimizer does: 344
decisions take 0.85 s against roughly five hours to execute the same queries.

## Layout

```
src/core/        engine and cost model, no DuckDB headers
src/duckdb/      extension glue (Phase 0 spike)
src/bench/       standalone benchmark harness
test/unit/       core tests, checked against brute-force ground truth
scripts/         build, benchmark and data-acquisition
DECISIONS.md     load-bearing decisions, with dates and open items
FINDINGS.md      what was measured, including the negative results
PLAN_ERRATA.md   errors found in the original plan
```
