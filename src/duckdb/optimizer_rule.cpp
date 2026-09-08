#include "factorize/optimizer_rule.hpp"

#include "factorize/logical_factorized.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

#include <algorithm>
#include <map>
#include <utility>

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

static bool BooleanSetting(ClientContext &context, const char *name, bool missing = false) {
	Value v;
	if (!context.TryGetCurrentSetting(name, v) || v.IsNull()) {
		return missing;
	}
	return BooleanValue::Get(v);
}

static bool ExplainPlanRequested(ClientContext &context) {
	return BooleanSetting(context, "factorize_debug_print_plan");
}

static bool ExplainRequested(ClientContext &context) {
	return BooleanSetting(context, "factorize_explain");
}

//===--------------------------------------------------------------------===//
// Matcher (plan §3.3)
//
// Bail-out is the default. Coverage is earned by *adding* an allowlist case,
// never by forgetting an exclusion. Every rejection is silent and non-fatal.
//===--------------------------------------------------------------------===//

//! The set of relations and equality edges the factorized region will execute.
//! One aggregate of the region: which fold, and for a sum, which column and
//! what type the answer has to come back as. DuckDB widens sum to HUGEINT even
//! over narrow integers, and the operators above were bound against that.
struct RegionAggregate {
	factorize::Aggregate kind = factorize::Aggregate::COUNT;
	ColumnBinding sum_binding;
	LogicalType type;
};

struct FactorizedRegion {
	//! Why this subtree was turned down, for factorize_explain.
	//!
	//! The matcher declines constantly and silently by design, which means the
	//! default experience of a misbehaving rule is nothing visibly happening.
	//! This exists because that cost a whole rebuild cycle to diagnose once.
	//!
	//! The FIRST reason found, which is the one to show: it is the outermost
	//! thing wrong with the query and usually the one a user can act on.
	string decline;
	//! Every reason found, not just the first.
	//!
	//! `decline` alone cannot answer "what would this query need in order to
	//! match", because the matcher used to stop at the first failure and a
	//! blocker sitting behind another one was invisible. That made the reason
	//! counts unusable for exactly the question they were reached for: an
	//! estimate built on them predicted +47 queries and shipping delivered +2,
	//! because "one reported reason" is not "one blocker". Collecting all of
	//! them makes the counts mean what they appear to mean.
	//!
	//! Order is the order found, outermost first, so `declines.front()` is
	//! `decline`.
	vector<string> declines;
	vector<reference<LogicalGet>> relations;
	//! The filter sitting above each relation's scan, if any. Parallel to
	//! `relations`.
	vector<optional_ptr<LogicalFilter>> leaf_filters;
	//! Equality edges, still in the plan's own terms. Resolving them to relation
	//! and column positions has to wait until every leaf is known, because a
	//! join's conditions are read before its subtrees are walked.
	vector<std::pair<ColumnBinding, ColumnBinding>> edges;
	//! Table index the replacement must expose its result under.
	//!
	//! Operators above the aggregate already reference ColumnBinding(
	//! aggregate_index, 0). Handing the replacement a *fresh* index silently
	//! breaks every one of them -- the plan still type-checks, and binding then
	//! fails at execution with "Failed to bind column reference". The
	//! replacement has to inherit the binding it is replacing.
	idx_t aggregate_index = 0;
	//! Set for `GROUP BY g..., count(*)`. The grouped answer has one column per
	//! key beside the aggregate, and they live under two different table
	//! indexes: DuckDB gives an aggregate one for its groups and another for
	//! its aggregates.
	bool grouped = false;
	idx_t group_index = 0;
	vector<ColumnBinding> group_bindings;
	vector<LogicalType> group_types;
	//! One per aggregate the query computes, in its own order, which is the
	//! order the answer's columns come back in.
	vector<RegionAggregate> aggregates;
};

//! Records why a subtree was turned down and declines it. Always returns false,
//! so every rejection site reads as `return Decline(region, "...")` where
//! stopping is required, and as a bare `Decline(region, "...")` beside a
//! cleared `ok` flag where the walk can safely carry on and find more.
//!
//! Carrying on is not always safe: a check whose failure makes the code below
//! it meaningless -- a cast that would be wrong, a child that may not exist --
//! must still stop. The rule is that a site keeps walking only when what
//! follows does not depend on the thing that just failed.
static bool Decline(FactorizedRegion &region, string reason) {
	if (region.decline.empty()) {
		region.decline = reason;
	}
	region.declines.push_back(std::move(reason));
	return false;
}

//! Accepts a scan of a stored table.
//!
//! The region replaces the plan's own scans outright and reads the base tables
//! itself (DECISIONS D18), so every restriction on a scan has to be either
//! carried across or declined -- one left behind would never be applied by
//! anything, giving a silently wrong count rather than a slow query.
static bool MatchLeaf(LogicalOperator &op, FactorizedRegion &region) {
	// A filter above the scan is kept, not refused. DuckDB's own statistics
	// propagation puts one here on ordinary join queries -- it derives a range
	// from the other side of a join, pushes it into most of the scans and leaves
	// it above the rest -- so refusing this shape refuses most real plans.
	// BindRegion translates the predicate into the filter the storage scan takes.
	optional_ptr<LogicalOperator> scan = op;
	optional_ptr<LogicalFilter> filter;
	if (scan->type == LogicalOperatorType::LOGICAL_FILTER) {
		if (scan->children.size() != 1) {
			return Decline(region, "filter has no single child");
		}
		filter = scan->Cast<LogicalFilter>();
		scan = *scan->children[0];
	}
	if (scan->type != LogicalOperatorType::LOGICAL_GET) {
		return Decline(region, "not a scan: " + LogicalOperatorToString(scan->type));
	}
	auto &get = scan->Cast<LogicalGet>();
	if (!get.GetTable()) {
		// A table function, a file read, a CTE scan: no stored table to re-scan.
		return Decline(region, "scan has no stored table behind it");
	}
	if (!get.children.empty() || !get.projected_input.empty()) {
		// Table-in-out function: its input is a subtree, not storage.
		return Decline(region, "scan reads a subtree, not storage");
	}
	// Restrictions that make a scan return less than the whole table, and that
	// nothing downstream would re-apply once the plan's own scan is gone.
	//
	// Three things are deliberately absent from this list. Pushed-down
	// `table_filters` are carried over by BindRegion and applied by the storage
	// scan itself. The join's `filter_pushdown` and the `dynamic_filters`
	// DuckDB's JoinFilterPushdown pass attaches to the probe side of nearly
	// every join only ever remove rows that could not have joined, so a count
	// taken without them is the same count -- and rejecting those would reject
	// nearly every query the rule exists for.
	if (get.extra_info.sample_options) {
		return Decline(region, "scan is sampled");
	}
	if (get.ordinality_idx.IsValid()) {
		return Decline(region, "scan is WITH ORDINALITY");
	}
	region.relations.push_back(get);
	region.leaf_filters.push_back(filter);
	return true;
}

