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
