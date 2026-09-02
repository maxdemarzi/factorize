#include "factorize/logical_factorized.hpp"

#include "factorize/physical_factorized.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"

namespace duckdb {

LogicalFactorized::LogicalFactorized(idx_t table_index_p)
    : LogicalExtensionOperator(), table_index(table_index_p) {
}

void LogicalFactorized::ResolveTypes() {
	// The sealed island emits exactly one scalar (plan §1.1). Phase 3 widens this
	// to the semiring's result type (§4.5), not to a tuple stream.
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
	result["Phase"] = "0 (stub)";
	result["Value"] = to_string(stub_value);
	return result;
}

void LogicalFactorized::Serialize(Serializer &serializer) const {
	// Plan §3.2: serialization may throw in v1. The consequence is that prepared
	// statements over a factorized plan cannot be serialized; documented, not fixed.
	throw NotImplementedException("LogicalFactorized cannot be serialized yet");
}

PhysicalOperator &LogicalFactorized::CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) {
	// Phase 0 ignores children entirely; the operator is a pure source.
	return planner.Make<PhysicalFactorized>(types, stub_value, estimated_cardinality);
}

} // namespace duckdb
