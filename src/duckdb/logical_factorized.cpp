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
	// The sealed island emits one scalar, or -- when the query groups -- one
	// group key beside it. It never becomes a tuple stream.
	//
	// The group key keeps the type the aggregate gave it rather than the int64
	// the engine works in: operators above were bound against that type, and
	// handing them a wider one is a plan that type-checks and then misbehaves.
	types = group_types;
	for (const auto &type : aggregate_types) {
		types.push_back(type);
	}
}

vector<ColumnBinding> LogicalFactorized::GetColumnBindings() {
	// DuckDB gives an aggregate two table indexes, and the operators above
	// reference both. Groups come first, matching LogicalAggregate's own order.
	vector<ColumnBinding> bindings;
	for (idx_t i = 0; i < group_types.size(); i++) {
		bindings.emplace_back(group_index, i);
	}
	for (idx_t i = 0; i < aggregate_types.size(); i++) {
		bindings.emplace_back(table_index, i);
	}
	return bindings;
}

void LogicalFactorized::ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) {
	// The fallback is a whole plan of its own and has to be resolved as one, or
	// it would be unrunnable on the day it is needed -- which is the day nothing
	// else is working either. Its bindings are then discarded rather than
	// returned: the factorized region exposes only its own.
	if (fallback) {
		res.VisitOperator(*fallback);
	}
	bindings = GetColumnBindings();
}

vector<idx_t> LogicalFactorized::GetTableIndex() const {
	if (grouped) {
		return {group_index, table_index};
	}
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
	if (grouped) {
		string keys;
		for (const auto &key : group_keys) {
			keys += (keys.empty() ? "" : ", ") + relations[key.relation].alias + "." +
			        relations[key.relation].column_names[key.column];
		}
		result["Group"] = keys;
	}
	string folds;
	for (const auto &entry : aggregates) {
		folds += (folds.empty() ? "" : ", ");
		folds += entry.kind == factorize::Aggregate::SUM
		             ? "sum(" + relations[entry.relation].alias + "." +
		                   relations[entry.relation].column_names[entry.column] + ")"
		             : "count(*)";
	}
	result["Aggregate"] = folds;
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
	auto &physical = planner.Make<PhysicalFactorized>(types, relations, graph, plan, estimated_cardinality);
	auto &factorized = physical.Cast<PhysicalFactorized>();
	if (fallback) {
		// Planned, but PhysicalFactorized::BuildPipelines keeps it out of the
		// executor's schedule, so nothing here runs unless it is asked to.
		factorized.children.push_back(planner.CreatePlan(*fallback));
	}
	factorized.grouped = grouped;
	factorized.group_types = group_types;
	factorized.group_keys = group_keys;
	factorized.aggregates = aggregates;
	factorized.aggregate_types = aggregate_types;
	return physical;
}

} // namespace duckdb
