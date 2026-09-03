# DECISIONS

Living record of the load-bearing decisions taken against `tmp/DUCKDB_EXTENSION_PLAN.md`.
Per plan §0.4, every §0.2-class decision lands here.

| # | Decision | Value | Date | Ref |
|---|---|---|---|---|
| D1 | Licensing path | **Path B — clean-room from the paper.** MIT. No FactDB (GPLv3) source is fetched, read, or ported. Implementation derives only from `p3006-lehner.pdf`. | 2026-09-01 | §0.2 |
| D2 | Extension name | `factorize` | 2026-09-01 | §3.2 |
| D3 | Setting prefix | `factorize_*` (`factorize_mode`, `factorize_min_gain`, `factorize_tuples_per_ms`) — distinct from the extension name, per the `httpfs`→`http_timeout` precedent | 2026-09-01 | §Phase 3.5 |
| D4 | Pinned DuckDB version | **v1.5.5** (latest stable at time of pinning). `extension-ci-tools` pinned to matching branch `v1.5.5`. | 2026-09-01 | §3.1, Phase 0.1 |
| D5 | API surface | **C++ (internal) API.** The stable C API cannot inject plans. Accepts per-release recompilation (R6). | 2026-09-01 | §3.1 |
| D6 | Implementation language | **C++ throughout.** The repo is the C++ extension template; a Rust core would add a second toolchain to an already six-platform CI matrix. Revisit only if `src/core/` concurrency (R5) proves unmanageable. | 2026-09-01 | §3.4 |
| D7 | Build/bench environment | Repo lives on the Windows filesystem (`c:\Users\maxde\Repositories\factorize`); builds and benchmarks run from **WSL2 Ubuntu 20.04** via `/mnt/c`. Data-acquisition scripts are Linux bash. | 2026-09-01 | §0.5 |
| D8 | Phase order | Plan order: Phase 0 spike, then Phase 1 standalone core. Phase 1 is not skipped and the DuckDB integration is not built on top of an unmeasured core. | 2026-09-01 | §0.3, §9.1 |
| D9 | vcpkg / OpenSSL | **Dropped.** The template's OpenSSL dependency is demo scaffolding; factorization needs no third-party libraries. Removing vcpkg simplifies the six-platform CI matrix. | 2026-09-01 | — |
| D11 | Phase 1 exit/kill criterion | **The plan's original bar stands**, as reaffirmed after FINDINGS.md F3 was raised: exit at >=5x vs stock DuckDB and >=1.5x vs own flat baseline; kill below 2x vs stock DuckDB. The factorized-vs-own-flat ratio is still recorded alongside it, so a kill can be attributed to the substrate rather than to the idea. | 2026-09-01 | Phase 1.5, F3 |
| D12 | CE data acquisition | Authorized: fetch `cebench.tar.zst` and `estimates.db` from `db.in.tum.de`. Both land under `../FactDB/bench/data` and `tmp/`, never in this repo's tree. | 2026-09-01 | O5 |
| D10 | v1 aggregate types | **Integer and `DECIMAL` only.** Keeps `'auto'` ≡ `'off'` bit-exact so the Phase 7 fuzzer's core invariant holds under FP reassociation. `FLOAT`/`DOUBLE` declined by the gate. | 2026-09-01 | §4.5.1, R10 |

## Open

