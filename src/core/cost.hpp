//===----------------------------------------------------------------------===//
//                         factorize
//
// core/cost.hpp
//
// The gate: decide *before running* whether factorizing a query will pay.
//
// Measurement makes this the load-bearing component rather than a safety
// wrapper. Fired unconditionally on the CE acyclic corpus the engine is 0.74x
// stock DuckDB -- a loss. Gated it is a win, because the benefit is not spread
// across queries but concentrated in a minority of them, and which minority is
// predictable. Measured across 741 queries with both timings, grouped by how
// far the f-representation compacts the result (FINDINGS F16):
//
//     compression   n    vs our flat path   median
//     >= 1x        185          0.85x        0.61x
//     >= 2x        128          1.15x        0.79x
//     >= 10x        64          2.12x        1.33x
//     >= 50x        27          7.84x       15.11x
//     >= 100x       19         14.66x       33.47x
//
// The threshold that reading implies does not work, and F18 says why. Speedup
// is compression x K, where K is the per-record speed advantage a compression
// threshold implicitly assumes is 1. Measured, K spans 84x:
//
//     dataset    compression   speedup       K
//     hetio          229.0x     30.6x     0.13
//     watdiv           0.7x      0.3x     0.44
//     yago             0.0x      0.2x     3.98
//     epinions        24.1x    264.9x    10.97
//
// So no threshold on compression can be right for all of them, which is why
// sweeping it left the measured result pinned near 1.5x wherever it was set,
// and why the gate declined 30 epinions queries that would each have won by
// 37x or more.
//
// The gate therefore estimates *time* for both engines and compares them.
// Measured against firing on the compression ratio (n=194 with both timings):
//
//                            fires  reweighted  geomean  regressions  worst
//     compression >= 50x        30       1.65x    2.28x            2   0.76x
//     cost model, margin 1.5x   47       2.26x    3.37x            4   0.13x
//     oracle (perfect)          84       5.87x    5.78x            0   1.00x
//
// The remaining regressions are all watdiv, and all of them fail on DuckDB's
// side: it runs 3-24x faster than predicted because our *flat* estimate is too
// high on uniform data (F18's known weak spot). Better flat estimation, not a
// better decision rule, is what would close them.
//
// No DuckDB headers (plan section 4).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "ftree.hpp"
#include "stats.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace factorize {

//! One relation as it enters the plan.
struct CostStep {
	//! This relation's join column, with its MCV list.
	ColumnStats key;
	//! Which key equivalence class `key` belongs to. Relations sharing a class
	//! join on the same values, so their skew compounds and has to be estimated
	//! together; relations in different classes are combined pairwise.
	//!
	//! Filling this in correctly *is* equality propagation. Attaching each
	//! relation beneath whichever relation its predicate happens to name --
	//! rather than beneath the shallowest equivalent attribute -- makes every
	//! query look like a chain, and chains do not compress, so the estimate
	//! comes back at about 1x for queries that compress 3000x.
	int key_group = 0;
	//! Index of the earlier step whose f-tree node this attaches beneath;
	//! -1 for the first relation.
	int parent_step = -1;
	//! The parent's join column, i.e. the other side of this edge. Only
	//! consulted when the edge crosses equivalence classes.
	ColumnStats parent_key;
};

//! What the gate concluded, and why.
struct CostEstimate {
	//! Estimated tuples in the flat result.
	double flat_tuples = 0;
	//! Estimated records in the f-representation.
	double factorized_records = 0;
	//! flat / factorized: the predicted compression. Reported because it is
	//! the quantity the literature discusses, but no longer the decision --
	//! see the header comment.
	double ratio = 1.0;
	//! Estimated milliseconds for each engine.
	double ours_ms = 0;
	double duckdb_ms = 0;
	//! Estimated bytes the f-representation will occupy.
	double bytes = 0;
	//! Whether the join graph is acyclic. The paper reports cyclic queries as
	//! 32% slower, so they are refused outright.
	bool acyclic = true;
	//! Set when the gate says to factorize.
	bool fire = false;
	//! Human-readable reason, for EXPLAIN and for diagnosing a decline.
	std::string reason;
};

//! Linear cost model for one engine: a fixed startup, a per-input-row scan
//! term, and a per-unit-of-output term.
//!
//! Coefficients are milliseconds and are fitted, so they are machine-specific.
//! Phase 3 must re-fit them or drive them from DuckDB's own cost model rather
//! than shipping these numbers.
struct EngineCost {
	double startup_ms = 0;
	double per_input_row_ms = 0;
	//! Per f-representation record for us, per result tuple for DuckDB.
	double per_output_ms = 0;

