//===----------------------------------------------------------------------===//
//                         factorize
//
// src/include/factorize/stats_cache.hpp
//
// Remembers the gate's sampled statistics so the same unchanged table is not
// re-read for every query.
//
// D92 measured that `auto` is slower than `off` on short queries even when it
// declines them outright -- 1.218x under 20ms, vanishing past 100ms, a fixed
// cost. D93 located all of it in statistics sampling: with sampling off the
// ratio is 1.000, not merely smaller. D94 then ruled out the cheap fix, because
// the cost is the per-relation setup of opening a sampling scan rather than
// the rows it reads -- 256 rows cost the same 2ms as 16384, and decided worse.
//
// So the scan has to be avoided rather than shortened, and it can be: the
// statistics of a table nobody has written to are the same statistics every
// time, and a session over a fixed schema re-derives them per query.
//
// **A stale entry cannot produce a wrong answer.** Everything here feeds the
// gate, which chooses between two plans that agree -- `auto` is bit-exact
// against `off` whatever it decides. The worst a wrong entry does is cost
// time, which is the same currency the cache is trying to save, and the
// reason this is allowed a heuristic key at all.
//
// The key carries the estimated cardinality, so an insert or delete that
// changes the row count misses and re-samples. A change that leaves the count
// identical -- delete one row, insert another -- hits a stale entry. That is
// accepted deliberately: the sample is 16384 rows of a table that may hold
// millions, so it was already an approximation of the data, and two rows of
// drift is smaller than the sampling error it sits inside.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/object_cache.hpp"

#include "../../core/cost.hpp"

#include <string>

namespace factorize {

//! One column's sampled statistics, held in DuckDB's per-database object cache
//! so that lifetime, locking and eviction are its problem rather than ours.
class CachedColumnStats : public duckdb::ObjectCacheEntry {
public:
	explicit CachedColumnStats(ColumnStats stats) : stats(std::move(stats)) {
	}

	ColumnStats stats;

	static std::string ObjectType() {
		return "factorize_column_stats";
	}
	std::string GetObjectType() override {
		return ObjectType();
	}
	//! The MCV list dominates: a vector of value/frequency pairs, capped at
	//! MCV_ENTRIES. The rest is a handful of doubles.
	duckdb::optional_idx GetEstimatedCacheMemory() const override {
		return sizeof(CachedColumnStats) + stats.mcv.size() * sizeof(stats.mcv[0]);
	}
};

//! Identifies the sample: which column of which table, how many rows were
//! read, and how many rows the table was thought to have at the time.
//!
//! The cardinality is what makes this safe enough. It is also what makes the
//! key cheap -- the gate already asks for it, so no extra catalog work is done
//! to build this.
inline std::string StatsCacheKey(const std::string &qualified_table, duckdb::idx_t physical_column,
                                 duckdb::idx_t sample_limit, double cardinality) {
	return "factorize_stats:" + qualified_table + ":" + std::to_string(physical_column) + ":" +
	       std::to_string(sample_limit) + ":" + std::to_string(static_cast<int64_t>(cardinality));
}

} // namespace factorize
