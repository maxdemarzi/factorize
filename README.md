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
materialise, the engine is faster on 23 and the gate fires on 35, for **1.71×**
on the corpus. Firing on every match instead would be **0.04×** — 524s against
22s — because for a `count(*)` DuckDB carries no payload columns through a join
and counts empty tuples faster than this engine can build a representation. The
gate exists to tell those two regimes apart, and the coefficients it uses were
fitted on one machine — `scripts/calibrate-synthetic.py` re-fits them on yours,
with the caveat in its docstring that it generates uniform data and the gate's
real difficulty is skew.

| | state |
|---|---|
| f-representation engine (`src/core/`) | working, measured |
| MCV statistics and cost model | working, measured |
| gate (fire/decline decision) | working — re-fitted against in-DuckDB timings, no regression on any query it fires on (DECISIONS D19) |
| `factorized_count()` table function | working — 238 CE queries, 0 mismatches against published sizes and stock DuckDB (DECISIONS D16) |
| optimizer rule | working — matches inner equi-join `count(*)`, `sum()`, `GROUP BY` and `EXISTS`, carries the plan's filters across, and answers identically to `'off'` (DECISIONS D18) |
| `factorize_mode='auto'` | working — fires on the gate's verdict; 600 random join graphs agree with `'off'` |
| memory | no spilling. A representation that will not fit is re-counted over a partition of its join key, slower, never a failure. A bucket that is one skewed value can be split on a second key, off by default (DECISIONS D55) |
| parallelism | working — one thread per bucket of the join key, 3.4x at 8 threads, same answer at every thread count (DECISIONS D20), and no longer given up when the fallback is carried — 1.49x over 42 corpus queries (D54) |

CI builds the extension on Linux, macOS, Windows and Wasm against DuckDB
v1.5.5.

```sql
SET factorize_mode = 'auto';         -- 'force' fires on every match, ignoring the gate
SET factorize_explain = true;        -- say what was taken over, or why not
SELECT count(*) FROM a, b, c WHERE a.x = b.x AND b.y = c.y;
```

Shapes the rule takes over: `count(*)` and `sum()` — several of them at once,
grouped or not — over inner equi-joins of stored tables on integer columns, with
filters DuckDB pushed into or left above the scans. Everything else — outer
joins, non-integer keys, cyclic join graphs, computed join keys — is declined
silently and answered by the stock plan. `SET factorize_explain = true` says
which, and why.

`EXISTS` is matched too, since DuckDB plans it as `count(*)` over `LIMIT 1` over
the join, and so is `count(*)` over any `LIMIT k` — but the gate declines both
unless `factorize_limit` is set. On all 119 runnable CE queries it agreed with
stock and was slower on 93 of them, because DuckDB stops its probe at the first
tuple while this stops at the first non-empty bucket of the join key. On
epinions-like data it is a 2.5× win; on watdiv it is 33× worse (DECISIONS D53).

### Settings

Every one has a description in `duckdb_settings()`; these are the ones worth
knowing about. Four of them — `factorize_limit`, `factorize_second_key` and the
two abandon floors — are built, tested and **off**, each because turning it on
was measured and was worse. They are listed because a switch nobody can find is
the same as a switch that does not exist, not because they are recommended.

| setting | default | what it does |
|---|---|---|
| `factorize_mode` | `off` | `auto` fires on the gate's verdict, `force` on every match, `off` disables the rule |
| `factorize_explain` | `false` | say what was taken over, or why not |
| `factorize_fallback` | `true` | carry the stock plan so an internal error costs time rather than the answer (§7.5) |
| `factorize_limit` | `false` | let the gate consider `EXISTS` and `count(*)` over `LIMIT k` (D53) |
| `factorize_second_key` | `false` | split a bucket that is one skewed key value on a different key instead of failing (D55) |
| `factorize_min_compression` | `0` (off) | abandon to the stock plan when a materialized join is not compressing (D37, D51) |
| `factorize_min_rate` | `0` (off) | abandon when the last materialized join is delivering fewer than this many tuples/ms (D56) |
| `factorize_min_gain` | `1.5` | how much faster the gate must predict this engine to be before firing |
| `factorize_gate_sample_rows` | `16384` | rows sampled per join column for the MCV list; `factorize_gate_exact_stats` scans instead |

### Beyond counting