//! Accepts a tree of INNER comparison joins whose every condition equates two
//! bare columns, bottoming out in scans.
static bool MatchJoinGraph(LogicalOperator &op, FactorizedRegion &region) {
	if (op.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return MatchLeaf(op, region);
	}
	auto &join = op.Cast<LogicalComparisonJoin>();
	bool ok = true;
	if (join.join_type != JoinType::INNER) {
		// Recorded rather than returned: what is wrong with this join says
		// nothing about the subtrees under it, and those are where most of the
		// other blockers live. Stopping here is what made the join type look
		// like a rare blocker when the aggregate above simply reached its own
		// verdict first.
		Decline(region, "join is " + JoinTypeToString(join.join_type) + ", not INNER");
		ok = false;
	}
	if (!join.duplicate_eliminated_columns.empty()) {
		// Delim joins carry correlated-subquery machinery the region cannot model.
		Decline(region, "join is duplicate-eliminated");
		ok = false;
	}
	if (join.predicate) {
		// An ON-clause restriction that references only one side. It filters the
		// join's output and nothing outside the region would apply it.
		Decline(region, "join carries an ON-clause filter");
		ok = false;
	}
	if (join.conditions.empty()) {
		Decline(region, "join has no conditions");
		ok = false;
	}
	if (join.children.size() != 2) {
		// The one that has to stop: everything below reads children[0] and
		// children[1], so carrying on would be reading operators that are not
		// there.
		Decline(region, "join does not have two children");
		return false;
	}
	for (auto &cond : join.conditions) {
		if (cond.comparison != ExpressionType::COMPARE_EQUAL) {
			Decline(region, "join condition is not an equality");
			ok = false;
			continue;
		}
		// A cast, or any other computed expression, would have to be evaluated
		// on values the f-representation holds in packed integer slots. Only a
		// bare column reference maps onto one.
		if (cond.left->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
		    cond.right->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			Decline(region, "join key is computed, not a plain column");
			ok = false;
			continue;
		}
		region.edges.emplace_back(cond.left->Cast<BoundColumnRefExpression>().binding,
		                          cond.right->Cast<BoundColumnRefExpression>().binding);
	}
	// Both sides, always: `&&` would have skipped the second subtree whenever
	// the first declined, hiding every blocker on the right of a bad left.
	const bool left = MatchJoinGraph(*join.children[0], region);
	const bool right = MatchJoinGraph(*join.children[1], region);
	return ok && left && right;
}

//! Accepts zero or more pure column-pruning projections above the join graph.
static bool MatchProjections(LogicalOperator &op, FactorizedRegion &region) {
	if (op.type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return MatchJoinGraph(op, region);
	}
	auto &proj = op.Cast<LogicalProjection>();
	bool ok = true;
	for (auto &expr : proj.expressions) {
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			// A constant per row changes no row's existence, so it changes no
			// count. DuckDB plants one of these under the aggregate it rewrites
			// EXISTS into.
			continue;
		}
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
		    StringUtil::StartsWith(expr->Cast<BoundFunctionExpression>().function.name, "__internal_compress")) {
			// DuckDB's compressed-materialization pass narrows the group key
			// before an aggregate and widens it again afterwards. Reproducing
			// that transformation here would tie this rule to the semantics of
			// an internal function; declining says so, and names the way out.
			Decline(region, "compressed materialization is in the way; "
			                "SET disabled_optimizers='compressed_materialization' to factorize this");
			ok = false;
			continue;
		}
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			// Any other computed expression would have to be evaluated on
			// flattened tuples, which the sealed island does not produce.
			Decline(region, "projection computes an expression");
			ok = false;
			continue;
		}
	}
	if (proj.children.size() != 1) {
		// Has to stop: there is no single child to descend into.
		Decline(region, "projection has no single child");
		return false;
	}
	const bool below = MatchProjections(*proj.children[0], region);
	return ok && below;
}

