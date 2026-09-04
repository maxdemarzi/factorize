//===----------------------------------------------------------------------===//
//                         factorize
//
// factorize/physical_factorized.hpp
//
// Physical operator for the factorized region.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "factorize/storage_source.hpp"

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

//! Executes the factorized region.
//!
//! A pure source with no children: it scans the base tables itself, builds the
//! f-representation, and emits the one scalar the region computes. Reading
//! storage directly rather than being fed by child pipelines is what lets this
//! share `factorized_count()`'s already-measured path end to end (DECISIONS
//! D18); the cost is that the matcher must refuse any scan the plan would have
//! restricted, since there is no child left to apply the restriction.
class PhysicalFactorized : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

public:
	PhysicalFactorized(PhysicalPlan &physical_plan, vector<LogicalType> types, vector<BoundRelation> relations,
	                   factorize::QueryGraph graph, factorize::Plan plan, idx_t estimated_cardinality);

	vector<BoundRelation> relations;
	factorize::QueryGraph graph;
	factorize::Plan plan;

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
