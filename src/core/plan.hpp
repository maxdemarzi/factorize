//===----------------------------------------------------------------------===//
//                         factorize
//
// core/plan.hpp
//
// Everything between a join graph and a count: join ordering, equality
// propagation, and the execution loop that drives join.hpp.
//
// This lived in the benchmark harness while there was only one caller. Phase 2
// adds a second -- a DuckDB table function over real catalog tables -- and the
// planning is the part that must not diverge between them. Equality propagation
// in particular is not an optimization but a correctness-of-shape requirement:
// attaching a relation beneath whichever relation its predicate happens to name,
// rather than beneath the shallowest equivalent attribute, turns a star into a
// chain and nothing in a chain is independent (FINDINGS F6). A second
// implementation of that rule is a second chance to get it wrong.
//
// Callers differ only in where the data comes from, which is what
// RelationSource abstracts. No DuckDB headers (plan section 4).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "cost.hpp"
#include "enumerate.hpp"
#include "ftree.hpp"
#include "join.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace factorize {

//! An equality between two relations' columns.
struct Predicate {
	size_t left_relation = 0;
	int left_column = 0;
	size_t right_relation = 0;
	int right_column = 0;
};

//! A join graph: relations, and the equalities connecting them.
//!
//! Relations are positions, not names. Several may refer to the same physical
//! table -- a self-join is two relations over one table -- so the caller owns
//! the mapping and this layer never resolves names.
struct QueryGraph {
	//! Columns each relation exposes. Join columns are indices into this.
	std::vector<size_t> column_counts;
	//! The storage width of each column: column_types[relation][column].
	//! Required to match column_counts exactly -- ExecuteCount throws rather
	//! than default a missing entry. There used to be a default (every column
	//! was silently ValueType::INT32), and it silently truncated BIGINT/UBIGINT
	//! join keys via a narrowing static_cast: two values differing only above
	//! bit 31 became indistinguishable, with no error. A caller whose columns
	//! are genuinely always 32-bit (the CE benchmark harness) still has to say
	//! so explicitly, which is one line per relation and costs nothing next to
	//! what the implicit version cost.
	std::vector<std::vector<ValueType>> column_types;
	std::vector<Predicate> predicates;
	//! Set when the graph has a cycle. Cyclic queries are refused by the gate;
	//! the paper reports them 32% slower.
	bool cyclic = false;

	size_t RelationCount() const {
		return column_counts.size();
	}
};

//! The attribute id for one relation's column. Attribute ids are global across
//! the query, so a relation's columns are contiguous from its base.
AttributeId AttributeOf(const QueryGraph &graph, size_t relation, size_t column);

//! Union-find over attributes, closing equality under transitivity.
class EquivalenceClasses {
public:
	explicit EquivalenceClasses(const QueryGraph &graph);

	size_t Find(size_t attribute);
	bool SameClass(AttributeId a, AttributeId b);
	size_t Size() const {
		return parent.size();
	}

private:
	std::vector<size_t> parent;
};

//! Rewrites a probe-side key onto the shallowest equivalent attribute already
//! present in the accumulated f-tree.
AttributeId ShallowestEquivalent(EquivalenceClasses &classes, const FTree &tree, AttributeId key);

struct PlanStep {
	size_t relation = 0;
	//! Predicates connecting the new relation to everything joined so far.
	std::vector<Predicate> edges;
};

//! A left-deep join order, plus why one could not be found.
struct Plan {
	std::vector<PlanStep> steps;
	bool complete = false;
	std::string reason;
};

//! Orders the joins greedily, taking the relation with the most edges into the
//! already-joined set at each step. A relation that cannot be connected means a
//! disconnected graph, which this refuses rather than turning into a product.
Plan BuildPlan(const QueryGraph &graph);

//! Supplies relation data on demand.
//!
//! Materializing every relation up front would defeat the point on the queries
//! this engine is for, where the inputs are small and only the *result* is
//! enormous -- but it also lets a caller stream from wherever it likes. The
//! harness reads CSVs; the DuckDB operator scans base tables.
class RelationSource {
public:
	virtual ~RelationSource() = default;
	//! Columns of `relation`, in declaration order. The reference must stay
	//! valid until the next call for a different relation.
	virtual const std::vector<std::vector<int64_t>> &Columns(size_t relation) = 0;
	//! Statistics for one column, for the gate.
	virtual ColumnStats Stats(size_t relation, size_t column) = 0;
};

//! Describes `plan` to the gate.
std::vector<CostStep> BuildCostSteps(const QueryGraph &graph, const Plan &plan, RelationSource &source);

struct ExecuteResult {
	bool ok = false;
	int64_t count = -1;
	std::string error;
	//! Set when the failure was the memory cap rather than anything about the
	//! query. Only this one is worth retrying, and ExecuteCountWithinMemory does.
	bool out_of_memory = false;
	//! How many slices the answer was assembled from; 1 when it fitted whole.
	size_t slices = 1;
	//! Records and bytes in the last materialized f-representation. Zero when
	//! the plan was a single fused count join, which never materializes.
	size_t records = 0;
	size_t bytes = 0;
};

//! Runs `plan`, returning the count without materializing the final join.
//!
//! The last join is fused (section 4.2.2, 4.5): its output exists only to be
//! counted, and building it is where stock DuckDB spends 96% of its time on
//! these shapes.
ExecuteResult ExecuteCount(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                           PathStrategy strategy = PathStrategy::LEVELWISE);

