# Findings

Empirical results that change how the plan should be executed. Derived from the
FactDB artifact's **published measurement logs** (`bench/results/paper/ce`), not
from its source — see DECISIONS.md D1.

Reproduce with:

```bash
python3 scripts/parse_paper_results.py --factdb ../FactDB -o tmp/ce_paper.csv
```

---

## F1 — The parse is validated against the paper's own prose

Before drawing any conclusion, the extracted per-query medians were checked
against the four figures section 6.2 states in words. All four reproduce:

| Quantity (FactDB factorized vs FactDB flat) | Paper says | Parsed |
|---|---|---|
| CE acyclic, all queries | −15% | **−15%** (0.849x, n=1803) |
| CE acyclic, flat >10 ms | +6.9% | **+7%** (1.074x, n=883) |
| CE acyclic, flat >100 ms | +52% | **+53%** (1.527x, n=200) |
| CE cyclic, all queries | −32% | **−33%** (0.669x, n=756) |
| naive vs improved root-to-leaf merge | naive +2.4% | naive **+2.6%** (n=2350) |

The numbers below rest on the same parse.

---

## F2 — Almost none of the "orders of magnitude over DuckDB" is factorization ⚠️

On CE acyclic queries where stock DuckDB takes >100 ms (n=734):

| Comparison | Geomean |
|---|---|
| FactDB **factorized** vs stock DuckDB | 5.62x |
| FactDB **flat** vs stock DuckDB | **5.25x** |
| FactDB factorized vs FactDB flat | **1.07x** |

FactDB's *flat* engine — no factorization at all — is already 5.25x faster than
DuckDB. Factorization adds 1.07x on top. The headline gap is the baseline gap:
code generation, an exact cardinality oracle (`estimates.db`) instead of
estimates, and Umbra-class engineering.

**A DuckDB extension inherits none of that.** It runs on DuckDB's vectorized
substrate, with DuckDB's plans and DuckDB's estimates. The only quantity it can
compete for is the last row: the factorization-only contribution.

