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
	//! The inputs, read once and shared. Built lazily under `lock` by whichever
	//! thread arrives first, because reading them is not free and there is no
	//! point doing it before knowing the operator will run.
	unique_ptr<SharedRelations> inputs;
	//! For a grouped query: the groups, and how many have been handed out. There
	//! can be more of them than fit one vector.
	unique_ptr<vector<std::pair<int64_t, int64_t>>> groups;
	idx_t handed_out = 0;
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
	if (grouped) {
		// Grouping runs on one thread. Partitioning by the join key would put
		// each group wholly inside one bucket *only* when the partitioned key is
		// the grouping key, and ChooseSliceColumns picks whichever key reaches
		// the most relations. Merging partial groups across buckets is the
		// correct general answer and is not written; one thread is.
		slices = 1;
	}
	if (aggregate == factorize::Aggregate::SUM) {
		// So does summing, and for a sharper reason: ExecuteSum takes the whole
		// source, not a bucket of it. Left parallel, every thread would sum the
		// entire join and the totals would be added together -- an answer N
		// times too large, on N threads, silently. Slicing the sum is the same
		// partition argument as for counting and is simply not written yet.
		slices = 1;
	}
	return make_uniq<FactorizedGlobalSourceState>(slices, static_cast<size_t>(budget / slices));
}

//! One row per group, computed once and handed out a vector at a time.
SourceResultType PhysicalFactorized::EmitGroups(ExecutionContext &context, DataChunk &chunk,
                                                FactorizedGlobalSourceState &gstate) const {
	auto &client = context.client;
	{
		lock_guard<mutex> guard(gstate.lock);
		if (!gstate.groups) {
			factorize::SetGlobalMemoryLimit(gstate.memory_per_slice);
			SharedRelations source(client, relations);
			auto result = aggregate == factorize::Aggregate::SUM
			                  ? factorize::ExecuteGroupSum(graph, plan, source, factorize::JoinMode::BOTTOM_INSERT,
			                                               group_relation, group_column, sum_relation, sum_column)
			                  : factorize::ExecuteGroupCount(graph, plan, source, factorize::JoinMode::BOTTOM_INSERT,
			                                                 group_relation, group_column);
			if (!result.ok) {
				throw InvalidInputException("factorize: %s", result.error);
			}
			gstate.groups = make_uniq<vector<std::pair<int64_t, int64_t>>>(std::move(result.groups));
		}
	}

	auto &groups = *gstate.groups;
	idx_t produced = 0;
	lock_guard<mutex> guard(gstate.lock);
	while (gstate.handed_out < groups.size() && produced < STANDARD_VECTOR_SIZE) {
		// The key goes back out in the type the aggregate gave it, not the int64
		// the engine counted in.
		chunk.SetValue(0, produced, Value::Numeric(group_type, groups[gstate.handed_out].first));
		chunk.SetValue(1, produced,
		               aggregate == factorize::Aggregate::SUM
		                   ? Value::HUGEINT(hugeint_t(groups[gstate.handed_out].second)).DefaultCastAs(sum_type)
		                   : Value::BIGINT(groups[gstate.handed_out].second));
		gstate.handed_out++;
		produced++;
	}
	chunk.SetCardinality(produced);
	return produced == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

SourceResultType PhysicalFactorized::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                     OperatorSourceInput &input) const {
	auto &gstate = input.global_state.Cast<FactorizedGlobalSourceState>();
	if (grouped) {
		return EmitGroups(context, chunk, gstate);
	}
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

	// Read the inputs once, however many threads want them. Every bucket has to
	// look at every row to find its own, so a private scan per thread would
	// re-read the whole input per thread -- which is exactly what made the first
	// parallel version no faster than the serial one.
	{
		lock_guard<mutex> guard(gstate.lock);
		if (!gstate.inputs) {
			gstate.inputs = make_uniq<SharedRelations>(client, relations);
		}
	}
	auto &source = *gstate.inputs;
	// Bottom-insert is the mode that carries the benefit (FINDINGS F4:
	// bottom-inserts alone are worth 1.9x, top-inserts 0.98x). Choosing per join
	// is a later step; this takes the better default.
	//
	// Within memory, not merely up to it: a bucket that does not fit is
	// subdivided rather than abandoned. The rule replaced a plan DuckDB could
	// have run, so failing here would turn a slow query into no query at all.
	// Summing partitions the same way counting does -- the buckets are disjoint
	// sets of tuples, so their sums add -- but it cannot use the sliced count
	// path, which fuses the last join and never builds the values.
	const auto result =
	    aggregate == factorize::Aggregate::SUM
	        ? factorize::ExecuteSum(graph, plan, source, factorize::JoinMode::BOTTOM_INSERT, sum_relation, sum_column)
	        : factorize::ExecuteCountSliceWithinMemory(graph, plan, source, factorize::JoinMode::BOTTOM_INSERT, slice,
	                                                  gstate.slices);

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
		chunk.SetValue(0, 0, aggregate == factorize::Aggregate::SUM
		                         ? Value::HUGEINT(hugeint_t(gstate.total)).DefaultCastAs(sum_type)
		                         : Value::BIGINT(gstate.total));
	}
	// Not FINISHED: that would tell DuckDB this thread is done with the operator
	// entirely, and with fewer threads scheduled than there are buckets the rest
	// would never be claimed -- the count would come out short, or never come
	// out at all. Asking to be called again is what makes a thread take a second
	// bucket.
	return SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
