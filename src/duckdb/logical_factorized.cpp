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
	// The fallback is resolved here because nothing else will. DuckDB resolves
	// types by walking `children` from the plan root, and the fallback is a
	// whole plan parked beside them rather than one of them, so the pass that
	// gives every operator its types reaches every node except that subtree.
	// Binding resolution does reach it, because ResolveColumnBindings visits it
	// by hand -- so the fallback was arriving at the physical planner with its
	// bindings resolved and its types not, and a node whose types were empty
	// took DuckDB's own PhysicalHashJoin constructor off the end of a vector
	// (D30). Resolving here rather than in CreatePlan keeps the order DuckDB
	// uses for everything else: types first, then bindings, then planning.
	if (fallback) {
		fallback->ResolveOperatorTypes();
	}

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
		bindings.emplace_back(TableIndex(group_index), ProjectionIndex(i));
	}
	for (idx_t i = 0; i < aggregate_types.size(); i++) {
		bindings.emplace_back(TableIndex(table_index), ProjectionIndex(i));
	}
	return bindings;
}

void LogicalFactorized::ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) {
	// The fallback is a whole plan of its own and has to be resolved as one, or
	// it would be unrunnable on the day it is needed -- which is the day nothing
	// else is working either. Its bindings are then discarded rather than
	// returned: the factorized region exposes only its own.
	if (fallback) {
		// A *fresh* resolver, not the one walking the plan this operator sits
		// in. DuckDB 2.0's ColumnBindingResolver carries `types` beside
		// `bindings` and verifies the two agree; handing it an unrelated plan
		// mid-walk makes it check the fallback's expressions against whatever
		// state the outer walk had reached. With compressed materialization
		// disabled -- the workaround this rule's own decline message
		// recommends -- a grouped aggregate over three relations then fails
		// with "inequal types (AGGREGATE_STATE != BIGINT)" before the query
		// runs at all (D116).
		//
		// The fallback is a whole plan of its own, as the comment here has
		// always said, and resolving it as one is what that now requires. Its
		// bindings are still discarded: the factorized region exposes only its
		// own.
		ColumnBindingResolver fallback_resolver;
		fallback_resolver.VisitOperator(*fallback);
	}
	bindings = GetColumnBindings();
}

vector<TableIndex> LogicalFactorized::GetTableIndex() const {
	// DuckDB 2.0 made plan indexes strongly typed -- TableIndex and
	// ProjectionIndex wrap idx_t and will not convert implicitly. The members
	// stay idx_t and are wrapped at the API boundary: this operator's own
	// bookkeeping does not benefit from the distinction, and the conversions
	// are the places where mixing the two would actually be a bug.
	if (grouped) {
		return {TableIndex(group_index), TableIndex(table_index)};
	}
	return {TableIndex(table_index)};
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
		if (entry.kind == factorize::Aggregate::COUNT) {
			folds += "count(*)";
			continue;
		}
		const char *fold_name = "max";
		switch (entry.kind) {
		case factorize::Aggregate::SUM:
			fold_name = "sum";
			break;
		case factorize::Aggregate::AVG:
			fold_name = "avg";
			break;
		case factorize::Aggregate::MIN:
			fold_name = "min";
			break;
		default:
			break;
		}
		folds += string(fold_name) + "(" + relations[entry.relation].alias + "." +
		         relations[entry.relation].column_names[entry.column] + ")";
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
	factorized.estimate_budget_bytes = estimate_budget_bytes;
	factorized.predicted_flat = predicted_flat;
	factorized.min_compression = min_compression;
	factorized.abandon_after_ms = abandon_after_ms;
	factorized.min_rate = min_rate;
	factorized.second_key = second_key;
	factorized.parallel_fallback = parallel_fallback;
	factorized.reduce_extremes = reduce_extremes;
	factorized.explain_steps = explain_steps;
	factorized.grouped = grouped;
	factorized.limited = limited;
	factorized.limit = limit;
	factorized.group_types = group_types;
	factorized.group_keys = group_keys;
	factorized.aggregates = aggregates;
	factorized.aggregate_types = aggregate_types;
	return physical;
}

} // namespace duckdb
