#include "factorize/logical_factorized.hpp"

#include "factorize/physical_factorized.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"

namespace duckdb {

LogicalFactorized::LogicalFactorized(idx_t table_index_p, vector<BoundRelation> relations_p,
                                     factorize::QueryGraph graph_p, factorize::Plan plan_p)
    : LogicalExtensionOperator(), table_index(table_index_p), relations(std::move(relations_p)),
      graph(std::move(graph_p)), plan(std::move(plan_p)) {
}

void LogicalFactorized::ResolveTypes() {
	// The sealed island emits exactly one scalar (plan §1.1). Widening this to
	// the semiring's other aggregates (§4.5) is a later step; it never becomes a
	// tuple stream.
	types = {LogicalType::BIGINT};
}

vector<ColumnBinding> LogicalFactorized::GetColumnBindings() {
	return {ColumnBinding(table_index, 0)};
}

void LogicalFactorized::ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) {
	// The factorized region consumes its children's bindings entirely and exposes
	// only its own aggregate column, so the resolver must not descend into them.
	bindings = GetColumnBindings();
}

vector<idx_t> LogicalFactorized::GetTableIndex() const {
	return {table_index};
}

string LogicalFactorized::GetExtensionName() const {
	return NAME;
}

InsertionOrderPreservingMap<string> LogicalFactorized::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Relations"] = DescribeRelations(relations);
	result["Predicates"] = DescribePredicates(relations, graph);
	result["Join Order"] = DescribeJoinOrder(relations, plan);
	return result;
}

void LogicalFactorized::Serialize(Serializer &serializer) const {
	// Plan §3.2: serialization may throw in v1. The consequence is that prepared
	// statements over a factorized plan cannot be serialized; documented, not fixed.
	throw NotImplementedException("LogicalFactorized cannot be serialized yet");
}

PhysicalOperator &LogicalFactorized::CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) {
	// A pure source: it has no children to plan, because it reads the tables in
	// `relations` itself rather than being fed by them.
	return planner.Make<PhysicalFactorized>(types, relations, graph, plan, estimated_cardinality);
}

} // namespace duckdb
