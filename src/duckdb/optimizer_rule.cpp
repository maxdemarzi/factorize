#include "factorize/optimizer_rule.hpp"

#include "factorize/logical_factorized.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Settings
//===--------------------------------------------------------------------===//
FactorizeMode GetFactorizeMode(ClientContext &context) {
	Value mode;
	if (!context.TryGetCurrentSetting("factorize_mode", mode) || mode.IsNull()) {
		return FactorizeMode::OFF;
	}
	auto str = StringUtil::Lower(mode.ToString());
	if (str == "auto") {
		return FactorizeMode::AUTO;
	}
	if (str == "force") {
		return FactorizeMode::FORCE;
	}
	return FactorizeMode::OFF;
}

static bool ExplainPlanRequested(ClientContext &context) {
	Value v;
	if (!context.TryGetCurrentSetting("factorize_debug_print_plan", v) || v.IsNull()) {
		return false;
	}
	return BooleanValue::Get(v);
}

//===--------------------------------------------------------------------===//
// Matcher (plan §3.3)
//
// Bail-out is the default. Coverage is earned by *adding* an allowlist case,
// never by forgetting an exclusion. Every rejection is silent and non-fatal.
//===--------------------------------------------------------------------===//

//! The set of relations and equality edges the factorized region will execute.
//! Phase 0 only proves the shape is recognisable; Phase 3 fills it in and hands
//! it to FTree::mergeTrees.
struct FactorizedRegion {
	vector<reference<LogicalOperator>> relations;
	idx_t equality_conditions = 0;
	//! Table index the replacement must expose its result under.
	//!
	//! Operators above the aggregate already reference ColumnBinding(
	//! aggregate_index, 0). Handing the replacement a *fresh* index silently
	//! breaks every one of them -- the plan still type-checks, and binding then
	//! fails at execution with "Failed to bind column reference". The
	//! replacement has to inherit the binding it is replacing.
	idx_t aggregate_index = 0;
};

//! Accepts LOGICAL_GET, optionally wrapped in a single LOGICAL_FILTER.
static bool MatchLeaf(LogicalOperator &op, FactorizedRegion &region) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_FILTER:
		if (op.children.size() != 1) {
			return false;
		}
		return MatchLeaf(*op.children[0], region);
	case LogicalOperatorType::LOGICAL_GET:
		region.relations.push_back(op);
		return true;
	default:
		return false;
	}
}

//! Accepts a tree of INNER comparison joins whose every condition is `=`,
//! bottoming out in scans.
static bool MatchJoinGraph(LogicalOperator &op, FactorizedRegion &region) {
	if (op.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return MatchLeaf(op, region);
	}
	auto &join = op.Cast<LogicalComparisonJoin>();
	if (join.join_type != JoinType::INNER) {
		return false;
	}
	if (!join.duplicate_eliminated_columns.empty()) {
		// Delim joins carry correlated-subquery machinery the region cannot model.
		return false;
	}
	if (join.conditions.empty()) {
		return false;
	}
	for (auto &cond : join.conditions) {
		if (cond.comparison != ExpressionType::COMPARE_EQUAL) {
			return false;
		}
	}
	region.equality_conditions += join.conditions.size();
	if (join.children.size() != 2) {
		return false;
	}
	return MatchJoinGraph(*join.children[0], region) && MatchJoinGraph(*join.children[1], region);
}

//! Accepts zero or more pure column-pruning projections above the join graph.
static bool MatchProjections(LogicalOperator &op, FactorizedRegion &region) {
	if (op.type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return MatchJoinGraph(op, region);
	}
	auto &proj = op.Cast<LogicalProjection>();
	for (auto &expr : proj.expressions) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			// Any computed expression would have to be evaluated on flattened
			// tuples, which the sealed island does not produce.
			return false;
		}
	}
	if (proj.children.size() != 1) {
		return false;
	}
	return MatchProjections(*proj.children[0], region);
}

//! Accepts an ungrouped COUNT(*) -- v1's only aggregate (plan §1.1). The
//! semiring generalisation (§4.5) widens this to sum/min/max/avg in Phase 1.
static bool MatchAggregate(LogicalOperator &op, FactorizedRegion &region) {
	if (op.type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return false;
	}
	auto &aggr = op.Cast<LogicalAggregate>();
	if (!aggr.groups.empty() || !aggr.grouping_sets.empty() || !aggr.grouping_functions.empty()) {
		// GROUP BY is a v2 item (plan §10.1), not a v1 shape.
		return false;
	}
	if (aggr.expressions.size() != 1) {
		return false;
	}
	auto &expr = *aggr.expressions[0];
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	region.aggregate_index = aggr.aggregate_index;
	auto &bound = expr.Cast<BoundAggregateExpression>();
	if (bound.IsDistinct() || bound.filter || bound.order_bys) {
		return false;
	}
	auto name = StringUtil::Lower(bound.function.name);
	if (name != "count_star" && !(name == "count" && bound.children.empty())) {
		return false;
	}
	if (aggr.children.size() != 1) {
		return false;
	}
	if (!MatchProjections(*aggr.children[0], region)) {
		return false;
	}
	// A single relation has nothing to factorize.
	return region.relations.size() >= 2 && region.equality_conditions >= 1;
}

//===--------------------------------------------------------------------===//
// Rule
//===--------------------------------------------------------------------===//

//! Walks the plan looking for a factorizable subtree, replacing the first match.
static void RewriteRecursive(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &op) {
	FactorizedRegion region;
	if (MatchAggregate(*op, region)) {
		auto replacement = make_uniq<LogicalFactorized>(region.aggregate_index);
		replacement->estimated_cardinality = 1;
		// Phase 0: the children are dropped, so this is only ever correct with
		// factorize_mode explicitly set. Phase 3 moves the base-table scans across.
		replacement->ResolveOperatorTypes();
		op = std::move(replacement);
		return;
	}
	for (auto &child : op->children) {
		RewriteRecursive(input, child);
	}
}

void FactorizeOptimizerExtension::Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	auto &context = input.context;

	if (ExplainPlanRequested(context)) {
		Printer::Print("[factorize] post-optimizer plan:\n" + plan->ToString());
	}

	auto mode = GetFactorizeMode(context);
	if (mode == FactorizeMode::OFF) {
		return;
	}
	RewriteRecursive(input, plan);
}

FactorizeOptimizerExtension::FactorizeOptimizerExtension() {
	// Post-builtin, so DuckDB's join order and estimated_cardinality annotations
	// are already in place (plan §3.2).
	optimize_function = Optimize;
}

void FactorizeOptimizerExtension::Register(DBConfig &config) {
	OptimizerExtension::Register(config, FactorizeOptimizerExtension());

	// Default is 'off' while the operator is a Phase 0 stub that returns a
	// constant. Phase 3 flips the default to 'auto' once it computes real counts.
	config.AddExtensionOption("factorize_mode",
	                          "Factorized execution: 'off', 'auto' (fire when the cost gate agrees) or "
	                          "'force' (fire whenever the plan shape matches; benchmarking only)",
	                          LogicalType::VARCHAR, Value("off"));
	config.AddExtensionOption("factorize_debug_print_plan",
	                          "Print the post-optimizer logical plan seen by the factorize rule", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(false));
}

} // namespace duckdb
