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

## D17 — Adversarial review of the whole tree; two P0 data-integrity bugs fixed before ever reaching origin

Ran the full orchestrated adversarial-repo-review protocol
(test/adversarial-repo-review.prompt) against the working tree at 6e55ce6f
(branch phase2-extension-surface, at the time still unpushed -- Phase 2's own
commits had not left this machine). Repo-map, split-plan, six serial
specialists with a handoff ledger, synthesis: verdict BLOCK, 2 P0, 9 P1, 17
P2, 28 findings total. Full report and every specialist artifact under
tmp/20260903-adversarial-*.md (gitignored; not reproduced here).

Every P0/P1 disposition below was independently re-verified against the
actual source before being trusted, not taken on the report's word -- in one
case (the NULL-desync bug) the review's evidence was right but the mechanism
turned out worse than first stated.

Fixed, each with a regression test, all under test/unit/:

- P0: ExecuteCount hardcoded every column to ValueType::INT32 (plan.cpp), so
  any BIGINT/UBIGINT join key outside int32 range was silently truncated via
  static_cast<int32_t> in join.cpp::MakeScan -- two distinct 64-bit values
  differing only above bit 31 became the same key, with no error.
  QueryGraph::column_types is now required (not defaulted) at every call
  site; table_function.cpp's RequireIntegerKey returns the real width,
  correcting a second latent instance of the same class of bug along the
  way -- UINTEGER (0..4294967295) does not fit a signed INT32 slot despite
  being nominally 32 bits, and needs INT64 storage. test_plan.cpp asserts the
  specific alias case: two BIGINT values sharing low 32 bits must not
  collide.
- P0: StorageSource::Load filtered NULLs independently per column
  (table_function.cpp), desynchronizing a relation's columns from each other
  the moment one column held a NULL a sibling didn't -- ordinary star-schema
  shapes, not an edge case. Worse than the review stated: since MakeScan
  indexes every column up to columns[0].size(), a relation whose later
  column ended up shorter than column 0 was an out-of-bounds vector read,
  not merely a misalignment. Fixed by deciding a row's fate across all of a
  relation's columns at once. Added a row-count assertion inside MakeScan
  itself so this class of bug cannot recur silently regardless of where it
  is next introduced. factorized_count.test gained a composite-key NULL case
  engineered to fabricate a nonexistent row (p=3,q=2) under the old bug.
- P1: unchecked int64 overflow in the count arithmetic
  (frep.cpp::SubtreeSize/Count, join.cpp's LowerSizeCounter/OutputCounter --
  the arithmetic FactorizedCountJoin runs for every real query).
  CheckedCardinalityAdd/Mul (frep.hpp) use portable manual overflow checks,
  not __builtin_mul_overflow/__builtin_add_overflow: those are GCC/Clang-only
  and this project's CI has a plain-MSVC Windows target (windows_amd64,
  distinct from windows_amd64_mingw) that does not support them -- the
  review's own suggested fix would have broken that target.
- P1: the memory limit only covered the output FRepresentation's arena, not
  ChainingHashTable's entry arena or the top-insert snapshot arena, so a
  large build side with a tiny output could exhaust memory with no check
  regardless of how small the eventual count was. Fixed at the Arena
  primitive itself (SetMemoryLimit, checked in Allocate) rather than by
  threading a parameter through each caller individually, so every Arena in
  the codebase is covered by one change, including ones added later.
