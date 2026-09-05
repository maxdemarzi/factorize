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
	//! Set for `GROUP BY g, count(*)`, which answers with two columns rather
	//! than one. DuckDB gives an aggregate two table indexes -- one for its
	//! groups, one for its aggregates -- and operators above reference both, so
	//! the replacement has to expose both.
	bool grouped = false;
	idx_t group_index = 0;
	LogicalType group_type = LogicalType::BIGINT;
	//! Which of the region's columns the grouping key is.
	size_t group_relation = 0;
	size_t group_column = 0;
	//! Which fold to run, and for sum, which column carries the values and what
	//! type the answer has to come back as. DuckDB widens sum to HUGEINT even
	//! over narrow integers, and the operators above were bound against that.
	factorize::Aggregate aggregate = factorize::Aggregate::COUNT;
	size_t sum_relation = 0;
	size_t sum_column = 0;
	LogicalType sum_type = LogicalType::HUGEINT;
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