//! Accepts an ungrouped COUNT(*) -- v1's only aggregate (plan §1.1). The
//! semiring generalisation (§4.5) widens this to sum/min/max/avg later.
static bool MatchAggregate(LogicalOperator &op, FactorizedRegion &region) {
	if (op.type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return false;
	}
	auto &aggr = op.Cast<LogicalAggregate>();
	// Recorded, not returned. Everything wrong with the aggregate is local to
	// this node, and the join graph underneath is walked regardless -- which is
	// the whole point: an aggregate blocker used to hide every blocker below it,
	// so the reason counts measured which check ran first rather than what a
	// query would need.
	bool ok = true;
	if (!aggr.grouping_functions.empty() || aggr.grouping_sets.size() > 1) {
		// GROUPING SETS / ROLLUP ask for several groupings at once.
		Decline(region, "aggregate has grouping sets");
		ok = false;
	}
	if (!aggr.groups.empty()) {
		// Any number of grouping columns. This used to stop at one, on the
		// reading that scattered keys need the cross product of independent
		// branches and are therefore a different algorithm (plan §10.1) -- true
		// about the cross product, wrong about the conclusion, since siblings
		// being independent is the property the representation is built on and
		// the branches can be grouped separately and combined.
		//
		// It is also, measured against TPC-DS, the single restriction that
		// mattered most: 47 of 99 queries were declined for this reason alone.
		region.grouped = true;
		region.group_index = aggr.group_index;
		for (auto &group : aggr.groups) {
			if (group->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				Decline(region, "aggregate groups on a computed expression");
				ok = false;
				continue;
			}
			region.group_bindings.push_back(group->Cast<BoundColumnRefExpression>().binding);
			region.group_types.push_back(group->return_type);
		}
	}
	if (aggr.expressions.empty()) {
		Decline(region, "aggregate computes nothing");
		ok = false;
	}
	if (aggr.expressions.size() > factorize::kMaxAggregates) {
		Decline(region, "aggregate computes more than " + std::to_string(factorize::kMaxAggregates) + " values");
		ok = false;
	}
	region.aggregate_index = aggr.aggregate_index;
	// Several aggregates are folded side by side in one walk of the
	// representation, because they are folds over the same tuples and differ
	// only in what each carries. This used to stop at one, and measured against
	// TPC-DS it was the largest remaining restriction by some way: 36 of 99
	// queries were declined for it alone, once multi-column grouping stopped
	// hiding them behind an earlier refusal.
	for (auto &expression : aggr.expressions) {
		if (expression->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			// Has to stop looking at THIS expression: the cast below would be
			// wrong. The walk still continues to the next one.
			Decline(region, "aggregate expression is not an aggregate");
			ok = false;
			continue;
		}
		auto &bound = expression->Cast<BoundAggregateExpression>();
		if (bound.IsDistinct() || bound.filter || bound.order_bys) {
			Decline(region, "aggregate is DISTINCT, FILTERed or ORDERed");
			ok = false;
			continue;
		}
		RegionAggregate entry;
		entry.type = expression->return_type;
		auto name = StringUtil::Lower(bound.function.name);
		if (name == "sum") {
			// Summing is the semiring generalisation the plan always had as the
			// widening step (§4.5), and on TPC-DS it is the aggregate that
			// actually appears: 193 occurrences over inner-only join graphs
			// against 24 for count(*). The column has to be a plain column of
			// the region, since it is folded through the representation rather
			// than evaluated.
			if (bound.children.size() != 1 ||
			    bound.children[0]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				Decline(region, "sum() of a computed expression");
				ok = false;
				continue;
			}
			entry.kind = factorize::Aggregate::SUM;
			entry.sum_binding = bound.children[0]->Cast<BoundColumnRefExpression>().binding;
		} else if (name != "count_star" && !(name == "count" && bound.children.empty())) {
			Decline(region, "aggregate is " + name + "(), not count(*) or sum()");
			ok = false;
			continue;
		}
		region.aggregates.push_back(std::move(entry));
	}
	if (aggr.children.size() != 1) {
		// Has to stop: there is no single child to walk.
		Decline(region, "aggregate has no single child");
		return false;
	}
	// Always walked, even when the aggregate above is already refused, because
	// what a query needs in order to match is the UNION of its blockers and
	// this is where most of them are.
	const bool below = MatchProjections(*aggr.children[0], region);
	if (region.relations.size() < 2 || region.edges.empty()) {
		// A single relation has nothing to factorize.
		Decline(region, "fewer than two joined relations");
		ok = false;
	}
	return ok && below;
}

//===--------------------------------------------------------------------===//
// Binding
//
// The matcher proves the shape; this turns it into the graph the core plans
// over. Kept separate because an edge cannot be placed until every relation has
// a position, and the matcher reads a join's conditions before walking its
// subtrees.
//===--------------------------------------------------------------------===//

//! One predicate, translated: which column it reads and the filter to apply.
using TranslatedFilter = std::pair<ColumnBinding, unique_ptr<TableFilter>>;

//! `5 < x` says the same thing as `x > 5`, and only the second form is a
//! TableFilter.
static ExpressionType MirrorComparison(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_LESSTHAN:
		return ExpressionType::COMPARE_GREATERTHAN;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return ExpressionType::COMPARE_GREATERTHANOREQUALTO;
	case ExpressionType::COMPARE_GREATERTHAN:
		return ExpressionType::COMPARE_LESSTHAN;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ExpressionType::COMPARE_LESSTHANOREQUALTO;
	default:
		return type; // = and <> read the same in both directions
	}
}

//! Turns one filter predicate into the TableFilter the storage scan already
//! knows how to apply, and says which of the scan's columns it reads.
//!
//! Deliberately narrow: a comparison against a constant, or a null test. That
//! is what DuckDB's statistics propagation leaves above a scan and what a
//! written-out WHERE clause reduces to. Anything else is a decline, because the
//! alternative -- evaluating arbitrary expressions here -- would be a second
//! implementation of DuckDB's own expression semantics, judged against a count
//! that has to match it exactly.
static bool TryTranslateFilter(const Expression &expr, vector<TranslatedFilter> &out) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto &comparison = expr.Cast<BoundComparisonExpression>();
		auto type = comparison.GetExpressionType();
		const Expression *column_side = comparison.left.get();
		const Expression *constant_side = comparison.right.get();
		if (column_side->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			std::swap(column_side, constant_side);
			type = MirrorComparison(type);
		}
		if (column_side->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
		    constant_side->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return false;
		}
		switch (type) {
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOTEQUAL:
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			break;
		default:
			// A null-safe comparison has different null semantics from the
			// filter that would be built for it.
			return false;
		}
		auto &constant = constant_side->Cast<BoundConstantExpression>();
		if (constant.value.IsNull() || constant.value.type() != column_side->return_type) {
			// A comparison across types is decided by rules a ConstantFilter on
			// the column's own type would not reproduce.
			return false;
		}
		out.emplace_back(column_side->Cast<BoundColumnRefExpression>().binding,
		                 make_uniq<ConstantFilter>(type, constant.value));
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_BETWEEN) {
		// What a two-sided range from statistics propagation arrives as, and the
		// most common thing left above a scan on this corpus. It is two constant
		// comparisons wearing one hat.
		auto &between = expr.Cast<BoundBetweenExpression>();
		if (between.input->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
		    between.lower->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
		    between.upper->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return false;
		}
		auto &lower = between.lower->Cast<BoundConstantExpression>();
		auto &upper = between.upper->Cast<BoundConstantExpression>();
		if (lower.value.IsNull() || upper.value.IsNull() || lower.value.type() != between.input->return_type ||
		    upper.value.type() != between.input->return_type) {
			return false;
		}
		const auto binding = between.input->Cast<BoundColumnRefExpression>().binding;
		out.emplace_back(binding, make_uniq<ConstantFilter>(between.LowerComparisonType(), lower.value));
		out.emplace_back(binding, make_uniq<ConstantFilter>(between.UpperComparisonType(), upper.value));
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conjunction = expr.Cast<BoundConjunctionExpression>();
		if (conjunction.GetExpressionType() != ExpressionType::CONJUNCTION_AND) {
			// An OR cannot become a per-column filter set, whose members are
			// themselves ANDed together.
			return false;
		}
		for (auto &child : conjunction.children) {
			if (!TryTranslateFilter(*child, out)) {
				return false;
			}
		}
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
		auto &op = expr.Cast<BoundOperatorExpression>();
		if (op.children.size() != 1 || op.children[0]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		const auto binding = op.children[0]->Cast<BoundColumnRefExpression>().binding;
		if (op.GetExpressionType() == ExpressionType::OPERATOR_IS_NULL) {
			out.emplace_back(binding, make_uniq<IsNullFilter>());
			return true;
		}
		if (op.GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL) {
			out.emplace_back(binding, make_uniq<IsNotNullFilter>());
			return true;
		}
		return false;
	}
	return false;
}

