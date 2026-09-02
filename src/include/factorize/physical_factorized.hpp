//===----------------------------------------------------------------------===//
//                         factorize
//
// factorize/physical_factorized.hpp
//
// Physical operator for the factorized region.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

//! Executes the factorized region.
//!
//! Phase 0: a pure source emitting one constant row, to verify the physical
//! plumbing. Phase 3 makes it a sink+source pipeline breaker (plan §3.2) --
//! Sink() feeds base-table vectors into the f-representation builder, Finalize()
//! runs the semiring traversal, GetData() emits the single scalar.
class PhysicalFactorized : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

public:
	PhysicalFactorized(PhysicalPlan &physical_plan, vector<LogicalType> types, int64_t stub_value,
	                   idx_t estimated_cardinality);

	//! Phase 0 stand-in for the aggregate result.
	int64_t stub_value;

public:
	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;

	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	bool IsSource() const override {
		return true;
	}
	//! Single scalar output: parallelising the source would buy nothing.
	bool ParallelSource() const override {
		return false;
	}

protected:
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
};

} // namespace duckdb