This is the number plan section 6 says nobody has published ("what fraction of
the paper's speedup survives without code generation?"). It can be estimated from
the authors' own logs before writing any engine, and the answer is: the
factorization component is real but modest, and it is *not* what produces the
headline.

## F3 — Consequently, the Phase 1 exit/kill criterion is mis-specified

The plan's Phase 1.5 gate reads:

> **Exit:** on CE acyclic queries where stock DuckDB takes >100 ms, the
> interpreted-factorized engine is **>=5x faster than stock DuckDB** and
> **>=1.5x faster than your own flat baseline**.
>
> **Kill:** if it is **<2x faster than stock DuckDB** on that set, stop the
> project.

Measured against that bar, **FactDB itself fails** — with code generation and a
perfect cardinality oracle. Its factorization contributes 1.07x geomean over its
own flat baseline on exactly that set, well under the required 1.5x. Its 5.62x
over DuckDB is carried by the baseline gap, which no extension can reproduce.

So the criterion would fire on a correct, well-implemented engine, and kill the
project for the wrong reason: it measures the substrate, not the idea.

**Proposed replacement**, isolating what the extension actually controls —
factorized vs flat on identical infrastructure, which is how the paper reports
its own results:

| Bucket (CE acyclic, by flat-baseline runtime) | FactDB achieves | Proposed bar |
|---|---|---|
| all queries | 0.85x | no bar (a loss here is expected and is the gate's job) |
| flat >10 ms | 1.07x | >= 1.0x |
| flat >100 ms | 1.53x | **>= 1.3x — exit** |
| flat >1000 ms | ~2.2x | >= 1.5x |

**Kill** if the engine cannot beat its own flat baseline by >=1.15x on the
flat >100 ms bucket: that is comfortably below what codegen-free factorization
should deliver, and failing it means the tree overheads have eaten the idea.

Keep a *separate*, non-gating measurement against stock DuckDB. It is what users
experience and what Phase 5's gate is tuned against, but it is not a verdict on
factorization.

## F4 — Bottom-inserts are essentially the entire benefit

The ablation ladder (single-threaded, left-deep, each feature added in turn,
versus FactDB's own flat engine on the same infrastructure):

| Configuration | All acyclic | Acyclic, flat >100 ms |
|---|---|---|
| top-insert only | 0.87x | **0.98x** |
| + **bottom-insert** | 1.45x | **1.91x** |
| + caching | 1.51x | 2.00x |
| + inlining | 1.68x | 2.22x |

Top-inserts alone are worth *nothing* — 0.98x, a wash, matching the paper's own
observation that top-insert-only plans "perform slightly worse than the
single-threaded flat baseline". Adding bottom-inserts takes it to 1.91x. Caching
and inlining add 16% between them.

Three consequences:

1. **The bottom-insert-capable child list (plan section 4.3) is the whole
   project.** It is already the design's crux; this quantifies it. Everything
   else is a rounding error by comparison.
2. **The concrete claim against prior work is sharp.** Kalumin & Deshpande put
   f-representations into DuckDB but their contiguous offset layout cannot do
   bottom-inserts (plan section 2.3) — i.e. they are in the 0.98x row. That
   asymmetry, not raw speed, is the thing to demonstrate.
3. **Do not sequence top-inserts first and treat bottom-inserts as a follow-up.**
   A milestone that ships top-inserts alone has, measurably, nothing to show.

## F5 — The improved root-to-leaf strategy is the slower default

Confirmed: naive merging is 2.6% faster on geomean across 2350 queries. The plan
already says to implement both and default to the improved variant for
robustness; that remains right, but the default costs a measurable 2.6% and the
per-query choice (plan Phase 5) is worth having.

## F6 -- Equality propagation is what decides the f-tree's shape

`a.s = b.s and b.s = c.s` puts all three attributes in one equivalence class, so
the second join may be keyed on `a.s` just as correctly as on `b.s`. Which one is
chosen is not a tuning detail; it decides whether factorization does anything at
all.

Keying on the most recently added relation nests every relation under the last,
producing a chain in which nothing is independent. Keying on the shallowest
equivalent attribute already in the tree makes the new subtree a sibling --
independent, hence a Cartesian product. Measured on `hetio_acyclic_202` (5
relations, 176M result tuples):

| probe key | f-tree | records | bytes | time |
|---|---|---|---|---|
| most recently added relation | `{A,B}({C,D}({E,F}({G,H}({I,J}))))` | 194,012,944 | 5.3 GB | 8564 ms |
| shallowest equivalent | `{A,B}({C,D},{E,F},{G,H},{I,J})` | 3,496,892 | 70 MB | 412 ms |

The second shape is the paper's own running example (Figure 3). This is ordinary
optimizer behaviour -- DuckDB propagates equality the same way -- but without it
a correct factorized engine measures as worthless, because a chain has nothing to
factor out. Any Phase 1 measurement taken before this is meaningless.

A related consequence: to keep the accumulated tree on top, the two insert modes
must take their arguments *swapped*, since top-insert puts the probe on top and
bottom-insert the build. With that done, both modes produce identical trees and
identical record counts, differing only in which side is indexed -- the paper's
`L (top-insert) R == R (bottom-insert) L`, confirmed on real data.

## F7 -- Dead records, not subtree duplication, were the dominant cost

An inner join drops a probe tuple that matches nothing. A factorized join leaves
a *record* behind for it: one whose subtree encodes no tuples, contributes 0 to
the count, and is therefore invisible to correctness. It is not invisible to
cost. The next join copies it, the one after copies the copies, and the
representation grows while the result shrinks.

Measured on `watdiv_acyclic_203_10` (6 relations, 1,811 result tuples):

| after join | records before | records after |
|---|---|---|
| 1 | 107,899 | 107,899 |
| 2 | 197,899 | 53,697 |
| 3 | 212,057 | 56,517 |
| 4 | 222,058 | 10,535 |
| 5 | **234,786** (32.7 MB) | **10,419** (0.6 MB) |

Skipping zero-size subtrees during iteration is always correct for an inner join,
since such a subtree encodes no tuples. Sizes have to be computed before pruning
is armed, because an uncomputed size cache is indistinguishable from a size of
zero. 22x fewer records, 54x less memory, and it cascades: downstream joins probe
2,820 rows instead of 14,158.

**This bears on plan section 9.2**, which says not to chase d-representations in
v1 because subtree duplication will "eat a month". That advice holds, but the
duplication that actually dominated here was *dead* records rather than live
shared subtrees, and pruning them is a few dozen lines rather than a month. Do
this before concluding anything about d-trees.

## F8 -- First measured Phase 1 distribution: sharply bimodal

**PROVISIONAL -- taken from a run that was still in progress.** The figures below
are a snapshot at 102 queries of a larger batch; they are directionally sound but
not the final distribution. (The snapshot was mistaken for a completed run
because `pgrep` matches the 15-character truncated process name, so
`pgrep -c factorize-standalone` silently returned zero. Use `pgrep -f`.)

102 CE acyclic queries (hetio, watdiv, yago), single-threaded, 8-core laptop,
factorized vs our own flat baseline on identical infrastructure -- same arena,
same chaining hash table, same plan, same machine.

**Correctness: 0 wrong answers.** 93 matched the CE oracle; 9 declined because
the f-representation hit its 4 GB cap. That distinction is not cosmetic. A
decline is safe -- the extension falls back to the stock plan and the user gets
a correct, slower answer. A wrong count would sink the project. Any analysis
that reports the two together is misleading, and the first version of
`scripts/analyze-ce.py` did exactly that until it was fixed.

| bucket (by flat-baseline runtime) | n | top | bottom | best-of | p10 | p90 | wins |
|---|---|---|---|---|---|---|---|
| all | 83 | 0.41x | 0.50x | 0.54x | 0.23 | 1.43 | 10/83 |
| flat > 100 ms | 47 | 0.57x | 0.65x | 0.73x | 0.23 | 7.34 | 10/47 |
| flat > 1000 ms | 6 | 3.58x | 4.15x | **4.75x** | 0.28 | 38.10 | 4/6 |

Plus **10 queries the flat baseline could not complete at all** (833M, 815M,
737M result tuples). Those are unqualified factorization wins and are *not*
included in the ratios above; excluding them silently is what made an earlier
partial run read 3-4x worse than it was.

Representation size tells the same story: median 0.4x (the f-representation is
*larger* than the flat result), p90 131x, max 3049x.

**The distribution is bimodal, not centrally located.** Factorization loses on
most queries and wins by one to three orders of magnitude on a minority. A
geometric mean over the whole corpus is close to meaningless here -- it
describes no actual query. This is the empirical case for plan Phase 5: the
gate is not a safety feature bolted on at the end, it is the mechanism that
converts a bimodal distribution into a usable one, and it matters more than any
constant-factor tuning of the engine.

Caveats that bound this result:

- It is measured against *our own* flat baseline. If that baseline is weak, the
  ratio flatters factorization. Tracked as DECISIONS.md O8 and resolved by
  comparing it against stock DuckDB at `threads=1` on the same machine.
- Single-threaded, 8 cores, three of the six CE datasets. The paper's numbers
  are 64-core.
- **The comparison currently disadvantages the factorized engine at the final
  join.** The flat baseline counts matches directly on its last join without
  materializing the result, as any real engine would for `count(*)`. The
  factorized engine materializes the complete output f-representation and only
  then counts it. So the factorized side does strictly more work at the last
  step, and the numbers above are if anything pessimistic.

  Fixing that is a real optimization rather than a benchmarking tweak: section
  4.5 notes the aggregate is usually the topmost operator, so the count should
  be *fused* into the last join and the final representation never built. The
  cost of not doing this is proportional to the final f-representation's size,
  which is largest exactly where compression is worst -- i.e. it penalises the
  queries factorization already loses. Worth doing before any constant-factor
  work on the engine.

## F9 -- Two harness parse bugs that produced plausible wrong behaviour

Both were in the CE query parser, and both failed in the same dangerous way:
the input violated an assumption, and the result was not an error but a
*well-formed query that meant something else*.

1. **CRLF plus the statement terminator.** `getline` leaves a ``, the trailing
   `;` then survives trimming, and a column reference reads as `"s;"` rather than
   `"s"`. The parser's `== "s" ? 0 : 1` default turned that into column `d`.
   Result: a valid query with the wrong join predicate, returning 0. The flat
   baseline agreed exactly, because it consumed the same mis-parsed predicate --
   so two independent engines agreeing was not the reassurance it appeared to be.

2. **Table aliases.** The corpus expresses self-joins as
   `from yago2 yago2_2, yago2 yago2_3`, and predicates reference the alias.
   Splitting the FROM list on commas alone makes `"yago2 yago2_2"` a single table
   name, so every predicate against `yago2_2` resolves to nothing and the query
   is reported as a **disconnected join graph** -- a plausible diagnosis that is
   entirely wrong. This silently excluded the whole yago dataset, which is all
   self-joins and therefore exactly the shape factorization handles well.

Both are fixed, and the column parse now throws on anything that is not exactly
`s` or `d` rather than defaulting. The general lesson is worth carrying into the
DuckDB matcher (plan section 3.3), where the same failure mode is far more
costly: **a parser that guesses produces wrong answers, while one that refuses
produces missed opportunities.** Bail-out must be the default, and anything
unrecognised must be rejected explicitly rather than folded into a default case.

---

## What is and is not usable from the artifact

`../FactDB` is present locally and is **GPLv3**. Under DECISIONS.md D1 the split
is:

| Path | Status | Why |
|---|---|---|
| `bench/results/paper/**` | **used** | Measurement logs. Data, not engine source. |
| `bench/ce/queries/*.sql`, `schema.sql`, `load_duckdb.sql` | **usable** | Query corpus with embedded `-- Result size:` oracles; the correctness oracle for Phases 1-2. |
| `bench/*/setup.sh` | **usable** | Data-acquisition scripts (downloads from db.in.tum.de). |
| `factDB/**` | **NOT read** | Engine source. Reading it converts D1 to path A and makes the extension GPLv3. |

The CE data itself is **not** in the checkout (`bench/data` is absent); it is a
~download from `db.in.tum.de/~birler/dbgen/cebench.tar.zst`, sha256-checked by
`bench/ce/setup.sh`. `estimates.db` is a separate download from
`db.in.tum.de/~lehner/estimates.db`. Both still need fetching before Phase 1.4.

## F10 — The predicted gate works, but the win is one dataset

Full CE acyclic corpus, 712 queries, gate thresholds `min_ratio=2`,
`min_flat_tuples=1e6`, `require_acyclic`. DuckDB timings on a systematic
every-5th sample (156 queries; hetio sampled at 16/16, watdiv 65/322,
yago 75/374).

**Correctness: 0 wrong answers in 712.** Unchanged across every run to date.
The gate fired on 48 queries and *none* of them then hit the memory cap, so
the gate never commits to a query it cannot finish.

**Predictor quality** (predicted vs actual compression, n=623):

- Spearman rank correlation on logs: **0.652**
- Pearson on logs: 0.606
- Median error: 0.5x (under-predicts by 2x)

Rank is what the gate needs, and rank is what it gets — but 0.65 is loose.

**Gate decisions vs actual compression >= 2x:** precision **0.94**, recall
**0.39** (45 true positives, 3 false positives, 69 missed). Deliberately
conservative, and the conservatism is cheap: a miss costs a forgone win, a
false positive costs a regression.

**End-to-end** (bottom-insert, gated: fire -> ours, decline -> DuckDB):

| | unweighted overlap | reweighted to corpus |
|---|---|---|
| 1 thread | 2.23x | **1.36x** |
| default threads | 3.21x | **1.49x** |

The unweighted figure is inflated because hetio is sampled at 100% and the
other two at 20%. Reweighting is the honest number. Ungated, the same engine
is 0.47x — the gate is worth about 3x, and is the difference between a loss
and a win.

**Per dataset (default threads):** hetio **16.14x**, watdiv **0.97x**,
yago **1.00x**.

That is the finding. **The entire benefit is hetio.** yago is exactly neutral
(the gate declines everything, correctly). watdiv is a small net *loss*: the
gate fires on 21 queries there, 6 of which regress, worst 0.22x.

**Why watdiv regresses even when the prediction is right:** the failures are
not mispredictions. `watdiv_acyclic_213_05` was predicted 186.8x and actually
compressed 176.2x -- and still lost, 257ms vs 167ms. These queries are simply
too small; DuckDB parallelizes a 200ms query and we do not (Phase 4). The
missing threshold is absolute work, not compression, and `min_flat_tuples=1e6`
is far too low to express it. Raising it to 1e9 eliminates every regression
(worst case exactly 1.00x) at the cost of dropping to 1.49x unweighted.

**Against the plan's Phase 1 bar (D11): >=5x vs DuckDB to pass, <2x to kill.
The reweighted result is 1.49x. The kill criterion triggers.**

## F11 — The CE benchmark excludes every query with more than 1e9 result tuples

Counting live vs commented-out `select` statements in
`FactDB/bench/ce/queries/*_acyclic*.sql`:

| dataset | live | commented out | median result, live | median result, commented |
|---|---|---|---|---|
| hetio | **16** | **344** | 3.12e8 | 1.29e12 (max 1.31e17) |
| dblp | 308 | 52 | 5.75e6 | 2.36e10 |
| epinions | 240 | 120 | 1.10e7 | 8.26e10 |
| job | 570 | 39 | 5.54e6 | 2.12e9 |
| watdiv | 322 | 38 | 1.21e6 | 8.61e9 |
| yago | 374 | 8 | 4.32e3 | 6.61e5 |

The cutoff is exact: **no live query exceeds 1e9 result tuples** (max 9.568e8),
and all but 9 commented-out queries are at or above it. The corpus ships with
its explosive queries disabled, presumably because a flat system cannot
materialize them.

Two consequences, and they point in opposite directions.

**Against us:** every number reported so far -- ours and the paper's -- is
measured on a corpus pre-filtered to the regime where factorization matters
least. The live set is, by construction, the set of queries a flat engine can
already handle.

**For us:** it explains the dataset spread exactly. Ranking datasets by the
median result size of their *live* queries gives hetio (3.12e8) >> epinions >
dblp > job > watdiv (1.21e6) > yago (4.32e3), and our measured speedup follows
the same order -- hetio 16.14x, watdiv 0.97x, yago 1.00x. yago is neutral
because its median live query returns 4,300 tuples: there is nothing to
compress. **hetio is not an outlier; it is the one dataset whose queries the
filter did not fully remove.** 95% of hetio was cut, and what survived is still
the largest-result live set in the corpus.

This makes "is hetio an outlier or a type?" answerable directly: run the 344
excluded hetio queries, which no flat system can answer, and see whether the
f-representation stays small. That is a different claim from the plan's -- not
"faster than DuckDB" but "answers what DuckDB cannot" -- and it is the claim
the data actually supports.

## F12 — Parallelism is worth about 1.1x here, not the missing 3x

Counterfactual on the 156-query DuckDB overlap, giving our engine exactly the
parallel speedup DuckDB achieves on the same query (which makes each query's
ratio equal its single-threaded ratio):

| | reweighted | unweighted | geomean | worst | regressions |
|---|---|---|---|---|---|
| today, single-threaded | 1.49x | 3.21x | 1.37x | 0.22x | 6 of 21 |
| perfect scaling | **1.63x** | 3.63x | 1.47x | 0.35x | 3 of 21 |

**Phase 4 moves 1.49x to 1.63x.** It removes 3 of the 6 regressions; 3 remain
against *single-threaded* DuckDB, so they are not parallelism at all.

Decomposing the fired queries that have a flat baseline (n=12) into
factorization gain and raw engine speed:

- factorized vs **our own flat path**: **5.22x** geomean
- our flat path vs DuckDB at 1 thread: **3.98x** geomean
- net: 20.78x

**On the queries the gate fires on, both of the plan's Phase 1 criteria pass
by a wide margin** (>=5x vs DuckDB: 20.78x; >=1.5x vs own flat: 5.22x). The
corpus-wide 1.49x is not a statement about the technique; it is a statement
about how few queries in this corpus qualify -- which F11 explains.

The two worst regressions are different failures, and neither is parallelism:

- `watdiv_acyclic_213_10`: factorization *gains* 1.34x over our flat path, but
  our flat path is 0.26x DuckDB. 5.0M input rows; DuckDB scans and joins them
  at 35M rows/s single-threaded, we manage 12M. A raw engine deficit.
- `watdiv_acyclic_212_10`: 19.2x actual compression and factorization still
  *loses* -- 461ms against our own flat path's 163ms. The only measured case
  where the factorized machinery costs more than the tuples it saves. 750K
  input rows, 107K records out, so it is neither scan- nor output-bound.

## F13 — The gate is blind to skew, and misses an entire dataset

Stride-5 sample of the three untested datasets (dblp 62, epinions 48, job 114
queries; job still running). Zero wrong answers.

| dataset | n | gate fires | median compression | max | median result |
|---|---|---|---|---|---|
| dblp | 62 | **0** | 0.47x | 3x | 2.88e6 |
| epinions | 48 | **0** | **14.15x** | **2015x** | 1.30e7 |
| job | 23 | **0** | 0.26x | 7x | 2.42e6 |

dblp and job are correct declines -- median compression below 1x means the
f-representation is *larger* than the flat result, and factorization is a
straight loss. That matches F11's ranking: their live queries return ~3e6
tuples, far too few to compress.

**epinions is a gate failure, and a total one: 0 of 48 fire on a dataset whose
top queries compress 300-2000x.** Against our own flat path those queries run
26-57x faster:

| query | compression | factorized | our flat | gain |
|---|---|---|---|---|
| epinions_acyclic_215_15 | 324x | 41ms | 2313ms | **56.9x** |
| epinions_acyclic_208_10 | 375x | 20ms | 846ms | 42.8x |
| epinions_acyclic_209_05 | 240x | 38ms | 1556ms | 40.7x |
| epinions_acyclic_208_15 | 606x | 21ms | 707ms | 34.2x |

**Why the gate declines them.** For `epinions_acyclic_202_00` the estimate is
1.101e5 flat tuples; the true result is 1.573e8 -- **under-predicted 1400x**.
Across the 48, predicted ratio has median 0.60x and *maximum* 1.08x, against
actual compression up to 2015x. Both thresholds fail, so nothing fires.

The cause is the textbook formula itself. `|R join S| = |R||S| / max(V_R, V_S)`
assumes uniformly distributed keys. epinions is a trust graph: its degree
distribution is heavy-tailed, a few nodes carry most edges, and the join
explodes on exactly those. Distinct-value counts cannot see that. The two
estimates are each wrong, and here they are wrong by different factors, so the
ratio does not survive the division.

This reframes F10's recall of 0.39. The misses are not scattered noise around a
conservative threshold -- **they are systematic, and they cluster by data
distribution.** Any dataset with skewed join keys is invisible to the current
gate, and skewed join keys are the normal case in graph-shaped data, which is
the workload the whole technique targets.

Fixing it needs a skew statistic, not a better threshold: most-common-value
frequency, or a sampled max-degree, in place of the uniformity assumption. That
is a hard requirement on Phase 3, not a tuning exercise.

## F14 — An 8-entry MCV list fixes the blind spot

Testing estimators against the *exact* size of each query's largest join
equivalence class (the star group that dominates the result). "uniform" is the
textbook formula the gate uses today; "mcv-K" computes the exact contribution
of the top-K most common values per column and applies uniform only to the
tail -- the standard PostgreSQL-style MCV list.

Error factors (exact / estimate, so 1.0x is perfect):

| query | uniform | mcv-1 | mcv-8 |
|---|---|---|---|
| epinions_acyclic_202_00 | **2814x** | 1.1x | **1.0x** |
| epinions_acyclic_202_10 | 2213x | 1.1x | 1.0x |
| epinions_acyclic_202_15 | 2292x | 1.4x | 1.0x |
| epinions_acyclic_203_00 | 9x | 2.1x | 1.2x |
| hetio_acyclic_202_04 | **209x** | 2.3x | **1.7x** |
| hetio_acyclic_202_03 | 62x | 21.8x | 1.4x |
| hetio_acyclic_202_01 | 17x | 16.9x | 1.8x |
| dblp_acyclic_201_00 | 3x | 2.6x | 2.6x |
| dblp_acyclic_201_15 | 2x | 1.7x | 1.7x |

**Eight most-common values per join column is enough.** It is a strict
improvement on every dataset tested -- it rescues epinions outright, cuts
hetio's worst error from 209x to 1.7x, and leaves dblp (which is already
roughly uniform, and which the gate correctly declines) unchanged.

The reason it works is visible in the data: for `epinions_acyclic_202_00`,
**five key values account for 90% of a 1.92e8-tuple join** (mcv-1 alone
recovers 1.74e8 of it). Distinct-value counts average that away completely;
the trust graph's hubs *are* the result.

Practical note: this needs a real statistic, not a derived one. DuckDB's
catalog carries approximate distinct counts, not MCV lists, so Phase 3 would
have to sample max-frequency per join column or add the statistic. Sampling is
cheap -- a few thousand rows recovers the head of a heavy-tailed distribution
-- but it is a dependency the plan does not currently name.

## F15 — Head to head on the excluded regime

Stratified sample of the excluded hetio queries, one per order of magnitude of
result size, run on both engines. DuckDB: 4 threads, 6 GB, 180 s cap. Ours:
single thread, 6 GB f-representation cap.

| result tuples | ours | records | compression | DuckDB | correct |
|---|---|---|---|---|---|
| 1.002e9 | **0.68s** | 1.15e6 | 874x | **49.9s** | exact |
| 1.029e10 | 6.76s | 1.69e7 | 610x | timeout | exact |
| 1.093e11 | *declined* | | | timeout | |
| 1.013e12 | **0.12s** | 4.97e5 | 2,037,221x | timeout | exact |
| 1.009e13 | 1.30s | 5.06e6 | 1,993,452x | timeout | exact |
| 1.169e14 | 15.63s | 1.06e8 | 1,107,592x | timeout | exact |
| 1.023e15 | 18.40s | 1.42e7 | **71,826,441x** | timeout | exact |
| 1.207e16 | *declined* | | | timeout | |
| 1.311e17 | *declined* | | | timeout | |

Six of nine answered, **all six exactly correct**. A quadrillion-tuple count in
18.4 seconds.

**The one directly measurable comparison is 73x** (0.68s vs 49.9s), and it is
the smallest query in the set -- the only one at the exclusion boundary that
DuckDB can finish at all. Everything above it is a timeout against a
1.3-to-18-second answer, and at DuckDB's measured 2.0e7 tuples/s the 1.009e13
query is ~5 days.

The three declines are the 6 GB cap, and they are safe (a decline, never a
wrong answer). Note they are **not monotonic in result size**: 1.013e12
succeeded in 0.12s with 497K records while 1.093e11 failed. Result size does
not determine f-representation size -- which is the entire premise, and also
means the memory cap is not a simple function of the query's answer.

**Independent correctness anchor:** DuckDB's own count for the 1.002e9 query
matched the benchmark's published result size, which grounds the chain. Our
agreement on the larger counts is against the paper's engine only, since no
flat system has produced them.

## F16 — The gate's threshold is wrong by 25x, and tuning it cannot reach the bar

All 741 queries across the six datasets that produced both a factorized and a
flat time. Grouping by *actual* compression and measuring factorized against
our own flat path:

| compression >= | n | geomean | median | worst |
|---|---|---|---|---|
| 1x | 185 | 0.85x | 0.61x | 0.18x |
| **2x** (current gate) | 128 | **1.15x** | **0.79x** | 0.27x |
| 5x | 89 | 1.57x | 0.94x | 0.27x |
| 10x | 64 | 2.12x | 1.33x | 0.27x |
| 25x | 39 | 3.89x | 1.66x | 0.27x |
| **50x** | **27** | **7.84x** | **15.11x** | 0.58x |
| 100x | 19 | 14.66x | 33.47x | 0.58x |

**At the gate's current 2x threshold the median query still loses** (0.79x) and
the geomean is barely break-even. Factorization does not begin to pay reliably
until about **50x**, where the geomean is 7.84x and the median 15.11x.

At >=50x, by dataset: **hetio 16.58x, epinions 16.54x**, watdiv 1.48x, job
1.62x (n=1), yago 0.58x (n=1), dblp none. The two datasets that pay are the two
skewed graph datasets -- and epinions contributes 12 of the 27 qualifying
queries, so F14's MCV fix roughly doubles the addressable set.

**But raising the threshold does not move the DuckDB number.** Oracle gate on
the 156-query overlap, vs DuckDB at default threads:

| compression >= | fires | reweighted | geomean | worst | regressions |
|---|---|---|---|---|---|
| 2x | 26 | 1.43x | 1.37x | 0.22x | 10 |
| 25x | 16 | 1.52x | 1.41x | 0.65x | 2 |
| **50x** | 14 | **1.51x** | 1.41x | 0.65x | **1** |
| 100x | 10 | 1.43x | 1.34x | 0.65x | 1 |

The reweighted result is pinned at ~1.5x across every threshold. Tuning buys
**safety** -- regressions fall from 10 to 1 -- not throughput.

This is the decisive answer to D11. **With perfect knowledge of compression and
a freely tunable threshold, this corpus yields ~1.5x against DuckDB.** The 5x
bar is not reachable by better gating, better estimation, or parallelism
(F12: +0.14x). It is not reachable at all on the live CE corpus, because
F11 already removed the queries that would reach it.

## F17 — All 344 excluded hetio queries: 53% answered, 0 wrong, 47% declined

Full run of the queries F11 found the corpus disables. Single thread, 6 GB
f-representation cap, no flat baseline.

| | |
|---|---|
| queries | 344 |
| answered correctly | **182** |
| **wrong answers** | **0** |
| declined (memory cap) | 162 |

Of the 182 answered:

| | median | max |
|---|---|---|
| result size | 4.10e11 | 7.27e15 |
| compression | **101,346x** | **2,630,790,273x** |
| our time | 1.57s | 2744s (p90 17.2s) |

**1.087e16 result tuples counted in 98 minutes of engine time.** At DuckDB's
measured 2.0e7 tuples/s that same work is 5.44e8 seconds -- **17 years**.

**The 47% decline rate is the real limitation, and it is not predictable from
result size.** Declined queries have median published result 9.26e12, but 35
*answered* queries have results at or above that, and the smallest decline is
2.85e9 -- below hundreds of queries that succeeded. Whether a query fits in
6 GB depends on the f-representation's shape, not the size of the answer. That
is the premise working, but it also means a gate for this regime cannot be
built from result-size estimates alone; it has to estimate f-representation
size, which is what `EstimateCost` already computes and what F14 shows can be
made accurate with an MCV list.

The timing tail is small but real: median 1.57s, p90 17.2s, one query at 46
minutes.

## F18 — The gate's criterion is wrong, not just its threshold

Chasing the concern from F16 (the MCV estimator being *more* dangerous than the
textbook one below 50x) produced a diagnosis, a failed fix, and a larger
finding that supersedes both.

**The diagnosis.** `ColumnStats::Frequency` returns the tail average for every
value it did not store and never zero, so each head value is counted as present
in every relation of the class. On `watdiv_acyclic_212_15`, an 8-relation star
over a 125,145-value key domain:

| relation | distinct | share of domain | actually holds |
|---|---|---|---|
| watdiv1052598 | 1,659 | 1.3% | **18%** of head values |
| watdiv1052590 | 7,410 | 5.9% | 30% |
| watdiv1052584 | 125,145 | 100% | 100% |

The join was over-predicted **84x**. This concentrates in classes with
mismatched cardinalities -- which watdiv and yago have and epinions does not,
exactly matching where the estimator regressed.

**The fix that failed.** Weighting each column's tail frequency by its share of
the key domain is the independence assumption; leaving it is containment, which
is what PostgreSQL assumes for the smaller side and what the tail formula
already uses. Swept over exponents 0 (containment), 0.33, 0.5 and 1.0
(independence):

| exponent | hetio | watdiv | epinions | overall | bias |
|---|---|---|---|---|---|
| **0 (containment)** | **3.2x** | 7.6x | 1.7x | 3.8x | **1.07x** |
| 0.33 | 3.9x | 6.5x | 1.7x | 3.6x | 0.54x |
| 0.5 | 4.5x | 8.1x | 1.7x | 4.0x | 0.42x |
| 1.0 (independence) | 6.3x | **6.5x** | 1.7x | 3.6x | 0.39x |

Overall error moves 3.8x -> 3.6x, inside the noise, while hetio degrades 2x and
a systematic under-prediction bias appears. **Reverted.** Containment is
*correct* for a foreign-key join, where the small side is contained by
construction -- a case the sweep cannot see and `test_cost.cpp` now covers,
where independence is 134x low. epinions is unaffected at every exponent (1.7x
throughout) because its relations all have similar cardinality.

**The larger finding.** With DuckDB timings for epinions, the gate's own
criterion turns out to be the binding constraint:

| epinions strategy | result |
|---|---|
| old gate (textbook, fires on nothing) | 1.00x |
| new gate (MCV, 50x, 16 fires) | 1.60x |
| **fire on everything** | **127.9x** |

On the 16 fired queries: **816x geomean, worst 480x, zero regressions.** But
**all 30 declined queries would also have won** -- geomean 145x, worst 37.9x.
Queries compressing **1x** still win 45x, because DuckDB needs 17-29 seconds
where we need 0.4.

Decomposing speedup as compression x K, where K is the per-record speed
advantage a compression threshold implicitly assumes is 1:

| dataset | compression | speedup | **K** |
|---|---|---|---|
| hetio | 229.0x | 30.6x | **0.13** |
| watdiv | 0.7x | 0.3x | 0.44 |
| yago | 0.0x | 0.2x | 3.98 |
| epinions | 24.1x | 264.9x | **10.97** |

**K spans 84x**, and that one term explains both failure modes at once: the
gate is far too conservative where K is high and too aggressive where it is
low. No threshold fixes that, which is why F16's sweep found the DuckDB result
pinned at ~1.5x no matter where the threshold was set.

Predicting the two times directly is the obvious replacement and is not ready
either: our records/s spans 1.07e5-6.29e6 and DuckDB's tuples/s spans
2.68e4-2.52e7, so both need a fixed-cost term first.

## F19 — Flat estimation on uniform data: three fixes tried, none kept

O12 named flat over-prediction on uniform data as the binding constraint: it
causes all four remaining regressions of the cost-model gate (D14), and it is
why MCVs made watdiv and yago *less* accurate (F18). The defect is precise --
`Frequency` returns the tail average for every value a column did not store and
never zero, so each head value is counted as present in every relation of the
class, and on `watdiv_acyclic_212_15` that over-predicts the join 84x.

Three replacements, each measured against exact brute-force sizes over 20
queries per dataset (geomean |error|):

| approach | epinions | dblp | job | hetio | watdiv |
|---|---|---|---|---|---|
| textbook (no MCVs) | 30.3x | 1.4x | 6.5x | 13.5x | 3.3x |
| **union + tail average (kept)** | **1.1x** | **1.6x** | **3.4x** | **1.5x** | **1.8x** |
| presence weighted by domain share | -- | -- | -- | 6.3x | 6.5x |
| head restricted to the intersection | 1.1x | 1.7x | 11.0x | **83.9x** | 3.4x |
| head weighted by k/n columns storing it | 1.1x | 1.7x | 6.6x | 2.2x | 1.8x |

- **Domain-share weighting** is the independence assumption. It fixes watdiv
  and is 134x wrong on a foreign-key join, where the small side is contained by
  construction. Swept over four exponents: overall error moved 3.8x -> 3.6x,
  inside the noise, while hetio degraded 2x and a systematic under-prediction
  bias appeared. Covered by `test_cost.cpp`.
- **Intersection only** guesses nothing -- it uses only values every column
  stored -- and is catastrophically worse, because hubs are not stored by every
  column. hetio 1.5x -> 83.9x.
- **k/n weighting** self-calibrates from how many columns called a value
  common, with no free parameter. Ties on epinions and watdiv, loses on job
  (3.4x -> 6.6x) and hetio (1.5x -> 2.2x).

**Union with the tail average beats every alternative constructed, so it
stands.** The 84x over-prediction on `watdiv_acyclic_212_15` is a documented
limitation, not an open bug with a known fix. Continuing to generate variants
would be fitting to that one query.

Consequence for D14: the gate's four regressions are not closable by better
estimation with the statistics currently available. Closing them needs either a
statistic that captures *joint* presence across columns (a sketch, not an MCV
list), or a runtime bail-out that abandons factorization when it exceeds its
predicted cost.