//! False if any part of the region cannot be expressed, which is a decline.
static bool BindRegion(ClientContext &context, FactorizedRegion &region, vector<BoundRelation> &relations,
                       factorize::QueryGraph &graph, vector<factorize::GroupKey> &group_keys,
                       vector<factorize::GroupAggregate> &aggregates) {
	std::map<idx_t, size_t> relation_of_table_index;
	for (size_t i = 0; i < region.relations.size(); i++) {
		auto &get = region.relations[i].get();
		if (!relation_of_table_index.emplace(get.table_index, i).second) {
			// Two scans sharing one table index: bindings would be ambiguous.
			return Decline(region, "two scans share a table index");
		}
		BoundRelation relation;
		// Taken from the plan DuckDB already bound, never re-resolved by name.
		relation.entry = get.GetTable();
		// A self-join is two relations over one table, and an EXPLAIN reading
		// "yago2.s = yago2.o" would say nothing about which scan is which.
		relation.alias = get.GetTable()->name;
		for (auto &earlier : relations) {
			if (earlier.alias == relation.alias) {
				relation.alias += "#" + to_string(i);
				break;
			}
		}
		relations.push_back(std::move(relation));
	}

	vector<factorize::Predicate> predicates;
	for (auto &edge : region.edges) {
		const ColumnBinding *sides[2] = {&edge.first, &edge.second};
		size_t edge_relation[2] = {0, 0};
		int edge_column[2] = {0, 0};
		for (int side = 0; side < 2; side++) {
			auto found = relation_of_table_index.find(sides[side]->table_index);
			if (found == relation_of_table_index.end()) {
				// A binding from outside the region.
				return Decline(region, "join key binds outside the region");
			}
			const size_t relation = found->second;
			auto &get = region.relations[relation].get();
			auto &column_ids = get.GetColumnIds();
			if (sides[side]->column_index >= column_ids.size()) {
				return Decline(region, "join key is not among the scanned columns");
			}
			auto &column_index = column_ids[sides[side]->column_index];
			if (column_index.IsRowIdColumn() || column_index.IsVirtualColumn() || column_index.HasChildren()) {
				// Not a stored scalar column of this table.
				return Decline(region, "join key is not a stored scalar column");
			}
			auto &definition = get.GetTable()->GetColumn(LogicalIndex(column_index.GetPrimaryIndex()));
			if (definition.Generated()) {
				// Computed on read, so scanning storage would not produce it.
				return Decline(region, "join key is a generated column");
			}
			factorize::ValueType value_type;
			if (!TryIntegerKeyType(definition.Type(), value_type)) {
				return Decline(region, "join key " + definition.Name() + " is " + definition.Type().ToString() + ", not an integer");
			}
			// Columns no predicate mentions are never read: they cannot change a
			// count, and carrying them would widen every record for nothing.
			const auto physical = static_cast<idx_t>(definition.Physical().index);
			auto &bound = relations[relation];
			if (bound.LocalIndex(physical) < 0) {
				bound.columns.push_back(physical);
				bound.column_names.push_back(definition.Name());
				bound.column_types.push_back(value_type);
			}
			edge_relation[side] = relation;
			edge_column[side] = bound.LocalIndex(physical);
		}
		if (edge_relation[0] == edge_relation[1]) {
			// Both sides in one scan is a filter, not a join edge, and the region
			// has nowhere to apply it. A self-join is two scans, not this.
			return Decline(region, "both sides of an equality are in one scan");
		}
		factorize::Predicate predicate;
		predicate.left_relation = edge_relation[0];
		predicate.left_column = edge_column[0];
		predicate.right_relation = edge_relation[1];
		predicate.right_column = edge_column[1];
		predicates.push_back(predicate);
	}

	for (auto &relation : relations) {
		if (relation.columns.empty()) {
			// No predicate reaches this relation, so the region is a product.
			return Decline(region, "relation " + relation.alias + " has no join predicate");
		}
	}

	// Everything scanned so far is a join key. What the aggregate adds below is
	// read for its values alone, and the two have different NULL rules, so the
	// boundary between them is recorded here.
	std::vector<size_t> join_columns;
	join_columns.reserve(relations.size());
	for (const auto &relation : relations) {
		join_columns.push_back(relation.columns.size());
	}

	for (const auto &aggregate : region.aggregates) {
		if (aggregate.kind != factorize::Aggregate::SUM) {
			// count(*) reads nothing: the representation knows how many tuples
			// a subtree denotes without looking at any column of them.
			aggregates.push_back(factorize::GroupAggregate {factorize::Aggregate::COUNT, 0, 0});
			continue;
		}
		// A summed column is not a join key, so unlike a join key it is *added*
		// to the scan rather than required to be there already. Appended, never
		// inserted: the join predicates above hold local column indices, and
		// inserting would move the ground under them. (Filters are re-keyed
		// below, after every append, for the same reason from the other
		// direction.)
		auto found = relation_of_table_index.find(aggregate.sum_binding.table_index);
		if (found == relation_of_table_index.end()) {
			return Decline(region, "summed column belongs to no relation of the region");
		}
		auto &get = region.relations[found->second].get();
		auto &column_ids = get.GetColumnIds();
		if (aggregate.sum_binding.column_index >= column_ids.size()) {
			return Decline(region, "summed column is not among the scanned columns");
		}
		auto &column_index = column_ids[aggregate.sum_binding.column_index];
		if (column_index.IsRowIdColumn() || column_index.IsVirtualColumn() || column_index.HasChildren()) {
			return Decline(region, "summed column is not a stored scalar column");
		}
		auto &definition = get.GetTable()->GetColumn(LogicalIndex(column_index.GetPrimaryIndex()));
		if (definition.Generated()) {
			return Decline(region, "summed column is generated");
		}
		factorize::ValueType value_type;
		if (!TrySummableType(definition.Type(), value_type)) {
			// Floating point is excluded on purpose rather than for want of a
			// fold: reassociation makes a sum depend on the order it was taken
			// in, and this rule's whole contract is that 'auto' and 'off' agree
			// (DECISIONS D10). A DECIMAL past precision 18 is int128-backed and
			// simply wider than the fold.
			return Decline(region, "summed column " + definition.Name() + " is " + definition.Type().ToString() +
			                           ", which the fold cannot carry");
		}
		auto &bound = relations[found->second];
		const auto physical = static_cast<idx_t>(definition.Physical().index);
		if (bound.LocalIndex(physical) < 0) {
			bound.columns.push_back(physical);
			bound.column_names.push_back(definition.Name());
			bound.column_types.push_back(value_type);
		}
		const auto local = static_cast<size_t>(bound.LocalIndex(physical));
		const bool row_matters_elsewhere = region.grouped || region.aggregates.size() > 1;
		if (row_matters_elsewhere && local >= join_columns[found->second]) {
			// A row whose summed value is NULL may be dropped only when this sum
			// is the whole answer, because it contributes NULL to its own sum
			// either way. It may not be dropped when anything else in the query
			// needs the row to exist: a count(*) comes back one short, and a sum
			// over another column loses that row's contribution to it. Grouped,
			// it may never be dropped -- a group whose every row is NULL here is
			// still a row of the answer, with a NULL sum.
			//
			// This read `region.grouped` alone, and `count(*), sum(x)` over a
			// nullable x silently returned one row short (D31). The reasoning
			// written here was sound about the sum and was applied to the query.
			//
			// Declined up front on the statistics, so the common case stays a
			// decline that DuckDB answers rather than an error; the run-time
			// guard below it is for the rows the statistics do not cover.
			auto statistics = get.GetTable()->GetStatistics(context, column_index.GetPrimaryIndex());
			if (!statistics || statistics->CanHaveNull()) {
				return Decline(region, "sum over " + definition.Name() + ", which may contain NULL and would " +
				                           (region.grouped ? "drop a group entirely"
				                                           : "drop the row from the query's other aggregates"));
			}
			bound.no_null_columns.push_back(static_cast<idx_t>(local));
		}
		aggregates.push_back(factorize::GroupAggregate {factorize::Aggregate::SUM, found->second, local});
	}

	for (const auto &binding : region.group_bindings) {
		// A grouping column is added to the scan if it is not already read, the
		// same way a summed column is: it changes no count, but the answer has
		// one row per distinct value of it, so its values have to be there.
		auto found = relation_of_table_index.find(binding.table_index);
		if (found == relation_of_table_index.end()) {
			return Decline(region, "grouping column belongs to no relation of the region");
		}
		auto &get = region.relations[found->second].get();
		auto &column_ids = get.GetColumnIds();
		if (binding.column_index >= column_ids.size()) {
			return Decline(region, "grouping column is not among the scanned columns");
		}
		auto &column_index = column_ids[binding.column_index];
		if (column_index.IsRowIdColumn() || column_index.IsVirtualColumn() || column_index.HasChildren()) {
			return Decline(region, "grouping column is not a stored scalar column");
		}
		auto &definition = get.GetTable()->GetColumn(LogicalIndex(column_index.GetPrimaryIndex()));
		if (definition.Generated()) {
			return Decline(region, "grouping column is generated");
		}
		factorize::ValueType value_type;
		if (!TryIntegerKeyType(definition.Type(), value_type)) {
			// A grouping key is a value the answer carries, so unlike a summed
			// column it has to fit a key slot and come back out as itself.
			return Decline(region, "grouping column " + definition.Name() + " is " + definition.Type().ToString() +
			                           ", which the representation cannot carry");
		}
		auto &bound = relations[found->second];
		const auto physical = static_cast<idx_t>(definition.Physical().index);
		const auto existing = bound.LocalIndex(physical);
		if (existing < 0 || static_cast<size_t>(existing) >= join_columns[found->second]) {
			// The scan drops a row when any column of `columns` is NULL. That
			// is right for a join key -- NULL equals nothing, so the row cannot
			// contribute to an inner join.
			//
			// It is wrong for a grouping column. SQL groups NULLs together and
			// answers for that group, so dropping those rows deletes a row of
			// the answer and the groups would no longer sum to the count.
			// Nothing in the representation can carry a NULL instead: a key slot
			// is an integer with every value already spoken for.
			//
			// So a grouping column that is not already a join key is a decline
			// unless it provably has no NULLs. The statistics say that exactly,
			// and say it without reading the data; a column with no statistics
			// is assumed to have them. They speak for committed row groups only
			// (DataTable::GetStatistics is row_groups->CopyStats), so rows
			// appended in the current transaction are not covered -- which is
			// why the scan also guards this at run time rather than trusting the
			// answer here.
			auto statistics = get.GetTable()->GetStatistics(context, column_index.GetPrimaryIndex());
			if (!statistics || statistics->CanHaveNull()) {
				return Decline(region, "grouping column " + definition.Name() +
				                           " may contain NULL, which is a group the representation cannot carry");
			}
		}
		if (existing < 0) {
			bound.columns.push_back(physical);
			bound.column_names.push_back(definition.Name());
			bound.column_types.push_back(value_type);
		}
		const auto local = static_cast<size_t>(bound.LocalIndex(physical));
		if (local >= join_columns[found->second]) {
			bound.no_null_columns.push_back(static_cast<idx_t>(local));
		}
		group_keys.push_back(factorize::GroupKey {found->second, local});
	}

	// Carry over every restriction the plan placed on each scan: the filters
	// DuckDB pushed into the scan, and any it left in a filter above it.
	// Dropping one would count rows the stock plan never sees, and replaying it
	// by hand would be a second implementation of DuckDB's filter semantics
	// judged against a count that has to match DuckDB's exactly. Re-keying them
	// onto our own scan and letting the storage layer apply them is neither.
	//
	// THIS MUST RUN LAST. A filter-only column is placed at `columns.size() +
	// its index in filter_columns`, because the scan reads `columns` and then
	// `filter_columns`. That position is derived from columns.size(), so
	// appending to `columns` afterwards moves every filter-only column and the
	// filter lands on whatever now occupies its old slot -- a wrong count, with
	// nothing to indicate it. This was live from the sum work until multi-column
	// GROUP BY, which appends more and made it easy to hit:
	//
	//     p(k, v, w), q(k):  SELECT sum(p.v) FROM p, q WHERE p.k = q.k AND p.w > 5
	//     filter on w:  position = columns.size()(=1, just k) + 0 = 1
	//     sum appends v:  columns = [k, v], so the scan reads k=0, v=1, w=2
	//     the filter still says 1, and `v > 5` is not `w > 5`: 700, not 500.
	//
	// The invariant a reader can check: nothing may be appended to `columns`
	// below this point.
	for (size_t i = 0; i < relations.size(); i++) {
		auto &get = region.relations[i].get();
		auto &bound = relations[i];
		auto remapped = make_shared_ptr<TableFilterSet>();

		// The scan's own column, as a position in the columns we will read,
		// adding it to the scan if only a filter wants it.
		auto scan_position = [&](const ColumnDefinition &definition, int &position) {
			if (definition.Generated() || definition.Type().IsNested()) {
				// Generated columns are not in storage at all, and a filter on a
				// struct field needs the scan to name the field, where this scan
				// names whole columns only.
				return false;
			}
			const auto physical = static_cast<idx_t>(definition.Physical().index);
			position = bound.LocalIndex(physical);
			if (position < 0) {
				auto found = std::find(bound.filter_columns.begin(), bound.filter_columns.end(), physical);
				if (found == bound.filter_columns.end()) {
					bound.filter_columns.push_back(physical);
					found = bound.filter_columns.end() - 1;
				}
				position = static_cast<int>(bound.columns.size() + (found - bound.filter_columns.begin()));
			}
			return true;
		};

		for (auto &filter : get.table_filters.filters) {
			// Pushed-down filters are keyed by the table's own column index, the
			// same space LogicalGet's column ids live in.
			int position = 0;
			if (!scan_position(get.GetTable()->GetColumn(LogicalIndex(filter.first)), position)) {
				return Decline(region, "pushed-down filter reads a column the scan cannot produce");
			}
			remapped->PushFilter(ColumnIndex(static_cast<idx_t>(position)), filter.second->Copy());
		}

		if (region.leaf_filters[i]) {
			// All or nothing per filter operator: a predicate half-translated is
			// a predicate half-applied, which is a wrong count.
			vector<TranslatedFilter> translated;
			for (auto &expr : region.leaf_filters[i]->expressions) {
				if (!TryTranslateFilter(*expr, translated)) {
					return Decline(region, "filter predicate does not reduce to constant comparisons");
				}
			}
			for (auto &entry : translated) {
				auto &binding = entry.first;
				if (binding.table_index != get.table_index) {
					return Decline(region, "filter reads a column from another relation");
				}
				auto &column_ids = get.GetColumnIds();
				if (binding.column_index >= column_ids.size()) {
					return Decline(region, "filter reads a column the scan does not produce");
				}
				auto &column_index = column_ids[binding.column_index];
				if (column_index.IsRowIdColumn() || column_index.IsVirtualColumn() || column_index.HasChildren()) {
					return Decline(region, "filter reads a column that is not stored");
				}
				int position = 0;
				if (!scan_position(get.GetTable()->GetColumn(LogicalIndex(column_index.GetPrimaryIndex())), position)) {
					return Decline(region, "filter reads a column the scan cannot produce");
				}
				remapped->PushFilter(ColumnIndex(static_cast<idx_t>(position)), std::move(entry.second));
			}
		}

		if (!remapped->filters.empty()) {
			bound.filters = std::move(remapped);
		}
	}

	for (auto &relation : relations) {
		graph.column_counts.push_back(relation.columns.size());
		graph.column_types.push_back(relation.column_types);
	}
	graph.predicates = std::move(predicates);
	return true;
}

