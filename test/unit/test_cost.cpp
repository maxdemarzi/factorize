//===----------------------------------------------------------------------===//
//                         factorize
//
// test/unit/test_cost.cpp
//
// The estimator is checked against joins whose exact answer is computed by
// brute force from the same frequency tables. That is the only honest test:
// the failure this code exists to fix (FINDINGS F13) was not a crash, it was a
// plausible-looking number that was wrong by 1400x, and only a comparison
// against ground truth catches that class of bug.
//
//===----------------------------------------------------------------------===//

#include "../../src/core/cost.hpp"
#include "../../src/core/stats.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <vector>

using namespace factorize;

static int failures = 0;
static int checks = 0;

static void Check(bool condition, const char *what) {
	checks++;
	if (!condition) {
		failures++;
		std::printf("  FAIL: %s\n", what);
	}
}

//! Within a factor of `tolerance` in either direction.
static void CheckClose(double got, double want, double tolerance, const char *what) {
	checks++;
	const double ratio = want > 0 && got > 0 ? got / want : (got == want ? 1.0 : 0.0);
	if (ratio < 1.0 / tolerance || ratio > tolerance) {
		failures++;
		std::printf("  FAIL: %s -- got %.6g, want %.6g (off by %.1fx)\n", what, got, want,
		            ratio > 1 ? ratio : 1 / ratio);
	}
}

//! Exact size of joining a set of frequency tables on equal values.
static void ExactGroup(const std::vector<std::map<int64_t, double>> &columns, double &flat, double &records) {
	flat = 0;
	records = 0;
	for (const auto &entry : columns[0]) {
		double product = 1;
		double sum = 0;
		for (const auto &column : columns) {
			const auto found = column.find(entry.first);
			if (found == column.end()) {
				product = 0;
				sum = 0;
				break;
			}
			product *= found->second;
			sum += found->second;
		}
		flat += product;
		records += sum;
	}
}

static ColumnStats ToStats(const std::map<int64_t, double> &column, size_t k) {
	ColumnStats stats;
	std::vector<std::pair<int64_t, double>> all(column.begin(), column.end());
	std::sort(all.begin(), all.end(),
	          [](const std::pair<int64_t, double> &a, const std::pair<int64_t, double> &b) {
		          return a.second > b.second;
	          });
	for (const auto &entry : all) {
		stats.rows += entry.second;
	}
	stats.distinct = static_cast<double>(all.size());
	all.resize(std::min(k, all.size()));
	stats.mcv = all;
	return stats;
}

//! A uniform column: every value appears the same number of times.
static std::map<int64_t, double> Uniform(int64_t values, double per_value) {
	std::map<int64_t, double> column;
	for (int64_t v = 0; v < values; v++) {
		column[v] = per_value;
	}
	return column;
}

//! A heavy-tailed column, which is what real join keys look like: a few hubs
//! carrying most of the rows, and a long flat tail.
static std::map<int64_t, double> Skewed(int64_t values, int64_t hubs, double hub_rows, double tail_rows) {
	std::map<int64_t, double> column;
	for (int64_t v = 0; v < values; v++) {
		column[v] = v < hubs ? hub_rows : tail_rows;
	}
	return column;
}

static void TestUniformIsUnchanged() {
	std::printf("uniform data: MCV must not make the estimate worse\n");
	// Five relations, 1000 values, 4 rows each. The textbook formula is exact
	// here, so the MCV version has to agree with it.
	std::vector<std::map<int64_t, double>> columns(5, Uniform(1000, 4));
	double flat = 0, records = 0;
	ExactGroup(columns, flat, records);

	for (size_t k : {size_t(0), size_t(8), size_t(128)}) {
		std::vector<ColumnStats> stats;
		for (const auto &column : columns) {
			stats.push_back(ToStats(column, k));
		}
		const auto size = EstimateGroup(stats);
		CheckClose(size.flat, flat, 1.05, "uniform flat");
		CheckClose(size.records, records, 1.05, "uniform records");
	}
}

static void TestSkewIsRecovered() {
	std::printf("skewed data: MCV must beat the textbook estimate by orders of magnitude\n");
	// Five relations over 6000 values, 5 hubs at 40 rows, tail at 1 row. This
	// is the epinions shape: the hubs are a rounding error in the distinct
	// count and the entire result in practice.
	std::vector<std::map<int64_t, double>> columns(5, Skewed(6000, 5, 40, 1));
	double flat = 0, records = 0;
	ExactGroup(columns, flat, records);

	std::vector<ColumnStats> uniform_stats;
	for (const auto &column : columns) {
		uniform_stats.push_back(ToStats(column, 0)); // no MCVs -> textbook
	}
	const auto without = EstimateGroup(uniform_stats);

	std::vector<ColumnStats> mcv_stats;
	for (const auto &column : columns) {
		mcv_stats.push_back(ToStats(column, 8));
	}
	const auto with = EstimateGroup(mcv_stats);

	const double error_without = flat / without.flat;
	const double error_with = flat / with.flat;
	std::printf("  exact %.6g   textbook %.6g (%.0fx off)   mcv-8 %.6g (%.1fx off)\n", flat, without.flat,
	            error_without, with.flat, error_with);
	Check(error_without > 10, "textbook estimate should be badly wrong on skew");
	CheckClose(with.flat, flat, 1.5, "mcv flat on skew");
	CheckClose(with.records, records, 1.5, "mcv records on skew");
}