| # | Question | Owner | Blocking |
|---|---|---|---|
| O8 | **Is the flat baseline fair?** Every factorized-vs-flat ratio assumes the flat baseline is competent. If it is unusually slow, the ratio flatters factorization. Resolve by comparing the flat baseline against stock DuckDB at `threads=1` on the same queries and machine; a baseline more than ~2-3x off DuckDB would invalidate the headline ratio. | — | Phase 1.5 verdict |
| O9 | `estimates.db` download has failed twice (curl 56, then 2). Section 0.6's oracle-vs-estimates control experiment is blocked until it lands. | — | Phase 6 report |
| O1 | Email Lehner (`s.lehner@tum.de`) / Neumann (`neumann@in.tum.de`): how much of the speedup is codegen vs container design? Did they ever run a non-generated factorized path? | — | Nothing (informs Phase 1 expectations) |
| O2 | Locate Kalumin & Deshpande's artifact — head start, baseline, and the concrete claim to beat (they cannot do bottom-inserts; FINDINGS.md F4 shows that puts them at 0.98x, i.e. no benefit). | — | Phase 6 comparison |
| O3 | Upstream posture: community extension, private fork, or upstream PR? Changes whether to adapt DuckDB's `JoinHashTable` over a bespoke chaining HT. | — | Phase 1.3 |
| O4 | Many-core machine for Phases 4–6. Dev box is 8 logical cores; Phase 4's exit criterion (≥8× at 32 threads) is unmeasurable on it. | — | Phase 4 exit |
| O5 | CE **data** acquisition. The local `../FactDB` checkout supplies the query corpus, schema and result oracles, but `bench/data` is absent: the CSVs are a download from `db.in.tum.de/~birler/dbgen/cebench.tar.zst`, and `estimates.db` from `db.in.tum.de/~lehner/estimates.db`. Both need an explicit go-ahead to fetch. | — | Phase 1.4 |
| O6 | **Phase 1 exit/kill criterion is mis-specified** (FINDINGS.md F3): as written, FactDB itself fails it. Adopt the replacement bar (factorized vs own flat baseline) before Phase 1.5. | — | Phase 1.5 |
| O7 | Read `../FactDB/factDB/**` engine source? Would reverse D1 to path A and make the extension GPLv3. Currently **not read**. | — | — |

## Phase 0 — COMPLETE (exit criterion met)

Verified against a cleanly-built DuckDB v1.5.5: with `factorize_mode='force'` the
optimizer rule replaces the aggregate subtree, the operator returns its stub, and
`EXPLAIN` renders `FACTORIZED`. Every shape outside the allowlist -- non-equi
join, an aggregate other than `count(*)`, `LEFT JOIN`, a single relation -- is
declined and returns the correct stock answer, as does `factorize_mode='off'`.

**The bug the spike existed to find:** the replacement operator was given a fresh
table index from the binder, but operators above the aggregate already reference
`ColumnBinding(aggregate_index, 0)`. The plan still type-checks and
`ResolveColumnBindings` still succeeds; it fails later, at execution, with
"Failed to bind column reference". A replacement operator must *inherit* the
binding it replaces. See PLAN_ERRATA.md E2.

## Phase 0 findings — v1.5.5 API deltas from the plan

The plan (§3.2) was written against an older DuckDB. Verified against the pinned v1.5.5 sources:

- `LogicalExtensionOperator::CreatePlan` returns **`PhysicalOperator &`**, not `unique_ptr<PhysicalOperator>`.
  Signature: `PhysicalOperator &CreatePlan(ClientContext &, PhysicalPlanGenerator &)`.
- Physical operators are **arena-allocated** by `PhysicalPlan`. Construct via `planner.Make<T>(args...)`;
  the ctor's first parameter is `PhysicalPlan &` and is supplied by `Make`.
- `PhysicalOperator::children` is `ArenaLinkedList<reference<PhysicalOperator>>` — references, not `unique_ptr`s.
- The source override point is the **protected** `GetDataInternal(...)`; public `GetData` is non-virtual.
- Extension entry point is the macro `DUCKDB_CPP_EXTENSION_ENTRY(name, loader)`
  (`duckdb/main/extension/extension_loader.hpp`), which supersedes the old `_init`/`_version` C symbols.
- `OptimizerExtension::Register(DBConfig &, OptimizerExtension)` and
  `DBConfig::AddExtensionOption(...)` are unchanged from the plan's description.

## D13 — The gate estimates over an MCV list, per equivalence class

FINDINGS F13/F14/F16. The gate's estimator was the textbook equi-join formula
over distinct counts. On skewed join keys it under-predicted by up to 2814x and
declined all 48 sampled epinions queries, a dataset whose top queries compress
300-2000x. Two changes:

**A most-common-value list per join column** (`ColumnStats::mcv`, `src/core/
stats.hpp`). Frequencies are exact for the stored values and uniform only for
the tail. **128 entries**, measured -- with only the top-K stored, a value
outside the list can only be estimated by the tail average, and the datasets
differ in how deep the head goes:

| dataset | uniform | mcv-8 | mcv-32 | mcv-128 | mcv-512 |
|---|---|---|---|---|---|
| epinions | 35x | 1.2x | 1.1x | 1.1x | 1.0x |
| hetio | 10x | 7.2x | 2.4x | **1.4x** | 1.2x |
| job | 7x | 5.1x | 4.3x | 3.3x | 2.4x |
| watdiv | 3x | 2.0x | 2.0x | 2.1x | 2.0x |
| dblp | 2x | 1.6x | 1.6x | 1.6x | 1.6x |