//===--------------------------------------------------------------------===//
// Gate (plan §5, DECISIONS D14)
//
// The decision has to be made before anything is scanned, which is the whole
// difficulty: the estimator was built and fitted against *exact* per-column
// statistics, and the catalog has approximations of some of them and none of
// the rest.
//===--------------------------------------------------------------------===//

//! Statistics from the catalog, for a gate that must not touch the data.
//!
//! Row counts come from DuckDB's own cardinality estimate, which already
//! accounts for the filters on each scan. Distinct counts come from the
//! catalog's sketch. There is no MCV list to be had: DuckDB does not keep one,
//! and an empty one degrades EstimateCost to the textbook estimator rather than
//! to nonsense (DECISIONS D13) -- it will under-read skew, and the calibration
//! below is what says whether that matters in practice.
class CatalogStats : public factorize::RelationSource {
public:
	CatalogStats(ClientContext &context, const FactorizedRegion &region, const vector<BoundRelation> &relations)
	    : context(context), region(region), relations(relations) {
	}

	const std::vector<std::vector<int64_t>> &Columns(size_t) override {
		throw InternalException("the factorize gate must not read data");
	}

	factorize::ColumnStats Stats(size_t relation, size_t column) override {
		factorize::ColumnStats stats;
		auto &get = region.relations[relation].get();
		stats.rows = static_cast<double>(get.EstimateCardinality(context));
		stats.distinct = stats.rows > 0 ? stats.rows : 1;

		auto &bound = relations[relation];
		if (column < bound.columns.size()) {
			auto table = get.GetTable();
			const auto physical = bound.columns[column];
			// The catalog is keyed by logical column, the scan by physical one.
			for (auto &definition : table->GetColumns().Logical()) {
				if (static_cast<idx_t>(definition.Physical().index) != physical) {
					continue;
				}
				auto statistics = table->GetStatistics(context, definition.Logical().index);
				if (statistics) {
					const auto distinct = static_cast<double>(statistics->GetDistinctCount());
					if (distinct > 0) {
						// Never more distinct values than rows: the sketch is
						// over the whole column, while `rows` may already have a
						// filter applied to it.
						stats.distinct = std::min(distinct, stats.rows > 0 ? stats.rows : distinct);
					}
				}
				break;
			}
		}
		stats.distinct = stats.distinct < 1 ? 1 : stats.distinct;
		return stats;
	}

private:
	ClientContext &context;
	const FactorizedRegion &region;
	const vector<BoundRelation> &relations;
};

