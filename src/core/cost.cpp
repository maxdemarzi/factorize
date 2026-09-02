#include "cost.hpp"

#include <algorithm>
#include <limits>
#include <cmath>
#include <map>
#include <vector>

namespace factorize {

namespace {

std::string Format(double value) {
	if (value >= 1e12) {
		return std::to_string(static_cast<long long>(value / 1e12)) + "T";
	}
	if (value >= 1e9) {
		return std::to_string(static_cast<long long>(value / 1e9)) + "B";
	}
	if (value >= 1e6) {
		return std::to_string(static_cast<long long>(value / 1e6)) + "M";
	}
	if (value >= 1e3) {
		return std::to_string(static_cast<long long>(value / 1e3)) + "K";
	}
	return std::to_string(static_cast<long long>(value));
}

//! Milliseconds, rounded; the tuple-count formatter's K/M/B suffixes read as
//! nonsense on a duration.
std::string Millis(double value) {
	return std::to_string(static_cast<long long>(value + 0.5));
}

std::string Round(double value) {
	const auto text = std::to_string(value);
	const auto dot = text.find('.');
	return dot == std::string::npos ? text : text.substr(0, dot + 2);
}

} // namespace

EngineCost FitEngineCost(const std::vector<CostSample> &samples, double quantile) {
	EngineCost fit;
	if (samples.empty()) {
		return fit;
	}
	// Pinball loss on log(predicted / actual). Log space because the costs span
	// five orders of magnitude and an absolute-error fit would see only the
	// slowest queries.
	const auto loss = [&](const EngineCost &candidate) {
		double total = 0;
		for (const auto &sample : samples) {
			const double predicted = candidate.Estimate(sample.input_rows, sample.output);
			if (predicted <= 0 || sample.millis <= 0) {
				return std::numeric_limits<double>::infinity();
			}
			const double residual = std::log(predicted / sample.millis);
			total += residual >= 0 ? (1.0 - quantile) * residual : -quantile * residual;
		}
		return total;
	};

	// A coarse grid to find the basin, then coordinate descent. The parameters
	// span many orders of magnitude, so the grid is logarithmic.
	double best = std::numeric_limits<double>::infinity();
	for (double startup : {0.0, 1.0, 3.0, 10.0, 30.0, 100.0, 300.0}) {
		for (int i = -16; i <= 0; i++) {
			for (int j = -17; j <= 0; j++) {
				EngineCost candidate {startup, std::pow(10.0, i / 2.0),
				                      j < -16 ? 0.0 : std::pow(10.0, j / 2.0)};
				const double value = loss(candidate);
				if (value < best) {
					best = value;
					fit = candidate;
				}
			}
		}
	}
	double *fields[3] = {&fit.startup_ms, &fit.per_input_row_ms, &fit.per_output_ms};
	for (int round = 0; round < 80; round++) {
		bool improved = false;
		for (double *field : fields) {
			for (double factor : {1.4, 1.0 / 1.4, 1.1, 1.0 / 1.1, 1.02, 1.0 / 1.02}) {
				const double saved = *field;
				if (saved <= 0) {
					continue;
				}
				*field = saved * factor;
				const double value = loss(fit);
				if (value < best - 1e-9) {
					best = value;
					improved = true;
				} else {
					*field = saved;
				}
			}
		}
		if (!improved) {
			break;
		}
	}
	return fit;
}

CostEstimate EstimateCost(const std::vector<CostStep> &steps, bool acyclic, const CostThresholds &thresholds) {
	CostEstimate estimate;
	estimate.acyclic = acyclic;
	if (steps.empty()) {
		estimate.reason = "empty plan";
		return estimate;
	}

	// Partition by equivalence class, keeping first-appearance order so the
	// class containing the plan's first relation stays the root.
	std::map<int, size_t> index_of_group;
	std::vector<int> group_ids;
	std::vector<std::vector<ColumnStats>> group_columns;
	std::vector<size_t> group_of_step(steps.size(), 0);
	std::vector<size_t> slot_of_step(steps.size(), 0);
	for (size_t i = 0; i < steps.size(); i++) {
		const int id = steps[i].key_group;
		auto found = index_of_group.find(id);
		if (found == index_of_group.end()) {
			found = index_of_group.emplace(id, group_columns.size()).first;
			group_ids.push_back(id);
			group_columns.emplace_back();
		}
		group_of_step[i] = found->second;
		slot_of_step[i] = group_columns[found->second].size();
		group_columns[found->second].push_back(steps[i].key);
	}

	// Each class sized whole: within a class a skewed value appears in every
	// relation at once, and that compounding is exactly what a per-edge scalar
	// cannot represent.
	std::vector<GroupSize> sizes(group_columns.size());
	for (size_t g = 0; g < group_columns.size(); g++) {
		sizes[g] = EstimateGroup(group_columns[g]);
	}

	// Edges that cross classes, in plan order. Each is the single point where
	// one class hangs beneath another.
	struct CrossEdge {
		size_t child_group;
		size_t parent_step;
		ColumnStats parent_column;
	};
	std::vector<CrossEdge> cross;
	std::vector<bool> attached(group_columns.size(), false);
	attached[group_of_step[0]] = true;
	for (size_t i = 1; i < steps.size(); i++) {
		const size_t group = group_of_step[i];
		if (attached[group]) {
			continue;
		}
		attached[group] = true;
		const size_t parent = steps[i].parent_step >= 0 ? static_cast<size_t>(steps[i].parent_step) : 0;
		cross.push_back({group, parent, steps[i].parent_key});
	}

	// The root class, then each further class folded in.
	//
	// A class is folded in as a whole -- a super-node with `flat` tuples over
	// `distinct` key values -- using the same recurrence the per-relation model
	// used. Carrying the parent's path cardinality is what distinguishes the two
	// shapes: siblings inside a class share one key node and their records add,
	// but a class hanging *below* another is re-instantiated under every tuple of
	// the path above it, so its records multiply. Dropping that multiplier makes
	// a chain look like it compresses 51x, which is what it did before this
	// recurrence replaced a flat sum (test_cost.cpp: "a chain should not fire").
	// `flat` is the whole result: the product over every edge, since sibling
	// classes branch and no single path holds it.
	//
	// Records are tracked per *step*, not per class. A child subtree is
	// instantiated once per parent record -- not once per parent tuple, because
	// records are precisely what the f-representation keeps small (using the
	// flat cardinality drove every predicted ratio to 1) -- and it attaches
	// beneath one relation's node, not beneath its whole class. Charging it the
	// class total over-counts the contexts by the number of siblings, which is
	// the common case: 68% of the corpus has a relation joined on both columns.
	std::vector<double> node_records(steps.size(), 0.0);
	const size_t root = group_of_step[0];
	const auto &root_size = sizes[root];
	for (size_t i = 0; i < steps.size(); i++) {
		if (group_of_step[i] == root) {
			node_records[i] = root_size.column_records[slot_of_step[i]];
		}
	}
	double flat = root_size.flat;
	double records = root_size.records;

	for (const auto &edge : cross) {
		const auto &child = sizes[edge.child_group];
		const double parent_distinct = std::max(1.0, edge.parent_column.distinct);
		const double divisor = std::max(parent_distinct, child.distinct);
		const double contexts = std::max(1.0, node_records[edge.parent_step]);

		// Uniformity is fair here: the skew inside each class has already been
		// accounted for, and what is left is how many of the parent's values find
		// a partner.
		flat = flat * child.flat / divisor;

		// Each context carries one connecting value, so it instantiates the
		// class's share of records for that value -- per relation, so a later
		// class can attach beneath the right one.
		const double child_distinct = std::max(1.0, child.distinct);
		for (size_t i = 0; i < steps.size(); i++) {
			if (group_of_step[i] != edge.child_group) {
				continue;
			}
			const double share = child.column_records[slot_of_step[i]] / child_distinct;
			node_records[i] = contexts * share;
			records += node_records[i];
		}
	}

	estimate.flat_tuples = std::max(0.0, flat);
	estimate.factorized_records = std::max(1.0, records);
	estimate.ratio = estimate.flat_tuples / estimate.factorized_records;

	// Input rows are the scan cost, and every relation contributes its own.
	double input_rows = 0;
	for (const auto &step : steps) {
		input_rows += step.key.rows;
	}
	estimate.ours_ms = thresholds.ours.Estimate(input_rows, estimate.factorized_records);
	estimate.duckdb_ms = thresholds.duckdb.Estimate(input_rows, estimate.flat_tuples);

	estimate.bytes = estimate.factorized_records *
	                 (thresholds.bytes_per_record +
	                  thresholds.bytes_per_relation * static_cast<double>(steps.size()));

	if (thresholds.require_acyclic && !acyclic) {
		estimate.reason = "cyclic join graph";
		return estimate;
	}
	// Checked before the margin, because "will not fit" is a different answer
	// from "will not pay" and deserves to say so.
	if (thresholds.memory_budget_bytes > 0 && estimate.bytes > thresholds.memory_budget_bytes) {
		estimate.reason = "predicted " + Format(estimate.bytes) + "B of f-representation, over the " +
		                  Format(thresholds.memory_budget_bytes) + "B budget";
		return estimate;
	}
	if (estimate.duckdb_ms < thresholds.margin * estimate.ours_ms) {
		estimate.reason = "predicted " + Millis(estimate.ours_ms) + "ms against DuckDB's " +
		                  Millis(estimate.duckdb_ms) + "ms, under the " + Round(thresholds.margin) +
		                  "x margin";
		return estimate;
	}

	estimate.fire = true;
	estimate.reason = "predicted " + Millis(estimate.ours_ms) + "ms against DuckDB's " +
	                  Millis(estimate.duckdb_ms) + "ms, from " + Format(estimate.flat_tuples) +
	                  " tuples compressed " + Round(estimate.ratio) + "x";
	return estimate;
}

} // namespace factorize