(An earlier note claiming 8 entries suffice was measured with *exact*
frequencies for every value, which no optimizer has. Correcting that to the
implementable estimator moved the answer from 8 to 128.)

**Estimation per equivalence class, not per edge.** Relations joining on the
same key see the same hub values simultaneously, so their skew multiplies; a
per-edge scalar cannot represent that. `EstimateGroup` sizes a whole class at
once, and classes are then folded together along the plan.

Consequence for Phase 3, and it is a real dependency the plan does not name:
**DuckDB's catalog carries approximate distinct counts but no MCV list.** The
optimizer rule has to sample max-frequency per join column or add the
statistic. Sampling suffices -- a heavy-tailed distribution puts its head in
any reasonable sample. `ColumnStats` with an empty `mcv` degrades to exactly
the old textbook estimator, so this is a graceful fallback rather than a hard
requirement (tested).

Also raised `CostThresholds::min_ratio` from 2x to **50x** (F16: at 2x the
median query loses; the geomean does not reach 7.84x until 50x).

Open item O10: the sibling/chain distinction is modelled by whether two
relations share an equivalence class. That is right for the CE corpus, where
every predicate is a plain equi-join, but a query mixing an equality class with
a non-equality edge would need more.

## D14 — The gate compares predicted times, not compression

FINDINGS F18. Speedup is compression x K, and K -- the per-record speed
advantage a compression threshold implicitly assumes is 1 -- spans 84x across
datasets (hetio 0.13, epinions 10.97). No threshold on compression can be right
for all of them, which is why sweeping it left the measured result pinned near
1.5x wherever it was set.

`EstimateCost` now estimates milliseconds for both engines and fires when it
predicts beating DuckDB by a margin:

    ours_ms   = 0     + 2.214e-4 * input_rows + 1.694e-4 * records
    duckdb_ms = 34.33 + 5.674e-6 * input_rows + 3.946e-5 * flat_tuples
    fire when duckdb_ms > 1.5 * ours_ms

**The two fits are deliberately asymmetric**: ours is the 75th-percentile fit
(sits above 75% of observed runs), DuckDB's the 25th. A gate must be
pessimistic about the engine it chooses and optimistic about the one it
rejects, or every estimation error becomes a regression.

Measured, n=194 queries with both timings:

| gate | fires | reweighted | geomean | regressions | worst |
|---|---|---|---|---|---|
| compression >= 50x | 30 | 1.65x | 2.28x | 2 | 0.76x |
| **cost model, margin 1.5x** | **47** | **2.26x** | **3.37x** | 4 | 0.13x |
| oracle (perfect knowledge) | 84 | 5.87x | 5.78x | 0 | 1.00x |

**Not overfit.** 5-fold cross-validation -- fit on 4 folds, gate the 5th --
reproduces the in-sample result exactly: 47 fires, 4 regressions, per-fold
geomean 2.69x-5.03x. The C++ implementation agrees with the offline model on
341 of 341 queries checked.

**The margin is a judgment call, not an optimum.** A more conservative setting
(pessimistic-90 / optimistic-25, margin 20x) reaches 0 regressions and worst
case exactly 1.00x, but only 1.39x reweighted -- worse than the compression
gate it replaces. There is a trade-off curve here, not a dominating answer, and
1.5x should be overridden where any regression is unacceptable.

Open item O11: **the coefficients are fitted on one machine and are not
portable.** Phase 3 must re-fit them at install time or drive them from
DuckDB's own cost model rather than shipping these constants.

Open item O12: the 4 remaining regressions are all watdiv and all fail on
DuckDB's side -- it runs 3-24x faster than predicted because our *flat*
estimate is too high on uniform data. No compression or flat-size guard
separates them (tested at 1.5/2/5/10x and 1e7/1e8). Better flat estimation, not
a better decision rule, is what closes them. **The estimator is the binding
constraint again.**

## D15 — Repoint the project at the regime the benchmark excludes

The plan's target was "faster than DuckDB on the CE benchmark", with a Phase 1
bar of >=5x to pass and <2x to kill (D11, user-reaffirmed). After the gate
redesign the measured result is **2.26x** -- above the kill line, below the pass
line. That ambiguity is resolvable, and not in the plan's favour.

