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
	// The tail holds at least one row for each value the MCV list does not
	// name, because a distinct value occurs at least once. That is an identity
	// about the column, not an estimate, and it has to be enforced here because
	// `rows - covered` can violate it from either direction: the sampled path
	// scales its MCV counts up and can over-shoot `rows`, and the exact path
	// stores only the top MCV_ENTRIES, so on a skewed column those few exact
	// counts can account for nearly every row on their own.
	//
	// Clamping at zero instead made `Frequency` return zero for every value
	// outside the list, which says the column contains none of the
	// `distinct - mcv.size()` values it is on record as having. In
	// `EstimateGroup` that zero multiplies through the per-value product, so one
	// such column collapses the whole class, and `flat = flat * partners`
	// carries the zero to the end of the query.
	//
	// Measured, the collapse hits 7 of 171 excluded queries on sampled
	// statistics and 43 of 171 on exact ones -- exact statistics make it worse,
	// which is why "ask for perfect statistics" was never the fix (D67).
	//
	// The unnamed count is used raw rather than through TailDistinct(), which
	// clamps to 1 so it is always safe to divide by. A genuinely complete MCV
	// list leaves nothing unnamed and must still report an empty tail, not a
	// phantom row.
	const double unnamed = std::max(0.0, distinct - static_cast<double>(mcv.size()));
	return std::max(unnamed, rows - covered);
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

bool ColumnStats::Stored(int64_t value) const {
	for (const auto &entry : mcv) {
		if (entry.first == value) {
			return true;
		}
	}
	return false;
}

GroupSize EstimateGroup(const std::vector<ColumnStats> &group, const EstimatorOptions &options) {
	GroupSize size;
	if (group.empty()) {
		return size;
	}
	if (group.size() == 1) {
		size.flat = group[0].rows;
		size.records = group[0].rows;
		size.distinct = group[0].distinct;
		size.column_records.assign(1, group[0].rows);
		// A single relation needs no per-value table -- every value it holds it
		// holds by itself -- but `FlatFor` is still asked about it by every
		// cross-class edge that attaches here, and leaving the per-value fields
		// at their zero defaults made it answer "no tuples" for every value of a
		// class holding `rows` of them.
		//
		// Measured on `watdiv_acyclic_217_05`: the edge joining
		// watdiv1052578.d to watdiv1052572.d asked for each of the parent's 24
		// values and was told zero for all of them, which zeroed `partners` and
		// so the whole query. Those two columns share all 24 values and their
		// join is 3,855,683 rows (D70).
		size.uniform_flat_per_value = size.flat / std::max(1.0, size.distinct);
		size.tail_flat_per_value = size.uniform_flat_per_value;
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

	// Containment weight per column: the share of the class's domain it covers,
	// raised to `containment_exponent`. At the default exponent of 0 every
	// weight is 1 and the loop below is the shipped arithmetic unchanged.
	double widest_domain = 1;
	for (const auto &column : group) {
		widest_domain = std::max(widest_domain, column.distinct);
	}
	std::vector<double> containment(group.size(), 1.0);
	if (options.containment_exponent > 0) {
		for (size_t i = 0; i < group.size(); i++) {
			const double share = std::min(1.0, std::max(0.0, group[i].distinct / widest_domain));
			containment[i] = std::pow(share, options.containment_exponent);
		}
	}

	double head_flat = 0;
	double head_records = 0;
	double head_distinct = 0;
	std::vector<double> column_records(group.size(), 0.0);
	for (int64_t value : values) {
		double product = 1;
		double sum = 0;
		for (size_t i = 0; i < group.size(); i++) {
			double frequency = group[i].Frequency(value);
			// Only a value this column did not store is discounted. One it did
			// store is an exact count and containment has nothing to say.
			if (containment[i] < 1.0 && !group[i].Stored(value)) {
				frequency *= containment[i];
			}
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
	//
	// Known bug, measured and left in place deliberately. The textbook rule
	// leaves min(V_R, V_S) values on the key after a join; this carries the
	// *max* forward, so a narrow relation after a wide one is divided by the
	// wide domain again and a class's size depends on the order of its
	// relations -- 0.017 tuples for a join of 5,519 on yago_acyclic_Star_6_22.
	// Fixed, it is right and the gate is worse: on watdiv containment does not
	// hold, the max had been cancelling that, and the one query the fix newly
	// admits runs 133x slower than stock (D45). It belongs with the fix for the
	// over-prediction on the runnable corpus, not before it.
	double tail_flat = group[0].TailRows();
	double tail_domain = group[0].TailDistinct();
	for (size_t i = 1; i < group.size(); i++) {
		const double divisor = std::max(group[i].TailDistinct(), tail_domain);
		tail_flat = tail_flat * group[i].TailRows() / divisor;
		tail_domain = options.tail_min_domain ? std::min(tail_domain, group[i].TailDistinct())
		                                      : std::max(tail_domain, group[i].TailDistinct());
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
	// Recomputed rather than accumulated above, because the loop that builds
	// head_flat runs over the union of the MCVs and this has to be keyed and
	// sorted for lookup. The same products, kept instead of summed away.
	size.flat_by_value.reserve(values.size());
	for (int64_t value : values) {
		double product = 1;
		for (size_t i = 0; i < group.size(); i++) {
			double frequency = group[i].Frequency(value);
			if (containment[i] < 1.0 && !group[i].Stored(value)) {
				frequency *= containment[i];
			}
			product *= frequency;
		}
		size.flat_by_value.emplace_back(value, product);
	}
	std::sort(size.flat_by_value.begin(), size.flat_by_value.end(),
	          [](const std::pair<int64_t, double> &a, const std::pair<int64_t, double> &b) {
		          return a.first < b.first;
	          });
	size.tail_flat_per_value = tail_flat / std::max(1.0, surviving);
	size.uniform_flat_per_value = size.flat / std::max(1.0, size.distinct);
	return size;
}

double GroupSize::FlatFor(int64_t value) const {
	const auto found = std::lower_bound(flat_by_value.begin(), flat_by_value.end(), value,
	                                    [](const std::pair<int64_t, double> &entry, int64_t v) {
		                                    return entry.first < v;
	                                    });
	if (found != flat_by_value.end() && found->first == value) {
		return found->second;
	}
	// No head at all means no per-value information, so the uniform rate is
	// the whole of what is known. Falling through to the tail rate instead
	// answers zero whenever a column's head covers all its rows, and zero
	// multiplies through `flat = flat * partners` to end the query (D70).
	if (flat_by_value.empty()) {
		return uniform_flat_per_value;
	}
	return tail_flat_per_value;
}

} // namespace factorize
