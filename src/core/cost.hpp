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
	EngineCost ours {0.0, 2.214e-4, 1.694e-4};
	//! DuckDB's cost, fitted to sit below 75% of observed runs.
	EngineCost duckdb {34.33, 5.674e-6, 3.946e-5};
	//! Fire only when we are predicted to beat DuckDB by this factor.
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
	//! Refuse cyclic join graphs.
	bool require_acyclic = true;
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
