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

static bool BooleanSetting(ClientContext &context, const char *name) {
	Value v;
	if (!context.TryGetCurrentSetting(name, v) || v.IsNull()) {
		return false;
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
struct FactorizedRegion {
	//! Why this subtree was turned down, for factorize_explain.
	//!
	//! The matcher declines constantly and silently by design, which means the
	//! default experience of a misbehaving rule is nothing visibly happening.
	//! This exists because that cost a whole rebuild cycle to diagnose once.
	string decline;
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
	//! Set for `GROUP BY g, count(*)`. The grouped answer has two columns and
	//! they live under two different table indexes: DuckDB gives an aggregate
	//! one for its groups and another for its aggregates.
	bool grouped = false;
	idx_t group_index = 0;
	ColumnBinding group_binding;
	LogicalType group_type;
	//! Which fold the aggregate asks for, and for sum, which column.
	factorize::Aggregate aggregate = factorize::Aggregate::COUNT;
	ColumnBinding sum_binding;
	LogicalType sum_type;
};

//! Records why a subtree was turned down and declines it. Always returns false,
//! so every rejection site reads as `return Decline(region, "...")`.
static bool Decline(FactorizedRegion &region, string reason) {
	region.decline = std::move(reason);
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
	if (join.join_type != JoinType::INNER) {
		return Decline(region, "join is " + JoinTypeToString(join.join_type) + ", not INNER");
	}
	if (!join.duplicate_eliminated_columns.empty()) {
		// Delim joins carry correlated-subquery machinery the region cannot model.
		return Decline(region, "join is duplicate-eliminated");
	}
	if (join.predicate) {
		// An ON-clause restriction that references only one side. It filters the
		// join's output and nothing outside the region would apply it.
		return Decline(region, "join carries an ON-clause filter");
	}
	if (join.conditions.empty() || join.children.size() != 2) {
		return Decline(region, "join has no conditions");
	}
	for (auto &cond : join.conditions) {
		if (cond.comparison != ExpressionType::COMPARE_EQUAL) {
			return Decline(region, "join condition is not an equality");
		}
		// A cast, or any other computed expression, would have to be evaluated
		// on values the f-representation holds in packed integer slots. Only a
		// bare column reference maps onto one.
		if (cond.left->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
		    cond.right->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return Decline(region, "join key is computed, not a plain column");
		}
		region.edges.emplace_back(cond.left->Cast<BoundColumnRefExpression>().binding,
		                          cond.right->Cast<BoundColumnRefExpression>().binding);
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
			return Decline(region, "compressed materialization is in the way; "
			                       "SET disabled_optimizers='compressed_materialization' to factorize this");
		}
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			// Any other computed expression would have to be evaluated on
			// flattened tuples, which the sealed island does not produce.
			return Decline(region, "projection computes an expression");
		}
	}
	if (proj.children.size() != 1) {
		return Decline(region, "projection has no single child");
	}
	return MatchProjections(*proj.children[0], region);
}

