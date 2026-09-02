#include "stats.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace factorize {

double ColumnStats::TailRows() const {
	double covered = 0;
	for (const auto &entry : mcv) {
		covered += entry.second;
	}
	return std::max(0.0, rows - covered);
}

double ColumnStats::TailDistinct() const {
	return std::max(1.0, distinct - static_cast<double>(mcv.size()));
}

double ColumnStats::Frequency(int64_t value) const {
	for (const auto &entry : mcv) {
		if (entry.first == value) {
			return entry.second;
		}
	}
	return TailRows() / TailDistinct();
}

GroupSize EstimateGroup(const std::vector<ColumnStats> &group) {
	GroupSize size;
	if (group.empty()) {
		return size;
	}
	if (group.size() == 1) {
		size.flat = group[0].rows;
		size.records = group[0].rows;
		size.distinct = group[0].distinct;
		size.column_records.assign(1, group[0].rows);
		return size;
	}

	// The head: every value any relation considered common enough to store.
	// A hub is a hub in all of them, so the union is small and the overlap
	// high -- which is why so few entries recover so much of the join.
	std::vector<int64_t> values;
	std::unordered_set<int64_t> seen;
	for (const auto &column : group) {
		for (const auto &entry : column.mcv) {
			if (seen.insert(entry.first).second) {
				values.push_back(entry.first);
			}
		}
	}

	// Known limitation, measured and left in place deliberately. `Frequency`
	// returns the tail average for every value it did not store and never zero,
	// so a head value is counted as present in every relation. Where
	// cardinalities differ sharply that over-predicts: on watdiv_acyclic_212_15
	// one relation holds 1,659 of a 125,145-value domain and genuinely contains
	// only 18% of the head values, and the join came out 84x high.
	//
	// Weighting by the share of the domain each column covers fixes that query
	// and loses more elsewhere -- it is the containment assumption, and
	// containment is *correct* for a foreign-key join, where the small side is
	// contained by construction (see test_cost.cpp). Swept over exponents 0,
	// 0.33, 0.5 and 1.0, overall error moved 3.8x -> 3.6x, inside the noise,
	// while hetio degraded 3.2x -> 6.3x and a systematic under-prediction bias
	// appeared (1.07x -> 0.39x). Not worth taking (FINDINGS F18).

	double head_flat = 0;
	double head_records = 0;
	double head_distinct = 0;
	std::vector<double> column_records(group.size(), 0.0);
	for (int64_t value : values) {
		double product = 1;
		double sum = 0;
		for (size_t i = 0; i < group.size(); i++) {
			const double frequency = group[i].Frequency(value);
			product *= frequency;
			sum += frequency;
			column_records[i] += frequency;
		}
		head_flat += product;
		head_records += sum;
		if (product > 0) {
			head_distinct += 1;
		}
	}

	// The tail: values nobody stored. Here uniformity is the right model,
	// because the tail is what is left after the skew has been taken out.
	double tail_flat = group[0].TailRows();
	double tail_domain = group[0].TailDistinct();
	for (size_t i = 1; i < group.size(); i++) {
		const double divisor = std::max(group[i].TailDistinct(), tail_domain);
		tail_flat = tail_flat * group[i].TailRows() / divisor;
		tail_domain = std::max(tail_domain, group[i].TailDistinct());
	}

	// Records in the tail: each relation contributes the rows that survive,
	// approximated by the share of its tail values that all the others also
	// hold. The smallest tail bounds how many values can survive at all.
	double surviving = group[0].TailDistinct();
	for (const auto &column : group) {
		surviving = std::min(surviving, column.TailDistinct());
	}
	double tail_records = 0;
	for (size_t i = 0; i < group.size(); i++) {
		const double share = group[i].TailRows() * surviving / group[i].TailDistinct();
		tail_records += share;
		column_records[i] += share;
	}

	size.flat = head_flat + tail_flat;
	size.records = head_records + tail_records;
	size.distinct = std::max(1.0, head_distinct + surviving);
	size.column_records = std::move(column_records);
	return size;
}

} // namespace factorize