- P1: unbounded recursion depth across every f-tree/materialize traversal,
  proportional to join-chain length, with nothing capping how many relations
  a caller (a public table function taking a caller-supplied list) can
  supply. BuildPlan now refuses more than 500 relations before any recursion
  runs -- generous against any real workload (12 relations is the most
  FINDINGS' own tables go to) and conservative against a 1MB thread stack
  even allowing several stacked calls per tree level. The full hardening fix
  (convert the hottest traversals to explicit-stack iteration) is deferred;
  the cap is the "minimal" fix the review itself named.
- P1: factorize_mode='auto' was byte-identical to 'force' -- no cost gate
  exists yet, so the option's own documented promise ("fire when the cost
  gate agrees") was silently false, and a stub-fabricated 42 could flow into
  HAVING/arithmetic over the aggregate with no visible 42 anywhere in the
  output. AUTO now behaves as OFF until Phase 3 implements a real gate;
  factorize_phase0.test gained a regression case.
- P1: the benchmark harness's "unsupported" CSV row was missing 3 trailing
  fields against the 14-column header, misaligning every downstream column
  and crashing analyze-ce.py on the first declined query in any run.
- P1: README.md's Status table claimed the table function and optimizer
  integration were "not built" one commit after three commits shipped
  exactly those things. Rewritten against the actual state.
- P1: CI's reusable-workflow refs were pinned to @v1.5-variegata, a branch
  (confirmed live via git ls-remote --heads vs --tags), not a tag -- CI's
  build/lint logic could silently drift to a different commit than the one
  actually vendored in this repo's extension-ci-tools submodule. Pinned both
  uses: refs and both ci_tools_version inputs to the exact SHA
  git ls-tree HEAD extension-ci-tools reports.
- P1 (closed as a side effect): plan.cpp had zero unit-test coverage, the
  module containing the ExecuteCount P0. test/unit/test_plan.cpp is new.
- P2 hygiene taken along the way: the leftover CMAKE_CXX_STANDARD "17"
  default in CMakeLists.txt (a no-op today, but contradicted the file's own
  C++14 rationale 30 lines later) changed to 14; .gitmodules's branch = main
  on both submodules (a footgun for a future git submodule update --remote,
  which would silently move off the pinned commits) removed rather than
  corrected to two different actual refs.

Deliberately not fixed, and why:

- P1: g_memory_limit is a process-global, unsynchronized size_t. Now read
  from more call sites than before (every Arena/ChainingHashTable the
  memory-limit fix above touches), which is a slightly larger surface for
  the exact same pre-existing race, not a new one. Not reachable today --
  Phase 4 parallelism does not exist -- but a real landmine for whenever it
  does. Left open rather than redesigned under time pressure; the right fix
  (capture the limit once per query, thread it explicitly or make it
  thread-local) touches every call site this pass already touched once and
  deserves its own dedicated pass with test cycles, not a second pass
  bolted onto this one.
- 16 P2 items (dead scripts, untested rejection paths, duplicated MCV
  computation between standalone.cpp and table_function.cpp, Arena's
  move-safety, license copyright text, doc staleness in test/README.md and
  docs/UPDATING.md) are recorded in the full report but not actioned here.

Verification: every fix has a dedicated regression test. Full core suite
(asan+ubsan and -O2, test_ftree/test_frep/test_join/test_cost/test_plan) is
98 checks, 0 failures across both configurations. One false alarm along the
way: a background WSL compile briefly showed an error for arena.hpp's new
memory_limit member that turned out to be a torn read of the file mid-edit
(the compiler running concurrently with an in-progress Edit call), not a
real failure -- discarded once the on-disk state was re-read directly and a
clean rebuild confirmed it.

## D18 — Phase 3: the optimizer rule computes real counts, by reusing Phase 2's path rather than building a second one

The rule matched plan shapes from Phase 0 onward but replaced them with an
operator that returned a hardcoded 42. It now runs the query.

**The design choice: reuse, not a native sink.** The obvious reading of the
plan (§3.2) is a sink+source pipeline breaker, with the base-table scans as
real children feeding `Sink()`. That needs N-ary sink pipelines, which nothing
in DuckDB's operator set does for free and which this codebase has never
built. The alternative -- keep `PhysicalFactorized` a childless *source* that
scans the base tables itself, exactly as `factorized_count()` does -- reuses
the scan/plan/execute path already measured against 238 CE queries end to end.
Same `StorageSource`, same `BuildPlan`, same `ExecuteCount`; the only new code
is the part that reads a join graph out of a bound plan instead of out of
argument strings.

What that buys: the risky half of the work was already validated. What it
costs, and it is the defining constraint of the whole design: **the rule drops
the plan's own scans, so any restriction on them that is not carried across
would never be applied by anything.** Not a slow query -- a wrong count. Every
matcher decision below follows from that single sentence.

`BoundRelation` and `StorageSource` moved out of `table_function.cpp` into a
shared header so there is one implementation of the scan, its NULL rule and its
statistics. It also stopped re-resolving tables by name: the optimizer takes
the `TableCatalogEntry` straight from the plan DuckDB already bound, because
re-resolving a bare name against the search path at execution time could find a
*different* table than the query was planned against.

**Four bugs, all found by tests, none by reading.** The first three were in the
new code; the fourth was already on origin/main.

- **Rejecting `dynamic_filters` rejected everything.** DuckDB's own
  JoinFilterPushdown pass attaches a `DynamicTableFilterSet` to the probe-side
  scan of essentially every join. Treating that as "this scan is restricted"
  declined every query the rule exists for. They only ever remove rows that
  could not have joined, so ignoring them leaves the count unchanged.
- **Statistics propagation restricts scans, and the rule has to honour it.**
  DuckDB derives a range from one side of a join (`s <= 75843`), pushes it into
  most of the scans as `table_filters` and leaves it above the rest as a
  `LOGICAL_FILTER`. Refusing either shape declines most real plans: it was 111
  of 119 CE queries. Rather than decline, `BindRegion` now re-keys DuckDB's own
  pushed-down filter set onto our scan and translates a filter above the scan
  (`column <op> constant`, `IS [NOT] NULL`) into the same `TableFilter` types,
  and the storage layer applies them. **This is deliberately not an expression
  evaluator.** Anything more complex declines, because evaluating arbitrary
  expressions here would be a second implementation of DuckDB's semantics
  judged against a count that has to equal DuckDB's exactly. The side effect is
  that filtered queries went from "declined" to "supported": `WHERE a.x = 5`
  factorizes now.
- **Cyclic joins died halfway through executing.** A triangle cannot be
  arranged as an f-tree -- its third relation reaches the other two through two
  different equivalence classes, so its keys never land on one level -- and the
  engine discovered this mid-flight with "key attributes did not converge on
  one level". The check belongs in `BuildPlan`, which is where a caller can
  still do something about it, and it is now there: a relation attaches only
  where the already-joined side of every edge carrying it shares an
  equivalence class. Counting predicates against relations (what the harness
  does) would be wrong in the other direction -- a star written with a
  redundant third predicate is *not* cyclic, and both cases are now unit
  tested.
- **Scanning an empty table crashed, on main, since Phase 2.**
  `DataTable::InitializeScan` asserts when a table has no row groups at all.
  Nothing in the CE corpus is empty, so `factorized_count()` never met one. The
  fix moves both entry points onto the scan API DuckDB's own sequential scan
  uses (`InitializeParallelScan`/`NextParallelScan`), which handles the empty
  case and, separately, is the one that sees rows still uncommitted in the
  transaction's local storage -- the old path got that wrong too.

**Observability, because the second bug cost a 31-minute rebuild to find.**
`factorize_explain` prints, per aggregate, either what was taken over or the
specific reason it was not ("join key w is VARCHAR, not an integer"). The
matcher declines constantly and silently by design, so without this the symptom
of a broken matcher is indistinguishable from a correctly conservative one.
EXPLAIN now also carries the relations, predicates and join order.

**AUTO still behaves as OFF.** The gate is the next piece of work, and until it
exists `auto` promises something it cannot do. Firing unconditionally is
measurably a loss (F16), so the default stays `off`.

**Measured.** The CE corpus, run as plain SQL through the rule rather than
through `factorized_count()`:

| | takes over | answers correctly | wrong |
|---|---|---|---|
| before the filter work | 8 / 119 | — | 0 |
| carrying pushed-down and above-scan filters | 59 / 77 measured | 54 | 0 |
| ... and BETWEEN | **119 / 119** | **110** | **0** |

Each step was a decline the matcher had no business making, and none of them
was visible by reading -- the first cost a full rebuild cycle to find, which is
what `factorize_explain` now exists to prevent. Also: 178 sqllogictest
assertions, and 100 core checks under both asan+ubsan and -O2, all passing.

**Known limitation, and it is the blocker for AUTO:** the 9 queries that do not
answer correctly do not answer at all -- their f-representation exceeds the
memory budget and the query *fails* under `force` rather than falling back to
the stock plan. All 9 are the shapes FINDINGS F17 already identified as the
regime where result size does not predict memory (7 watdiv, 2 yago chains).
Plan item 3.8 called for an abortable operator for exactly this reason. Under
the reuse-first design there is nothing to fall back *to* -- the stock subtree
was dropped at optimize time -- so fallback means keeping that subtree as a
child of `LogicalFactorized` and planning it as well. `force` is documented as
benchmarking-only and a clean error is defensible there; `auto` cannot ship
until this is fixed.

## D19 — Phases 5 and 6: the gate was calibrated against the wrong model of DuckDB, and the corpus measures the wrong regime

Two findings, and the second explains the first.

**The shipped cost model over-charged DuckDB by 45x per result tuple.**
Timing every CE query under `'off'` and `'auto'` on a release build
(`scripts/calibrate-gate.sh`, 119 queries) failed the phase's own exit
criterion: geomean 1.229x, but seven queries regressed and the worst ran 143x
slower -- 74ms against 10s. Re-fitting the model on those measurements
(`scripts/refit-cost.py`) found the error in both directions at once:

| | shipped | measured here | |
|---|---|---|---|
| DuckDB, per result tuple | 3.946e-5 ms | 8.788e-7 ms | 45x too pessimistic |
| DuckDB, startup | 34.33 ms | 9.80 ms | 3.5x |
| ours, per input row | 2.214e-4 ms | 1.225e-3 ms | 5.5x too optimistic |

The per-tuple term is the one that matters, and the reason is specific: for a
`count(*)` DuckDB carries **no payload columns** through the join. The tuples
in its pipeline are empty, and it counts them at a rate the fitted constant did
not imagine. A gate that over-charges the engine it is rejecting fires on
queries that engine was about to win, and it fired on seven.

Compounded, the model believed factorizing was ~250x more favourable than it
is. With the measured coefficients the gate wants the result to be roughly a
thousand times the input before it fires, and re-running the calibration gives
**geomean 1.057 with no regression on any query it fires on** -- the exit
criterion, met. It now fires on 1 of these 119 queries.

**Which is the right answer, because this corpus is the wrong regime.** The
runnable subset tops out at 8e8 result tuples, where DuckDB is entirely
comfortable; D15 already said the project's target is the regime the benchmark
excludes. That regime is real and was measured earlier (tmp/ce_excluded_*.csv):
DuckDB times out past 180s on results from 1e10 to 1.3e17, while the engine
answers a 9.7e11-tuple count in 4 seconds.

Confirmed end to end with the shipping build, on a four-way self-join of a
4.5M-row table over 40k distinct keys:

    SELECT count(*) FROM watdiv1052651 a, watdiv1052651 b,
                         watdiv1052651 c, watdiv1052651 d
     WHERE a.s = b.s AND b.s = c.s AND c.s = d.s;

    factorize_mode='auto': 10,835,546,035,024 in 80.8s, gate fired unprompted
    factorize_mode='off':  no answer in 180s (DuckDB's own measured per-tuple
                           rate puts it near 2.6 hours)

So the calibrated gate does both halves of its job: it declines the regime
where DuckDB wins, and it fires where DuckDB cannot finish at all.

**What this does not say.** The coefficients are this machine's, which is why
the fitting script is checked in rather than the numbers being presented as
constants. `ours.per_output` is inherited rather than re-fitted -- these
measurements cannot separate a per-record term from a per-input-row one. And
the engine is single-threaded against a DuckDB that used 3.5 cores on the
queries above; the gap the gate is measuring is partly a gap in parallelism,
which is Phase 4's subject and would move these numbers.

## D20 — Phase 4: parallelism by partitioning the join key, and the default that hid it

**The design.** The paper parallelises inside the join: concurrent bottom-inserts
into one shared representation, with only the insertion-point nodes locked. This
does something else -- each thread counts one bucket of a hash partition of the
join key, and the buckets are summed. Every output tuple assigns one value to
that attribute, so bucketing on it cuts the output into disjoint pieces, and
thread-count invariance follows from the partition rather than from locking
discipline. The trade is that a bucket still has to look at every row to find
itself, and that insertion never has to be made thread-safe (risk R5, which this
sidesteps rather than solves).

Two details that are not optional. A bucket that will not fit is subdivided
*within itself* -- the buckets of a finer modulus congruent to it -- so a thread
refining its own work cannot touch another's. And the memory cap is thread-local,
so it is divided by the number of buckets; handing every thread the whole budget
would let N threads use N times it.

**The bug worth recording.** The first version ran on one thread whatever
`threads` was set to, and nothing about the operator was wrong. DuckDB never
asked: `Pipeline::ScheduleParallel` gives up when the pipeline's *sink* is
serial, the sink is the result collector, and DuckDB picks its single-threaded
form when it believes the plan preserves insertion order.
`PhysicalOperator::SourceOrder()` defaults to `INSERTION_ORDER`, and this
operator had never said otherwise -- so a source emitting exactly one row was
treated as order-bearing, and `ParallelSource()` was never consulted at all.

Declaring `NO_ORDER` is the entire fix, and it is worth being clear about why
the tests did not find it: they assert answers, and the answers were right the
whole time. A default in a base class that the extension never mentions is
invisible to any test that does not measure the thing it silently governs.

Measured on the four-way self-join whose result is 1.08e13 tuples:

| threads | time | speedup |
|---|---|---|
| 1 | 82.7s | 1.00x |
| 2 | 47.2s | 1.75x |
| 4 | 29.3s | 2.83x |
| 8 | 24.3s | 3.40x |

Identical answers throughout. The query stock DuckDB does not finish in 180
seconds now answers in 24. Scaling is well short of linear, and the reason is
inherent to the design: every bucket reads every row to find its own, so the
filtering pass does not divide even though the join does. Phase 4's stated exit
bar (>=8x at 32 threads) remains unmeasurable here -- this box has 8 logical
cores (open item O4) -- and on the evidence of 3.4x at 8, partition-parallelism
alone would not reach it.

Two smaller changes went in alongside, neither of which moved the timings: the
scan decides a column's type once per batch rather than once per value, and
relations are read once into a shared snapshot rather than once per thread. Both
were hypotheses about the flat scaling, and both were wrong about that -- the
sink was the answer -- but a scan that re-decides a type 18 million times is
worth not having either way.

## D21 — The cost coefficients, checked against an unbiased sample

D19's coefficients were fitted on 24 measurements of this engine, all of them
queries the *previous* gate had chosen -- the sample most likely to flatter it.
Re-fitting after Phase 4 exposed how badly that biases: with a well-calibrated
gate firing on one query in the corpus, the sample was one query, and replaying
the resulting model would have fired on 14 with 8 of them slower. The old
regressions, re-derived from a single point.

The calibration now times `'force'` on every query, which measures this engine
whether or not the gate wanted it. On 104 such samples:

| | shipped | unbiased re-fit |
|---|---|---|
| ours, per input row | 1.225e-3 | 1.278e-3 |
| DuckDB, startup | 9.799 ms | 9.254 ms |
| DuckDB, per input row | 1.105e-6 | 1.221e-6 |
| DuckDB, per flat tuple | 8.788e-7 | 9.961e-7 |

Within a few percent throughout, from four times the data and none of the
selection bias, so the shipped numbers stand. Replaying the re-fit fires on one
query and that query is faster.

Worth noting what the agreement does *not* say: our per-input-row cost is
unchanged by Phase 4's parallelism, because the corpus queries are milliseconds
long and dividing a millisecond across threads buys nothing. The 3.4x is real
and it is measured on the queries this engine exists for -- which are, once
again, not the ones in this corpus.

## D22 — DuckDB's own tests, with the extension loaded

Risk R9 is that an optimizer rule sees every plan in every query, so the blast
radius is the whole engine rather than the queries it takes over. The evidence
that matters is DuckDB's own suite passing with the extension in the process,
at the default `factorize_mode='off'` -- the configuration a user gets by
installing it and doing nothing, and the one where a change in results would be
least forgivable.

| suite | assertions | result |
|---|---|---|
| `test/sql/join/*` | 13,745 | pass (3 skipped, require tpch) |
| `test/sql/aggregate/*` | 251,914 | pass (9 skipped) |
| `test/optimizer/*` | 4,218 | pass (23 skipped) |

`scripts/duckdb-regression.sh` runs these three: joins are the shape the rule
matches, aggregates are the operator it replaces, and the optimizer tests are
where a rule that rewrites plans wrongly surfaces first.

The plan also asks for a version matrix (§7.1). This extension supports exactly
one DuckDB version -- v1.5.5, pinned in D4 -- because the C++ API it uses is
version-locked (D5), so a matrix over versions it does not claim to support
would test nothing. The matrix that exists is over platforms, minus WASM.

## D23 — The v2 roadmap: what §10 asks for, and which parts of it are real

Section 10 is explicitly post-ship and says not to begin before Phase 6. Phase 6
is done, so here is all of it, with the parts that turned out cheaper and dearer
than the plan expected.

**§10.4 EXISTS — cheap, as advertised, and cheaper than expected.** The plan
budgets 1-2 weeks for a semi-join that stops at the first witness. Phase 4's
partitioning had already made it nearly free: a join is non-empty exactly when
some bucket of its join key is non-empty, so the buckets are examined one at a
time and the first that yields a tuple ends the query. `factorized_exists`.

**§10.3 tuple output and §10.2 LIMIT — one piece of work, not two.** The plan
sizes LIMIT at "3-4 weeks on top of a working flattening iterator", with an
O(log n) seek built on per-node prefix sums. That machinery is for *random
access* by tuple id -- what §5.1 wanted for parallel enumeration. LIMIT does not
need it: taking the first k tuples needs only a prefix, and a prefix is what an
iterator that can stop already gives. So `Enumerate` takes a limit and stops,
and §10.2's demo falls out of §10.3 with no id arithmetic at all.

The §4.6 hazard the plan flags -- bottom-inserts leave records whose child slot
is empty, and enumerating one invents tuples that do not exist -- is handled by
construction rather than by a check: an empty slot iterates nothing, exactly as
SubtreeSize multiplies by zero. The regression test is a three-relation chain
whose middle relation has rows that join upward but not downward.

One piece of C++ worth recording. The natural way to write the enumerator is
with a templated continuation ("what to do once this subtree is fixed"), and it
cannot be done: the continuation at depth d has a type built from the type at
depth d-1, so the compiler would have to instantiate a family of functions whose
size is a property of the *data*. It is type-erased through a function pointer
instead, one indirect call per level, which is the price of the recursion
terminating at compile time.

**§10.1 GROUP BY — the case the plan names, and a decline for the rest.** When
the grouping key is at the top of the f-tree, each root record *is* a group and
the tuples belonging to it are the ones its subtree denotes -- a number the
representation already memoizes. Summing `SubtreeSize` per distinct value is the
whole algorithm, and it never enumerates a tuple.

Below the root it declines. Descending to a deeper key is reachable in principle
(carry the product of the sibling slots not descended into), and a key scattered
across independent sibling branches needs their cross product and is a different
algorithm entirely. The plan says detect that case and decline; this declines
one case more than it strictly must, and says which.

**§10.5 and §10.6 are not implemented, deliberately.** §10.5's outer and
non-equi joins the paper itself calls "naïve" and leaves as future work, and the
plan says to treat them as research and not put them on a schedule. §10.6 is a
list of things not to pursue -- `COUNT(DISTINCT)` is not a semiring, `ORDER BY`
and window functions need the flat ordered relation -- and implementing them
would mean disagreeing with the plan's reasoning rather than executing it.

All four are table functions rather than optimizer rules. The rule matches
`count(*)` and nothing else, so `SELECT ... LIMIT 100` over a join is still
answered by DuckDB unless the user calls `factorized_tuples` by name. Wiring
these into the matcher is the obvious next step and is not done.

## D24 — A `git checkout` under a running build produces a binary of no particular version

Two sessions were working in this repository at once, in one working tree
rather than two clones. The other session needed to put its work on a branch,
so it ran `git checkout -b`, committed, pushed, and switched back — ninety
seconds, and it verified afterwards that none of this session's files had been
disturbed. They had not been.

The damage was to a build that was running at the time. `git checkout` rewrites
the working tree; a compiler reading that tree does not stop. Object files
produced before the switch were compiled against one version of
`src/core/join.cpp` and those produced after against another. The link then
succeeds, because the two versions differ in ways that do not change any symbol
the linker checks, and the result is a binary that corresponds to no commit,
no branch, and no state the source was ever in. Nothing warns.

This is DECISIONS D17's torn read -- a background compile reading a file
mid-edit -- one level up: there the inconsistency was within a file, here it is
across the tree. The lesson generalises the same way. **An artifact built while
its inputs were changing is not evidence, whatever it reports.** The build was
discarded and re-run over a stable tree, at a cost of twenty minutes, which is
the whole price of noticing.

Both sessions now hold to: announce before any git command that rewrites the
working tree (`checkout`, `switch`, `restore`, `reset`, `merge`, `rebase`,
`stash`) and wait, rather than checking for signs of a build and guessing --
"is anyone building right now" is not answerable by looking. Read-only commands
and `add <path>` / `commit` / `push`, which do not move the tree, need no
announcement.

The better answer, and what to reach for next time, is not to move the shared
tree at all:

    git worktree add ../factorize-branch -b <branch> <base>
    # commit and push from there
    git worktree remove ../factorize-branch

A second checkout backed by the same `.git`. The shared tree's HEAD never
moves, so nothing anyone is reading or compiling changes, and the branch still
reaches the remote.

## D25 — Four bugs behind one blind spot: a missing row is not a wrong number

Multi-column `GROUP BY` (plan §10.1) needed grouping columns that are not join
keys, which meant appending them to the scan the way a summed column already
was. Asking what happens to a row the scan drops turned up four bugs. Three
were live on `origin/main` and none had anything to do with the feature being
added.

    H1  IS NULL on a non-join column          3     = 3        not a bug
    H2  sum, every contributing row NULL      NULL vs 0        BUG
    H5  filter on a non-join column + sum     500  vs 700      BUG
    H6  same with count(*), nothing appended  2     = 2        not a bug
    H7  sum over a join matching nothing      NULL vs 0        BUG
    H8  grouped sum, one all-NULL group       2 rows vs 1 row  BUG

**H5, the filter position.** `StorageSource` reads `bound.columns` and then
`bound.filter_columns`, so a filter-only column is placed at
`columns.size() + its index in filter_columns`. That position is *derived from*
`columns.size()`, and the summed column is appended to `columns` afterwards, so
every filter-only position was short by the number appended and the filter
landed on whatever now occupied its slot:

    p(k, v, w), q(k):  SELECT sum(p.v) FROM p, q WHERE p.k = q.k AND p.w > 5
    filter on w:  position = columns.size()(=1, just k) + 0 = 1
    sum appends v:  columns = [k, v], so the scan reads k=0, v=1, w=2
    the filter still says 1, and `v > 5` is not `w > 5`: 700, not 500.

Introduced by 767409d and live for two commits. The comment above the append
was worse than the bug -- "Appended, never inserted: inserting would move the
ground under them" is true of `columns` and blind to the positions computed
*from* `columns.size()`, so it is the reasoning that says the bug cannot exist.
Fixed by running the aggregate's appends before the filter re-keying, and the
comment now states the invariant a reader can check: nothing may be appended to
`columns` below that point.

**H2 and H7 are one bug, and it is not about NULL.** `sum` over zero
contributing tuples returned 0 where SQL says NULL, because a total of zero
cannot distinguish an empty join from a join of zeroes. NULLs only make it
easier to reach -- an ordinary join that happens to match nothing is enough.
`ExecuteSum` now counts the tuples beside the sum and the operator emits NULL
when that count is zero. The count is taken *after* the NULL drop, so it is
exactly the number of join tuples built from rows with a non-NULL value, and
SQL returns NULL precisely when that is zero: the same set, not a proxy for it.

The first version of H7 used disjoint key ranges, DuckDB proved the join empty,
planted an `EMPTY_RESULT`, the rule declined, and the answer came back correct.
A false negative. **A test for an empty join needs one whose emptiness is only
discoverable at run time**, or it tests the decline path instead.

**H8, and what the drop rule actually is.** The scan drops a row holding a NULL
in any column of `bound.columns`. Correct for a join key: NULL equals nothing,
so the row cannot contribute to an inner join. Correct for a summed column when
ungrouped: such a row contributes NULL to the sum either way. Wrong for a
grouping column, and wrong for a summed column when grouped -- a group whose
every row is NULL there is still a row of the answer, with a NULL sum, and
dropping those rows deletes the group. Nothing in the representation can carry a
NULL instead: a key slot is an integer with every value already spoken for.

Handled in two layers, and the order matters. **Decline** when the statistics
say the column may be NULL, so the common case is a query DuckDB answers rather
than an error. **Throw** from the scan when a NULL turns up anyway, because
`DataTable::GetStatistics` is `row_groups->CopyStats` -- committed row groups
only, so rows appended in the current transaction are not covered, and
`StorageSource` reads transaction-local storage. A throw as the *primary*
behaviour would turn a working query into an error under `auto`, which breaks
the contract from the other side; a throw as the backstop keeps it.

The second layer was shipped untested, which is worth admitting because the
argument that it was covered sounded good: a corpus re-run came back
byte-for-byte identical across 2590 queries. It could not have exercised the
guard. The run used `EXPLAIN`, which optimizes without executing, and the guard
fires at scan time -- and the corpus tables are empty, so executing would not
have helped either. **A run that cannot reach the code proves nothing about it,
however many queries it contains.** Exercising it needs data and a transaction:

    committed (1,10),(2,20)   statistics see no NULL -> takes over, answers
    BEGIN; INSERT (1,NULL)    statistics still see no NULL -> takes over,
                              and the scan throws
    committed (1,NULL)        statistics see it -> declines, DuckDB answers,
                              NULL group present

All three are now in `test/sql/factorized_optimizer.test`. The middle one is the
whole reason the guard exists, and until it was written the hole was argued for
rather than demonstrated.

**H1 is the most useful of the six, and it passed.** Filter-only columns never
enter `bound.columns` -- they are scanned but never read into the row buffer --
so the drop rule is "any *join* column", not "any scanned column", and filter
columns were always exempt. Only the aggregate's appends put value-carrying
columns into that set.

The invariant that falls out of it is worth more than the bug that found it:

> A relation's columns live in two sets with different NULL semantics, and only
> one of them is filtered. `bound.columns` is read into the row buffer and a row
> is dropped if any of them is NULL; `bound.filter_columns` is pushed into the
> scan as a filter and never read, so those columns are exempt. **Any future
> feature that needs a column's NULLs removed must put that column in
> `bound.columns` deliberately** -- arriving as a filter column silently skips
> the filtering.

The known case is the `<>` join count planned in
`tmp/20260904-10.5-other-join-types.md`, computed as the difference of two
equi-join counts, which is exact only over inputs with NULL-valued rows dropped
on both sides. Measured, the unfiltered form gave 12 against a truth of 5.

**The blind spot.** Four bugs, one gap: every `sum` test joined on equalities
only, every group test used a join column as the key, and no test had a NULL
near an aggregate. H5 needs *both* a filter on a column the region does not
otherwise read *and* an aggregate that appends one -- the H6 twin has the same
filter, no append, and was always right. Each fixture now states what it needs
to reproduce, so the pair says the append is the cause rather than the filter.

The general form is worth keeping: **these are all missing rows, not wrong
numbers.** A count that is wrong looks wrong. A row that is absent looks like
the query. Comparing a single scalar against stock DuckDB cannot see any of
them, which is why the fuzzer now generates grouped queries and compares whole
result blocks rather than one line.

**And the corpus could not have caught them either, which is the more
uncomfortable half.** None of the 2590 CE queries is capable of reaching any of
the four: every one is an ungrouped `count(*)` over equality predicates with no
NULLs. H2 and H7 need a sum; H5 needs a sum and a filter; H8 needs a grouped
sum. Zero of 2590 qualify -- not because the corpus is small, it is 2590
queries, but because it is *uniform*. 466 assertions and 2590 corpus queries
passing is not evidence about grouped sums with NULLs; it is evidence about
ungrouped counts, 2590 times over.

So the reaction to four bugs is not only "add tests". It is that **corpus
breadth and corpus size are different quantities, and this project has size.**
The 99.3% acyclic coverage figure measured against CE is a statement about one
query shape, and reporting it without that qualification would turn a
matcher-coverage number into an implied claim about SQL in general.

## D26 — The gate declined everything, and a threshold is why

Measured on a synthetic star -- 1000 keys, three arms of 30000 rows, a 27
million tuple join -- the engine is **32x faster** than stock DuckDB and
`factorize_mode='auto'` **declined it**. So did every other aggregate shape over
the same join, with an identical prediction: 123ms for us against 21ms for
DuckDB. On a release build the truth is 3ms against 96ms.

That is not a coverage problem. Multi-column `GROUP BY`, `sum`, and
multi-expression aggregates all landed in the preceding commits, and on this
shape, in the mode a user actually gets, none of them did anything.

**Two errors, and they interact through a threshold rather than multiplying.**

    ours    123ms predicted,   3.5ms actual    35x pessimistic
    duckdb   21ms predicted,    94ms actual    4.5x optimistic

The obvious move -- fix our own side, the larger error, and leave DuckDB
under-charged because that is the conservative direction -- **fires on nothing**.
Measured across 15 shapes it changed no decision at all. `min_duckdb_work_ms`
compares against `duckdb_ms - startup_ms`, and with DuckDB charged 0.88ns per
tuple its predicted *work* does not reach 10ms until roughly 10^10 tuples. The
floor was refusing every query regardless of what our side cost.

So under-charging the engine you are trying to beat is not the safe direction
when a floor sits underneath the comparison. **A threshold turns an
under-estimate into a blanket refusal**, and no amount of accuracy on the other
side of the comparison can recover it.

**Why our own side was 35x out, and it is structural.** `ours.startup_ms` was
pinned to 0, so the per-input-row slope had to carry the fixed cost as well. A
slope carrying an intercept over-charges by that intercept times every row: at
91,000 rows it predicted 111ms of scan for a query that runs end to end in
3.5ms. A free intercept is the difference between a model that is imprecise and
one that cannot express the shape at all.

    ours    {0.0,   1.225e-3, 1.694e-4}  ->  {0.108542, 2.445e-5, 3.961e-5}
    duckdb  {9.799, 1.105e-6, 8.788e-7}  ->  {0.0,      2.324e-5, 3.981e-6}

The DuckDB per-tuple value has now been wrong in both directions: 3.946e-5
over-charged and caused D19's seven regressions; 8.788e-7 under-charged and
caused this. The new value is 10x *below* the one that caused regressions, which
bounds the risk of having moved back toward it.

**Measured, 15 shapes, release build, held-out validation:**

                        fires-and-wins   misses a >=2.5x win   regressions
    shipped                    0                  6                 0
    ours refitted only         0                  6                 0
    both refitted              3                  3                 0

End to end, `off` against `auto` rather than against `force`, which is the
question a user asks:

    star n=3 d=1000  r=100000    36ms -> 15ms   2.40x
    star n=4 d=10000 r=100000    53ms ->  8ms   6.62x
    star n=5 d=1000  r=10000     46ms ->  5ms   9.20x
    the 27M star                 96ms ->  3ms  ~32x
    the other 12 shapes          declined, unchanged

**The grid was win-shaped, which is the criticism that mattered most.** Every
shape in it was a star or a chain -- shapes with fan-out, where factorization
has something to exploit and the engine was always going to win. A fit validated
against such a grid is validated against nothing, because every error that makes
the gate fire *too eagerly* is invisible in it. Two counter-shapes now sit in
the grid and in the test suite:

  - a join on a unique key, no fan-out anywhere, where the representation is the
    same size as the flat result and pays for structure it never uses. Forced,
    4ms becomes 16ms. The estimator sees it: predicted compression 0.6x, a
    representation *larger* than the tuples it stands for. Declined.
  - a star too small to matter, where DuckDB finishes in under a millisecond.
    Declined by the floor, which is a different refusal for a different reason
    and worth exercising separately.

Across all 22 shapes under `auto`, including the ones chosen to lose, **nothing
is slower than 1.00x**.

And the converse belongs here too, because it contradicts the folklore: a chain
of three is the shape where nothing is independent and the representation is the
same order as the flat result. It wins **24x** anyway -- 148ms against 6ms --
because the win is not compression, it is that the join is never materialised.
A gate reasoning about compression would decline it. D14 already chose to
compare predicted *times* instead; the chain is the case that proves the
distinction was worth making.

**The real bug was that nothing could tell you.** `refit-cost.py` and
`calibrate-gate.sh` both read `tmp/ce_runnable.psv`, so re-fitting needed a
5.3 GB corpus nobody had downloaded: on a fresh checkout the coefficients could
only be inherited, which is the one thing DECISIONS O11 says not to do with
them. And none of the four quantities the gate reasons about was observable from
SQL, so a wrong decline could not be diagnosed from outside -- the decline
message printed the two times and withheld the two sizes, which were printed
only on the firing path, where nothing needed diagnosing.

`scripts/calibrate-synthetic.py` needs nothing but `range()`.
`factorized_stats(tables, joins)` returns count, records, bytes and slices.
Declines now carry the sizes.

**Caveats, since this is a calibration.** Synthetic uniform data, one machine.
F18 records that our flat estimate is *already* weakest on uniform data, so this
describes the friendly case: it is a way to find a coefficient wrong by an order
of magnitude, not a substitute for calibrating against a corpus resembling the
user's queries. And two measurement bugs were caught in the harness before it
was trusted -- `-c` does not interpret dot commands, so `.timer on` silently
produced no timings; and `SET` emits its own ~0ms `Run Time` line, so taking a
minimum reported 1.0ms for both modes on a query that takes 94ms and 3.5ms.
Both would have fitted coefficients confidently on garbage.

## D27 — `PhysicalRecursiveCTE` is not a template, and the fallback costs parallelism

Plan §7.5 asks that an internal error in the factorized path fall back to the
unmodified plan rather than surface. The plan the operator replaces was being
dropped, so there was nothing to fall back *to*; it is now carried, planned, and
built into a pipeline the executor is deliberately not given.

It works. The scan guard of D25 -- a NULL the statistics could not see, because
`DataTable::GetStatistics` reads committed row groups while the scan also reads
the current transaction's -- used to fail the query. It now falls back and
answers correctly, NULL group included.

**The mechanism, and why it looked easy.** `PhysicalRecursiveCTE` builds a
`MetaPipeline` for its recursive side that is constructed *standalone* rather
than registered with its parent, so the executor never schedules it, and drives
it by hand from `GetData` with `ReschedulePipelines` + `WorkOnTasks`. That is
exactly the shape a fallback needs: free until used.

**Three ways the precedent does not transfer**, each discovered by a failure far
from its cause.

1. *It is never readied.* `Executor::ScheduleEventsInternal` readies standalone
   meta pipelines only through a hard-coded `Cast<PhysicalRecursiveCTE>` over a
   list an extension cannot join -- `AddRecursiveCTE` takes a `PhysicalOperator&`
   and the loop casts. An extension must call `MetaPipeline::Ready()` itself.
   The symptom otherwise is `"Attempted to access index 1 within vector of size
   1"` from inside a worker thread.
2. *It never re-enters.* `WorkOnTasks` runs arbitrary queued tasks, and one can
   be another `GetData` on this operator **on this thread**, which a held mutex
   deadlocks against. A recursive CTE is a serial source, so a second task for
   its pipeline cannot exist and the question never arises.
3. *It never parks a worker.* Same reason, and this is the one that decided the
   design. With a parallel source, several tasks arrive in `GetData` and block
   on the state lock while the owner drives the fallback -- and a parked worker
   is one the executor cannot use to run the pipeline the owner is waiting for.

        threads=1   40 fallbacks, 40/40, 4s
        threads=2   40/40, 4s
        threads=4   hung after 24
        threads=8   hung after 3

   `gdb` on the hung process: six worker threads blocked on the same futex
   inside `GetDataInternal`/`EmitFallback`, one idle in
   `TaskScheduler::ExecuteForever`.

**So the fallback and parallel counting cannot both be had**, and the trade is
priced rather than assumed: a serial source gives up D20's slicing, measured at
7ms against 2-4ms at eight threads on the 27M star. `factorize_fallback`
defaults to **true**, because on that star the engine is 32x faster than stock
and serialising leaves about 11x -- trading 32x for 11x to make failure
impossible is a good trade, and taking it silently would not have been.

**A predicate that reads like the question you are asking.**
`Exception::InvalidatesTransaction` returns `true` by *default* for nearly every
type, `INVALID_INPUT` included. It describes what a connection must do once an
error has *propagated*; using it to ask whether a caught, unpropagated error is
recoverable rethrows everything, the fallback never runs, and the test written
to prove it passes. Only `FATAL` (nothing left to fall back to) and `INTERRUPT`
(the user asked for it to stop) are rethrown.

**Fault injection, deliberately.** `factorize_debug_fail` makes the factorized
path throw. The only natural trigger needs a grouped query, an open transaction
and a statistics miss at once, so without it four of the five aggregate shapes
would have shipped with a recovery path nobody had run -- which is the failure
this project has now catalogued eight times.

**And the one lesson that is about changes rather than measurements.** The
livelock was introduced *by a fix*. Holding a lock across `WorkOnTasks` was a
real hazard, reasoned about correctly and never observed; replacing the lock
with an atomic and returning early broke the case that had already been measured
working. Of the eight, this is the only one where the fix was the defect.

> **Re-run the case that already worked before believing the fix.**

Every other instance was something that never ran. This was something that ran,
and was wrong, because a hazard was reasoned about instead of reproduced. It has
since happened a second time, in the other session: a validation added for an
ambiguity that had been reasoned about, which broke every existing caller and
was caught by `test_outer` aborting under the sanitizers.

Both were caught by a test written for something else. A test written *for* a
change tests that change's intent, and in both cases the defect was collateral:

> **The tests worth having are the ones covering what already works, not only
> what just changed.**

That is a reason to keep old tests rather than fold them into new ones, which is
the opposite of what a tidy suite looks like.

The narrower form, from the same pair of bugs, is about the shape that invites
them. `preserve` and `kind` could both say "this is an outer join"; my two
candidate exception predicates could both claim to answer "is this
recoverable". Redundancy kept consistent by discipline rather than by
construction:

> **Two fields that must agree are two fields that can disagree.**

The fix in both cases was to delete the redundancy, not to reconcile it.

## D28 — Composite-key joins were refused as "cyclic", and the check was a proxy

`A.k1 = B.k1 AND A.k2 = B.k2` is what every composite foreign key writes, and
`BuildPlan` refused it project-wide with `"cyclic join graph: no relation
attaches on a single key"`. The graph is acyclic and the engine computes it
correctly, so the message was wrong twice over. Verified reachable from SQL --
the matcher does not decline it earlier, which was the question that would have
made it moot:

    same tables, single-column key   takes over
    two relations, composite key     declined "cyclic join graph"
    three relations, one composite   declined "cyclic join graph"

**The check was a proxy for the real constraint.** `MakeKeyReader` reads every
key attribute from *one output level* and throws `"key attributes did not
converge on one level"` otherwise -- mid-query, where a caller can do nothing
about it. The planner approximated that by requiring the joined-side keys to
share an *equivalence class*. Class-equality implies same-node; same-node does
not imply class-equality; and every composite key is in the gap.

**The proxy could not be tightened, so it was replaced by the thing itself.** An
`FTree` is pure structure over attribute ids -- no data -- so the planner can
construct the tree the engine will build and ask where the keys land:

    FTree probe_shape(simulated);
    const FNode &insertion = probe_shape.CreateRootToLeafPath(keys, LEVELWISE);
    every key must satisfy insertion.HasAttribute(key)

and then advance `simulated` through `MergeTrees` exactly as execution will.
This is not a better approximation, it is the same computation, so it cannot
drift from the engine's behaviour the way a proxy does.

Three details that had to be right:

- **The accumulated side is the upper tree under either insert mode.**
  Bottom-insert puts the build on top and calls it `build`; top-insert puts the
  probe on top and calls it `probe`; the accumulated side is whichever that is.
  So the tree *shape* is mode-independent and only the locking differs -- which
  is what lets one simulation stand for both.
- **LEVELWISE, not NAIVE.** `MergeNaive` collapses every required node into the
  root, so its keys always converge; LEVELWISE builds a path and is the stricter
  of the two. A plan accepted under LEVELWISE is safe under either.
- **Equality propagation belongs in the simulation.** The join maps each
  accumulated-side key through `ShallowestEquivalent` against the live tree, so
  the planner must too, or it would be testing different attributes than the
  ones the join will read.

**The 64-bit cap is load-bearing, and the rule is not the obvious one.** The
packed key is a single word: the constraint is the *sum* of the key columns'
widths, 32 per INT32 and 64 per INT64. So one INT64 key fits at exactly 64,
INT32+INT32 fits at 64, and **INT32+INT64 does not** -- a small discriminator
beside a bigint id, which is the ordinary composite key in a real schema. A rule
written as "not two INT64s" would have let precisely the common case through to
the mid-join exception this exists to prevent. Both sides are measured, since a
key can be INT32 on one relation and INT64 on the other.

It is also load-bearing for *correctness* rather than only for support: the
INT64 arm of `KeyReader::Read` omits the `<< bit` shift its INT32 sibling has.
That is unreachable today, because any key with an INT64 beside another column
is over 64 bits and refused before `Read` runs. Widening the cap without fixing
that shift would make every multi-column key containing an INT64 collide into
one hash key -- wrong counts, no throw. Recorded here so the cap does not look
like a limitation that can be relaxed for free.

One consequence worth stating plainly: because `range()` and most integer
expressions yield BIGINT, a composite key written over BIGINT columns is still
refused. Composite keys work where the columns are `INTEGER` or narrower, which
is the common case in real schemas and not in ad-hoc test data -- the first
end-to-end probe of this fix declined for exactly that reason, correctly, and
looked like a failure.

## D29 — Equality propagation substituted equalities no join had enforced

`factorized_count` returned **5** where the answer is **3**:

    q0(c0) = 1,2,3   q1(c0,c1) = (1,1),(2,2),(2,3)   q2(c0) = 1,2,2
    q0.c0 = q1.c0  AND  q1.c1 = q2.c0  AND  q2.c0 = q0.c0

Silently, through a documented table function, and present since long before the
composite-key work -- the planner at `f2f769a` gives 5 too. Found by writing a
regression test for something else.

**The cause.** `EquivalenceClasses` merged every predicate of the graph before
any join ran, so `ShallowestEquivalent` could resolve a key through an equality
that was only *implied* by predicates not yet applied. Here the two edges
attaching `q2` together imply `q1.c1 = q0.c0`; both accumulated keys collapse
onto one attribute; the join enforces `q2.c0 = X` twice instead of two different
constraints; and the implied equality is never checked. The class said they were
equal. Nothing made them equal.

**The fix** is an ordering, not an algorithm: propagate only over equalities a
join has *already enforced*, accumulated per step. Resolve this join's keys,
then enforce its edges -- using them first assumes the conclusion.

**Measured over 3000 random graphs**, counting every accepted graph twice, once
by the engine and once by brute force over the full cross product:

                              accepted   silently wrong
    old planner (f2f769a)         2245              129
    composite fix, unsound        2599              132
    both fixes                    2470                0

Tree-shaped graphs were always sound (1000 accepted, 0 wrong, unchanged), and
the fixed planner accepts **225 more** graphs than the old one, so composite
keys survive the soundness fix rather than being paid for with it.

**Two scoping corrections, both of which changed the code and not just its
description.** The obvious readings of the example are wrong:

- *Not only cyclic graphs.* 25 of the 132 are acyclic -- a doubled edge between
  one pair of relations, which is the composite-key shape.
- *Not only a self-equality within one relation.* 54 imply no equality between
  two columns of one relation at all; and 482 graphs that DO imply one answer
  correctly, so it is neither necessary nor sufficient.

A fix framed around either reading leaves wrong answers behind. The causal
statement -- *substituting an equality no join has enforced* -- covers all of
them, and is the same defect as the null-extension case in the §10.5 notes,
where propagation crosses a boundary that does not establish the equality.

**One site keeps the full closure, deliberately.** `ChooseSliceColumns`
partitions the *input* by hashing a column of each relation, and every relation
whose column is transitively equated must land in the same bucket or tuples that
would have joined are separated and the count comes out short. That is a
question about which rows can possibly match, which the whole graph decides --
not about which substitutions a join has earned. Measured rather than argued:
the same corpus through `ExecuteCountSliced` at 2, 3, 5 and 8 slices, 0 wrong at
every modulus. `BuildCostSteps` also keeps it, where a wrong class costs a gate
decision and never a count.

**The regression test asserted the bug.** It was written to check that the bad
graph computes 3. Under the fix it *declines* -- propagation no longer narrows
the key, so the width cap catches it first -- and that looks exactly like a
break unless you already know the premise was wrong. The test now asserts the
property instead: **declined or correct, never wrong.** Declining is always
safe; answering wrongly never is.

> A test written from the example encodes the example. A test written from the
> property survives the fix.

**Why a second reader is worth more than a more careful first one.** Both of
this session's fix-caused defects, and the harness bug below, were caught by the
same asymmetry:

> You test that the changed thing changed. Someone else tests that the unchanged
> thing did not.

The `Report()` fix is the clean illustration. Its author mutation-tested that a
broken check turns its own group's line to `FAIL`; the reviewer checked that a
*clean* group after a failing one still says `ok`. A global latch passes the
first test and fails the second, and it would have been wrong in the opposite
direction -- every group after the first failure reported as failing. Neither
question is harder. A single author just tends to ask only the first about their
own change, because the second means suspecting yourself in a direction you were
not already looking.

That is a stronger argument for keeping the harnesses than any count of bugs
found, because it does not depend on anyone being careful. It is also why
descriptions and implementations must be checked separately: a rule that reached
this codebase as "two INT64s throw" was a true example presented as a rule, and
implementing the message rather than reading `join.cpp:161` would have shipped a
boundary letting INT32+INT64 through to the very exception the decline exists to
prevent. Trusting a summary over the code it summarises is the same error as
trusting an argument over a measurement, pointing the other way.

**And a tenth instance of the session's pattern, in the harness itself.**
`Report()` printed `ok` unconditionally, so a group whose checks had failed
printed its FAIL lines and then said ok. Totals and exit code were right, so CI
caught it -- but a human reads the last line of a group, and that line was a
lie.

**Then the fix for it was wrong the same way.** Replacing the unconditional `ok`
with a global `g_reported` watermark still computed the verdict from state
outside the group. A group that fails and *returns early* -- which several do on
purpose, so that one broken invariant does not print a hundred FAIL lines --
never reaches its own report, and the watermark stamps its failure on the next
group instead. The same injected failure through both harnesses:

    old   FAIL INJECTED failure
          FAIL a composite key counts the same under both insert modes   <- passed
    new   FAIL INJECTED failure
          FAIL two relations joined on two columns at once               <- actually failed
          ok   a composite key counts the same under both insert modes

The old one also loses the failing group's line entirely: seven group lines for
eight groups. The single line a reader uses to localise a failure named a group
that passed, and hid the group that did not.

**The third shape cannot fail that way.** A `Group` object, constructed at the
top of each group with the failure count as it stood then, reporting from its
destructor. An early return still reports, and reports under its own name,
because leaving the scope is what prints. The first two shapes both needed every
group to reach its report and nothing made them; this one makes reaching it the
only way to leave. Now in all eight files and all 52 groups -- one of which had
never printed a verdict line at all. Mutation-tested in both directions: a group
that fails mid-way, and a group that fails and then returns early, each turn
their own line to FAIL, and the group after them still says ok.

Reported by the second session, which hit it writing a sweep that returns on the
first mismatch -- the same reason mine does.

**The refactor that fixed it deleted three assertions.** Rewriting the
`if (failures == 0) { Report(...) }` tails swallowed the `Expect(failures == 0,
...)` line above each one, in the three largest randomized groups -- the single
check each of those groups exists to make. Caught by counting `Expect(` sites per
file against `HEAD` before building. Nothing else would have caught it: a suite
with an assertion deleted goes green, and slightly faster than before. For a
mechanical edit to test code that invariant is cheap enough to assert every
time -- the number of checks must not change.

**And once more in the check rather than in the code.** The run that first
"passed" this fix was `core-test.sh 2>&1 | tail -70`: the exit status belonged to
`tail`, and the log held the last of twenty-two runs. It could not have reported
red. That is the same defect as the coverage analyser that ran, exited 0, and
re-read the previous directory because its `sed` had matched nothing -- so it
belongs among the instances above rather than beside them as a slip, because the
question that catches it is the one this whole entry is about. Not *did the check
pass* but *could this check have failed?* A pipeline that takes its exit status
from its last stage answers no before it is ever run.

**The same question asked of the other harnesses, which found two more.**
`factorized_optimizer.test` documented the key-width cap as a sum -- *two BIGINT
columns are 128 bits and refused, a single one is exactly 64 and still fires* --
over a row count that is 5000 either way. A regression narrowing the cap to "no
BIGINT anywhere" would have passed it, and that is the plausible wrong rule,
since it is what the refusal looks like from outside; the 128-bit half had no
query at all. Both halves are now asserted on the plan, and both new assertions
were inverted once and watched to fail, because an assertion nobody has seen
fail is not yet a check. The implementation turned out to be right -- the point
is that the suite could not have told us either way.

**Five in one night, and the last two were ours after we had written this down.**
Counting only signals that could not have gone red: a fuzzer whose queries all
declined, so three modes agreed trivially; a soundness harness that printed 132
wrong answers and returned 0; a matcher whose decline counts measured which
check ran first; a fuzz wrapper reporting "0 seeds failing" from a loop where
seven of ten seeds never ran, because a seed that does not run cannot fail; and
a build reporting success with compile errors in its log. The last two were
produced by the two of us within an hour of writing the entry above, by the two
people who had just written it.

That is the argument for structural guards rather than for care. Knowing the
shape has not once prevented producing it. What works is a check that does not
need anyone to remember it at the moment it matters:

- the fired counter, which makes a vacuous run fail rather than pass;
- counting `Expect(` sites against `HEAD`, which catches a refactor deleting
  assertions;
- stat-ing the binary before and after a measurement, which discards a result
  taken while something else was rebuilding it;
- requiring a summary line per unit of work, so a unit that never ran is named
  rather than counted as passing;
- reading exit status from the process that did the work, never from a pipeline.

Each of those is one line and none of them asks anyone to notice anything.

**A sixth, and not the same kind.** `$?` does not survive
`wsl.exe -- bash -lc "..."`. The outer Git Bash expands it before the inner
shell ever sees it, so it reads 0 whatever happened:

    wsl.exe -- bash -lc "false; echo EXIT=$?"   ->  EXIT=0
    wsl.exe -- bash -lc "true;  echo EXIT=$?"   ->  EXIT=0

Escaping it does not help and neither does single-quoting the `-lc` argument.
Every `BUILD-EXIT=` and `exit=` printed that way tonight was a constant. What
works is a script file, where the status is taken by the shell that ran the
command rather than interpolated by the one that did not:

    wsl.exe -- bash -lc "bash /path/run.sh false"   ->  INNER-STATUS=1

The five above were checks we wrote that could not go red. This is a channel
that silently replaces a measurement with a constant, and no care on either side
of it would have revealed that -- it surfaced only because build artefacts were
older than the edits that should have produced them. Found by the second
session; it explains its own `BUILD-EXIT=0` on a failed build, which was never
`make`'s doing.

Two claims of ours were re-checked through a script file afterwards and both
stand: `wsl-build.sh` does propagate a failed build, because `time` is a bash
keyword that passes the status through (`time false` -> 1, `time (exit 7)` -> 7,
a failing `make` -> 2); and the unittest binary does exit 0 when its filter
matches no test, which is what `duckdb-regression.sh` now treats as a failure.
The builds behind D30 and D31 were never in doubt for a better reason than
their exit status: both logs contain no `error:` line, and both fixes were
checked by running the repro and by running the regression section against the
build that has the bug.

> Verifying a result through the channel that produced it is not verification.
> Both sessions were saved here by having read a *content* check beside every
> status -- assertion counts, plan files, actual query output -- which is habit
> rather than judgement.

A note on the shared checkout, which produced two of the five. Two sessions
divided the source files between them and divided neither the git index nor the
build outputs. A commit picked up the other's staged files; a build emptied the
binary a measurement was reading. Both are the same omission: a file-level
division says nothing about the artefacts both sides read and write.

`duckdb-regression.sh` took the unittest binary's exit status, which is **0 when
a filter matches no test**. Its default suites are DuckDB's own paths, so a
version bump moving one of them would have turned the engine-regression run --
the script whose entire claim is that DuckDB still passes with the extension
loaded -- into a green run of nothing. A run of zero tests supports that claim
exactly as well as a run that failed, so "No tests ran" is now a failure.

And one that came back clean, which is worth recording at the same weight: the
same script's `"$UNITTEST" ... | tail -3` looks exactly like the mistake I had
just made in my own invocation, and is not one, because `set -o pipefail` at the
top of the file means the pipeline keeps the binary's status. Verified with a
two-line experiment rather than reasoned about, since the reasoning is what had
just been wrong.

**And the one that had already been learned, in the very function that had it.**
`fuzz-modes-agree.py` runs a random query under `off`, `force` and `auto` and
compares the three answers. `make_table` chose its column type per TABLE from
four types, so a join between an INTEGER table and a BIGINT one takes a cast,
the rule declines it as a computed key, and the three modes then agree
trivially -- fifteen queries in sixteen at three tables. Measured by adding a
counter of how many queries the rule actually took over:

    fixed fuzzer            exit=3   8 random queries, 0 disagreements, 0 taken over
    same, rule ripped out   exit=3   8 random queries, 0 disagreements, 0 taken over

Indistinguishable. Before the counter both printed `8 random queries, 0
disagreements` and exited 0, which is the strongest result this file can report.

The part worth keeping is the comment already sitting inside `make_table`, from
an earlier fix to it:

> Name the VALUES columns rather than relying on the generated names, which
> differ between DuckDB versions and silently made an earlier version of this
> script generate tables that never got created -- *a fuzzer that tests nothing
> reports no failures, which reads exactly like success.*

Someone hit this condition, understood it exactly, wrote that sentence, fixed
the cause, and the tool walked back into the identical condition by a different
road. That is a better argument for guarding the condition than either instance
above, because it is the careful version failing: knowing the shape did not
prevent reproducing it. The type is now chosen once per query, and a run that
took nothing over exits 3 rather than reporting success -- the same twelve
queries now fire on five, and the counter passes as well as fails, which a
control has to do to be one.

## D30 — The §7.5 fallback can make a query fail that would otherwise succeed

Found by `fuzz-modes-agree.py` on the first run in which it had ever actually
exercised the operator (see D29). Fixed; the cause is at the end of the entry.

    CREATE TABLE f0 AS SELECT CAST(v0 AS SMALLINT) AS c0 FROM (VALUES (1), (NULL)) AS src(v0);
    CREATE TABLE f1 AS SELECT CAST(v0 AS SMALLINT) AS c0, CAST(v1 AS SMALLINT) AS c1
                      FROM (VALUES (0,1),(1,0),(1,1),(0,1),(0,1)) AS src(v0, v1);
    CREATE TABLE f3 (c0 SMALLINT, c1 SMALLINT);          -- empty

    SET disabled_optimizers='compressed_materialization';   -- REQUIRED, see below
    SET factorize_mode='force';
    SELECT t2.c0, sum(t0.c0) FROM f1 t0, f3 t1, f0 t2
     WHERE t0.c1 = t1.c0 AND t0.c0 = t2.c0 GROUP BY t2.c0;

    INTERNAL Error: Attempted to access index 0 within vector of size 0

**What it takes.** Every one of these is necessary; removing any makes it pass:
a grouped `sum` (plain `count(*)` is fine), a relation with no rows, and a NULL
in the relation the grouping column comes from. `off` and `auto` both answer
correctly -- `auto` because the gate declines at this size, which is why no test
caught it.

**Where.** Plan time, not run time: `EXPLAIN` alone triggers it, which is also
why §7.5's run-time fallback cannot catch it. The throw is DuckDB's own, in
`PhysicalHashJoin`'s constructor at `physical_hash_join.cpp:89`, reading
`rhs_input_types[rhs_col]` where the right child's types are empty and the
projection map still asks for column 0.

**Why it is ours.** `SET factorize_fallback=false` makes the query work. The
fallback plan is a snapshot of the logical plan taken when the rule fires,
parked in a member that is not in `children`, and planned later by
`LogicalFactorized::CreatePlan`. Optimizer passes that run after the rule
therefore never reach it. Disabling either `statistics_propagation` or
`filter_pushdown` avoids the crash, which is the prediction that diagnosis
makes: an empty relation is exactly what those passes rewrite, and only the
hidden copy keeps the pre-rewrite shape.

So the feature built to stop an internal error reaching the user is, on this
shape, the thing producing one. It is pre-existing -- a build from before
today's merges fails identically -- and it needs no factorized execution to
happen at all.

**User-reachability is open, and this entry first claimed otherwise.** The
`disabled_optimizers` line above is required, and was missing from the first
version of this entry; the second session could not reproduce it without that
line and said so. Without it the rule declines with compressed materialization
in the way, so no fallback is built and nothing goes stale. This entry then said
"a shape like this on tables big enough for the gate to say yes would reach a
user", which was never measured. What is now measured: the rule *can* fire on a
grouped sum with compressed materialization left on -- a group key whose range is
too wide to compress escapes it -- so that pass is not an absolute barrier. But
no construction has yet produced the crash with it enabled. Firing is reachable;
this crash is not yet known to be. Recorded as conjecture until someone builds
the shape.

**The fix is three lines, and the diagnosis above is what made it look bigger.**
"Passes running after the rule never reach it" is true and is not the cause.
DuckDB resolves types in one pass at the start of physical planning:

    void LogicalOperator::ResolveOperatorTypes() {
        types.clear();
        for (auto &child : children) child->ResolveOperatorTypes();
        ResolveTypes();
    }

`PhysicalPlanGenerator::ResolveAndPlan` calls that on the plan root, and it
walks `children`. The fallback is parked beside `children` rather than in them,
so it is the one subtree the pass never reaches -- while binding resolution
*does* reach it, because `ResolveColumnBindings` visits it by hand. The fallback
arrived at the physical planner with its bindings resolved and its types not,
and a node with empty types is what walked off the end of that vector.

So `LogicalFactorized::ResolveTypes` now calls `fallback->ResolveOperatorTypes()`
itself, which also restores the order DuckDB uses everywhere else: types, then
bindings, then planning.

The fix first proposed here was to make `fallback` a real child, which would
have handed the subtree to every later pass and then required proving that none
of them rewrites it into something other than the query it replaced -- a hazard
invented by the fix rather than found in the bug. Reading
`ResolveOperatorTypes` before writing the change is what replaced a design
change and an open question with three lines and none.

> A diagnosis can be true, predictive, and still not name the cause. This one
> predicted the `statistics_propagation` and `filter_pushdown` results
> correctly, which is exactly what made it convincing.

Regression test in `test/sql/factorized_optimizer.test`, which asserts the query
still FIRES before asserting its answer -- a decline would pass the answer check
while testing nothing. Validated by running the section against the build that
has the bug: it fails there and passes here.

## D31 — A NULL in a summed column silently dropped the row from `count(*)`

The same fuzzer run that produced D30 produced a wrong answer, which is worse.
All four seeds failed; this is the shape behind the fourth.

    CREATE TABLE t AS SELECT * FROM (VALUES (NULL::INTEGER, 0), (0, 1), (0, 1)) AS v(c0, c1);
    SET factorize_mode='force';
    SELECT count(*), sum(a.c0) FROM t a, t b WHERE a.c1 = b.c1;
    -- 4, and the answer is 5

`count(*)` alone agrees. `sum(a.c0)` alone agrees. `count(*)` with a sum over a
*non-nullable* column agrees. Only the combination is wrong, and it is wrong
silently, in `auto` as well as `force` whenever the gate says yes.

**The cause is a correct sentence applied to the wrong scope.** Binding a summed
column, `optimizer_rule.cpp` guarded NULLs only when `region.grouped`, over this
reasoning: *ungrouped, a row whose summed value is NULL can be dropped, it
contributes NULL to the sum either way.* That is true of the sum. It is false of
everything else in the query. `storage_source.cpp` drops any row with a NULL in
any column the region reads, so adding the summed column to the scan makes the
NULL row vanish from `count(*)` too -- and from any second sum, which loses that
row's contribution.

**The fix** is the condition, not the reasoning: a row may be dropped only when
that sum is the whole answer. `region.grouped || region.aggregates.size() > 1`.
A lone `sum` over a nullable column still runs, which is the common case; adding
any second aggregate declines it on the statistics as the grouped path already
did.

**What that costs, measured on both sides.** Locally: `count(*), sum(nullable)`
declines, `count(*), sum(non-nullable)` fires, a lone `sum` over a nullable
column fires, and an all-NULL sum still returns NULL rather than 0 -- all four
asserted in the suite, so the cost is a checked fact rather than a sentence. On
TPC-DS, measured by the second session, the cost is currently **zero**: all five
previously-firing queries still fire with identical takeover counts, q77's six
regions included, so the declined shape was not among what the rule was already
reaching.

That measurement had to be asked for. A decline is invisible to the differential
that found the bug -- `off` and `force` agree perfectly when `force` is not
running -- so trading correctness for coverage here would have shown up as a
clean run either way. The guard is a coverage decision as much as a correctness
one, and neither of our harnesses can see the coverage half.

> A justification that is sound about one part of the answer will happily be
> written next to a condition that governs all of it.

Both D30 and D31 were found by the first fuzzer run that ever exercised the
operator, which is the entire argument for the positive control in D29: this
tool reported "0 disagreements" for its whole life, and the first run that could
have disagreed did, four times out of four seeds.

## D32 — The sanitizer was never missing; the input was

Summing negative values was undefined behaviour:

    frep.hpp:50: runtime error: signed integer overflow:
                 9223372036854775807 - -5 cannot be represented in type 'long int'

`CheckedCardinalityAdd` and `CheckedCardinalityMul` guard sums as well as counts.
Written for cardinalities, which cannot be negative: `max - b` overflows for
b < 0, and the multiply's `max / a` has the wrong sign for a < 0, so the
comparison it feeds means nothing.

**Why this is a different kind from the six in D29.** The core suite has run
under asan and ubsan since it was written, exactly to catch this class. The
check was present, correct, and wired up. It was simply never handed an input
that would turn it: `fuzz-modes-agree.py` produced `rng.randrange(domain)`,
which is never negative, and no SQL test summed a negative column. D29's
instances are signals that could not have gone red. This one could have, and
nothing ever asked it to.

> Running under a sanitizer is a claim about what would be caught, not about
> what was tried.

**The fix is in two halves and the second is the durable one.** Both guards now
cover the full int64 range, with -1 separated out because `INT64_MIN / -1`
overflows and so cannot be used as a divisor -- still no `__builtin_*` and no
`__int128`, so the plain-MSVC target (D4) is unaffected. Then the inputs: the
fuzzer's values straddle zero, `test_enumerate.cpp` has a negative-sum group
that runs under the sanitizers and asserts the total really is negative, and
`factorized_optimizer.test` covers it at the SQL surface both grouped and
ungrouped. The guards being wrong is the bug; nothing ever producing a negative
number is why it survived every run this project has made.

**It bears on `<>` too.** Inclusion-exclusion is a difference of two counts and
these guards protect both terms, so an overflow in one term yields a wrong
difference with both halves looking plausible -- the failure mode the second
session flagged when that feature landed and could not test for from inside it.

## D33 — The first measurement on real TPC-DS data: the rule never fires

Every coverage figure this project has quoted came from **23 empty tables** in
**force** mode. Force bypasses the gate, so that set is what the matcher accepts,
not what a user gets. Measured properly -- generated data at scale factor 1,
2,880,404 `store_sales` rows, a release build, this machine:

    auto  fires on   0 of 99
    force fires on   1 of 99   (q96)

Against the 5 of 99 that has been the headline. The number does not survive
contact with data, because real statistics make DuckDB's optimizer produce
different plans, so the matcher sees different shapes.

**Verified before it was believed.** "0 fires" is indistinguishable from "the
extension did not load", so a positive control ran first on the same binary: the
shape `test/sql` asserts under `auto` fires. The measurement could have gone the
other way.

**The gate is right to decline the one query that reaches it.**

    q96: gate says no: predicted 107ms against DuckDB's 67ms, under the 1.5x
         margin; estimated 10K tuples in 911K records, compressed 0.0x

10K tuples in 911K records is an f-representation *larger* than the flat result.
There is no fan-out in that shape to exploit, and declining it is the gate doing
its job rather than the gate being miscalibrated.

**What blocks the other 98, and why fixing the top blocker delivers nothing.**
On real data the matcher's declines are dominated by one cause:

     45  compressed materialization is in the way
     13  sum() of a computed expression
      8  aggregate has grouping sets
      8  aggregate is avg()
      1  fires

Forty-five is an upper bound, not a delivery. Disabling that pass -- which
simulates handling it perfectly -- moves the numbers to **force 1, auto 0**.
Every one of the 45 hits another blocker immediately behind it. This is the
+47-predicted/+2-delivered pattern from the coverage analyser, at feature scale:
the largest blocker by count is worth zero queries.

**And it revises D31's cost.** With that pass out of the way, 12 queries are
blocked by D31's NULL guard (`sum over ss_ext_sales_price ... may contain NULL`).
The measured cost of that guard is zero *today*, and that zero is conditional on
compressed materialization blocking those queries first. Restoring the coverage
it took would matter only in a world where the pass above it was already solved.

**The conclusion for the backlog.** None of the remaining candidates changes this
benchmark: outer/semi at the SQL surface was measured at 0 TPC-DS queries, `<>`
at 0, and compressed materialization at 0 delivered. TPC-DS does not contain the
shapes factorization exploits, and where it contains one, the fan-out is not
there. That is a fact about the benchmark, not a verdict on the technique -- but
it means no coverage work on this corpus can be justified by this corpus.

> A benchmark that cannot exercise a feature cannot justify building more of it,
> and cannot condemn it either.

Scope, stated so it is not read as more: one machine, scale factor 1, one build,
one benchmark. The honest answer to "are the queries that fire faster on real
data" is that at sf=1 there are none to time.

## D34 — The first speed measurement: 3 wins, 8 losses, and the gate cannot tell them apart

D33 found that TPC-DS never fires, so there was nothing to time. The CE corpus
(graph joins, already downloaded, 5.3 GB of source data) is the opposite case:
**force fires on 119 of 119** and the gate lets **12** through. Those 12 are the
first queries this project has ever timed against stock DuckDB on real data.

    query                          expected      off s   auto s   ratio
    epinions_acyclic_215_12       132265073       0.65     0.11     5.8x   win
    hetio_acyclic_222_16          815717690       3.43     0.53     6.5x   win
    epinions_acyclic_215_04        93418738       0.73     0.20     3.7x   win
    hetio_acyclic_205_00          337331612       0.38     0.44     0.9x
    watdiv_acyclic_210_15          12177467       0.11     1.64    0.07x
    watdiv_acyclic_215_17            531172       0.12     2.08    0.06x
    watdiv_acyclic_216_10         170447782       0.85    10.31    0.08x
    watdiv_acyclic_217_01         483756743       1.62    11.19    0.14x
    watdiv_acyclic_201_10         167849271       0.25    27.96   0.009x
    watdiv_acyclic_206_16          39891620       0.16    26.16   0.006x
    watdiv_acyclic_212_05         178516440       1.31    59.11   0.022x
    yago_acyclic_Chain_12_17              2       0.11     0.23     0.5x

Every answer matches `off`. The correctness work holds. The speed does not: the
gate exists to decline losses and it accepted eight, the worst of them **163x
slower** than doing nothing.

**Counting a 815-million-tuple join in 0.53s against DuckDB's 3.43s is the paper's
claim working.** So is 5.8x on epinions. The technique is not the problem.

**Two separable causes, both measured.** On `watdiv_acyclic_206_16`, a
seven-relation star:

    stock DuckDB          0.189 s
    ours, fallback ON    20.070 s
    ours, fallback OFF   10.352 s

The 7.5 fallback costs a clean **2x** whenever the operator runs, because the
attached child makes the operator serial (`ParallelSource` returns
`children.empty()`), and it is on by default. That is a real cost of a
correctness feature and it was never measured against anything but a synthetic
grid.

Underneath it, the engine is still **55x** slower than DuckDB on that shape. The
losers are all wide stars on one hub column; the winners are chains and mixed
shapes. Whatever the cause, it is not the gate's fault and not the fallback's.

**What the gate got wrong is narrower than it looks.** Its coefficients were
fitted on synthetic uniform data by `calibrate-synthetic.py`, and F18 already
recorded that our flat estimate is weakest there. This is the first evidence
that the fit does not transfer: it is not mildly optimistic, it is wrong by two
orders of magnitude on a shape family it never saw.

> A cost model fitted on shapes you generate can only be as good as your
> imagination of what shapes exist. Ours had never imagined watdiv.

Scope: one machine, one build, `ce_runnable.psv` (119 queries), minimum of two
runs per mode, results compared for equality on every query.

### The fix: bound the bet, because the prophecy cannot be improved here

Refitting the coefficients was the obvious move and it is wrong. The gate
predicted **228ms** for `watdiv_acyclic_212_05`, which ran for **59,110ms**, and
the reason is not the coefficients but their input:

    query              est records    ACTUAL records     error
    watdiv_212_05            589 K       121,021,261     205x under
    watdiv_206_16              3 M        69,985,962      23x under
    watdiv_201_10              3 M        37,192,978      12x under
    epinions_215_04          235 K           224,042     about right
    hetio_222_16               2 M           872,178     over, by 2.3x

No curve fit repairs a 205x error in the number being fed to the curve. It is
cardinality estimation on skewed graph stars.

So the gate now passes the size it predicted to the operator, and the
representation is held to it times `factorize_estimate_slack`. Outgrowing the
budget means the decision rested on a number that is not true, so the operator
throws, §7.5's fallback catches it, and the plan we replaced answers. The throw
is deliberately **not** `MemoryLimitExceeded`: that means "this machine cannot
hold it", which slicing answers correctly, whereas this means "the estimate was
wrong", which slicing answers by spending longer being wrong.

**The default is measured, not chosen.** Slack 1/2/4/8 against the corpus: the
wins do not move at all (`hetio_acyclic_222_16` is 0.509 / 0.534 / 0.535 / 0.490
against 3.5s for DuckDB) while the worst loser runs 8.6 / 10.2 / 13.7 / 22.2.
The estimate is either close (every win within 2.5x, one of them over) or
hopeless (12x-205x low, every loss), with nothing in between -- so a tight bound
costs the winners nothing. 2 keeps a margin for ordinary noise on a query that
would win.

**Measured effect, all 12 re-timed:**

    worst single slowdown      163x  ->   86x
    total across the 8 losses  139s  ->   62s
    the three wins              3.7x, 5.8x, 6.5x  ->  3.9x, 6.1x, 6.8x
    answers                    identical to `off` on all 12, before and after

**It is a bound, not a repair, and the residue is two things this does not
touch.** With the fallback disabled entirely the engine is still **55x** slower
than DuckDB on `watdiv_acyclic_206_16`, so no gate setting can recover that
shape. And the fallback runs serially, because `ParallelSource` returns
`children.empty()` to stop worker starvation, so the recovery path competes
against DuckDB on eight threads. A user running watdiv-shaped stars still loses,
by 86x instead of 163x.

> A cost model that cannot be trusted can still be made survivable. Bounding the
> loss needs no better estimate than the one that is already wrong.

## D35 — The star was never factorized: a scan held one record per row

D34 bounded the losses without repairing them, and said so: with the fallback
disabled entirely the engine was still **55x** slower than DuckDB on
`watdiv_acyclic_206_16`, a seven-relation star, so no gate setting could reach
that shape. This is what that was.

**The measurement that located it.** `factorized_stats` reports what the
representation actually held:

    records        69,985,962
    bytes                1.96 GB
    inputs          8,202,775 rows across 7 relations
    answer         39,891,620 tuples

A correctly factorized star holds one root per matching key plus the rows that
survive beneath it. Here that is 1,118 matching values of `s` plus 186,126
surviving rows: **187,244 records**. We were building **374x** that -- and 1.75x
more records than the flat answer has tuples. On the paper's flagship shape the
engine was doing worse than materializing the answer.

**The cause is one line, and it is not the join.** `FTree::Scan` builds the
trivial single-node f-tree and `MakeScan` fills it with one record per input
row. That is exactly section 4.2.1 and it is correct -- but this engine scans
only the columns a query's joins mention, because the binder projects the rest
away before the graph exists (`table_function.cpp` fills `bound.columns` from
the predicates alone). Projecting `watdiv1052651` onto `s` turns 4,491,142 rows
into 4,491,142 records over **39,781 distinct values**, and every later join
then re-pairs all 113 copies of a value instead of pairing the value once. The
projection that makes the scan cheap is what makes the representation
quadratic.

> Factorization removes the redundancy *between* relations. It does nothing
> about the redundancy a projection creates *inside* one, and this engine
> created that redundancy itself.

**Confirmed by prediction, not by inspection.** The nesting model says the
materialized records are the running prefix products, so it can be checked
against the engine before changing anything:

    relations   predicted                                  measured
    2           4,491,142  (the scan, unpruned)            4,491,142
    3           4,491,142 + 3,139,902 = 7,631,044          7,631,044

Both exact, which is what turned "the star looks unfactorized" into "the scan
holds one record per row".

**The fix: a record carries how many identical tuples it stands for.**
Identical scanned rows collapse into one record with a multiplicity, and
`SubtreeSize` seeds from that instead of from 1. `SubtreeSum` then needed no
change at all, because it already derives everything from `SubtreeSize` --
`others = count / slot_count` carries the multiplicity through the combination
rule without knowing it exists.

The group-by fold did need it, and for the opposite reason: it builds its own
semiring pairs rather than reading `SubtreeSize`, so a record's own contribution
had to become `(weight, value * weight)` instead of `(1, value)`. The rule that
falls out is: anything deriving from `SubtreeSize` was already correct, and
anything computing its own size beside it was not. That is what picks out the
four sites in the join -- the two counters that size a subtree from the plan
without building it, and the two materializers that build one.

The field is stored **biased by one**, so a zeroed record reads as weight 1.
The arena already hands back zeroed memory, which means every representation
built before this existed keeps its exact former meaning, no constructor writes
the field, and the ungrouped path pays neither a store nor a branch.

**Rejected: fixing the join order instead.** Ordering the star by ascending
relation size predicts 918,744 records against the measured 69,985,962 -- a
genuine 76x, for a much smaller change. It was rejected because it is a
heuristic that needs plan-time cardinalities this project has already measured
as wrong by 205x (D34), and because it treats the symptom: with grouped scans
every level is bounded by *distinct values* rather than by a product of row
counts, which makes the order nearly irrelevant instead of critical.

**Grouping is abandoned when it does not pay.** A relation whose rows are
already distinct would buy a hash table the size of its input and get nothing,
so after 65,536 rows the grouper checks whether it is earning its keep and drops
the table if the rows so far are more than 90% distinct; it also abandons rather
than grow past a budget, since the table is heap and the representation's own
memory limit cannot see it. Abandoning is safe at any point *because the
multiplicity is a property of each record and not of the relation*: what is
already grouped stays grouped, every row after it gets a record of its own, and
the two together denote the same bag. That is what lets the decision be a guess
-- being wrong costs time, never an answer.

**What it cost to get right.** Two things had to learn about multiplicity, and
both were found by tests rather than by reading:

- `MaterializeLower` builds the lower side's records on its own rather than
  through `MaterializeSubtree`, and copying the payload without the weight made
  a materialized join disagree with the fused count over the same inputs. The
  existing `fused count == materialize-then-count` differential caught it on the
  first run. The sweep that would have found it without a test is `grep
  CopyPayload`, which lists every site that builds a record: there are exactly
  two.
- `test_join.cpp` carries its own reference decoder, separate from
  `core/enumerate.hpp`, and it read a grouped relation as though it were a set.
  Teaching it the format is not teaching it the answer -- the oracle is
  `FlatJoin` over the raw input rows, which knows nothing about how any of this
  is stored.

**The abandonment rule, checked against the corpus rather than reasoned about.**
Distinct values among the first 65,536 rows of each relation in
`watdiv_acyclic_206_16`:

    relation          rows      distinct in sample     grouping
    watdiv1052651    4,491,142        0.9%             kept
    watdiv1052644    3,289,307        2.3%             kept
    watdiv1052642      152,275       13.3%             kept
    watdiv1052650       69,970      100.0%             abandoned
    watdiv1052643      100,000      100.0%             abandoned
    watdiv1052645       59,784      (under the sample) kept, gains nothing
    watdiv1052646       40,297      (under the sample) kept, gains nothing

It keeps exactly the two relations holding 7.78M of the 8.2M rows, and drops the
table on the ones where there was nothing to collapse. The two below the sample
size pay a hash pass for no gain, which is ~100K rows and not worth a second
heuristic to avoid.

### What it actually bought, measured against HEAD

Same build configuration, same harness, a discarded warm-up in each mode, only
the code differing. `auto` seconds, with stock DuckDB alongside:

    query                HEAD    mine   effect     DuckDB
    watdiv_206_16       9.110   4.192   2.17x       0.058
    watdiv_215_17       1.293   0.190   6.8x        0.019
    hetio_222_16        0.398   0.175   2.27x       3.230
    watdiv_212_05       6.707   4.337   1.55x       0.991
    watdiv_201_10       6.153   4.053   1.52x       0.178
    hetio_205_00        0.246   0.175   1.41x       0.288
    watdiv_217_01       7.684   6.406   1.20x       1.409
    watdiv_210_15       0.741   0.667   1.11x       0.018
    epinions_215_04     0.114   0.102   1.12x       0.667
    epinions_215_12     0.032   0.034   noise       0.579
    watdiv_216_10       5.150   5.262   noise       0.748
    yago_Chain_12_17    0.132   0.152   noise       0.029

Faster on 9 of 12, slower on none beyond noise, every answer identical to
`factorize_mode='off'`.

**And it does not close the gap.** On `watdiv_acyclic_206_16` the representation
is now 1035x smaller and 341x lighter, and that bought **2.17x**: 127x slower
than DuckDB before, 72x slower after.

> The representation was genuinely broken -- worse than materializing the answer
> -- and fixing it was worth doing on its own. But it was never what made this
> query slow, and the record counts said so all along: 70M records was a bug,
> and 67K records is still 4.2 seconds.

**Where the remaining 72x lives, measured rather than guessed.** The core is not
the bottleneck: the standalone harness ran this query with the *old* 70M-record
representation in 6.8s, faster than the DuckDB path manages with the 67K one.
Timing prefixes puts the cost on the scan and not on the join -- a query touching
the 4.49M-row relation costs the same whether it scans 4.5M rows or 9M -- and
`StorageSource` is why: it materializes every input column into
`std::vector<int64_t>`, row at a time, single-threaded, before the engine starts.
DuckDB scans, joins seven relations and counts in 0.058s, vectorized and
parallel. That is an architectural difference, not a tuning one.

### The measurements this project has been quoting were taken under sanitizers

`build/relassert` compiles with `-fsanitize=address -fsanitize=undefined` (456 of
521 objects, DuckDB's own code included), and the corpus scripts default to it --
`run-ce-extension.sh`, `cross-check-optimizer.sh`, `cross-check-duckdb.sh`,
`run-duckdb-ce.sh` all carry `DUCKDB="${DUCKDB:-build/relassert/duckdb}"`. For
those it is the right default: they check answers, and assertions plus
sanitizers are exactly what one wants there.

The two calibration scripts already knew better and take `build/release`,
saying why in a header comment. So this is not a blind spot everywhere -- it is
a blind spot in *timing*, where nothing enforced the distinction.

D34 records only "one build" without naming it, so what follows is inference,
not a fact I can show: its `auto` figure for `watdiv_acyclic_206_16` is 26.16s,
which sits beside the 22.5s this session measured on relassert and nowhere near
the 9.1s the same pre-change code measures on release.

Both sides are instrumented, so the comparison is not meaningless -- but ASan
taxes row-at-a-time code far more than vectorized code, which is precisely the
axis these two engines differ on. The same query measures 127x on release and
was reported as 55x on relassert. It also made the scan look like the whole
story: under ASan `factorized_count` spent 21.5s where release spends 4.2s,
while stock DuckDB read the same table in 4ms.

    build        wall (full)   binary     sanitized objects
    relassert       12m 09s    2.57 GB          456 / 521
    release          2m 54s      52 MB                  0

> A harness that defaults to the sanitizer build measures the sanitizer. The
> calibration scripts guarded against that and said so; the timing runs never
> had a default to guard, because nothing in the repo does timing.

## D36 — The scan gave its buffers back after every relation

D35 fixed a representation that was 1035x too large and bought 2.17x. The query
was still 72x slower than DuckDB, and the reason turned out to have nothing to
do with factorization.

**The measurement that found it.** Timing `source.Columns` and `MakeScan` per
relation, on the query D35 could not rescue:

    [scan] relation 0  rows 4,491,142   Columns 3.855s   MakeScan 0.040s
    [scan] relation 4  rows 3,289,307   Columns 0.011s   MakeScan 0.032s

Two tables of comparable size, **350x apart**, in the same query, warm. DuckDB
reads the same column with `sum()` in 0.010s.

**The experiment that identified it.** Listing the relations in a different
order moved the cost instead of leaving it with the table:

    651 listed first    651 = 7.183s    644 = 0.015s
    644 listed first    644 = 3.973s    651 = 3.233s

With 651 (36 MB of int64) scanned first, 644 (26 MB) afterwards is free -- it
reuses the block 651 just released. Reverse them and both pay, because 651 needs
more than 644 freed. A cost that follows allocation order rather than the data
is an allocation cost, and that ruled out the table, its compression and its
values in one run.

**The cause is one line.** `StorageSource::Load` began with

    held.assign(bound.columns.size(), {});

which destroys the column vectors and hands their pages back on *every*
relation. Each scan then re-grew from zero through `push_back`'s doubling --
about 23 reallocations for 4.5M rows -- and faulted in every page again. Fixed
by clearing (which keeps capacity) and reserving to the table's cardinality.

    watdiv_acyclic_206_16       before    after
    relation 0 scan             3.855s    0.016s     240x
    factorized_count total      3.970s    0.165s      24x

**Both fixes together, against HEAD, release build, warmed, answers identical:**

    query                HEAD    +D35     +D36    DuckDB    total
    watdiv_206_16       9.110   4.192    0.621     0.083    14.7x
    watdiv_201_10       6.153   4.053    0.651     0.170     9.5x
    watdiv_212_05       6.707   4.337    0.897     1.351     7.5x  -> a win
    watdiv_215_17       1.293   0.190    0.201     0.019     6.4x
    watdiv_216_10       5.150   5.262    1.333     0.746     3.9x
    watdiv_210_15       0.741   0.667    0.304     0.020     2.4x
    hetio_222_16        0.398   0.175    0.184     3.485     2.2x
    hetio_205_00        0.246   0.175    0.130     0.293     1.9x
    watdiv_217_01       7.684   6.406    7.173     1.455     1.07x

`watdiv_acyclic_206_16` goes from **127x slower than DuckDB to 7.5x**, and the
gate's record on this corpus from 4 wins / 8 losses to 5 / 7.

**Why this hid D35.** While every query paid seconds in the scan, no
improvement to the representation could show up: shrinking the f-representation
1035x moved the total from 9.1s to 4.2s because 3.9s of what remained was
`held.assign`. The two bugs were independent, and the larger one was invisible
in every metric the project collects -- record counts, bytes, compression ratio
-- because it is not in the representation at all.

> Two of the three numbers this project reports about a query describe the
> representation. The thing that made this query slow never touched it.

**And it is a reminder about inference.** The first diagnosis here was
"the cost does not scale with rows scanned, so it is not the scan". The premise
was correct and the conclusion was backwards: it did not scale with rows because
it scaled with *first allocation of a given size*, which is a scan cost that is
insensitive to row count. Instrumenting took one 46-second build and settled in
one run what three rounds of reasoning had got wrong.

## D37 — Stop predicting: measure the compression, and abandon when it is not there

Every number the gate consults is computed before the query runs. D34 showed
each of them wrong by orders of magnitude on exactly the queries that lose, and
the research that followed showed why no better estimate is available: pessimistic
degree-sequence bounds are valid but a median 240,064× too loose, cardinality is
the wrong objective (the plans with fewer tuples cost 5× more records), and the
f-tree's own structural exponents rank real instances at +0.11 against speedup.
Then a sweep of `factorize_min_work_ms` — the one existing floor — found that
**every** setting of it left the corpus slower than turning factorization off:
24.33s at best against a 21.66s stock baseline.

The one statistic that did correlate with winning (+0.77 within the fired set)
was measured compression, which is not knowable in advance. So this stops trying
to know it in advance.

**The mechanism.** After each materialized join, `ExecuteCount` reads two numbers
off the representation it just built — the tuples it denotes and the live records
it took — and abandons when their ratio is below a floor. Abandoning throws a
plain exception, which reaches the operator as a failed result and is answered by
the §7.5 fallback running the plan that was replaced. That is the same route D34's
estimate budget takes, and deliberately *not* `MemoryLimitExceeded`: slicing a
query that is not compressing makes several smaller copies of the same mistake.

It is close to free. `FactorizedJoin` already ends in `PruneEmptySubtrees`, which
computes every subtree size, so the tuple count is a sum over memoized roots
rather than a traversal. Measured end to end, an abandoned query costs the stock
plan plus **4–75 ms**.

**The first calibration was measured on the wrong plan.** `factorized_steps` —
added here, one row per join — builds its plan from the table function's relation
list. The optimizer builds a different join order from bound SQL, and per-step
compression is a property of that order, not of the query. Calibrated against the
table function, the floor came out at 2.5; run through the operator it abandoned
four of the five wins. The operator was then asked what *it* measures, by running
with the fallback off and reading the failing step out of the error message:

    query                min compression   verdict   at stake
    watdiv_216_10              0.230        loss      0.59 s
    yago_Chain_12_17           0.292        loss      0.13 s
    watdiv_217_01              0.403        loss      5.81 s
    watdiv_210_15              0.438        loss      0.25 s
    hetio_205_00               0.476        WIN       0.16 s
    epinions_215_04            0.742        WIN       0.58 s
    hetio_222_16               0.845        WIN       3.23 s
    epinions_215_12            1.484        WIN       0.56 s
    watdiv_215_17             48            loss      0.18 s
    watdiv_206_16             56            loss      0.54 s
    watdiv_212_05            535            WIN       0.25 s
    watdiv_201_10           1311            loss      0.44 s

Compression below 1 is normal at an early join, which is the part that is not
intuitive: records grow by a *sum* over joins while tuples grow by a *product*,
and the product has not overtaken yet. A floor set where the word "compression"
suggests it should be abandons everything.

**The floor is swept, not fitted.** The five lowest values are all losses and the
next four all wins, so a threshold in (0.438, 0.476) separates this sample
exactly — a 9% gap against 4% run-to-run variation, which is a coincidence to
measure past rather than a rule. Seven floors, timed end to end over the 12
queries the gate fires on, corpus totals by substitution (a declined query runs
the stock plan whichever floor is set):

    floor   fired s   corpus s   vs stock   worst regression
      0      11.251     24.70      0.88x    watdiv_217_01  +5.42 s
      0.3    12.015     25.46      0.85x    watdiv_217_01  +5.40 s
      0.45    6.277     19.72      1.10x    watdiv_216_10  +1.06 s
      0.6     5.232     18.68      1.16x    watdiv_206_16  +0.55 s
      0.8     5.897     19.34      1.12x    watdiv_206_16  +0.53 s
      1.0     8.756     22.20      0.98x    watdiv_206_16  +0.54 s
      2.5     9.508     22.95      0.94x    watdiv_206_16  +0.54 s

0 wrong answers across all 96 runs. The whole window [0.45, 0.8] beats stock, so
**0.6** is a plateau rather than a spike, and it is that window's geometric
centre. It abandons `watdiv_217_01` (6.820s → 1.478s against a 1.398s stock
plan), `watdiv_216_10`, `watdiv_210_15` and `yago_Chain_12_17`; it forfeits
`hetio_205_00`, a 0.16-second win; and it leaves every win worth more than that
alone.

> **`factorize_mode='auto'` is net-positive on this corpus for the first time:
> 18.68s against 21.66s stock, where firing without the check costs 24.70s.**

**What it does not fix.** Three losses survive at any floor —
`watdiv_201_10`, `watdiv_206_16`, `watdiv_215_17` — and they compress at 1311×,
56× and 48×. Factorization is working perfectly on them; there was simply nothing
to win, their stock plans taking 0.165s, 0.060s and 0.018s. That is the job of
`factorize_min_work_ms`, which cannot do it because it predicts. The two checks
are complementary and only one of them has stopped guessing.

**Inert under `force`.** Small fixtures barely compress — 200 keys times 10 rows
is 0.9 tuples per record — so a floor that applied under `force` would quietly
turn the whole SQL suite into a test of the fallback: every answer still correct,
every assertion still passing, and the factorized path never once run. `force`
means "run it whatever we think of the idea", and this is a thing we think of the
idea.

**And one bug found on the way.** The four table functions set the memory cap and
left the estimate budget as an earlier query on the same thread had it — these
limits are thread-local, and `factorized_count` could therefore abandon against a
prediction made for a different query entirely. `SetGlobalLimits` now takes all
three together, which is what makes the omission impossible to write again.

## D38 — The compression floor is overfit, and a matched pair says why

D37 swept `factorize_min_compression` end to end and shipped 0.6. The sweep was
honest and the number was still wrong, because both the calibration and the
evaluation used the same 12 queries -- the ones the gate happens to fire on in
the 119-query runnable corpus.

Run against the population it was not fitted on, it fails. The CE benchmark
*disables* every query whose result exceeds 1e9 tuples; D15 says that regime is
what this engine is actually for. Of the 481 disabled queries, 390 have their
tables loaded and **248 fire**. With the floor at 0.6:

    227  ran factorized
     17  abandoned by the floor
      4  hit the pre-existing estimate budget or memory cap

Every one of the 17 is a query DuckDB does not finish inside 180 seconds, so
each abandonment trades about a second for never. Their compressions:

    0.378  0.379  0.381  0.391  0.458  0.500 x5  0.526  0.553  0.555
    0.584  0.594  0.600  0.600

`watdiv_217_01`, the +5.4s in-sample loss the floor existed to catch, sits at
**0.403 -- inside that range**. There is no threshold that separates them.

**The matched pair.** `watdiv_216_10` loses (0.598s stock, 0.998s factorized).
`watdiv_216_01` is one of the 17: DuckDB cannot answer it at all. Their
per-join traces:

    216_10   comp 0.50 0.98 0.97 0.49 0.33 0.25   growth - 24.5 32.5 2.0 0.9 1.2
    216_01   comp 0.50 0.33 1.00 0.50 0.33 0.25   growth - 23.5 32.6 2.0 1.2 -

Indistinguishable on compression and on record growth; opposite verdicts. And
record growth fares no better as a statistic in its own right: `watdiv_217_01`
grows records 28.7x at its last join and must be abandoned, while
`hetio_205_08` grows them 28.3x and must not.

> No function of the representation's per-join statistics separates them,
> because the two queries build nearly the same representation. What differs is
> what the *stock* plan costs, and nothing on our side can see that.

The default goes back to 0. The mechanism, the setting, the tests and
`factorized_steps` stay -- a user who knows their workload can set it, and the
abandonment path is exercised either way.

**The instrument that should have existed first.** Per-join numbers were only
ever observable through `factorized_steps`, which plans from a relation *list*
rather than from bound SQL. That is a different join order, and per-join
compression is a property of the order -- which is how D37's first calibration
came out four times too high. The only other way to see the operator's own
numbers was to make it fail and read the message. `factorize_explain` now prints
them, and the matched pair above is the first thing it found.

## D39 — Eight filtering passes over the input, on one thread, by default

`watdiv_217_01` ran 6.820s against a 1.415s stock plan. D37 built a whole
run-time gate to abandon it. The actual cause was one line.

    idx_t slices = TaskScheduler::GetScheduler(context).NumberOfThreads();

`ParallelSource()` returns `children.empty()`, and the operator carries the §7.5
fallback as a child whenever `factorize_fallback` is on -- which is the default,
because a source that may have to drive the fallback's pipeline cannot be
parallel (D30). So the slice count was set from the thread count while the
operator was not allowed to use more than one thread. One thread then executed
all eight buckets in sequence, and since every bucket has to look at every row
to find its own, that is **eight full filtering passes over the input for no
parallelism whatsoever**.

Both halves are correct on their own, which is why it survived. It was found by
stack sampling under gdb, which put 21 of 30 working samples in
`SlicedSource::Columns` and two in the storage scan it wraps.

    two relations, 7.78M rows, forced
    8 buckets on 1 thread (default)   0.574s
    8 buckets, fallback off, parallel 0.121s
    1 bucket  on 1 thread             0.115s

The fix is to read the thread count only when the operator is actually a
parallel source. The 12 queries the gate fires on, re-timed:

    query                  stock    before    after   speedup   was
    epinions_215_04        0.612    0.109     0.023    26.6x    5.6x
    epinions_215_12        0.581    0.036     0.018    32.3x   16.1x
    hetio_222_16           3.298    0.184     0.135    24.4x   17.5x
    hetio_205_00           0.298    0.128     0.038     7.8x    2.3x
    watdiv_212_05          1.020    0.884     0.519     2.0x    1.2x
    watdiv_201_10          0.177    0.608     0.180     1.0x    0.29x
    watdiv_217_01          1.415    6.820     1.502     0.9x    0.21x

Corpus total 21.66s stock, 17.26s with `auto` -- **1.25x, and with the
compression floor switched off**. D37's floor bought 1.16x by abandoning the
symptom of this bug.

> The +5.4s regression that justified building a run-time gate was eight
> redundant passes over a column, not a bad join order and not a failure to
> compress.

**What this invalidates.** Every timing this project has taken of the factorized
path was taken with this tax in place, including the ones the cost model's
coefficients were fitted against -- `calibrate-synthetic.py` times under
`force`, so it went through the operator and paid it.

*Corrected.* This paragraph first claimed the coefficients therefore
over-estimate our cost, so the gate must be declining queries it should fire on.
Re-running the fit says the opposite -- `ours` comes back at
`{0.653, 3.393e-5, 1.611e-4}` against the shipped `{0.109, 2.445e-5, 3.961e-5}`,
which is *more* expensive on every term, and would make the gate fire less. The
reasoning was sound and the direction was a guess; the guess was wrong.

Two things are worth keeping from it. The synthetic grid tops out at 200,000
rows, where the tax was small and the per-record term dominates, so it is not
obviously measuring what changed. And the fit's own docstring says the data is
uniform and "a fit from here describes the friendly case" -- which is exactly
the regime where our per-record cost looks worst and the CE corpus's fan-out
wins do not appear at all. Neither the old coefficients nor the new ones have
been checked against what the engine now actually does on the corpus, and that
measurement comes before any re-fit.

### D39a — What one bucket costs, and what it was hiding

Two things were tied to the bucket count that should not have been.

**The memory bound.** `budget / slices` bounds what one attempt may hold, and it
exists because without it "the engine allocates until the kernel kills the
process, taking the whole session with it". Once `slices` became 1 that ceiling
rose eightfold as a side effect of a parallelism fix, and on the >1e9-tuple
corpus two queries stopped reporting a limit and started returning nothing at
all -- `hetio_210_07` and `hetio_216_15`, killed rather than declined. It now
divides by the *thread* count, which keeps peak memory exactly where it was: a
bucket that does not fit is sub-divided by ExecuteCountSliceWithinMemory's retry
rather than refused, so the only cost of staying conservative is that a query
which genuinely needs more discovers it sooner. Both queries answer or decline
cleanly again.

**The estimate budget, which was 8x lenient by accident.** D34's budget is an
absolute whole-query number -- predicted bytes times a slack factor -- but with
eight buckets each one only ever built about an eighth of the representation, so
it was never really tested against it. With one bucket the comparison is the one
D34 wrote: whole representation against whole-query prediction.

That is the intended behaviour, and it costs exactly one query. Across the 248
fired queries of the excluded regime, the count that hits a limit goes 4 -> 5,
the newcomer being `hetio_225_00`. Its representation genuinely exceeds twice
the predicted size, so the gate's prediction for it was wrong and D34 says
abandon; it simply was not reachable before. The remedy is not a looser slack --
it is a prediction worth holding the operator to, which is the next thing to
work on.

    excluded regime, 248 fired queries, shipped defaults
      243  answer correctly
        5  abandon on the D34 estimate budget or memory cap, then fall back to a
           stock plan that does not finish inside 300s
        0  wrong

## D40 — The gate declines 17 wins, and the reason is one estimate pointing one way

With the slicing tax gone (D39) the engine is several times faster on the
scan-dominated path, so the first thing to establish was what it now wins.
Forced against stock across all 119 runnable CE queries, both timed, 0 wrong:

    the engine is faster on            23 of 119
    the gate fires on                  12 of 119
      of those, it wins                 6
      wins it declines                 17

    corpus total, stock              22.17 s     --
    corpus total, gate as it stands  17.18 s   1.29x
    corpus total, perfect gate       10.98 s   2.02x
    corpus total, always fire       524.73 s   0.04x

The gate is doing real work -- firing on everything is 24x *slower* than stock --
but it leaves 5.4s on the table and takes only 0.8s of damage from the six it
gets wrong. All 17 missed wins are epinions, with speedups up to 92x.

**Why it declines them.** Two reasons, and they are one error:

    epinions_202_04  "DuckDB's own work is 4ms, under the 10ms floor"   stock:  481ms
    epinions_202_12  "DuckDB's own work is 2ms, under the 10ms floor"   stock:  739ms
    epinions_216_00  "predicted 19ms against DuckDB's 12ms"             actual: 1063 vs 40ms

DuckDB's predicted work is 100x to 350x low, because the estimated tuple count
is low on skewed joins -- F13/F14 again. And the error is not symmetric.
DuckDB's predicted cost is dominated by *tuples*; ours by *records*; records grow
far more slowly. So under-estimating cardinality shrinks DuckDB's side of the
comparison much harder than ours, and every such error points the same way:
against firing.

> The gate's failures are not noise around a correct model. They are a bias
> with a direction, and the direction is always "decline".

**What was tried and did not work.** Re-fitting the cost model.
`calibrate-synthetic.py` returns `ours {0.653, 3.393e-5, 1.611e-4}` against the
shipped `{0.109, 2.445e-5, 3.961e-5}` -- more expensive on every term, which
would make the gate fire *less*. Its own docstring says why not to trust it here:
the data is uniform and "a fit from here describes the friendly case", while
every one of the 17 missed wins is skew. A fit taken where the problem is absent
cannot measure the problem. (This also corrects a claim in D39.)

**What did work: stop asking the biased number for so much.** If the estimate is
systematically low, a wide margin compounds it. Sweeping both thresholds and
scoring each configuration exactly against the measured off/force times:

    work   gain  fires   total s  vs stock   wins  losses
       5    1.5     12     17.18     1.29x      6       6     <- shipped
       5    1.2     15     16.12     1.38x      9       6     <- new
       5    1.0     17     16.96     1.31x     10       7
      25    1.5     11     17.88     1.24x      5       6

`min_gain` 1.5 -> 1.2 adds three wins and no new losses, with the worst
regression unchanged at +0.32s. `min_work_ms` 10 -> 5 lets through the three the
floor was rejecting on a prediction it cannot read; 0, 1, 2 and 5 are
indistinguishable on both corpora, so 5 is the conservative member of a measured
plateau rather than the removal of a guard.

**Checked, not tuned, out of sample.** These were chosen on the 119-query
corpus, which is how D37 went wrong. So they were then run against the 481
queries of the excluded regime -- where firing more can only help, because
DuckDB does not finish at all -- and the fired count goes 248 -> 249. Strictly
more, never fewer.

**What is left.** Thresholds recover 1.06s of the 6.2s between the shipped gate
and a perfect one. The other 5.1s is the cardinality estimate, and no threshold
reaches it. That remains the open problem it has been since F13.

## D41 — The statistic D13 asked for was never built, and the gate has been running on the estimator it was meant to replace

D40 established that the gate declines 17 wins worth 5.4s, all epinions, because
DuckDB's predicted work is 100x to 350x low. Two things then ruled out the
obvious explanations.

**It is not our estimator being worse than DuckDB's.** DuckDB's own
`estimated_cardinality`, which the rule could read for free since it runs
post-optimizer, is 3x to 735x low on the same queries -- median 76x. Every
estimator built on independence assumptions fails this data the same way.

**It is not the cross-class fold, though that was worth fixing.** Grouping the
119 queries by equivalence class count:

    classes   queries   engine wins   gate fires   missed
       1         17          2             3          0
       2         11          2             0          2
       3         17          5             0          5
       4         20          8             4          5
       5         22          5             3          4
       6          9          1             1          1

The gate is perfect on single-class queries and blind on multi-class ones; all
17 missed wins have two classes or more. `EstimateCost` sizes each class with
the MCV lists and then folds classes together with `child.flat /
max(parent_distinct, child.distinct)`, commenting "Uniformity is fair here: the
skew inside each class has already been accounted for". It is not fair -- a hub
value appears in the parent thousands of times *and* carries thousands of child
tuples, so the two skews multiply exactly where an average says they cancel.
That is now weighted by the parent's own value distribution, with `GroupSize`
keeping per-value sizes for the purpose.

**And it changed nothing, which is how the real cause surfaced.** The estimates
came back byte-identical, because `CatalogStats` -- the `RelationSource` the
gate actually uses -- never populates `mcv` at all. It cannot: "the factorize
gate must not read data".

D13 named this dependency in as many words:

> Consequence for Phase 3, and it is a real dependency the plan does not name:
> **DuckDB's catalog carries approximate distinct counts but no MCV list.** The
> optimizer rule has to sample max-frequency per join column or add the
> statistic.

It was never built, and D13 also records why nobody noticed: "`ColumnStats` with
an empty `mcv` degrades to exactly the old textbook estimator, so this is a
graceful fallback rather than a hard requirement". Graceful, silent, and the old
textbook estimator is the one F14 measured at 2814x low and which "declined all
48 sampled epinions queries". The gate has been running on the estimator F13 was
written to replace, in the one place it matters, since Phase 3.

**What the statistic is worth.** Measured by letting the gate read every
relation exactly (`factorize_gate_exact_stats`, kept as the measurement it is):

                          fires  wins  losses  missed   corpus   vs stock
    catalog stats            15     9       6      14   16.12 s    1.38x
    exact stats              23    19       4       4   11.48 s    1.93x
    a perfect gate           23    23       0       0   10.98 s    2.02x

95% of the achievable gain, from a statistic that was specified and skipped.

**Sampling, which is what D13 actually asked for.** 16,384 rows per join column,
one chunk taken from each of the table's parallel scan ranges -- spread rather
than a prefix, because a prefix samples load order. Two things had to be right:

*A sample is not a hub list.* Scaled straight up, a value seen once in a 1.5%
sample is reported as 68 occurrences, and the estimator multiplies these across
every relation in a class, so the error compounds: the first sampled build fired
on 49 of 119 against an oracle of 23. Requiring 30 sightings before a value
counts -- where a count's relative standard error falls under 20% -- took it to
36. Below that a value belongs to the tail, which is what the tail is for.

*A sample and a catalog must add up.* The head came from the sample, the row and
distinct counts from the catalog, and nothing made them agree. When a scaled
head swallowed every row, `TailRows()` hit zero and `Frequency` then returns
**zero** for every unstored value -- the class product collapses and the gate
declines reporting "estimated 0 tuples in 28M records". A representation of 28M
records denotes no tuples in no world; that is two sources of truth disagreeing.
The head is now clamped to leave one row for each value it did not name.

**Ask both, and fire if either says yes.** Sampling alone was worth 15.94s ->
12.41s in sample and cost 7 fires out of 248 in the excluded regime, where a
decline means 180 seconds instead of one. That trade is bad however it is
weighed. But the direction of the estimator's error is known -- under-predicting
argues against firing (D40) -- so of two under-estimates the one that declines is
the one more likely to be wrong. Consulting the catalog as a second opinion can
only move the gate toward firing, which is where the measured mistakes are.

    corpus, 119 queries          end to end     vs stock    excluded regime
    factorize off                  22.17 s         --             --
    auto, start of the day         24.70 s       0.88x            --
    auto, after D39/D40            15.94 s       1.39x        248 of 481 fire
    auto, with the sample          12.95 s       1.71x        252 of 481 fire
    a perfect gate                 10.98 s       2.02x            --

Measured end to end, so the 12.95s includes what the gate spends sampling --
about 0.5s across 119 queries, against 3.0s bought.

**And the margin goes back to 1.5.** D40 narrowed it to 1.2 to buy three wins
back from a biased estimator. That was compensation for a broken input, and once
the input was fixed it became a cost: 12.95s at 1.5 against 13.07s at 1.2. A
threshold tuned to absorb an error in an input is a thing to undo when the input
is fixed, not to keep.

> Every calibration in D37, D38 and D40 was fitting thresholds to compensate for
> a statistic that was specified in D13, measured to be worth 2814x, and then
> not wired up.

## D42 — The size pre-check was banning queries on a number that is 1,390x wrong

With the statistic fixed (D41), the census of what the gate still declines is
lopsided. Across the 481 queries of the excluded regime -- the >1e9-tuple corpus
D15 says the engine is for -- **134 of 138 declines are the memory pre-check**,
and only 4 are anything else.

That check refuses a query whose predicted f-representation exceeds the memory
budget. Measured against what those queries actually build:

    query               predicted   actual bytes   over by   forced   DuckDB
    hetio_203_16            24 GB        17.3 MB    1,390x     0.8s   no answer
    hetio_203_19            16 GB        14.7 MB    1,090x     0.3s   no answer
    hetio_204_02            25 GB         1.03 GB      24x     5.4s   no answer

Identical with the MCV sample on and off, so this is not something D41
introduced. It is the record recurrence: records are accumulated as
`contexts * share` down the join tree, so the error compounds multiplicatively
with depth. The same model's *tuple* estimate runs 100x low (D41) while its
*record* estimate runs 1,000x high -- and they are not independent mistakes,
they are one recurrence read in two directions.

**Nothing about memory safety runs through this check.** Exceeding the real
budget is handled twice at run time and has been since D20 and D34:
`ExecuteCountSliceWithinMemory` partitions the join key and re-counts rather
than failing, and the estimate budget abandons to the stock plan when the
representation outgrows what the gate bet on. The pre-check exists only because
slicing costs a pass over the input per slice -- a statement about *time*,
resting on a number wrong by three orders of magnitude.

**The check is also redundant with the margin, which is how this was safe to
relax.** Of the 9 queries it declines on the runnable corpus, every one is a
catastrophic loss -- 3.55s of stock plans against 381.5s forced, one of them
0.550s against 256s -- so the check is doing real work there and must not stop.
Raised to 64x, all 9 still decline: eight of them on the margin, reporting
"predicted 104238ms against DuckDB's 200ms", and one still on size at 6TB. The
size check and the margin read the same record estimate, so when it is huge both
fire; the size check merely gets there first with a worse message.

Out of sample the margin does *not* decline them, because there DuckDB's
predicted cost is huge too and the ratio clears. So the two corpora want
different answers from the same check, and only the pre-check was giving them
the same one.

    memory_slack   excluded regime   runnable corpus
       1 (was)      252 of 481 fire   unchanged, verified per query
       8            295
      64 (now)      332
    1024            354

The runnable corpus total does not move at any of these -- and it cannot resolve
a difference this small anyway: measured four settings in order and then in
reverse, the totals fell monotonically with *position* both times, 15.02s down
to 12.85s forwards and 12.99s down to 12.11s backwards. That is cache warming
across whole-corpus passes, not the setting, and it is why the per-query check
above is the evidence and the corpus total is not.

64 is chosen against the measured over-prediction rather than as a round number:
at roughly 1,000x over, a 64x slack still corresponds to about 0.06x of the real
budget. It admits queries predicted up to 832GB and leaves the one predicted at
6TB declined.

**What is not known.** The benefit rests on a 12-query sample of the 80 newly
admitted: 10 answered, in 0.3s to 219s, on queries DuckDB does not finish inside
180s; 2 hit a 300s timeout. Running all 80 to completion is hours, and has not
been done.

### D42a — 64 was wrong, 8 is the number, and the machine paid to find out

D42 shipped `memory_slack = 64` on a 12-query sample of the 80 it newly admits.
Running all 80 is what settled it, and the run did not finish: at query 25,
`hetio_acyclic_216_04` drove the WSL VM to 31.1GB of the 31GB it was allowed,
after which `free`, `ps` and `uptime` stopped returning and `wsl --shutdown`
took several attempts and a reboot to land. 24 of 80 completed first: 18
correct, 6 spending the full 600s cap.

Re-run afterwards on a VM capped at 15GB, where nothing can take the host down:

    hetio_acyclic_216_04, 150s cap
      off             no answer, memory untouched  (misread -- see below)
      auto slack=64   no answer, peak 13.7GB, abandons on the memory limit and
                      falls back

*Corrected in D43.* This paragraph concluded that the factorized path consumed
the memory and the stock plan did not, so declining saved the machine. The
`off` reading behind it was taken after the process had exited -- `free`
sampled a machine that had already been given the memory back. Measured while
running, at a 4GB limit, `off` peaks at 4077MB and the fallback at 4085MB. Both
fill DuckDB's limit; firing adds the time spent slicing first, and nothing
else.

At 8 that query declines on its 433GB prediction, and the excluded regime still
gains 45 of the 88 fires 64 was reaching for. Re-measured under the 6.3GiB
budget the smaller VM implies:

    slack   12.5GiB budget   6.3GiB budget
        1      252 of 481       235 of 481
        8      295              280
       64      332              323
     1024      354              351

Halving the budget moves each point by 10-17 queries and changes nothing about
the shape, so D42's finding survives; only its chosen constant does not.

**Two things this cost, worth keeping.**

*Re-running a known machine-killer to diagnose it.* After the first crash the
evidence needed was already in the six "fell back to the stock plan" rows. Going
back to the same query on the same machine -- with a `memory_limit` believed
sufficient, which it was not -- took the host down a second time. A shrunken
copy of the tables answers the same question and cannot.

*A memory cap that silently did not apply.* `.wslconfig` was first written with
`memory=16.25GB`. WSL rejects a decimal there: it prints "Invalid memory string",
discards the entry, and boots at the 31GB default. A cap that looks set and is
not is worse than no cap, because it is trusted. `16640MB` is the same number
and parses.

**Open, and not explained.** That 13.7GB peak should not be reachable. The
operator caps each slice at `budget / threads`, which on this machine is 0.79GiB,
and the arena checks it on every segment allocation. Something outside the arena
is holding an order of magnitude more than the cap allows -- the scanned base
columns are outside it by design (which is why the budget is half of DuckDB's
limit), but eight hetio relations are a few hundred MB, not thirteen GB. Nothing
here depends on the answer; the number is simply not accounted for.

## D43 — The per-slice memory limit capped every piece and never the whole

After D42a the open question was a 13.7GB peak that the engine's own limits
should not have allowed. Each slice was capped at `budget / threads` -- 0.79GiB
on the capped VM -- and checked on every allocation. Measured instead of
reasoned about, because three successive hypotheses about it were wrong.

**The shape of the problem.** Forced through the factorized path with the
fallback off, `hetio_acyclic_216_04` peaked at DuckDB's whole `memory_limit`
at every setting and every thread count:

    memory_limit   per-slice cap   peak above idle
        1 GB           64 MB           1280 MB
        2 GB          128 MB           2126 MB
        4 GB          256 MB           4086 MB
      threads=1      2048 MB           3956 MB
      threads=8       256 MB           4025 MB

So the peak followed the limit and ignored the cap. Its inputs are 792,481
rows, 12MB -- the data is not where the memory is.

**Three things it was not.**

*The hash table's index vectors.* `entries` and `directory` are heap vectors
the arena's limit never saw, and `BytesAllocated` reported the directory
without anything enforcing it -- 17 to 26 bytes per entry against a 32-byte
`Entry`. That was a real hole and is now closed: the table enforces its limit
over everything it holds. It moved the peak from 1280/2126/4086MB to
1123/2092/3995MB. Correct, and not the cause.

*Allocator retention.* The retry loop builds to the budget, throws, frees and
rebuilds, which is the classic pattern for glibc keeping freed memory through
its dynamic mmap threshold. Pinning `MALLOC_MMAP_THRESHOLD_`,
`MALLOC_TRIM_THRESHOLD_` and `MALLOC_ARENA_MAX` changed nothing: 2231 against
2235MB, 4142 against 4134MB. And the result means what it appears to, because
DuckDB's bundled jemalloc is prefixed (`duckdb_je_malloc`) -- global `new` does
reach glibc, so those were the right knobs.

*DuckDB's buffer pool.* The arena allocates with plain `new`, outside it.

**What it was.** The limit was applied to each structure separately and never to
their sum. One materializing join holds four or five capped structures at once
-- both inputs, the output, the snapshot arena and the hash table -- and the
fused count join three. Each was allowed the whole per-slice budget, so the
slice as a whole was allowed several times it.

**The fix** is one account per slice. `SliceBudget` is thread-local, because a
slice runs start to finish on one thread; every `Arena::Grow` charges it before
the memory exists and after the allocation succeeds, every arena releases to the
budget it charged, and moves carry the charge rather than duplicating it. The
hash table's index vectors are charged to the same account. `SetGlobalLimits`
sets its limit, so the one number now bounds the sum.

**Verified against the process, not just the account.** A high-water mark on the
budget, printed per slice under `factorize_explain`, set beside the process's
own peak RSS on `hetio_acyclic_204_02`, which slices and still finishes:

    one slice (fallback on)       engine held   process RSS   RSS/held
      2 GB, cap 128 MB              110.8 MB        172 MB       1.6x
      4 GB, cap 256 MB              209.8 MB        289 MB       1.4x

    eight slices (fallback off)   max held   sum held   RSS    RSS/sum
      2 GB, cap 128 MB             117.8 MB   922 MB    712 MB   0.8x
      4 GB, cap 256 MB             209.8 MB  1574 MB   1488 MB   0.9x

Every slice stays under its cap, the answer is correct, and the query takes
5.7-6.4s against 5.4s unconstrained. The process holds a fixed ~60-80MB beyond
what the engine accounts for, and with eight slices slightly *less* than the sum
of their peaks, because the peaks do not all coincide. There is no unaccounted
memory in the engine.

**A cost the sum makes visible.** Every arena reserves a 64KB first chunk however
little it stores, and several are alive at once, so a slice has a fixed cost of
a few hundred KB. Per-structure caps never saw it; a sum does.
`TestOutOfMemoryFallsBackToSlices` hard-coded 220KB and started failing -- not
because slicing broke, but because 220KB had fallen below the fixed cost, so
every slice failed along with the whole. It now sets its limit to half of what
the undivided run was measured to hold, which states the intent instead of a
number that stopped meaning it. Irrelevant at real budgets; worth knowing at
tiny ones.

**The crash scenario was never this engine.** Re-run in the configuration that
took the host down -- forced to fire, fallback on, default limit -- our path now
abandons exactly as designed, with "the engine exceeded its per-slice memory
budget", capped near 0.79GiB. The process still peaked at 13.05GB against 13.8GB
before. The rest is the section 7.5 fallback, and the fallback is simply
DuckDB's plan. At a 4GB limit, run alone:

    hetio_acyclic_216_04, 4GB memory_limit, 120s cap
      off                         peak 4077MB, no answer
      fallback (debug_fail)       peak 4085MB, no answer

DuckDB fills its own limit on this query whichever way it is reached.

So what took the host down was DuckDB's default `memory_limit` -- 80% of what
the VM sees, 25GiB of an uncapped 31GB VM -- on a query that wants more than any
limit. A user running it with factorization switched off would have lost the
machine the same way. The engine's over-commit was real and is fixed above, but
it was not the cause; the `.wslconfig` cap is what actually protects the host.

This corrects D42a, whose premise was an `off` run that "used no memory". That
reading came from `free`, sampled after the timed-out process had already exited
and returned its memory. It also reopens D42a's decision: the case for slack 8
over 64 was partly that firing cost the machine, and it does not. What remains
is time spent slicing on queries nothing can answer, against answers on the
ones 64 would add -- 18 of the 24 completed in D42a's run were correct, on a
corpus DuckDB answers none of inside 180s. That trade has not been re-measured.

That run also cost the VM a second time: it was launched beside the sanitizer
build, reasoning that a memory measurement could share the machine with a
CPU-heavy job. The ASan link is memory-heavy too, and the crash test was known
to reach 13GB of a 15GB VM. The build died mid-link and WSL needed a forced
shutdown. Memory-heavy experiments run alone.

### D43a — What the per-slice budget costs, and dividing it by slices instead

With the sum enforced, the fired set was timed A/B inside one binary: each query
run `off`, `auto`, and `auto` at a 100GB limit -- where a slice is allowed
6.25GiB, which no runnable query approaches, so the budget is inert -- all in
one session, with a discarded warm-up per mode so that cache warming across
passes cannot pose as a result.

    35 fired queries, 0 wrong answers
      off                  33.33s
      auto                 12.37s    2.70x
      auto, budget inert   11.12s

Honest accounting costs 1.11x on the fired set, and it lands almost entirely on
queries the gate should not be firing on: `watdiv_216_10` (+1.47s) runs 1.04s
stock and 2.87s even with the budget inert, and `epinions_217_12` (+0.13s) 0.15s
stock against 0.20s.

The per-slice budget divides by the thread count even when there is one slice,
which was D39a's correction for per-structure caps -- a reason D43 removed.
Dividing by slices instead, forced to fire at the default limit with the
fallback on, run alone:

    query                  peak     elapsed    outcome
    hetio_acyclic_210_07   6.4 GB   300s cap   no answer (answered in 269s with a share)
    hetio_acyclic_216_15   6.4 GB   300s cap   no answer
    hetio_acyclic_216_04   6.4 GB   300s cap   no answer (neither engine can)

Nothing was killed, and every peak sits at the 6.3GiB budget -- which is D43
doing its job, and means D39a's reason for the division no longer holds. But the
larger budget was not simply better: a bucket allowed more spends longer
building before it discovers it does not fit, and 210_07 went from an answer to
none. Against the 1.11x above, that is one query, one run, near a timeout --
not enough to choose on after a day of memory decisions reversed on thin
evidence. The division stays at threads, which is what shipped and was
validated, and the trade is open.

The first report of that run called the two no-answers "KILLED": the script
tested for empty output before it tested the exit code, and a process that
`timeout` stops at 300s has not printed anything yet. Exit 124 is a timeout; a
kill is 137.

## D44 — The size check charged a join the engine never builds

D43 reopened `memory_slack`, 8 against 64. Re-measuring it against the
estimate it compensates for would have spent four hours answering a question
about a known-wrong number, so the estimate went first. D42 established the
record estimate was ~1,000x high and attributed it to the recurrence as a
whole; nobody had set it beside the operator step by step. With the gate now
printing its predicted records per step under `factorize_explain`, in the shape
of the operator's own `StepStats`, the seven excluded-regime queries that answer
fastest were run both ways.

**Most of the error was one step, and not the recurrence.** A plain `count(*)`
fuses its last join into the count (`FactorizedCountJoin`); that join's output
is counted as it is produced and never held. The estimator charged it anyway,
and being the deepest step -- the one multiplied most -- it was the largest
term in every prediction:

    query          last join's share   predicted bytes        engine's own
                   of predicted        before      after      peak held
    hetio_203_16         96%           24.1 GB  (133x)  1.06 GB  (5.9x)    172 MB
    hetio_203_19         78%           16.2 GB  (144x)  3.49 GB (31.1x)    107 MB
    hetio_204_02         87%           25.0 GB   (17x)  3.28 GB  (2.2x)   1442 MB
    hetio_204_03         79%           29.2 GB   (22x)  6.15 GB  (4.5x)   1292 MB
    hetio_208_15         78%           17.2 GB  (160x)  3.72 GB (34.5x)    103 MB
    hetio_216_01         84%          181.5 GB  (848x) 29.63 GB (138x)     204 MB
    hetio_216_02         96%           33.7 GB   (38x)  1.32 GB  (1.5x)    845 MB

"Peak held" is the slice budget's high-water mark (D43), which is what the size
check is actually a claim about; D42's 1,390x was measured against the final
representation's bytes, a smaller and less relevant number.

The fix charges only what is built: `CostThresholds::last_join_fused`, set by
the gate exactly when the operator's `IsPlainCount` holds, drops the last
step's records from the byte estimate. Sums and grouped queries go through the
fold, which materializes every join, and are charged in full as before. Only
the byte estimate changes -- `factorized_records` still feeds the time model,
whose per-record coefficient was fitted against it, so no gate decision moves
except through the size check.

**What is left is pruning, and it runs the other way.** A later join removes
parent records whose value finds no partner, so the measured representation
often *shrinks* at a step -- `hetio_203_19` goes from 422K records to 273K --
while the recurrence only ever adds. That is the residual 1.5x-138x, median
about 6x. Modelling it means per-step survival, which the recurrence has no
term for; not attempted here.

**Attribution, measured rather than assumed.** Each variant swept over both
corpora with EXPLAIN (planning only):

    variant                     runnable fires   excluded fires (slack 8)
    before                            35              280 of 481
    byte fix only (shipped)           35              312
    byte fix + FlatFor fix            43              318

The byte fix leaves the runnable corpus's fired set identical and admits 32
excluded-regime queries -- 24 of them among the 43 that raising the slack to 64
was reaching for. Correcting the estimate recovers most of what inflating the
tolerance around it did.

### D44a — A real estimator bug, fixed, measured, and not taken

The step comparison also exposed a bug in D41's cross-class fold.
`EstimateGroup` returns early for a one-relation class without filling
`flat_by_value` or `tail_flat_per_value`, so `GroupSize::FlatFor` returns 0 for
every value -- and a one-relation class hanging beneath a two-column relation
is the common shape on graph data. The parent's MCV head then contributes
nothing to the seam. Against a closed form (hubs of 400 and 300 on the same
five values over a tail of ones), the estimate was 7,485 tuples for an exact
605,995, 81x low; with the per-value sizes filled in, 607,488. A two-relation
child class was already exact.

Fixed, it admits 8 more runnable queries, and every one of them loses:

    query                      off       auto      
    epinions_acyclic_211_00   0.023s    0.054s   0.43x
    epinions_acyclic_211_08   0.020s    0.057s   0.35x
    epinions_acyclic_211_16   0.017s    0.056s   0.30x
    epinions_acyclic_218_08   0.024s    0.062s   0.39x
    epinions_acyclic_218_16   0.036s    0.066s   0.55x
    watdiv_acyclic_213_09     0.018s    0.134s   0.13x
    yago_acyclic_Chain_9_24   0.008s    0.030s   0.27x
    yago_acyclic_Chain_9_48   0.022s    0.038s   0.58x

All correct, all tiny, together +0.33s. On the excluded regime it adds 6 fires.
The corrected number is right and the decision it drives is worse, because the
runnable corpus's tuple estimate already runs high -- median 15x over, p90 over
2,000x -- and raising DuckDB's predicted cost further lifts small queries over
the work floor. Shipping it would mean either taking those losses or tuning a
threshold to cancel them, and this project has twice paid for compensating a
broken input with a threshold (D41's margin, D42's slack). So it is recorded
here and left out: the fix is a few lines in the size-1 branch of
`EstimateGroup` (copy `mcv` into `flat_by_value`, sorted by value; set
`tail_flat_per_value` to `TailRows() / TailDistinct()`), and it belongs with
whatever corrects the over-prediction on the runnable corpus, not before it.

### D44b — What the fix admits, and the slack re-decided

**What the 32 it admits did.** Each run twice, alone on the VM, factorized
(`auto`) then stock (`off`), 300s cap, peak RSS from the process:

    outcome                                  queries
    answered only when factorized                 9   0.9s-166s; stock times out on all nine
    answered by both, stock faster                4   all watdiv: 344.9s here, 138.5s stock
    answered by neither                          19
    answered by stock and not here                0

Every answer is correct. Of the 19 no-answers, 16 exceeded the per-slice
budget and abandoned to the section 7.5 fallback, whose stock plan then timed
out as the stock run did; 3 (`hetio_216_07`, `218_11`, `218_19`) ran
factorized to the cap. Every peak above 1.5GB was the stock plan after a
fallback, and the stock run of the same query matched it: 13,050MB against
12,922MB on `watdiv_210_16`, 11,160 against 11,130 on `hetio_218_15`.

The four watdiv queries are the cost, and they correct a premise. The excluded
regime is defined by result size -- CE disables anything over 1e9 tuples --
not by DuckDB failing, and on these four DuckDB answers in 13-89s:

    query                   here      stock
    watdiv_acyclic_217_05   63.0s     13.3s   abandoned to the fallback
    watdiv_acyclic_217_10   23.5s     12.9s   abandoned to the fallback
    watdiv_acyclic_217_15  119.8s     89.3s   abandoned to the fallback
    watdiv_acyclic_218_15  138.6s     23.0s   finished factorized

Before this change they declined on size, which was the right answer for the
wrong reason. What admits them now is the watdiv error the gate has always
had -- DuckDB runs 3-24x faster there than predicted (cost.hpp header) -- no
longer hidden behind an inflated size. Nine answers nobody else produces
against 206s on four queries that are still answered is the trade taken. The
watdiv error belongs to the tuple estimate and is recorded here, not
compensated with a threshold.

**Slack, re-decided.** Fires on the excluded regime at the 6.3GiB budget:

    slack   before   after
       1      235      262
       8      280      312
      64      323      345

8 stays, now for a reason that holds: it covers the residual over-prediction --
1.5x-138x, median about 6x, never under on the queries measured -- rather than
a 17x-848x one. 64 still adds 33, `hetio_acyclic_216_04` among them, which
neither engine answers.

Validated: 682 core checks and 739 SQL assertions, 0 failures, no sanitizer
reports; CE corpus forced, 119 taken over, 0 wrong; auto with shipped
defaults, 35 taken over -- the identical set -- 0 wrong.

## D45 — A second correct estimator fix, measured and not taken

After D44 the open item was the tuple estimate. Split by shape, the runnable
corpus's worst errors were single-class yago stars: 575x *low* at the median
and 700,000x low at p10 -- `yago_acyclic_Star_6_22` predicted at 0.017 tuples
for a join of 5,519. Rerun with `factorize_gate_exact_stats` the same query
came out 13-72x high rather than 300,000x low, so the collapse was in the
statistics path, not the head of the model; and a single class never reaches
the cross-class fold, so it had to be `EstimateGroup`'s tail.

**The bug.** The tail combines relations pairwise with the textbook rule,
`|R join S| = |R||S| / max(V_R, V_S)`, but carried the running *maximum*
domain forward, where the rule leaves `min(V_R, V_S)` values on the key. Every
narrow relation after a wide one was divided by the wide domain again, so a
class's size depended on the order its relations were listed in. On
Star_6_22 -- two 118K-value columns, then four 1.8K-value ones -- that is four
extra factors of about 67. It had been there since the first commit
(`f416f0a`) and no test could see it: the foreign-key test's one narrow
relation comes last, where max and min agree, and the randomised test gives
every column the same domain.

**Fixed, it is right.** A test with the same six relations in two orders went
from 1,000,000x low (wide first) against exact (narrow first) to exact in both.
On the corpus:

    log10(predicted / exact)            before         after
    yago Star, median                   -2.76  (575x)   0.62  (4x high)
    runnable single-class, p10          -5.51           -0.20
    excluded single-class, median       -1.97  (93x)   -1.48  (30x)

**And it makes the decisions worse.** The excluded regime's fired set does not
move (312). The runnable corpus gains one fire, `watdiv_acyclic_202_06`, and it
is the worst loss this gate has admitted in months: 0.044s stock, 5.837s fired,
133x. Its prediction went from 3.4e7 tuples (3x over the exact 1.13e7, declined
on the margin) to 1.9e8 (17x over, fired). The rule the fix restores is
containment -- the narrow relation's values all appear in the wider one -- and
on watdiv they mostly do not (F18 measured 18% on `watdiv_212_15`), so the
correct textbook rule over-predicts there, and the max had been cancelling it.
Our side is off on this query too: 12.7M records across its steps in 4.1s on
one thread, about 8x the fitted per-record cost.

This is D44a again. Two correct fixes to the tuple estimate, each of which
raises it, meet a runnable corpus where it already runs 10x high at the median
and a watdiv regime where both engines' costs are mispredicted, and each is
paid for there. Not taken: the fix is one token (`std::max` to `std::min` on
the tail's running domain) plus `TestClassSizeIgnoresOrder`, both kept out of
the tree with D44a's.

**What this says about the order of work.** The estimator cannot be repaired
from the bottom up one bug at a time: its errors currently cancel, and removing
one that under-predicts exposes the ones that over-predict. The over-prediction
on the runnable corpus -- containment on watdiv, and `Frequency` treating every
head value as present in every relation -- has to be corrected first or
together with these, or every correct fix will cost the runnable corpus.

## D46 — Three sampler errors, fixed together, and the gate is slower for it

D45 said the runnable corpus's over-prediction had to come first. Split by
where the statistics came from, most of it is not the model. Swept with
`factorize_gate_exact_stats` against the shipped sample:

    median log10(predicted/exact)   sampled   exact
    2 classes                         1.74     0.00
    3 classes                         1.72     0.07
    4+ classes                        1.01    -0.08
    watdiv                            0.81     0.13
    epinions                          1.34    -0.42

With exact statistics the gate fires on 20 runnable queries, a strict subset of
the sample's 35, and the 15 only the sample fires on are a net loss: 6.98s stock
against 8.10s fired, nine of them watdiv at 0.05x-0.38x.

**Three errors, all in how the statistics are gathered.**

*Clustered storage.* The sample takes one 2048-row chunk per scan range and
scales each value's count by rows / sampled, which assumes rows were drawn
independently of their value. `watdiv1052644` holds 3.29M rows over 77,757
values of `s`, and `s` changes value 77,756 times in storage order -- one run
per value. A chunk of such a table holds whole groups, and scaling them by ~200
invents hubs. epinions and watdiv are clustered on `s`; hetio is in load order
on both keys, which is why the sample worked there. Reproduced on synthetic
tables clustered in groups of 32-64: two relations predicted at 3.1e8 for an
exact 6.4e6, four at 6.0e15 for 5.2e10.

*A prefix, not a sample.* The loop stops when the scan ranges run out, so a
table of one or two row groups was sampled by its first 2048 rows. An epinions
relation of 10K rows clustered on `s` reported its first 1,300 groups, with the
real hubs elsewhere -- and the 5x scale-up happened to land its estimate near
the truth on `epinions_216_08`, a 26x win.

*The catalog's distinct count.* Rows and distinct counts always come from
DuckDB's catalog, whose sketch reports 2,169 values for a clustered column of
6,250. D41's second opinion -- fire if the catalog-only estimate says yes --
then overruled an exact 3.2M-tuple estimate with that 3x-low count.

**The fix, as built.** `SampleHoldsWholeGroups` counts runs in storage order,
restarted at each chunk, and when the sample holds whole groups their counts are
kept unscaled. A table no bigger than eight samples is read whole and its exact
statistics used. The second opinion is asked only when some relation was
actually sampled, and is printed under `factorize_explain` -- it had been
deciding queries invisibly.

It needed D44a with it. Read whole, a relation's MCV list covers every row, the
cross-class fold puts all its weight on the child's per-value sizes, and D44a's
`FlatFor` returns 0 for a one-relation class: three hetio queries were predicted
at 0 tuples and declined. They are real wins on the shipped binary -- 0.1s
against 35.4s, 0.8s against 9.3s, and 92.6s against no answer in 300s -- and
with D44a in they fire again, predicted close to exact.

**Together they are right, and on the runnable corpus they are slower.**
Every query either binary fires on, three ways in one session per binary:

    46 queries    stock 16.48s   shipped 6.89s   candidate 8.41s   0 wrong

Gained: the five watdiv losers stop firing (-0.77s). Lost: tiny queries that
accurate estimates now admit -- `yago_acyclic_Chain_12_25` 0.015s stock against
1.065s, `Chain_9_40` 0.007s against 0.270s, six epinions queries +0.05-0.11s
each -- and 6-10ms of planning on every fired epinions query, from reading its
relations whole and sorting them for statistics. The excluded regime goes from
312 fires to 321, none lost; the SQL suite passes 620 of 620.

So the inputs are better and the decisions are worse, for the reason D44a and
D45 already found one layer up: the decision layer -- D41's margin, the 5ms
work floor, the fitted coefficients -- was tuned on inflated inputs, and on a
tiny query our fixed per-query cost is under-charged. Not taken; the combined
diff (429 lines, both tests included) is kept out of the tree with D44a's and
D45's. The next step is refitting the cost model on these inputs and measuring
what whole reads cost at plan time, then retrying this with them.

One measurement correction on the way: `.timer on` prints a Run Time for `SET`
statements too, so the first three-way run read a `SET` as the shipped timing
(0.000s everywhere) and was discarded; the harness that timed D44a's losers had
the same off-by-one in its auto column, which biases those times low and
reverses none of them.

## D47 — Counting fires in the excluded regime does not measure benefit

D42 wrote that "in the >1e9-tuple regime DuckDB does not finish, so firing more
is strictly better there", and every decision since has used the excluded
regime's fire count as the benefit side of its trade: D42's slack curve
(252/295/332/354), D42a's re-measurement at a smaller budget, D44b's case for
keeping `memory_slack` at 8 (262/312/345), and D46's "+9 fires, none lost".

Measured, it is false. D46's nine new fires, each run both ways alone on the
VM, 300s cap:

    query                      fired      stock       outcome
    hetio_acyclic_210_09        142.8s     no answer   win
    hetio_acyclic_211_14         80.5s     no answer   win
    watdiv_acyclic_210_06         0.5s      12.6s      win, 25x
    hetio_acyclic_204_01         45.0s      35.7s      +9.3s, fell back
    hetio_acyclic_204_08         39.3s      29.6s      +9.7s, fell back
    yago_acyclic_Chain_12_63      0.6s       0.02s     +0.6s
    yago_acyclic_Chain_12_06     23.7s       0.02s     +23.7s
    yago_acyclic_Chain_9_71   no answer      3.9s      +296s
    watdiv_acyclic_205_19     no answer      5.1s      +295s

Three wins, six losses, two of them a 300-second cap against a 4-5 second
stock answer. The regime is defined by *result size* -- CE disables anything
over 1e9 tuples -- and a query can have 1e10 tuples and still be answered in
seconds, because a count(*) carries no payload and DuckDB's hash joins are
fast. "DuckDB does not finish" was measured on a sample of hetio queries and
generalised to 481 queries of four datasets, three of which it does not
describe.

**What this invalidates.** Any decision whose benefit was a fire count on this
corpus: the slack curves in D42, D42a and D44b, and D46's nine fires. It does
not touch decisions measured by running the queries: D44's byte fix was judged
on its 32 admissions run both ways (9 answered that stock does not, 0 lost, 4
answered by both but slower here), and that measurement stands.

**What it costs to fix.** The excluded corpus needs a stock baseline -- every
query timed under `factorize_mode='off'` at a fixed cap -- so "fires" can be
replaced by "answers stock does not produce, minus time lost on the ones it
does". Until that exists, this corpus can say a setting is *safe* (nothing
declined that used to fire) but not that it is *better*.

**D46 is rejected.** It made the runnable corpus 1.5s slower and the excluded
regime three wins and six losses. Recorded above; the diff stays out of the
tree.
