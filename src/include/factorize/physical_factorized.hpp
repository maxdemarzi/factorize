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
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"

namespace duckdb {

//! Executes the factorized region.
//!
//! A source that scans the base tables itself, builds the f-representation, and
//! emits the one scalar the region computes. Its one child is not an input: it
//! is the stock plan this operator replaced, kept for §7.5's fallback and never
//! scheduled. Reading
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
	//! Set for `GROUP BY g1, ..., gn`: one row per group rather than one row.
	bool grouped = false;
	//! One per key, in the aggregate's order, which is the answer's order.
	vector<LogicalType> group_types;
	vector<factorize::GroupKey> group_keys;
	//! One per aggregate, in the query's order, folded side by side in a single
	//! walk, with the type each has to come back as.
	vector<factorize::GroupAggregate> aggregates;
	vector<LogicalType> aggregate_types;
	//! The stock plan for the region, built into a pipeline the executor is not
	//! given (plan §7.5). Nothing here runs unless the factorized path throws.
	//!
	//! The pattern is PhysicalRecursiveCTE's: a MetaPipeline constructed
	//! standalone rather than registered with the parent is never scheduled, and
	//! can be driven by hand later. That is what makes a fallback free when it
	//! is not needed -- a child wired in the ordinary way would be executed on
	//! every query, which is the cost the whole operator exists to avoid.
	shared_ptr<MetaPipeline> fallback_meta_pipeline;
	//! True when the whole answer is one count(*) over the whole join, which is
	//! the only shape with a fused, sliced, parallel path behind it.
	bool IsPlainCount() const {
		return !grouped && aggregates.size() == 1 && aggregates[0].kind == factorize::Aggregate::COUNT;
	}

public:
	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;

	//! Only this operator is a source. The fallback child is a plan of its own
	//! and must not be gathered into the surrounding pipeline as one, or it
	//! would run on every query.
	vector<const_reference<PhysicalOperator>> GetSources() const override;
	void BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) override;

	//! Sink, but only for the fallback pipeline: this operator is what the stock
	//! plan's rows are collected into when it has to be run. Never a sink in the
	//! surrounding plan, where BuildPipelines makes it a source.
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	bool IsSink() const override {
		return !children.empty();
	}
	bool ParallelSink() const override {
		return true;
	}

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
	//! Parallel only when there is no fallback to drive.
	//!
	//! Driving the fallback's pipeline means several tasks arrive in GetData and
	//! park on the source state's lock while the owner runs it -- and a parked
	//! worker is one the executor cannot use to run the very pipeline it is
	//! waiting for. Measured on a 27M-tuple star, 40 fallbacks in a row: 40/40
	//! at one thread and at two, hung after 24 at four. PhysicalRecursiveCTE
	//! never meets this because it is a serial source, so a second task for its
	//! pipeline cannot exist -- which is the third way that precedent does not
	//! transfer to an extension.
	//!
	//! The cost is real and is the reason `factorize_fallback` exists: a serial
	//! source gives up the slicing of DECISIONS D20, measured here at 7ms
	//! against 2-4ms at eight threads.
	bool ParallelSource() const override {
		return children.empty();
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

private:
	//! Runs the plan this operator replaced and hands back its rows.
	//! The factorized path proper, split out so its caller can catch it whole.
	SourceResultType Factorized(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input,
	                            class FactorizedGlobalSourceState &gstate) const;
	SourceResultType EmitFallback(ExecutionContext &context, DataChunk &chunk,
	                              class FactorizedGlobalSourceState &gstate) const;
	SourceResultType ScanFallback(DataChunk &chunk, class FactorizedGlobalSourceState &gstate) const;
	SourceResultType EmitGroups(ExecutionContext &context, DataChunk &chunk,
	                            class FactorizedGlobalSourceState &gstate) const;
};

} // namespace duckdb
