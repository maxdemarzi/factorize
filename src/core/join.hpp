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
	//! `with_weights` reserves room for per-record multiplicities. Off by
	//! default: the field costs 16 bytes a record after alignment, so it is
	//! reserved only where something can actually set it.
	FactorizedRelation(FTree tree, AttributeTypes types, bool with_weights = false);

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

//! What the join emits, as opposed to which side survives it.
//!
//! `Preserve` answers "which input keeps its tuples"; this answers "what is
//! done with the partner". They are separate axes because SEMI and ANTI are not
//! products at all: their output is one input's tuples, filtered by whether a
//! partner exists, so no tuple of the other side ever appears. Folding them
//! into `Preserve` would have made `Preserve::BUILD` mean two different things.
enum class JoinKind : uint8_t {
	//! The product: an inner join, or an outer one if `preserve` names a side.
	//! `preserve` alone already says which, so there is no separate OUTER --
	//! two fields that must agree are two fields that can disagree.
	PRODUCT,
	//! The tuples of the `preserve` side that have at least one partner.
	//! `preserve` must name exactly one side.
	SEMI,
	//! The tuples of the `preserve` side that have no partner. NOT the
	//! complement of SEMI over the product -- it is the complement over that
	//! side's own tuples, which is why it counts the same units SEMI does.
	ANTI
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
//! `kind` extends that to semi- and anti-joins (plan section 10.4 and the
//! §10.5 staging), which cost less than counting rather than more: the marker
//! an outer join already maintains is the whole mechanism, and the only
//! difference is what the matched weight becomes at the insertion level.
//!
//!     inner   size *= matched                        (PRODUCT, preserve NEITHER)
//!     outer   size *= (matched == 0 ? 1 : matched)     (PRODUCT, preserve a side)
//!     semi    size  = (matched >  0 ? size : 0)
//!     anti    size  = (matched == 0 ? size : 0)
//!
//! For SEMI and ANTI, `preserve` names the side whose tuples are emitted, and
//! must be BUILD or PROBE -- BOTH and NEITHER have no meaning there and throw.
int64_t FactorizedCountJoin(const FactorizedRelation &build, const FactorizedRelation &probe, const JoinKeys &keys,
                            JoinMode mode, PathStrategy strategy = PathStrategy::LEVELWISE, JoinStats *stats = nullptr,
                            Preserve preserve = Preserve::NEITHER, JoinKind kind = JoinKind::PRODUCT);

//! Memory cap applied to every f-representation the core creates; 0 = none.
//!
//! Per thread, despite the name, and set by whoever is about to run the engine
//! on that thread. Exceeding it throws MemoryLimitExceeded, which
//! ExecuteCountWithinMemory answers by re-counting over a partition of the join
//! key rather than by giving up.
void SetGlobalMemoryLimit(size_t bytes);
size_t GetGlobalMemoryLimit();

//! Budget from the gate's own size estimate; 0 = none. Per thread, like the
//! memory cap. Exceeding it throws a plain exception rather than
//! MemoryLimitExceeded, so it is answered by abandoning rather than by slicing.
void SetGlobalEstimateBudget(size_t bytes);
size_t GetGlobalEstimateBudget();

//! Least compression a materialized join must have achieved for the plan to
//! carry on; 0 = no check. Per thread, like the two caps above.
//!
//! Different in kind from both of them, and the difference is the point. The
//! memory cap asks "does this fit", the estimate budget asks "was the gate's
//! prediction right"; this asks "is factorizing this query working", and
//! answers it from what the representation actually holds rather than from any
//! number decided before the query ran. Falling below it throws a plain
//! exception, so it abandons to the replaced plan (section 7.5) exactly as the
//! estimate budget does -- the two share a mechanism and differ only in what
//! they consult (D37).
void SetGlobalMinCompression(double tuples_per_record);
double GetGlobalMinCompression();

//! Sets all three at once, because they are thread-local and a caller that
//! sets one and inherits another gets a limit no query asked for.
//!
//! That is not hypothetical: the table functions set the memory cap and left
//! the estimate budget as an earlier query on the same thread had it, so
//! `factorized_count` could abandon against a prediction made for a different
//! query entirely. Taking all three together is what makes the omission
//! impossible to write.
//! Milliseconds of slice time before the compression floor may abandon.
//!
//! The floor alone was measured and shelved: at 0.6 it abandoned 17 of 248
//! out-of-sample queries (D37), because a query that compresses badly and
//! finishes in 20ms costs nothing to finish. Time is what separates those from
//! the ones worth stopping, and it is measured rather than predicted -- which
//! is the property five rejected estimator fixes were missing (D50).
void SetGlobalAbandonAfter(double milliseconds);
double GetGlobalAbandonAfter();

//! Milliseconds since this slice's limits were set.
double ElapsedSliceMs();

//! Tuples per millisecond this slice must be delivering by its last
//! materialized join for the plan to carry on; 0 = no check.
//!
//! The compression floor asks whether the representation is small. This asks
//! the question that actually decides the race: how fast tuples are being
//! delivered, in the same unit DuckDB's own cost model is stated in, so the two
//! can be compared at all. Measured across six wins and four losses (D52), the
//! cumulative rate at the last materialized join was 1.11e5 to 6.28e7 tuples/ms
//! for the wins and 17 to 4.59e4 for the losses -- separated, with DuckDB's own
//! fitted rate of 2.51e5 sitting between them.
//!
//! Only at the last materialized join, and that restriction is the whole of
//! what makes it usable: at the second join the same measurement has a win at
//! 0.05 and a loss at 207, so a check there abandons the queries it exists to
//! protect.
void SetGlobalMinRate(double tuples_per_ms);
double GetGlobalMinRate();

//! Whether a bucket that no modulus of its own key can split may be split on a
//! *different* key; false = fail instead, which is what happened before this
//! existed.
//!
//! Off, and measured rather than assumed. It converts a query that cannot be
//! answered at all into one that can (D55), and on the corpus it also converts
//! two queries that used to fail in 20s and be answered by the stock plan in
//! another few into queries this engine answers itself in 270 and 280 seconds.
//! Recovering a query the stock plan could have had in 13s is not a win, and
//! telling those two cases apart is the problem D34 through D56 have not
//! solved. So the mechanism ships and the switch stays off.
void SetGlobalSecondKey(bool enabled);
bool GetGlobalSecondKey();

void SetGlobalLimits(size_t memory_bytes, size_t estimate_budget_bytes, double min_compression,
                     double abandon_after_ms = 0, double min_rate = 0, bool second_key = false);

//! Builds a flat, single-node relation from columnar input. This is the
//! trivial f-representation of section 4.2.1.
FactorizedRelation MakeScan(const std::vector<AttributeId> &attributes, const AttributeTypes &types,
                            const std::vector<std::vector<int64_t>> &columns);

} // namespace factorize
