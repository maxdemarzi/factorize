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

#include "duckdb/planner/operator/logical_extension_operator.hpp"

namespace duckdb {

//! Logical placeholder for the factorized region.
//!
//! Phase 0: carries no f-tree and ignores its children; it exists to prove the
//! OptimizerExtension -> LogicalExtensionOperator -> PhysicalOperator chain
//! composes against the pinned DuckDB version. Phase 3 gives it the f-tree, the
//! per-level layouts, and the base-table LOGICAL_GETs as real children.
class LogicalFactorized : public LogicalExtensionOperator {
public:
	static constexpr const char *NAME = "FACTORIZED";

public:
	explicit LogicalFactorized(idx_t table_index);

	//! Table index owning this operator's single output column.
	idx_t table_index;
	//! Phase 0 stand-in for the aggregate result.
	int64_t stub_value = 42;

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