static void TestRandomisedAgainstBruteForce() {
	std::printf("randomised: 200 groups against brute force\n");
	std::mt19937_64 rng(20260902);
	double worst = 1;
	for (int trial = 0; trial < 200; trial++) {
		const size_t arity = 2 + rng() % 4;
		const int64_t values = 200 + static_cast<int64_t>(rng() % 2000);
		const int64_t hubs = 1 + static_cast<int64_t>(rng() % 16);
		const double hub_rows = 5 + static_cast<double>(rng() % 200);
		std::vector<std::map<int64_t, double>> columns;
		for (size_t i = 0; i < arity; i++) {
			columns.push_back(Skewed(values, hubs, hub_rows, 1 + static_cast<double>(rng() % 3)));
		}
		double flat = 0, records = 0;
		ExactGroup(columns, flat, records);
		if (flat <= 0) {
			continue;
		}
		std::vector<ColumnStats> stats;
		for (const auto &column : columns) {
			stats.push_back(ToStats(column, 128));
		}
		const auto size = EstimateGroup(stats);
		const double ratio = size.flat / flat;
		worst = std::max(worst, ratio > 1 ? ratio : 1 / ratio);
	}
	std::printf("  worst error across 200 groups: %.2fx\n", worst);
	Check(worst < 2.0, "randomised groups should stay within 2x");
}

//! A foreign-key-shaped join: six wide relations and one narrow one whose keys
//! are all contained in them. This is the case that makes the obvious fix for
//! the corpus's worst over-prediction unsafe -- weighting frequencies by each
//! column's share of the key domain assumes the narrow side is usually absent,
//! and here it is always present. Estimating this join 134x low is worse than
//! estimating watdiv_acyclic_212_15 84x high (FINDINGS F18).
static void TestMismatchedCardinalities() {
	std::printf("mismatched cardinalities: containment holds for a foreign-key join\n");
	// Six wide relations over 50,000 values, and one narrow one holding 500.
	// The narrow relation bounds the join: at most 500 values can survive.
	std::vector<std::map<int64_t, double>> columns;
	for (int i = 0; i < 6; i++) {
		columns.push_back(Skewed(50000, 8, 300, 2));
	}
	columns.push_back(Uniform(500, 1));
	double flat = 0, records = 0;
	ExactGroup(columns, flat, records);

	std::vector<ColumnStats> stats;
	for (const auto &column : columns) {
		stats.push_back(ToStats(column, 128));
	}
	const auto size = EstimateGroup(stats);
	std::printf("  exact %.6g   estimated %.6g (%.1fx)\n", flat, size.flat,
	            size.flat > flat ? size.flat / flat : flat / size.flat);
	CheckClose(size.flat, flat, 10.0, "mismatched-cardinality flat");
}

//! Calibration has to recover coefficients it was given, and the quantile has
//! to actually move the fit -- otherwise the asymmetry the gate depends on is
//! silently absent. DECISIONS O11: the shipped constants are one machine's, so
//! this is the supported way to replace them.
static void TestCalibration() {
	std::printf("calibration: recover known coefficients, and respect the quantile\n");
	const EngineCost truth {5.0, 1e-4, 2e-4};
	std::mt19937_64 rng(7);
	std::vector<CostSample> samples;
	for (int i = 0; i < 300; i++) {
		CostSample sample;
		sample.input_rows = 1e3 + static_cast<double>(rng() % 10000000);
		sample.output = 1e2 + static_cast<double>(rng() % 5000000);
		// +/-2x of noise, so the fit has to be robust rather than exact.
		const double noise = std::pow(2.0, (static_cast<double>(rng() % 2001) - 1000.0) / 1000.0);
		sample.millis = truth.Estimate(sample.input_rows, sample.output) * noise;
		samples.push_back(sample);
	}
	const auto median = FitEngineCost(samples, 0.5);
	CheckClose(median.per_input_row_ms, truth.per_input_row_ms, 2.0, "calibrated input coefficient");
	CheckClose(median.per_output_ms, truth.per_output_ms, 2.0, "calibrated output coefficient");

	auto above = [&](const EngineCost &fit) {
		int n = 0;
		for (const auto &sample : samples) {
			if (fit.Estimate(sample.input_rows, sample.output) > sample.millis) {
				n++;
			}
		}
		return n;
	};
	const auto pessimistic = FitEngineCost(samples, 0.75);
	const auto optimistic = FitEngineCost(samples, 0.25);
	std::printf("  sits above the data: optimistic %d%%, median %d%%, pessimistic %d%% of 300\n",
	            above(optimistic) / 3, above(median) / 3, above(pessimistic) / 3);
	Check(above(pessimistic) > above(median), "a higher quantile must sit above more of the data");
	Check(above(median) > above(optimistic), "a lower quantile must sit below more of the data");
}