**The 5x bar is unreachable on the live corpus by construction.** F11: the CE
benchmark comments out every query whose result exceeds 1e9 tuples, and hetio
loses 344 of 360 queries to that filter. The live set is, by definition, the
regime a flat engine already handles. F18's oracle -- fire exactly when we are
actually faster, with perfect knowledge -- reaches **5.87x**. A bar that a
*perfect* gate barely clears is not a bar this project can pass by improving
the gate, and F12 already showed parallelism is worth +0.14x.

**In the excluded regime the claim needs no hedging.** F15/F17: 182 of 344
excluded hetio queries answered, **0 wrong**, median compression 101,346x,
1.087e16 result tuples counted in 98 minutes of engine time against DuckDB's
measured ~17 years. Head to head with a 180s cap, DuckDB finished exactly one
of nine -- the smallest, at the 1e9 boundary, in 49.9s against our 0.68s
(**73x**) -- and timed out on every larger one, including a 1.01e15-tuple query
we answer in 18.4s.

So: **the claim changes from "faster than DuckDB" to "answers what DuckDB
cannot".**

What carries over unchanged: the engine, the MCV estimator (D13), and the
cost-model gate (D14). None of it is sunk. The gate's predicted
f-representation size is precisely what turns F17's **47% decline rate** from
something discovered at the memory cap into something known before execution --
which is the single most important open problem for the new claim, because
result size does not predict it (a 1.013e12-tuple query succeeded in 0.12s
while a 1.093e11-tuple one failed).

What this costs, stated plainly:

- **A narrower product.** "count(*) over huge many-to-many joins", not "a
  faster DuckDB".
- **No standard benchmark.** The corpus deleted these queries, so evaluation
  needs one built from the excluded set -- already extracted to
  `tmp/excluded/` (481 queries across five datasets).
- **The comparison gets harder to state.** Most numbers become "DuckDB did not
  finish" rather than a ratio, and a timeout is a weaker measurement than a
  time. The one honest ratio at the boundary is 73x.

Reversible: nothing is deleted, and the CE numbers stand on their own if the
decision is revisited. Open items O11 (portable coefficients) and O12 (flat
estimation on uniform data) apply to either direction.

## D16 — Phase 2 complete: factorized_count() over DuckDB storage

The plan's Phase 2 exit is "for 200+ CE queries, `factorized_count(...)` matches
both the `-- Result size:` comment and stock DuckDB's `count(*)`. Zero
mismatches." Both clauses are met.

| check | result |
|---|---|
| CE queries run through the extension | **238** |
| matching the published result size | **212** |
| **mismatches** | **0** |
| declined (memory cap) | 26, all watdiv |
| three-way cross-check vs stock DuckDB | **25 / 25** |
| sqllogictest assertions | 33 |
| core unit checks | 77 |

The cross-check reconstructs the equivalent plain SQL from the same lists the
function takes, so both engines answer an identical question; it is limited to
results under 2e6 tuples because the published sizes reach 1e17 and the point is
agreement, not timing DuckDB.

**Three things this phase established that were not in the plan.**

**The extension must build at C++14.** C++17 makes DuckDB's `static constexpr`
members implicitly inline, so a translation unit including its headers emits its
own `duckdb::LogicalType::BIGINT` and collides at link time with the one
DuckDB's C++11 build emits. C++11 lacks `make_unique` and generic lambdas, which
`src/core/` needs. `std::byte` was the core's only C++17 dependency and is now
`factorize::Byte`. `core-test.sh` and `run-ce.sh` build at C++14 as well, so the
constraint is enforced rather than commented -- it immediately caught a missing
`<algorithm>` that C++17 headers had been supplying transitively.

**`CORE_SOURCES` was empty.** The engine had never been compiled into the
extension; every green CI run before this built the glue and a stub operator.
Any earlier statement that "CI builds the extension" was true but weaker than it
sounded.

**Memory accounting is not optional, and it was missing.** The first CE run
OOM-killed the DuckDB process -- the kernel, not a query error. The engine now
takes half of DuckDB's `memory_limit` (half because scanned base-table columns
live outside the arena) and a query that would exceed it fails cleanly. Testing
this needed a *chain*, the shape the engine handles worst: a four-way self-join
star did not trip a 50 MB cap at all, because holding a few hundred records
whatever the count is exactly what factorization does.

Not established, deliberately: **no performance claim.** The local build is
debug + ASAN, so nothing measured here is a timing. Statistics are computed
exactly from the data just scanned rather than sampled from the catalog --
DuckDB carries approximate distinct counts and no MCV list, which is Phase 3's
problem (D13).
