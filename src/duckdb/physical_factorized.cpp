#include "factorize/physical_factorized.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/config.hpp"

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
	//! The scalar is emitted exactly once, however many threads ask for it.
	atomic<bool> emitted {false};
};

unique_ptr<GlobalSourceState> PhysicalFactorized::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<FactorizedGlobalSourceState>();
}

SourceResultType PhysicalFactorized::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                     OperatorSourceInput &input) const {
	auto &gstate = input.global_state.Cast<FactorizedGlobalSourceState>();
	if (gstate.emitted.exchange(true)) {
		return SourceResultType::FINISHED;
	}

	auto &client = context.client;

	// Bound to DuckDB's own memory_limit. Without this the engine allocates until
	// the kernel kills the process -- which is what happened on the CE corpus,
	// taking the whole session with it. Half the limit, not all of it: the
	// base-table columns being scanned are held outside the arena and are not
	// counted by it.
	const auto &config = DBConfig::GetConfig(client);
	const idx_t available = config.options.maximum_memory == DConstants::INVALID_INDEX
	                            ? static_cast<idx_t>(4) * 1024 * 1024 * 1024
	                            : config.options.maximum_memory;
	factorize::SetGlobalMemoryLimit(static_cast<size_t>(available / 2));

	StorageSource source(client, relations);
	// Bottom-insert is the mode that carries the benefit (FINDINGS F4:
	// bottom-inserts alone are worth 1.9x, top-inserts 0.98x). Choosing per join
	// is a later step; this takes the better default.
	const auto result = factorize::ExecuteCount(graph, plan, source, factorize::JoinMode::BOTTOM_INSERT);
	if (!result.ok) {
		// The alternative -- returning some other number -- is the one thing this
		// operator must never do. A query that fails is recoverable by setting
		// factorize_mode='off'; a query that answers wrongly is not.
		throw InvalidInputException("factorize: %s", result.error);
	}

	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(result.count));
	return SourceResultType::FINISHED;
}

} // namespace duckdb