static double DoubleSetting(ClientContext &context, const char *name, double fallback) {
	Value value;
	if (!context.TryGetCurrentSetting(name, value) || value.IsNull()) {
		return fallback;
	}
	return value.GetValue<double>();
}

//! What the engine is allowed to hold, and therefore what the gate should treat
//! as "will not fit". Matches what the operator sets at execution: half of
//! DuckDB's limit, because the scanned base columns live outside the arena.
static idx_t MemoryBudget(ClientContext &context) {
	const auto &config = DBConfig::GetConfig(context);
	const idx_t available = config.options.maximum_memory == DConstants::INVALID_INDEX
	                            ? static_cast<idx_t>(4) * 1024 * 1024 * 1024
	                            : config.options.maximum_memory;
	return available / 2;
}

//! Whether factorizing this region is predicted to beat the stock plan.
static bool GateAgrees(ClientContext &context, const FactorizedRegion &region, const vector<BoundRelation> &relations,
                       const factorize::QueryGraph &graph, const factorize::Plan &plan, string &reason,
                       double &predicted_bytes) {
	CatalogStats stats(context, region, relations);
	factorize::CostThresholds thresholds;
	thresholds.margin = DoubleSetting(context, "factorize_min_gain", thresholds.margin);
	thresholds.min_duckdb_work_ms = DoubleSetting(context, "factorize_min_work_ms", thresholds.min_duckdb_work_ms);
	// Predicted not to fit is no longer a refusal -- ExecuteCountWithinMemory
	// slices instead -- but it is still a reason to decline: every slice is
	// another pass over the input, and the gate is a bet about time.
	thresholds.memory_budget_bytes = static_cast<double>(MemoryBudget(context));
	// BuildPlan has already refused anything that cannot be arranged as a tree.
	const auto estimate = factorize::EstimateCost(factorize::BuildCostSteps(graph, plan, stats), true, thresholds);
	reason = estimate.reason;
	predicted_bytes = estimate.bytes;
	return estimate.fire;
}

