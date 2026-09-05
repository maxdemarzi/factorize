# factorize

A DuckDB extension that answers `SELECT count(*)` over many-to-many equi-join
graphs without materialising the join, by keeping intermediates in factorised
(f-)representations.

Clean-room implementation of Lehner & Neumann, *The Data World Is Not Flat:
Efficient Factorized Execution for Relational Systems* (PVLDB 19(11):3006–3019,
2026). Derived from the paper only — no GPLv3 source was read or ported
([DECISIONS](DECISIONS.md) D1). MIT licensed.

## Status: plain SQL is accelerated automatically, on the queries where it pays

The engine, the cost model, an explicit `factorized_count()` table function and
the optimizer rule all work against real DuckDB storage. With
`SET factorize_mode='auto'` an ordinary `count(*)` over an equi-join is taken
over when the gate predicts a win, and left alone otherwise.

What that is worth, measured on a release build (DECISIONS D19):

```
SELECT count(*) FROM watdiv1052651 a, watdiv1052651 b,
                     watdiv1052651 c, watdiv1052651 d
 WHERE a.s = b.s AND b.s = c.s AND c.s = d.s;

auto: 10,835,546,035,024   in 24 seconds (8 threads)
off:  no answer in 180 seconds
```

And what it is not worth: across the 119 CE queries whose results DuckDB *can*
materialise, the gate fires on one of them, because for a `count(*)` DuckDB
carries no payload columns through a join and counts empty tuples faster than
this engine can build a representation. Geomean 1.06x, no query regressing.
The gate exists to tell those two regimes apart, and the coefficients it uses
were fitted on one machine — `scripts/refit-cost.py` re-fits them on yours.

| | state |
|---|---|
| f-representation engine (`src/core/`) | working, measured |
| MCV statistics and cost model | working, measured |
| gate (fire/decline decision) | working — re-fitted against in-DuckDB timings, no regression on any query it fires on (DECISIONS D19) |
| `factorized_count()` table function | working — 238 CE queries, 0 mismatches against published sizes and stock DuckDB (DECISIONS D16) |
| optimizer rule | working — matches inner equi-join `count(*)`, carries the plan's filters across, and answers identically to `'off'` (DECISIONS D18) |
| `factorize_mode='auto'` | working — fires on the gate's verdict; 600 random join graphs agree with `'off'` |
| memory | no spilling. A representation that will not fit is re-counted over a partition of its join key: slower, never a failure |
| parallelism | working — one thread per bucket of the join key, 3.4x at 8 threads, same answer at every thread count (DECISIONS D20) |

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
stock plan. `SET factorize_explain = true` says which, and why.

### Beyond counting

Four things the representation can answer that an aggregate cannot, each an
explicit table function rather than something the optimizer rule fires on:

```sql
-- Does this join have any tuple? Stops at the first one it finds.
SELECT * FROM factorized_exists(['a', 'b'], ['a.x = b.x']);

-- The join itself. The third argument is a limit, and it is the interesting
-- part: a hundred rows out of a join with a trillion, without building the
-- trillion. Stock DuckDB materialises the hash-join intermediates regardless
-- of the LIMIT.
SELECT * FROM factorized_tuples(['a', 'b'], ['a.x = b.x'], 100);

-- One row per group, counted without enumerating the tuples in it. Works when
-- the grouping key is at the top of the f-tree, and declines when it is not.
SELECT * FROM factorized_group_count(['a', 'b'], ['a.x = b.x'], 'a.x');
```

### Limitations worth knowing before you switch it on

- **No spilling.** A representation too large for the memory budget is
  re-counted over a partition of its join key, which costs a pass over the
  input per partition. Skew is the case that defeats it: no number of buckets
  separates one value from itself.
- **An error in the factorized path fails the query.** The plan asks for a
  fallback to the stock plan instead (§7.5), and this design cannot give one —
  the rule drops DuckDB's scans at optimize time, so by execution there is no
  stock plan left to fall back to. What protects you is that the matcher
  declines anything it cannot model, rather than that failures are recoverable;
  `factorize_mode='off'` is the recovery.
- **The cost model's coefficients were fitted on one machine.** They are
  checked in as defaults, not as constants. `scripts/refit-cost.py` re-fits
  them against `scripts/calibrate-gate.sh` output from yours.
- **One DuckDB version.** The C++ extension API is version-locked; this builds
  against v1.5.5 and no other.
- **Not built for WASM,** deliberately — see the CI configuration.

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
