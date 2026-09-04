#include "factorize/physical_factorized.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

namespace duckdb {

PhysicalFactorized::PhysicalFactorized(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                       vector<BoundRelation> relations_p, factorize::QueryGraph graph_p,
                                       factorize::Plan plan_p, idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, TYPE, std::move(types), estimated_cardinality),
      relations(std::move(relations_p)), graph(std::move(graph_p)), plan(std::move(plan_p)) {
}

string PhysicalFactorized::GetName() const {
	return "FACTORIZED";
}

InsertionOrderPreservingMap<string> PhysicalFactorized::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Relations"] = DescribeRelations(relations);
	result["Predicates"] = DescribePredicates(relations, graph);
	result["Join Order"] = DescribeJoinOrder(relations, plan);
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class FactorizedGlobalSourceState : public GlobalSourceState {
public:
	FactorizedGlobalSourceState(idx_t slices_p, size_t memory_per_slice_p)
	    : slices(slices_p), memory_per_slice(memory_per_slice_p) {
	}

	idx_t MaxThreads() override {
		return slices;
	}

	//! Buckets of the join-key partition, one unit of work each.
	const idx_t slices;
	//! What one bucket may hold. The cap is per thread, so handing every thread
	//! the whole budget would let N threads use N times it.
	const size_t memory_per_slice;
	atomic<idx_t> next_slice {0};
	//! Guards the running total. Contended once per bucket, which is nothing
	//! beside the cost of counting one.
	mutex lock;
	int64_t total = 0;
	idx_t completed = 0;
	string error;
};

unique_ptr<GlobalSourceState> PhysicalFactorized::GetGlobalSourceState(ClientContext &context) const {
	// Bound to DuckDB's own memory_limit. Without this the engine allocates until
	// the kernel kills the process -- which is what happened on the CE corpus,
	// taking the whole session with it. Half the limit, not all of it: the
	// base-table columns being scanned are held outside the arena and are not
	// counted by it.
	const auto &config = DBConfig::GetConfig(context);
	const idx_t available = config.options.maximum_memory == DConstants::INVALID_INDEX
	                            ? static_cast<idx_t>(4) * 1024 * 1024 * 1024
	                            : config.options.maximum_memory;
	const idx_t budget = available / 2;

	// One bucket per thread this query is allowed. More buckets than that would
	// only add passes over the input; fewer would leave threads idle. A
	// single-threaded database gets exactly the old behaviour: one bucket
	// covering everything.
	idx_t slices = TaskScheduler::GetScheduler(context).NumberOfThreads();
	if (slices < 1) {
		slices = 1;
	}
	return make_uniq<FactorizedGlobalSourceState>(slices, static_cast<size_t>(budget / slices));
}

SourceResultType PhysicalFactorized::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                     OperatorSourceInput &input) const {
	auto &gstate = input.global_state.Cast<FactorizedGlobalSourceState>();
	const idx_t slice = gstate.next_slice.fetch_add(1);
	if (slice >= gstate.slices) {
		// Every bucket is claimed; this thread has nothing left to do.
		return SourceResultType::FINISHED;
	}

	auto &client = context.client;
	// Set per thread, because the cap is per thread. A worker that never sets it
	// would run uncapped, which is what thread-local storage trades away for
	// having no data race.
	factorize::SetGlobalMemoryLimit(gstate.memory_per_slice);

	StorageSource source(client, relations);
	// Bottom-insert is the mode that carries the benefit (FINDINGS F4:
	// bottom-inserts alone are worth 1.9x, top-inserts 0.98x). Choosing per join
	// is a later step; this takes the better default.
	//
	// Within memory, not merely up to it: a bucket that does not fit is
	// subdivided rather than abandoned. The rule replaced a plan DuckDB could
	// have run, so failing here would turn a slow query into no query at all.
	const auto result = factorize::ExecuteCountSliceWithinMemory(graph, plan, source, factorize::JoinMode::BOTTOM_INSERT,
	                                                            slice, gstate.slices);

	idx_t completed;
	{
		lock_guard<mutex> guard(gstate.lock);
		if (!result.ok) {
			if (gstate.error.empty()) {
				gstate.error = result.error;
			}
		} else if (gstate.error.empty()) {
			gstate.total = factorize::CheckedCardinalityAdd(gstate.total, result.count);
		}
		completed = ++gstate.completed;
	}

	// The thread that finishes the last bucket is the one holding the whole sum,
	// and the only one with anything to emit.
	if (completed == gstate.slices) {
		if (!gstate.error.empty()) {
			// The alternative -- returning some other number -- is the one thing
			// this operator must never do. A query that fails is recoverable by
			// setting factorize_mode='off'; a query that answers wrongly is not.
			throw InvalidInputException("factorize: %s", gstate.error);
		}
		chunk.SetCardinality(1);
		chunk.SetValue(0, 0, Value::BIGINT(gstate.total));
	}
	// Not FINISHED: that would tell DuckDB this thread is done with the operator
	// entirely, and with fewer threads scheduled than there are buckets the rest
	// would never be claimed -- the count would come out short, or never come
	// out at all. Asking to be called again is what makes a thread take a second
	// bucket.
	return SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
