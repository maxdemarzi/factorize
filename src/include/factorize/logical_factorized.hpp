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

	//! Table index owning this operator's single output column.
	idx_t table_index;
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