	double Estimate(double input_rows, double output) const {
		return startup_ms + per_input_row_ms * input_rows + per_output_ms * output;
	}
};

//! Thresholds the gate applies. Exposed because they are the knobs Phase 5
//! tunes, and because a constant tuned on one machine mis-gates on another.
struct CostThresholds {
	//! Our own cost, fitted to sit *above* 75% of observed runs. A gate must be
	//! pessimistic about the engine it is choosing and optimistic about the one
	//! it is rejecting, or its errors all point at regressions.
	//!
	//! Re-fitted a second time (scripts/calibrate-synthetic.py), and the change
	//! is structural rather than a nudge. The previous value pinned startup_ms
	//! to 0, so the per-input-row slope had to carry the fixed cost as well --
	//! and a slope carrying an intercept over-charges by that intercept times
	//! every row. On a 91,000-row star it predicted 123ms for a query that runs
	//! in 3.5ms, a 35x error, and the gate declined a 27x win because of it.
	//!
	//! A free intercept is the difference between a model that is imprecise and
	//! one that cannot express the shape at all. Held out on half the grid, this
	//! predicts 1.63x of actual at the median (pessimistic, as it must be) with
	//! a worst case of 7.5x on a chain whose records the model over-charges.
	EngineCost ours {0.108542, 2.445e-5, 3.961e-5};
	//! DuckDB's cost, fitted to sit below 25% of observed runs.
	//!
	//! Re-fitted twice, and it has now been wrong in both directions. The
	//! original 3.946e-5 ms/tuple over-charged DuckDB and made the gate fire on
	//! seven queries DuckDB was about to win. The correction to 8.788e-7 went
	//! too far the other way: measured on a release build, DuckDB does 27M
	//! tuples in 94ms, which is 3.5e-6 ms/tuple, so it under-charged by 4x.
	//!
	//! Under-charging looks like the safe direction and is not, because of the
	//! floor below. With DuckDB predicted at 0.88ns/tuple its *work* never
	//! reaches min_duckdb_work_ms on anything short of a 10^10-tuple join, so
	//! the floor declined every query regardless of what our side cost. Measured
	//! across 15 synthetic shapes the old pair fired on none of them, including
	//! ones the engine wins by 8.8x.
	//!
	//! This value is 10x *below* the 3.946e-5 that caused the seven regressions,
	//! which bounds the risk of having moved back toward it.
	//!
	//! These are this machine's numbers, fitted on synthetic uniform data. That
	//! is not a disclaimer to be apologised for -- it is why the fitting script
	//! is checked in, and why calibrate-synthetic.py needs no corpus: the two
	//! scripts that came before it required a 5.3GB download, so in practice the
	//! coefficients could not be re-fitted at all, only inherited.
	EngineCost duckdb {0.0, 2.324e-5, 3.981e-6};
	//! Fire only when we are predicted to beat DuckDB by this factor.
	//!
	//! 1.5, and it was briefly 1.2 for a reason worth recording: with the gate's
	//! estimate biased low, a wide margin compounded the bias, and narrowing it
	//! bought three wins. That was compensation for a broken input, not a better
	//! threshold, and once the input was fixed the compensation became a cost.
	//! Measured end to end over the corpus with the MCV sample in place, 1.5
	//! comes out at 12.41s and 1.2 at 12.79s (D41).
	//!
	//! The bias it was compensating for: both sides of the comparison are driven
	//! by an estimated tuple count, and it does not bias them evenly. DuckDB's
	//! predicted cost is dominated by *tuples* while ours is dominated by
	//! *records*, and records grow far more slowly, so under-estimating
	//! cardinality shrinks DuckDB's side much harder than ours -- every such
	//! error argues against firing. Widening or narrowing a margin cannot fix
	//! that; supplying the statistic the estimator was written to use can.
	double margin = 1.5;
	//! Bytes an f-representation record occupies: a fixed header plus a slot
	//! per relation. Measured across 857 CE queries -- median 47 bytes overall,
	//! rising from 37 at three relations to 64 at twelve.
	double bytes_per_record = 28.0;
	double bytes_per_relation = 3.0;
	//! Decline when the f-representation is predicted not to fit. Zero means no
	//! limit.
	//!
	//! This is the difference between declining a query and being killed by it.
	//! F17: 47% of the excluded-regime queries exceed a 6 GB budget, and result
	//! size does not predict which -- a 1.013e12-tuple query fits in 497K
	//! records while a 1.093e11-tuple one does not.
	//!
	//! Measured on the CE corpus at a 4 GB budget it catches 47 of 75 overruns
	//! and refuses 87 of 840 queries that would have fit. The refusals are free
	//! there: the gate fires on *none* of them. AUC is 0.87, and predicting
	//! bytes is no better than predicting records alone (0.875 vs 0.874) -- the
	//! width term buys nothing but a number in the units budgets are set in.
	double memory_budget_bytes = 0;
	//! How far past the budget the *prediction* may go before the gate declines
	//! on size alone.
	//!
	//! Not a safety margin -- nothing about memory safety runs through here.
	//! Exceeding the real budget is handled twice at run time already:
	//! ExecuteCountSliceWithinMemory partitions the join key and re-counts
	//! rather than failing (D20), and D34's estimate budget abandons to the
	//! stock plan when the representation outgrows what the gate bet on. This
	//! check exists only because slicing costs a pass over the input per slice,
	//! so a query predicted not to fit is predicted to be slow.
	//!
	//! It is set to 64 because the prediction is not good enough to ban a query
	//! on. Measured against what those queries actually build: `hetio_203_16`
	//! is predicted at 24GB and holds 17MB, over by 1,390x; `hetio_203_19` at
	//! 16GB against 15MB, over by 1,090x. Both answer in under a second, and
	//! DuckDB answers neither at all. On the corpus the CE benchmark disables,
	//! 134 of 138 declines were this one check firing on numbers like those.
	//!
	//! The direction of the error is what settles the value. A record count
	//! comes out of a recurrence that multiplies down the join tree, so its
	//! errors compound upward, while the tuple estimate feeding the same model
	//! runs 100x *low* (D41). Being over by three orders of magnitude is the
	//! normal case, not the tail.
	double memory_slack = 64.0;
	//! Refuse cyclic join graphs.
	bool require_acyclic = true;
	//! Decline when the work DuckDB is predicted to do -- its per-row and
	//! per-tuple terms, *excluding* its fixed startup -- is under this many
	//! milliseconds.
	//!
	//! Without it the gate fires on trivially small queries, because the fitted
	//! `duckdb.startup_ms` of 34ms is larger than anything our side costs on a
	//! handful of rows, so the margin is cleared by a constant that has nothing
	//! to do with the query. Below this floor there is nothing to win: DuckDB is
	//! finishing in microseconds and our fixed costs -- scanning the inputs a
	//! second time, building a representation -- are the entire runtime. The
	//! plan (§5.3) asks for this floor in exactly these terms.
	//!
	//! 5ms rather than 10, because at 10 the floor was rejecting on a number it
	//! cannot read. It compares against DuckDB's *predicted* work, and that
	//! prediction is 100x to 350x low on skewed joins: `epinions_202_12` was
	//! declined for 2ms of predicted work against a stock plan that takes 739ms,
	//! and `epinions_202_04` for 4ms against 481ms. Three such queries, worth
	//! 1.7s, were turned down by a floor whose whole purpose is to spot queries
	//! that are too small -- on queries that are not small at all.
	//!
	//! Not zero. The floor's intent is sound and a query DuckDB finishes in
	//! microseconds really has nothing to win; 0, 1, 2 and 5 are indistinguishable
	//! on both corpora, so 5 is the conservative member of a measured plateau
	//! rather than the removal of a guard (D40).
	double min_duckdb_work_ms = 5.0;
};

