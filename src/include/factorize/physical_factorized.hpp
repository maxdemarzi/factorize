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
	//! One row comes out, but the work behind it divides.
	//!
	//! Each thread counts one bucket of a hash partition of the join key and the
	//! buckets are summed, which makes the answer independent of how many
	//! threads ran -- the partition is the same partition however it is dealt
	//! out. The paper parallelises inside the join instead, across concurrent
	//! bottom-inserts into a shared representation; this trades that efficiency
	//! for not having to make insertion thread-safe, and for an invariance that
	//! holds by construction rather than by locking discipline.
	bool ParallelSource() const override {
		return true;
	}
	//! One row has no order to preserve.
	//!
	//! Saying so is what makes the parallelism above reachable at all. Left at
	//! the default INSERTION_ORDER, DuckDB treats the plan as order-preserving,
	//! picks the single-threaded result collector, and then declines to
	//! parallelise the pipeline because its *sink* is serial -- ParallelSource()
	//! is never even consulted. The symptom is a source that looks parallel,
	//! passes every invariance test, and runs on one thread whatever `threads`
	//! is set to.
	OrderPreservationType SourceOrder() const override {
		return OrderPreservationType::NO_ORDER;
	}

protected:
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
};

} // namespace duckdb
