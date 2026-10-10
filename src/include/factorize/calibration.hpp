//===----------------------------------------------------------------------===//
//                         factorize
//
// src/include/factorize/calibration.hpp
//
// What the gate learns from the queries it has already answered.
//
// D73 ended the search for a plan-time feature that separates a loss from a
// rescue: four were tried and each cost four to five good fires per bad one
// stopped, because on every axis the gate can see, the losses sit among the
// rescues. What actually distinguishes them is not visible at plan time -- all
// six losses are watdiv, where DuckDB costs 1.6e-6 ms per result tuple against
// hetio's 2.8e-6 and yago's 5.0e-5 (D65), so an identical predicted tuple count
// means different things depending on the data.
//
// But one signal is free, and it is exactly the missing one. For a `count(*)`
// over a join the answer *is* the number of flat tuples, so every query this
// engine completes reports the true cardinality of its own join -- and the
// relations it ran over say which data that was. Comparing it against what the
// gate predicted gives a per-table correction that no amount of statistics
// gathered before execution could provide, because it is measured on the
// finished join rather than estimated from its inputs.
//
// Kept deliberately small. Per table, a running mean of log(actual/predicted)
// and a count of observations; a query's correction is the geometric mean over
// the tables it touches, and 1.0 where nothing has been seen. Geometric because
// these errors are multiplicative and span six orders of magnitude, so an
// arithmetic mean would be the largest observation and nothing else.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace factorize {

class Calibration {
public:
	static Calibration &Get() {
		static Calibration instance;
		return instance;
	}

	//! Record one finished query: what the gate predicted for the flat result
	//! against what it turned out to be. Ignores the degenerate cases rather
	//! than letting them define a table's correction.
	void Observe(const std::vector<std::string> &tables, double predicted, double actual) {
		if (!(predicted > 0) || !(actual > 0) || tables.empty()) {
			return;
		}
		const double ratio = std::log(actual / predicted);
		std::lock_guard<std::mutex> guard(lock);
		for (const auto &table : tables) {
			auto &entry = by_table[table];
			entry.log_sum += ratio;
			entry.count += 1;
		}
	}

	//! The factor to multiply a predicted flat result by, for a query over
	//! these tables. 1.0 when nothing is known, which is the shipped behaviour.
	//!
	//! A table is only consulted once it has `min_observations` behind it. One
	//! query is a sample of size one, and the first thing it would do is
	//! correct every later query by whatever that one got wrong.
	double CorrectionFor(const std::vector<std::string> &tables, double min_observations) const {
		double log_sum = 0;
		double seen = 0;
		std::lock_guard<std::mutex> guard(lock);
		for (const auto &table : tables) {
			const auto found = by_table.find(table);
			if (found == by_table.end() || found->second.count < min_observations) {
				continue;
			}
			log_sum += found->second.log_sum / found->second.count;
			seen += 1;
		}
		if (seen == 0) {
			return 1.0;
		}
		return std::exp(log_sum / seen);
	}

	//! For tests and for `factorize_calibration` to print.
	size_t Tables() const {
		std::lock_guard<std::mutex> guard(lock);
		return by_table.size();
	}
	void Clear() {
		std::lock_guard<std::mutex> guard(lock);
		by_table.clear();
	}

private:
	struct Entry {
		double log_sum = 0;
		double count = 0;
	};
	mutable std::mutex lock;
	std::unordered_map<std::string, Entry> by_table;
};

} // namespace factorize
