//===----------------------------------------------------------------------===//
//                         factorize
//
// factorize/logical_factorized.hpp
//
// Logical operator representing a sealed factorized region: flat base-table
// scans in, one scalar aggregate out (plan §1.1).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "factorize/storage_source.hpp"

#include "duckdb/planner/operator/logical_extension_operator.hpp"

namespace duckdb {

//! Logical placeholder for the factorized region.
//!
//! It has no children. The matcher's subtree is dropped rather than carried,
//! because the operator reads the base tables itself through the same storage
//! path `factorized_count()` uses (DECISIONS D18) -- which is why MatchLeaf
//! refuses any scan that returns less than the whole table.
class LogicalFactorized : public LogicalExtensionOperator {
public:
	static constexpr const char *NAME = "FACTORIZED";

public:
	LogicalFactorized(idx_t table_index, vector<BoundRelation> relations, factorize::QueryGraph graph,
	                  factorize::Plan plan);

	//! Table index owning this operator's aggregate column.
	idx_t table_index;
	//! Set for `GROUP BY g1, ..., gn, count(*)`, which answers with one column
	//! per key beside the aggregate rather than a single scalar. DuckDB gives an
	//! aggregate two table indexes -- one for its groups, one for its aggregates
	//! -- and operators above reference both, so the replacement has to expose
	//! both.
	bool grouped = false;
	idx_t group_index = 0;
	//! One per key, in the aggregate's own order, which is the order the answer
	//! columns come back in. Kept as the types the aggregate bound rather than
	//! the int64 the engine works in.
	vector<LogicalType> group_types;
	//! Which of the region's columns each grouping key is, same order.
	vector<factorize::GroupKey> group_keys;
	//! One per aggregate, in the query's own order, which is the order the
	//! answer's columns come back in. They are folded side by side in a single
	//! walk of the representation rather than one walk each.
	vector<factorize::GroupAggregate> aggregates;
	//! What each has to come back as. count(*) is BIGINT; DuckDB widens sum to
	//! HUGEINT even over narrow integers, and operators above were bound to it.
	vector<LogicalType> aggregate_types;
	//! The tables to scan, and the columns of each that a predicate touches.
	vector<BoundRelation> relations;
	//! The join graph, indexed by position in `relations`.
	factorize::QueryGraph graph;
	//! The join order, decided when the region was matched. Ordering at match
	//! time is what lets an unorderable graph be a decline instead of a query
	//! that fails partway through executing.
	factorize::Plan plan;

public:
	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override;

	string GetExtensionName() const override;
	vector<ColumnBinding> GetColumnBindings() override;
	void ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;

	vector<idx_t> GetTableIndex() const override;

	//! Plan §3.2: v1 does not serialize the factorized region. Reporting this
	//! honestly makes DuckDB skip serialization instead of failing inside it.
	bool SupportSerialization() const override {
		return false;
	}
	void Serialize(Serializer &serializer) const override;

protected:
	void ResolveTypes() override;
};

} // namespace duckdb