//! Accepts an ungrouped COUNT(*) -- v1's only aggregate (plan §1.1). The
//! semiring generalisation (§4.5) widens this to sum/min/max/avg later.
static bool MatchAggregate(LogicalOperator &op, FactorizedRegion &region) {
	if (op.type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return false;
	}
	auto &aggr = op.Cast<LogicalAggregate>();
	if (!aggr.grouping_functions.empty() || aggr.grouping_sets.size() > 1) {
		// GROUPING SETS / ROLLUP ask for several groupings at once.
		return Decline(region, "aggregate has grouping sets");
	}
	if (aggr.groups.size() > 1) {
		// Several keys need the cross product of independent branches when they
		// are scattered, which is a different algorithm (plan §10.1).
		return Decline(region, "aggregate groups on more than one column");
	}
	if (aggr.groups.size() == 1) {
		// One grouping key is the case the representation answers directly: it
		// sits at the top of the f-tree, so each root record is a group and the
		// tuples under it are a number already memoized.
		if (aggr.groups[0]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return Decline(region, "aggregate groups on a computed expression");
		}
		region.grouped = true;
		region.group_index = aggr.group_index;
		region.group_binding = aggr.groups[0]->Cast<BoundColumnRefExpression>().binding;
		region.group_type = aggr.groups[0]->return_type;
	}
	if (aggr.expressions.size() != 1) {
		return Decline(region, "aggregate computes more than one value");
	}
	auto &expr = *aggr.expressions[0];
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return Decline(region, "aggregate expression is not an aggregate");
	}
	region.aggregate_index = aggr.aggregate_index;
	auto &bound = expr.Cast<BoundAggregateExpression>();
	if (bound.IsDistinct() || bound.filter || bound.order_bys) {
		return Decline(region, "aggregate is DISTINCT, FILTERed or ORDERed");
	}
	auto name = StringUtil::Lower(bound.function.name);
	if (name == "sum") {
		// Summing is the semiring generalisation the plan always had as the
		// widening step (§4.5), and on TPC-DS it is the aggregate that actually
		// appears: 193 occurrences over inner-only join graphs against 24 for
		// count(*). The column has to be a plain column of the region, since it
		// is folded through the representation rather than evaluated.
		if (bound.children.size() != 1 ||
		    bound.children[0]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return Decline(region, "sum() of a computed expression");
		}
		region.aggregate = factorize::Aggregate::SUM;
		region.sum_binding = bound.children[0]->Cast<BoundColumnRefExpression>().binding;
		region.sum_type = expr.return_type;
	} else if (name != "count_star" && !(name == "count" && bound.children.empty())) {
		return Decline(region, "aggregate is " + name + "(), not count(*) or sum()");
	}
	if (aggr.children.size() != 1) {
		return Decline(region, "aggregate has no single child");
	}
	if (!MatchProjections(*aggr.children[0], region)) {
		return false;
	}
	if (region.relations.size() < 2 || region.edges.empty()) {
		// A single relation has nothing to factorize.
		return Decline(region, "fewer than two joined relations");
	}
	return true;
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
static bool BindRegion(FactorizedRegion &region, vector<BoundRelation> &relations, factorize::QueryGraph &graph,
                       size_t &group_relation, size_t &group_column, size_t &sum_relation, size_t &sum_column) {
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

	// Carry over every restriction the plan placed on each scan: the filters
	// DuckDB pushed into the scan, and any it left in a filter above it.
	// Dropping one would count rows the stock plan never sees, and replaying it
	// by hand would be a second implementation of DuckDB's filter semantics
	// judged against a count that has to match DuckDB's exactly. Re-keying them
	// onto our own scan and letting the storage layer apply them is neither.
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

	if (region.aggregate == factorize::Aggregate::SUM) {
		// The summed column is the first thing the region reads that is not a
		// join key, so unlike the grouping key it is *added* to the scan rather
		// than required to be there already. Appended, never inserted: the
		// predicates above hold local column indices, and inserting would move
		// the ground under them.
		auto found = relation_of_table_index.find(region.sum_binding.table_index);
		if (found == relation_of_table_index.end()) {
			return Decline(region, "summed column belongs to no relation of the region");
		}
		auto &get = region.relations[found->second].get();
		auto &column_ids = get.GetColumnIds();
		if (region.sum_binding.column_index >= column_ids.size()) {
			return Decline(region, "summed column is not among the scanned columns");
		}
		auto &column_index = column_ids[region.sum_binding.column_index];
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
		sum_relation = found->second;
		sum_column = static_cast<size_t>(bound.LocalIndex(physical));
	}

	if (region.grouped) {
		// The grouping key has to be a column the region reads, and it only
		// reads join columns: anything else was never scanned, because it cannot
		// change a count. That is also exactly the condition ExecuteGroupCount
		// needs -- a key that is not a join key is not at the top of the f-tree.
		auto found = relation_of_table_index.find(region.group_binding.table_index);
		if (found == relation_of_table_index.end()) {
			return Decline(region, "grouping column belongs to no relation of the region");
		}
		auto &get = region.relations[found->second].get();
		auto &column_ids = get.GetColumnIds();
		if (region.group_binding.column_index >= column_ids.size()) {
			return Decline(region, "grouping column is not among the scanned columns");
		}
		auto &column_index = column_ids[region.group_binding.column_index];
		if (column_index.IsRowIdColumn() || column_index.IsVirtualColumn() || column_index.HasChildren()) {
			return Decline(region, "grouping column is not a stored scalar column");
		}
		auto &definition = get.GetTable()->GetColumn(LogicalIndex(column_index.GetPrimaryIndex()));
		const auto physical = static_cast<idx_t>(definition.Physical().index);
		const auto local = relations[found->second].LocalIndex(physical);
		if (local < 0) {
			return Decline(region, "grouping column " + definition.Name() + " is not a join column of this query");
		}
		group_relation = found->second;
		group_column = static_cast<size_t>(local);
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
                       const factorize::QueryGraph &graph, const factorize::Plan &plan, string &reason) {
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
		size_t group_relation = 0;
		size_t group_column = 0;
		size_t sum_relation = 0;
		size_t sum_column = 0;
		if (BindRegion(region, relations, graph, group_relation, group_column, sum_relation, sum_column)) {
			// The join order is decided here rather than at execution: a graph
			// the planner cannot order (a disconnected one, most of all) has to
			// be a decline, not a query that fails halfway through running.
			auto plan = factorize::BuildPlan(graph);
			if (!plan.complete) {
				region.decline = plan.reason;
			} else {
				string gate_reason;
				// FORCE skips this and only this: the matcher's refusals are
				// about what the engine can compute at all, while the gate is
				// about whether computing it that way is a good idea.
				const bool fire = !gated || GateAgrees(context, region, relations, graph, plan, gate_reason);
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
					replacement->group_type = region.group_type;
					replacement->group_relation = group_relation;
					replacement->group_column = group_column;
					replacement->aggregate = region.aggregate;
					replacement->sum_relation = sum_relation;
					replacement->sum_column = sum_column;
					replacement->sum_type = region.sum_type.id() == LogicalTypeId::INVALID ? LogicalType::HUGEINT : region.sum_type;
					replacement->estimated_cardinality = 1;
					replacement->ResolveOperatorTypes();
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
	config.AddExtensionOption("factorize_min_work_ms",
	                          "Fire only when DuckDB's own predicted work, excluding its fixed startup, exceeds "
	                          "this many milliseconds; below it there is nothing to win",
	                          LogicalType::DOUBLE, Value::DOUBLE(10.0));
	config.AddExtensionOption("factorize_explain",
	                          "Print, per aggregate, whether the factorize rule took the plan over and why not",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));
	config.AddExtensionOption("factorize_debug_print_plan",
	                          "Print the post-optimizer logical plan seen by the factorize rule", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(false));
}

} // namespace duckdb