//! One observation for calibration: what a query cost an engine, and the two
//! quantities the model charges for.
struct CostSample {
	double input_rows = 0;
	//! Records for us, result tuples for DuckDB.
	double output = 0;
	double millis = 0;
};

//! Fits an EngineCost to observations by quantile regression in log space.
//!
//! `quantile` is the share of the data the fit should sit *above*: 0.75 makes a
//! pessimistic model, 0.25 an optimistic one, 0.5 a median fit. The asymmetry
//! is the point -- see CostThresholds.
//!
//! This exists because the defaults in CostThresholds were fitted on one
//! machine and do not transfer (DECISIONS O11). Anything shipping this gate has
//! to re-fit rather than inherit them.
EngineCost FitEngineCost(const std::vector<CostSample> &samples, double quantile);

//! Estimates the two sizes and applies the thresholds.
//!
//! Relations are first partitioned by equivalence class and each class sized
//! whole (see EstimateGroup), because within a class a skewed value appears in
//! every relation at once and its contribution multiplies. Classes are then
//! combined pairwise along the plan with the textbook rule, where uniformity is
//! a fair assumption because the skew has already been accounted for.
CostEstimate EstimateCost(const std::vector<CostStep> &steps, bool acyclic,
                          const CostThresholds &thresholds = CostThresholds());

} // namespace factorize