Four things the representation can answer that an aggregate cannot. `EXISTS` is
now reachable through the rule as well (above); the other three are explicit
table functions, because tuple output would have to carry payload columns the
representation does not hold:

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
  input per partition. Nothing is ever written to disk.

  Skew defeats it, because no number of buckets separates one value from itself
  and every retry refines the *same* key. `factorize_second_key` reaches for a
  different one and partitions inside the bucket, which is sound for the same
  reason the first partition is, and answers queries that otherwise cannot be —
  but it is **off**, because on the corpus it also spends 269s answering a query
  the stock plan has in 13 (DECISIONS D55). What has no answer either way is
  skew on every key at once.
- **An error in the factorized path runs the stock plan instead.** The plan the
  rule replaces is carried rather than dropped, and built into a pipeline the
  executor is not given, so it costs nothing until a failure needs it (§7.5).

  This used to cost the parallelism too, and no longer does. The thread driving
  the fallback held the operator's state lock across the work, so every other
  worker parked on it — and a parked worker is one the executor cannot use to
  run the pipeline being waited for, which hung at four threads. They help run
  it now instead, and the operator is a parallel source whether or not it
  carries a fallback (DECISIONS D54). `factorize_fallback` still turns the
  fallback off, but it is no longer a trade against parallelism.

  `factorize_mode='off'` remains the blunt recovery. `FATAL` and `INTERRUPT` are
  never recovered from: the first leaves nothing to fall back to, and the second
  is you asking it to stop.
- **The cost model's coefficients were fitted on one machine.** They are
  checked in as defaults, not as constants, and they have been wrong in both
  directions (DECISIONS D19, D26). `scripts/calibrate-synthetic.py` re-fits them
  against shapes it generates itself and needs nothing downloaded;
  `scripts/refit-cost.py` re-fits against `scripts/calibrate-gate.sh` output,
  which needs the CE corpus. `factorized_stats(tables, joins)` reports the sizes
  the gate predicts, so a decline can be checked rather than guessed at.
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

And then it was, on data the fit had not seen. Firing under the gate left the
corpus **slower than not factorizing at all** — 24.70s against a 22.17s stock
baseline — and no setting of any threshold repaired it. Two rounds of trying to
build a better decision rule (D37, D38) produced a run-time check that survives
as a setting and a negative result that closes the question: no statistic of the
representation separates the queries that lose from the ones where factorizing
is the only way to get an answer, because they build nearly the same
representation.

The gap was never the decision. It was one line in the operator: the slice count
was read from the thread count while the operator was not allowed to use more
than one thread, so a single thread made **eight full filtering passes over the
input** for no parallelism at all.

Fixing that got the corpus to 1.39×. The rest of the gap was one statistic. The
gate compares two predictions and both are driven by an estimated join size, but
the error does not bias them evenly — DuckDB's predicted cost is dominated by
result *tuples*, ours by *records*, and records grow far more slowly, so
under-estimating a skewed join shrinks DuckDB's side much harder than ours.
Every such error argues against firing.

D13 specified the fix in 2 sentences — a most-common-value list per join
column — and the code that consumes it was written. The code that *supplies* it
to the optimizer never was, and an empty list "degrades to exactly the old
textbook estimator", silently. The gate spent its whole life running on the
estimator that list was added to replace. It now samples 16,384 rows per join
column, and asks the catalog as a second opinion whenever the sample says no.

| | corpus, 119 queries | vs stock |
|---|---|---|
| factorize off | 22.17 s | — |
| auto, before | 24.70 s | 0.88× |
| **auto, now** | **12.95 s** | **1.71×** |
| a gate with perfect knowledge | 10.98 s | 2.02× |
| firing on every match | 524.73 s | 0.04× |

Measured end to end, so 12.95 s includes what the gate spends sampling.
`watdiv_217_01` went 6.820 s → 1.502 s. DECISIONS D38–D41, worth reading for how
long the symptom was mistaken for the disease.

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
  runtime bail-out, not a better decision rule. Two run-time bail-outs are now
  built and neither works, which is why both default to off:

  - `factorize_min_compression` abandons when a materialized join is not
    compressing. Measured out of sample it abandons queries DuckDB cannot
    answer at all, and D38 has a matched pair showing why no statistic of the
    *representation* can decide this.
  - `factorize_min_rate` abandons on tuples delivered per millisecond, which is
    at least the unit DuckDB's own cost model is stated in. It separated six
    wins from four losses when it was fitted and did not survive being measured
    again: `hetio_acyclic_205_03` was recorded at 1.11e5 tuples/ms and
    re-measures at 34,296, inside the loss range, so a floor abandons a query
    this engine answers in 129 s and the stock plan never does (D56).

  That is ten criteria now. Every quantity visible from inside our own
  execution separates the queries it was derived from and overlaps on the next
  ones, because what separates them is how fast DuckDB would have been — and
  D56 records why running both plans to find out is not reachable from an
  extension.
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