//===--------------------------------------------------------------------===//
// Rule
//===--------------------------------------------------------------------===//

//! Walks the plan looking for a factorizable subtree, replacing the first match.
static void RewriteRecursive(ClientContext &context, unique_ptr<LogicalOperator> &op, bool gated, bool explain) {
	FactorizedRegion region;
	if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY && MatchAggregate(*op, region)) {
		vector<BoundRelation> relations;
		factorize::QueryGraph graph;
		vector<factorize::GroupKey> group_keys;
		vector<factorize::GroupAggregate> aggregates;
		if (BindRegion(context, region, relations, graph, group_keys, aggregates)) {
			// The join order is decided here rather than at execution: a graph
			// the planner cannot order (a disconnected one, most of all) has to
			// be a decline, not a query that fails halfway through running.
			auto plan = factorize::BuildPlan(graph);
			if (!plan.complete) {
				region.decline = plan.reason;
			} else {
				string gate_reason;
				double predicted_bytes = 0;
				// FORCE skips this and only this: the matcher's refusals are
				// about what the engine can compute at all, while the gate is
				// about whether computing it that way is a good idea.
				const bool fire = !gated || GateAgrees(context, region, relations, graph, plan, gate_reason, predicted_bytes);
				if (!fire) {
					region.decline = "gate says no: " + gate_reason;
				} else {
					if (explain) {
						Printer::Print("[factorize] took over: " + DescribeRelations(relations) + " on " +
						               DescribePredicates(relations, graph) + ", joining " +
						               DescribeJoinOrder(relations, plan));
					}
					auto replacement = make_uniq<LogicalFactorized>(region.aggregate_index, std::move(relations),
					                                               std::move(graph), std::move(plan));
					replacement->grouped = region.grouped;
					replacement->group_index = region.group_index;
					replacement->group_types = region.group_types;
					replacement->group_keys = std::move(group_keys);
					replacement->aggregates = std::move(aggregates);
					for (const auto &entry : region.aggregates) {
						// count(*) is BIGINT; a sum keeps the type DuckDB gave
						// it, because the operators above were bound to that.
						replacement->aggregate_types.push_back(
						    entry.kind == factorize::Aggregate::SUM ? entry.type : LogicalType::BIGINT);
					}
					// Hold the operator to the size the gate bet on. The gate is a
					// prediction and predictions are wrong; this bounds how wrong.
					// A representation that outgrows the estimate by this factor
					// means the decision rested on a number that is not true, and
					// finishing the query only spends longer being wrong (D34).
					// Zero disables the check and restores the old behaviour.
					// The same idea taken one step further: rather than hold the
					// operator to a predicted *size*, hold it to a measured
					// *compression*. The estimate budget still needs the gate's
					// number to have been roughly right; this needs nothing
					// predicted at all, which matters because the corpus says
					// every prediction available here is wrong on the queries
					// that lose (D37).
					//
					// Gated modes only. FORCE means "run the factorized path
					// whatever we think of the idea", which is what every test
					// in the suite relies on: small fixtures compress barely at
					// all -- 200 keys times 10 rows is 0.9 tuples per record --
					// so a floor that applied under FORCE would quietly turn the
					// whole SQL suite into a test of the fallback, passing all
					// the way and covering nothing (D25, D26 again).
					replacement->min_compression = gated ? DoubleSetting(context, "factorize_min_compression", 0.0) : 0.0;
					replacement->explain_steps = explain;
					const auto slack = DoubleSetting(context, "factorize_estimate_slack", 2.0);
					if (slack > 0 && predicted_bytes > 0) {
						const double budget = predicted_bytes * slack;
						replacement->estimate_budget_bytes =
						    budget >= 9e18 ? 0 : static_cast<idx_t>(budget) + 1024 * 1024;
					}
					replacement->estimated_cardinality = 1;
					replacement->ResolveOperatorTypes();
					// The plan being replaced is carried, not dropped: §7.5 wants
					// an internal error to fall back to it rather than surface,
					// and it cannot be recovered later -- this is the only moment
					// it exists.
					//
					// Optional, because carrying it costs parallelism rather than
					// nothing: an operator that may have to drive the fallback's
					// pipeline cannot be a parallel source (see ParallelSource),
					// which gives up D20's slicing. Correctness by default,
					// since a failed query is worse than a slower one -- but the
					// trade is a setting rather than a decision taken silently on
					// the user's behalf.
					if (BooleanSetting(context, "factorize_fallback", true)) {
						replacement->fallback = std::move(op);
					}
					op = std::move(replacement);
					return;
				}
			}
		}
	}
	// Declining leaves the stock plan in place, which is always correct, and the
	// walk continues in case a nested aggregate is factorizable on its own. The
	// reason is reported outside the match, because the declines worth asking
	// about are exactly the ones where the match itself failed.
	if (explain && !region.decline.empty()) {
		Printer::Print("[factorize] declined: " + region.decline);
		// The rest, when there are more. The first line stays as it was so that
		// reading one reason still works; these are what turn the reason counts
		// into an answer to "what would this query need", which the first line
		// alone cannot give -- it names whichever check ran first, not the set
		// of things standing in the way.
		for (size_t i = 1; i < region.declines.size(); i++) {
			Printer::Print("[factorize] also: " + region.declines[i]);
		}
	}
	for (auto &child : op->children) {
		RewriteRecursive(context, child, gated, explain);
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
	RewriteRecursive(context, plan, mode == FactorizeMode::AUTO, ExplainRequested(context));
}

