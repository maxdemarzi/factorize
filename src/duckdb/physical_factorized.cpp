#include "factorize/physical_factorized.hpp"

namespace duckdb {

PhysicalFactorized::PhysicalFactorized(PhysicalPlan &physical_plan, vector<LogicalType> types, int64_t stub_value_p,
                                       idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, TYPE, std::move(types), estimated_cardinality), stub_value(stub_value_p) {
}

string PhysicalFactorized::GetName() const {
	return "FACTORIZED";
}

InsertionOrderPreservingMap<string> PhysicalFactorized::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Phase"] = "0 (stub)";
	result["Value"] = to_string(stub_value);
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
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(stub_value));
	return SourceResultType::FINISHED;
}

} // namespace duckdb