//! Runs `plan` `slices` times, each pass keeping only the rows whose join key
//! falls in one hash bucket, and sums the counts.
//!
//! Exact, not approximate. Every output tuple assigns one value to the chosen
//! attribute -- that is what an equivalence class means -- so bucketing on that
//! value partitions the output, and the parts sum to the whole. Every relation
//! holding an attribute of the class is filtered by the same bucket, which is
//! what makes each pass small: the f-representation shrinks with its inputs.
//!
//! Costs one pass over the inputs per slice. That is the trade being made --
//! the alternative for a query whose representation does not fit is not a
//! slower answer but no answer.
ExecuteResult ExecuteCountSliced(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                 size_t slices, PathStrategy strategy = PathStrategy::LEVELWISE);

struct MaterializeResult {
	bool ok = false;
	std::string error;
	bool out_of_memory = false;
	//! One entry per tuple, each holding one value per attribute in the order
	//! the graph declares them: relation 0's columns, then relation 1's, and so
	//! on.
	std::vector<std::vector<int64_t>> tuples;
	//! Records and bytes the representation the tuples came out of occupied.
	size_t records = 0;
	size_t bytes = 0;
};

//! Runs `plan` and hands back the flat tuples, at most `limit` of them (0 for
//! all).
//!
//! The honest accounting (plan section 10.3): emitting N tuples costs Omega(N)
//! and factorization does not change that. What it changes is the
//! *intermediate* cost -- a flat engine duplicates at each of the n-1 joins,
//! this pays once, at the end -- and, when `limit` is small, what gets built at
//! all. A hundred rows out of a join with a trillion of them costs a hundred
//! rows of enumeration over a representation that was cheap to build, which is
//! a thing stock DuckDB cannot do at any speed: it materialises the hash-join
//! intermediates whatever the LIMIT says.
MaterializeResult ExecuteMaterialize(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                     size_t limit, PathStrategy strategy = PathStrategy::LEVELWISE);

struct GroupCountResult {
	bool ok = false;
	std::string error;
	bool out_of_memory = false;
	//! One entry per distinct value of the grouping column, value first.
	std::vector<std::pair<int64_t, int64_t>> groups;
	size_t records = 0;
	size_t bytes = 0;
};

//! `SELECT g, count(*) ... GROUP BY g`, without materializing the join.
//!
//! This is the case plan section 10.1 describes as workable: the grouping key
//! sits at the top of the f-tree, so each root record *is* a group, and the
//! tuples belonging to it are exactly the ones its subtree denotes -- a number
//! the representation already knows how to compute without enumerating them.
//! Summing memoized subtree sizes per distinct value is the whole algorithm.
//!
//! When the key is not at the root this declines rather than guessing. A key
//! further down is reachable in principle -- descend to it, carrying the
//! product of the sibling slots you did not descend into -- but a key scattered
//! across independent sibling branches needs their cross product and is a
//! different algorithm. Declining the first case costs coverage; guessing at
//! the second would cost correctness.
GroupCountResult ExecuteGroupCount(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                   size_t group_relation, size_t group_column,
                                   PathStrategy strategy = PathStrategy::LEVELWISE);

//! Runs `plan` for tuples, and if the representation does not fit, gathers them
//! a bucket of the join key at a time instead.
//!
//! The union of the buckets' tuples is the join's tuples, so this is the same
//! result assembled differently. With a limit it is better than that: one
//! bucket usually supplies the whole prefix, and the rest are never built.
MaterializeResult ExecuteMaterializeWithinMemory(const QueryGraph &graph, const Plan &plan, RelationSource &source,
                                                 JoinMode mode, size_t limit,
                                                 PathStrategy strategy = PathStrategy::LEVELWISE);

//! Whether the join has any tuple at all, stopping at the first witness.
//!
//! Counting asks a harder question than EXISTS does, and the difference is
//! worth taking: the join is non-empty exactly when *some* bucket of the join
//! key is non-empty, so the buckets are examined one at a time and the first
//! that yields anything ends the query. On data where witnesses are common --
//! which is most data -- that reads a fraction of the input, and on data where
//! they are not it costs one extra pass over inputs that were about to be read
//! anyway.
//!
//! `count` is 0 or 1, so a caller that wants a boolean has one.
ExecuteResult ExecuteExists(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                            PathStrategy strategy = PathStrategy::LEVELWISE);

//! Counts one bucket of the partition: the tuples whose join key hashes to
//! `slice` of `slices`. Summing every bucket gives the whole count, and the
//! buckets are independent, which is what lets them run on separate threads.
ExecuteResult ExecuteCountSlice(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                size_t slice, size_t slices, PathStrategy strategy = PathStrategy::LEVELWISE);

//! One bucket, subdivided further if it does not fit.
//!
//! The subdivision refines the same partition rather than replacing it: the
//! buckets of a finer modulus that fall inside bucket `slice` are exactly those
//! congruent to it, so a bucket can be cut up without the pieces overlapping
//! any other thread's work.
ExecuteResult ExecuteCountSliceWithinMemory(const QueryGraph &graph, const Plan &plan, RelationSource &source,
                                            JoinMode mode, size_t slice, size_t slices,
                                            PathStrategy strategy = PathStrategy::LEVELWISE);

//! Runs `plan` whole, and if the representation does not fit, runs it again in
//! more and more slices until it does.
//!
//! This is what makes the memory cap a cost rather than a failure, which is
//! what an optimizer rule needs: a rule that turns a query DuckDB could answer
//! into one that errors is worse than a rule that never fires.
ExecuteResult ExecuteCountWithinMemory(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                       PathStrategy strategy = PathStrategy::LEVELWISE);

} // namespace factorize
