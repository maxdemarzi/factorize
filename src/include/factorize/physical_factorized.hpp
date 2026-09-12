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
	//! Bytes the gate predicted, times a slack factor; 0 = unbounded.
	idx_t estimate_budget_bytes = 0;
	//! Least compression a materialized join must reach for the plan to carry
	//! on; 0 = no check. Measured after the join rather than predicted before it.
	double min_compression = 0;

	//! Milliseconds of slice time before `min_compression` may abandon (D51).
	double abandon_after_ms = 0;
	//! Tuples per millisecond the last materialized join must have delivered
	//! for the plan to carry on; 0 = no check (D52).
	double min_rate = 0;
	//! Split a skewed bucket on a different key rather than failing; off (D55).
	bool second_key = false;
	//! Print what each materialized join left behind, for factorize_explain.
	bool explain_steps = false;
	bool grouped = false;
	//! Set for `count(*)` over a `LIMIT k`, the shape EXISTS is planned into.
	//! The answer is min(k, |join|), reached by counting buckets of the join key
	//! until k tuples have been seen rather than by counting all of them.
	bool limited = false;
	idx_t limit = 0;
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
	//! Parallel, fallback or no fallback.
	//!
	//! This used to be `children.empty()` -- serial whenever the §7.5 fallback
	//! was carried, which is the default. The reason was real but was a property
	//! of how the fallback was driven rather than of driving one: the thread
	//! running its pipeline held the source state's lock, every other task
	//! parked on that lock, and a parked worker is one the executor cannot use
	//! to run the very pipeline it is waiting for. Measured then on a 27M-tuple
	//! star, 40 fallbacks in a row: 40/40 at one thread and at two, hung after
	//! 24 at four.
	//!
	//! The lock is no longer held across the work and the threads that arrive
	//! meanwhile run the pipeline's tasks instead of waiting for them, so the
	//! starvation has nothing left to starve. What it costs to be wrong about
	//! this is a hang, so it is measured at 1, 2, 4 and 8 threads rather than
	//! argued.
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

private:
	//! Runs the plan this operator replaced and hands back its rows.
	//! The factorized path proper, split out so its caller can catch it whole.
	SourceResultType Factorized(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input,
	                            class FactorizedGlobalSourceState &gstate) const;
	SourceResultType EmitFallback(ExecutionContext &context, DataChunk &chunk,
	                              class FactorizedGlobalSourceState &gstate) const;
	//! Runs the fallback's pipeline to completion. One thread only, and never
	//! with the source state's lock held.
	void DriveFallback(class FactorizedGlobalSourceState &gstate) const;
	//! What every other thread does meanwhile: run the executor's tasks, which
	//! are the pipeline being driven, rather than block on a lock and starve it.
	void HelpFallback(class FactorizedGlobalSourceState &gstate) const;
	SourceResultType ScanFallback(DataChunk &chunk, class FactorizedGlobalSourceState &gstate) const;
	SourceResultType EmitGroups(ExecutionContext &context, DataChunk &chunk,
	                            class FactorizedGlobalSourceState &gstate) const;
};

} // namespace duckdb