static void TestGateDecision() {
	std::printf("gate: a star fires, a chain does not\n");
	// A star: five relations on one key. Skewed, so it compresses hugely.
	{
		std::vector<CostStep> steps;
		for (int i = 0; i < 5; i++) {
			CostStep step;
			step.key = ToStats(Skewed(6000, 5, 40, 1), 8);
			step.key_group = 0;
			step.parent_step = i == 0 ? -1 : 0;
			step.parent_key = step.key;
			steps.push_back(step);
		}
		const auto estimate = EstimateCost(steps, true);
		std::printf("  star:  ratio %.1fx flat %.4g -> %s (%s)\n", estimate.ratio, estimate.flat_tuples,
		            estimate.fire ? "FIRE" : "decline", estimate.reason.c_str());
		Check(estimate.ratio > 50, "a skewed star should predict large compression");
		Check(estimate.fire, "a skewed star should fire");
	}
	// A chain: every relation in its own class, joined pairwise. The deepest
	// node already holds the whole result, so there is nothing to compress.
	{
		std::vector<CostStep> steps;
		for (int i = 0; i < 5; i++) {
			CostStep step;
			step.key = ToStats(Uniform(5000, 4), 8);
			step.key_group = i;
			step.parent_step = i == 0 ? -1 : i - 1;
			step.parent_key = ToStats(Uniform(5000, 4), 8);
			steps.push_back(step);
		}
		const auto estimate = EstimateCost(steps, true);
		std::printf("  chain: ratio %.1fx flat %.4g -> %s (%s)\n", estimate.ratio, estimate.flat_tuples,
		            estimate.fire ? "FIRE" : "decline", estimate.reason.c_str());
		Check(estimate.ratio < 10, "a chain should predict little compression");
		Check(!estimate.fire, "a chain should not fire");
	}
	// Cyclic queries are refused outright regardless of the numbers.
	{
		std::vector<CostStep> steps;
		for (int i = 0; i < 3; i++) {
			CostStep step;
			step.key = ToStats(Skewed(6000, 5, 40, 1), 8);
			step.key_group = 0;
			step.parent_step = i == 0 ? -1 : 0;
			steps.push_back(step);
		}
		const auto estimate = EstimateCost(steps, false);
		Check(!estimate.fire, "a cyclic query should not fire");
	}
}

//! A query that would win on time but cannot fit must be declined for the
//! right reason, and the reason has to say so -- "will not fit" is a different
//! answer from "will not pay", and confusing them makes the decline
//! undiagnosable.
static void TestMemoryBudget() {
	std::printf("memory budget: refuse what will not fit, and say why\n");
	std::vector<CostStep> steps;
	for (int i = 0; i < 5; i++) {
		CostStep step;
		step.key = ToStats(Skewed(6000, 5, 40, 1), 8);
		step.key_group = 0;
		step.parent_step = i == 0 ? -1 : 0;
		step.parent_key = step.key;
		steps.push_back(step);
	}

	CostThresholds generous;
	const auto unlimited = EstimateCost(steps, true, generous);
	Check(unlimited.fire, "with no budget this query should fire");
	Check(unlimited.bytes > 0, "bytes must be estimated");

	CostThresholds tight = generous;
	tight.memory_budget_bytes = unlimited.bytes / 2;
	const auto refused = EstimateCost(steps, true, tight);
	std::printf("  %.4g bytes predicted, budget %.4g -> %s (%s)\n", refused.bytes,
	            tight.memory_budget_bytes, refused.fire ? "FIRE" : "decline", refused.reason.c_str());
	Check(!refused.fire, "over budget must decline");
	Check(refused.reason.find("budget") != std::string::npos, "the reason must name the budget");

	CostThresholds ample = generous;
	ample.memory_budget_bytes = unlimited.bytes * 2;
	Check(EstimateCost(steps, true, ample).fire, "under budget must still fire");
}

static void TestEmptyMcvDegradesToTextbook() {
	std::printf("no MCV list: must degrade to the textbook estimator, not to garbage\n");
	std::vector<ColumnStats> stats;
	for (int i = 0; i < 3; i++) {
		ColumnStats column;
		column.rows = 10000;
		column.distinct = 2500;
		stats.push_back(column);
	}
	const auto size = EstimateGroup(stats);
	// 10000 * 10000 / 2500 * 10000 / 2500 = 160000
	CheckClose(size.flat, 160000, 1.05, "textbook fallback");
	Check(size.records > 0, "records must be positive");
}

int main() {
	TestUniformIsUnchanged();
	TestSkewIsRecovered();
	TestRandomisedAgainstBruteForce();
	TestMismatchedCardinalities();
	TestCalibration();
	TestGateDecision();
	TestMemoryBudget();
	TestEmptyMcvDegradesToTextbook();
	std::printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