FactorizeOptimizerExtension::FactorizeOptimizerExtension() {
	// Post-builtin, so DuckDB's join order and estimated_cardinality annotations
	// are already in place (plan §3.2).
	optimize_function = Optimize;
}

void FactorizeOptimizerExtension::Register(DBConfig &config) {
	OptimizerExtension::Register(config, FactorizeOptimizerExtension());

	// The default stays 'off' while 'auto' has no gate to consult: firing on
	// every matching shape is measurably a loss (FINDINGS F16), so an ungated
	// default would make the extension slower to install than not to.
	config.AddExtensionOption("factorize_mode",
	                          "Factorized execution: 'off', 'auto' (fire when the cost gate predicts a win) or "
	                          "'force' (fire whenever the plan shape matches, ignoring the gate; benchmarking only)",
	                          LogicalType::VARCHAR, Value("off"));
	// The margin, not a compression ratio: speedup is compression times a
	// per-record factor that spans 84x across datasets, so no threshold on
	// compression alone is right for all of them (DECISIONS D14).
	config.AddExtensionOption("factorize_min_gain",
	                          "Fire only when factorizing is predicted to beat the stock plan by this factor",
	                          LogicalType::DOUBLE, Value::DOUBLE(1.5));
	// How far past its own size estimate the representation may grow before the
	// operator abandons and lets the replaced plan answer. The gate is a
	// prediction; this bounds the cost of it being wrong. 0 disables.
	//
	// 2 is measured rather than chosen. On the CE corpus the estimate is either
	// close (the three queries that win are within 2.5x, one of them over) or
	// hopeless (12x to 205x low, on every query that loses), with nothing in
	// between -- so a tight bound costs the winners nothing and catches every
	// observed misprediction. Measured at slack 1/2/4/8, the wins do not move at
	// all (0.49-0.53s for hetio_acyclic_222_16 at every setting) while the worst
	// loser goes 8.6s / 10.2s / 13.7s / 22.2s. 1 is better still on the losers;
	// 2 keeps a margin for ordinary estimate noise on a query that would win.
	config.AddExtensionOption("factorize_estimate_slack",
	                          "Abandon to the stock plan if the f-representation outgrows the gate's "
	                          "size estimate by more than this factor (0 disables)",
	                          LogicalType::DOUBLE, Value::DOUBLE(2.0));
	// The one check that consults no prediction. Every other number the gate
	// uses -- join sizes, record counts, DuckDB's own cost -- is computed
	// before the query runs, and on the CE corpus each has been wrong by orders
	// of magnitude on exactly the queries that lose. This one is read off the
	// representation after a join has built it, so it cannot be wrong about
	// what happened; it can only be wrong about what happens next.
	//
	// Off by default, and that is a measurement rather than caution.
	//
	// Swept over the 12 queries the gate fires on, 0.6 was the best floor and a
	// real improvement: 18.68s against 21.66s stock, where firing with no
	// run-time check costs 24.70s. Then it was run against the 248 queries the
	// CE benchmark *disables* for exceeding 1e9 result tuples -- the regime D15
	// says this engine is actually for, and the one no floor had ever seen. It
	// abandoned 17 of them. Every one is a query DuckDB does not finish inside
	// 180 seconds, so each abandonment trades about a second for never.
	//
	// Their compressions run 0.378 to 0.600, and the in-sample loss the floor
	// was worth catching -- watdiv_217_01, +5.4s -- sits at 0.403, inside that
	// range. So no threshold on compression alone separates them, and the 0.6
	// that looked so clean was a 6%-wide coincidence in a sample of twelve
	// (D38). The mechanism stays, exposed and tested; the number does not,
	// until a rule exists that survives the population it was not fitted on.
	//
	// Under 1, which is the part that is not intuitive: a record holding less
	// than one tuple is normal early on, because records grow by a sum over
	// joins while tuples grow by a product, and the product has not overtaken
	// yet. A floor set where "compression" sounds like it should be abandons
	// everything.
	config.AddExtensionOption("factorize_min_compression",
	                          "Abandon to the stock plan when a materialized join leaves fewer than this many "
	                          "tuples per record, measured rather than predicted (0 disables)",
	                          LogicalType::DOUBLE, Value::DOUBLE(0.0));
	config.AddExtensionOption("factorize_min_work_ms",
	                          "Fire only when DuckDB's own predicted work, excluding its fixed startup, exceeds "
	                          "this many milliseconds; below it there is nothing to win",
	                          LogicalType::DOUBLE, Value::DOUBLE(10.0));
	config.AddExtensionOption("factorize_explain",
	                          "Print, per aggregate, whether the factorize rule took the plan over and why not",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));
	// Fault injection, and it earns its place rather than being a convenience.
	// The fallback of plan §7.5 is an error path, and an error path that cannot
	// be reached on demand cannot be tested -- which is how a scan guard shipped
	// in this project having never once fired (DECISIONS D25, D26). The only
	// natural trigger is a NULL the statistics did not predict, which needs a
	// grouped query and an open transaction, so without this the fallback would
	// be exercised for exactly one shape out of five.
	config.AddExtensionOption("factorize_fallback",
	                          "Run the replaced plan when the factorized path fails, instead of failing the query; "
	                          "costs the parallelism of the factorized count (plan section 7.5)",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	config.AddExtensionOption("factorize_debug_fail",
	                          "Make the factorized path throw, to exercise the fallback to the stock plan",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));
	config.AddExtensionOption("factorize_debug_print_plan",
	                          "Print the post-optimizer logical plan seen by the factorize rule", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(false));
}

} // namespace duckdb
