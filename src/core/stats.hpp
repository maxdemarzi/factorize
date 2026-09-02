//===----------------------------------------------------------------------===//
//                         factorize
//
// core/stats.hpp
//
// The statistic the gate was missing.
//
// FINDINGS F13: the gate declined all 48 sampled epinions queries, on a
// dataset whose top queries compress 300-2000x, because the textbook estimator
// under-predicted the join size by 1400x. F14/F16 trace that to the uniformity
// assumption in |R join S| = |R||S| / max(V_R, V_S). On graph-shaped data
// uniformity is not an approximation, it is a category error: for
// epinions_acyclic_202_00, five key values carry 90% of a 1.92e8-tuple join.
// Distinct counts average exactly that away.
//
// A most-common-value list restores it. Frequencies are exact for the values
// that matter and uniform only for the tail, which is the part uniformity
// actually describes.
//
// No DuckDB headers (plan section 4).
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace factorize {

//! One join column's distribution, in the form an optimizer can store it:
//! row and distinct counts, plus the exact frequency of the most common values.
//!
//! `mcv` is the only field a conventional catalog does not already carry.
//! Phase 3 has to supply it -- DuckDB's catalog has approximate distinct counts
//! but no MCV list -- either by sampling max-frequency per join column or by
//! adding the statistic. Sampling suffices: heavy-tailed distributions put
//! their head in any reasonable sample, and it is the head that decides.
struct ColumnStats {
	//! Rows in the relation, after any pushed-down filters.
	double rows = 0;
	//! Distinct values of this column.
	double distinct = 1;
	//! (value, frequency) for the most common values, descending by frequency.
	//! Values must be unique. An empty list degrades this to the textbook
	//! estimator exactly, which is the intended fallback when no MCV list is
	//! available.
	std::vector<std::pair<int64_t, double>> mcv;

	//! Rows not covered by the MCV list.
	double TailRows() const;
	//! Distinct values not covered by the MCV list. At least 1, so it is always
	//! safe to divide by.
	double TailDistinct() const;
	//! Frequency of `value`: exact when it is in the MCV list, the tail average
	//! otherwise. The tail average is all a real optimizer knows about a value
	//! it did not store, and pretending otherwise is what made the oracle
	//! version of this estimator look better than it is (F14 corrected).
	double Frequency(int64_t value) const;
};

//! Both sizes of one join equivalence class, in one pass.
//!
//! Every relation in a class joins on the same values, so a hub value appears
//! in all of them at once and its contribution multiplies. That is precisely
//! the compounding a per-edge scalar estimate cannot represent, and it is why
//! this is computed per class rather than per edge.
//!
//! For a star over key k with children R1..Rn, section 2.5's size rule gives
//! the two quantities directly:
//!
//!     flat    = sum over v of   product over i of freq_i(v)
//!     records = sum over v of   sum     over i of freq_i(v)
//!
//! -- the f-representation stores each relation's matches for v side by side,
//! while the flat result takes their cross product. Their ratio is the
//! compression, which F10 showed is what actually predicts speedup.
struct GroupSize {
	//! Tuples in the flat join of this class's relations.
	double flat = 0;
	//! Records the f-representation holds for them.
	double records = 0;
	//! Distinct key values surviving the join, for combining classes.
	double distinct = 1;
	//! Records held by each column's node, in the order the columns were given.
	//! Needed because a class hanging below another attaches beneath one
	//! relation's node, not beneath the class as a whole -- and using the whole
	//! class over-counts the contexts by the number of its siblings.
	std::vector<double> column_records;
};

//! Estimates one equivalence class exactly over the union of the stored MCVs
//! and uniformly over the tail.
GroupSize EstimateGroup(const std::vector<ColumnStats> &group);

} // namespace factorize
