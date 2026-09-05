//===----------------------------------------------------------------------===//
//                         factorize
//
// core/join.hpp
//
// Factorized equi-join (paper section 4.4), in both insert modes.
//
// Both modes build the same output f-tree and differ only in which input
// becomes its upper part -- the symmetry the paper proves in section 4.2.3.
// What differs is the mechanics:
//
//   TOP_INSERT     the probe tree is the upper part. The build side is indexed,
//                  and each match is materialized *below* the probe record, as
//                  in Figure 9.
//
//   BOTTOM_INSERT  the build tree is the upper part, so it is materialized
//                  during the build phase and the hash table stores a pointer
//                  to the insert location; probes then fill in the lower parts
//                  (Figure 11). Because one upper record collects matches from
//                  many probes, this is a full pipeline breaker and its
//                  insertion point is the only place needing a lock (5.2).
//
// FINDINGS.md F4: bottom-inserts are worth ~1.9x on their own while top-inserts
// alone are worth 0.98x, so this file is where the project's value lives.
//
// No DuckDB headers (plan section 4).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frep.hpp"
#include "ftree.hpp"
#include "hashtable.hpp"
#include "layout.hpp"
#include "materialize.hpp"

#include <memory>
#include <string>
#include <vector>

namespace factorize {

using AttributeTypes = std::vector<std::pair<AttributeId, ValueType>>;

//! An intermediate result: a shape, its record layout, and the records.
//!
//! Layout and FRepresentation are held behind unique_ptr because the
//! representation stores a pointer to its layout; moving the relation must not
//! move either.
class FactorizedRelation {
public:
	FactorizedRelation(FTree tree, AttributeTypes types);

	const FTree &Tree() const {
		return tree;
	}
	const Layout &GetLayout() const {
		return *layout;
	}
	FRepresentation &Rep() {
		return *frep;
	}
	const FRepresentation &Rep() const {
		return *frep;
	}
	const AttributeTypes &Types() const {
		return types;
	}

	int64_t Count() const {
		return frep->Count();
	}

private:
	FTree tree;
	AttributeTypes types;
	std::unique_ptr<Layout> layout;
	std::unique_ptr<FRepresentation> frep;
};

//! Which input of an outer join keeps its tuples when nothing matches.
//!
//! Named for the two arguments rather than for SQL's LEFT and RIGHT, because
//! the core has no idea which side the user wrote first and every mapping from
//! one to the other is a chance to swap them. `A LEFT JOIN B` preserves
//! whichever of `build`/`probe` A was passed as; the caller owns that mapping.
//!
//! Not the same axis as JoinMode. Mode chooses which side is indexed, which is
//! a performance question and leaves the answer unchanged; this changes the
//! answer.
enum class Preserve : uint8_t {
	//! An inner join: an unmatched tuple contributes nothing.
	NEITHER,
	BUILD,
	PROBE,
	//! FULL OUTER.
	BOTH
};

//! Statistics for a single join, for the Phase 1 harness and later EXPLAIN.
struct JoinStats {
	size_t build_keys = 0;
	size_t probe_rows = 0;
	size_t matches = 0;
	size_t output_records = 0;
	//! Records still encoding at least one tuple after empty-subtree pruning.
	size_t live_records = 0;
	size_t output_bytes = 0;
	//! True when section 4.3 had to merge nodes, i.e. partial flattening was
	//! genuinely exercised rather than being a 1:1 copy.
	bool merged_nodes = false;
};

//! Joins two factorized relations on `keys`.
//!
//! Handles the general case: when the root-to-leaf transformation merges nodes,
//! the affected levels are partially flattened (section 4.6) rather than
//! rejected.
FactorizedRelation FactorizedJoin(const FactorizedRelation &build, const FactorizedRelation &probe,
                                  const JoinKeys &keys, JoinMode mode, PathStrategy strategy = PathStrategy::LEVELWISE,
                                  JoinStats *stats = nullptr);

//! Performs a join and returns the cardinality of its result *without ever
//! building it*.
//!
//! Section 4.2.2 notes the aggregate is usually the topmost operator, and
//! section 4.5 that counting is a traversal over a semiring rather than an
//! enumeration. Together those mean the final join never needs to materialize:
//! its output is consumed by the count and discarded.
//!
//! This is not a micro-optimization. Profiling stock DuckDB on
//! `hetio_acyclic_219_06` (620,423,586 result tuples) puts 36.9s of its 38.4s in
//! the single topmost hash join, which exists only to feed `count(*)`; every
//! other operator costs under 0.2s combined. Materializing a result in order to
//! count it is the dominant cost on exactly the queries this engine is for.
//!
//! The count is a property of the relation, so both insert modes yield the same
//! answer; `mode` still selects which side is indexed and which is scanned.
//!
//! `preserve` makes it an outer join (plan section 10.5, paper section 4.8).
//! The paper's sketch is about representing null-extension, and a count never
//! looks at a value -- so here the whole of it is arithmetic: an unmatched
//! tuple has to contribute 1 rather than 0. Nothing about the record format
//! changes, and no NULL is ever stored.
//!
//! Only this function takes it. FactorizedJoin, which builds a representation
//! rather than a number, would need a record for the null-extended row and a
//! way to say its columns are absent, and it would need PruneEmptySubtrees to
//! stop dropping exactly those records. So an outer join is supported where it
//! is the *last* join of a plan -- the one that is fused into the count -- and
//! declined anywhere else.
int64_t FactorizedCountJoin(const FactorizedRelation &build, const FactorizedRelation &probe, const JoinKeys &keys,
                            JoinMode mode, PathStrategy strategy = PathStrategy::LEVELWISE, JoinStats *stats = nullptr,
                            Preserve preserve = Preserve::NEITHER);

//! Memory cap applied to every f-representation the core creates; 0 = none.
//!
//! Per thread, despite the name, and set by whoever is about to run the engine
//! on that thread. Exceeding it throws MemoryLimitExceeded, which
//! ExecuteCountWithinMemory answers by re-counting over a partition of the join
//! key rather than by giving up.
void SetGlobalMemoryLimit(size_t bytes);
size_t GetGlobalMemoryLimit();

//! Builds a flat, single-node relation from columnar input. This is the
//! trivial f-representation of section 4.2.1.
FactorizedRelation MakeScan(const std::vector<AttributeId> &attributes, const AttributeTypes &types,
                            const std::vector<std::vector<int64_t>> &columns);

} // namespace factorize
