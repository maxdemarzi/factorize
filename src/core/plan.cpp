#include "plan.hpp"

#include <map>
#include <set>
#include <stdexcept>

namespace factorize {

namespace {

//! Attribute ids are assigned by walking relations in order, so a relation's
//! columns occupy a contiguous range. Callers only ever see the composed id.
size_t AttributeBase(const QueryGraph &graph, size_t relation) {
	size_t base = 0;
	for (size_t i = 0; i < relation && i < graph.column_counts.size(); i++) {
		base += graph.column_counts[i];
	}
	return base;
}

size_t TotalAttributes(const QueryGraph &graph) {
	size_t total = 0;
	for (size_t count : graph.column_counts) {
		total += count;
	}
	return total;
}

} // namespace

AttributeId AttributeOf(const QueryGraph &graph, size_t relation, size_t column) {
	return static_cast<AttributeId>(AttributeBase(graph, relation) + column);
}

EquivalenceClasses::EquivalenceClasses(const QueryGraph &graph) {
	parent.resize(TotalAttributes(graph));
	for (size_t i = 0; i < parent.size(); i++) {
		parent[i] = i;
	}
	for (const auto &predicate : graph.predicates) {
		size_t a = Find(AttributeOf(graph, predicate.left_relation, static_cast<size_t>(predicate.left_column)));
		size_t b = Find(AttributeOf(graph, predicate.right_relation, static_cast<size_t>(predicate.right_column)));
		if (a != b) {
			parent[a] = b;
		}
	}
}

size_t EquivalenceClasses::Find(size_t attribute) {
	while (parent[attribute] != attribute) {
		parent[attribute] = parent[parent[attribute]];
		attribute = parent[attribute];
	}
	return attribute;
}

bool EquivalenceClasses::SameClass(AttributeId a, AttributeId b) {
	return Find(a) == Find(b);
}

AttributeId ShallowestEquivalent(EquivalenceClasses &classes, const FTree &tree, AttributeId key) {
	AttributeId best = key;
	int best_depth = tree.DepthOfAttribute(key);
	for (size_t candidate = 0; candidate < classes.Size(); candidate++) {
		const auto attribute = static_cast<AttributeId>(candidate);
		if (attribute == key || !classes.SameClass(attribute, key)) {
			continue;
		}
		const int depth = tree.DepthOfAttribute(attribute);
		if (depth < 0) {
			continue; // not joined yet
		}
		if (best_depth < 0 || depth < best_depth) {
			best = attribute;
			best_depth = depth;
		}
	}
	return best;
}

//! Every f-tree/materialize traversal downstream of a plan (ftree.cpp,
//! materialize.cpp) is plain C++ recursion, one stack frame per tree level,
//! with no depth cap of its own -- a stack overflow crashes the whole process,
//! not just the offending query, which is a worse failure mode than any
//! std::runtime_error this codebase otherwise uses for bad input. Tree depth
//! is bounded by the number of relations in the worst case (a pure chain, no
//! equality propagation flattening it into a shallower star), so bounding
//! relation count here protects every downstream traversal at the one place
//! every caller's query already passes through, without touching each
//! traversal individually. 500 is generous relative to any real workload
//! (FINDINGS' own bytes-per-record table tops out at 12 relations) and
//! conservative relative to a 1MB thread stack even allowing several stacked
//! recursive calls per tree level.
static constexpr size_t kMaxRelations = 500;

Plan BuildPlan(const QueryGraph &graph) {
	if (graph.RelationCount() > kMaxRelations) {
		Plan plan;
		plan.reason = "too many relations (" + std::to_string(graph.RelationCount()) + " > " +
		              std::to_string(kMaxRelations) + "); f-tree traversals recurse per level with no depth cap";
		return plan;
	}
	Plan plan;
	std::set<size_t> joined;
	std::vector<bool> used(graph.predicates.size(), false);
	EquivalenceClasses classes(graph);

	// A relation can only attach where every edge carrying it into the joined set
	// reaches attributes that end up on *one* f-tree level, and section 4.3
	// guarantees that happens exactly when the already-joined side of each of
	// those edges shares an equivalence class. A triangle is the smallest graph
	// that fails: its third relation reaches the other two through two different
	// classes at once.
	//
	// This is checked here rather than left to the engine because the engine's
	// answer is an exception thrown partway through a running query
	// ("key attributes did not converge on one level"). Planning is where a
	// caller can still do something about it -- the table function reports which
	// query it cannot run, and the optimizer rule quietly leaves the stock plan
	// alone.
	auto converges = [&](const std::vector<Predicate> &edges) {
		bool have_first = false;
		AttributeId first = 0;
		for (const auto &edge : edges) {
			const bool left_joined = joined.count(edge.left_relation) > 0;
			const auto attribute =
			    left_joined ? AttributeOf(graph, edge.left_relation, static_cast<size_t>(edge.left_column))
			                : AttributeOf(graph, edge.right_relation, static_cast<size_t>(edge.right_column));
			if (!have_first) {
				first = attribute;
				have_first = true;
			} else if (!classes.SameClass(first, attribute)) {
				return false;
			}
		}
		return true;
	};

	// Seed with the relation carrying the first predicate.
	const size_t seed = graph.predicates.empty() ? 0 : graph.predicates[0].left_relation;
	joined.insert(seed);
	plan.steps.push_back(PlanStep {seed, {}});

	while (joined.size() < graph.RelationCount()) {
		std::map<size_t, std::vector<Predicate>> candidates;
		for (size_t i = 0; i < graph.predicates.size(); i++) {
			if (used[i]) {
				continue;
			}
			const auto &predicate = graph.predicates[i];
			const bool left_in = joined.count(predicate.left_relation) > 0;
			const bool right_in = joined.count(predicate.right_relation) > 0;
			if (left_in == right_in) {
				continue; // both or neither joined
			}
			candidates[left_in ? predicate.right_relation : predicate.left_relation].push_back(predicate);
		}
		if (candidates.empty()) {
			plan.reason = "join graph is disconnected";
			return plan;
		}
		// A candidate whose edges do not converge is skipped rather than refused:
		// another relation may attach cleanly now, and the residual-predicate
		// check below still catches an edge that never gets used.
		auto best = candidates.end();
		for (auto it = candidates.begin(); it != candidates.end(); ++it) {
			if (!converges(it->second)) {
				continue;
			}
			if (best == candidates.end() || it->second.size() > best->second.size()) {
				best = it;
			}
		}
		if (best == candidates.end()) {
			plan.reason = "cyclic join graph: no relation attaches on a single key";
			return plan;
		}
		plan.steps.push_back(PlanStep {best->first, best->second});
		joined.insert(best->first);
		for (size_t i = 0; i < graph.predicates.size(); i++) {
			const auto &predicate = graph.predicates[i];
			if (used[i]) {
				continue;
			}
			const bool covers = (predicate.left_relation == best->first && joined.count(predicate.right_relation)) ||
			                    (predicate.right_relation == best->first && joined.count(predicate.left_relation));
			if (covers) {
				used[i] = true;
			}
		}
	}

	for (size_t i = 0; i < graph.predicates.size(); i++) {
		if (!used[i]) {
			// A predicate between two already-joined relations is a selection on
			// the f-representation, which v1 does not implement.
			plan.reason = "residual predicate needs a selection operator";
			return plan;
		}
	}
	plan.complete = true;
	return plan;
}

std::vector<CostStep> BuildCostSteps(const QueryGraph &graph, const Plan &plan, RelationSource &source) {
	EquivalenceClasses classes(graph);
	std::vector<CostStep> steps;

	// Which column each relation joins on. For every relation but the first
	// this is the column its own attaching edge names; the first has no
	// attaching edge, so it takes the first edge that mentions it.
	std::map<size_t, size_t> join_column;
	for (size_t i = 1; i < plan.steps.size(); i++) {
		for (const auto &edge : plan.steps[i].edges) {
			join_column.emplace(edge.left_relation, static_cast<size_t>(edge.left_column));
			join_column.emplace(edge.right_relation, static_cast<size_t>(edge.right_column));
		}
	}

	// Mirrors the engine exactly, including equality propagation: a relation
	// attaches beneath the *shallowest* already-joined attribute of its
	// equivalence class, not beneath whichever relation the predicate happens to
	// name. That choice turns a chain into a star, and a star is the only shape
	// that compresses.
	std::map<size_t, int> depth_of;
	std::map<size_t, size_t> step_of;

	const size_t seed_relation = plan.steps[0].relation;
	const size_t seed_column = join_column.count(seed_relation) ? join_column[seed_relation] : 0;
	CostStep first;
	first.key = source.Stats(seed_relation, seed_column);
	first.key_group = static_cast<int>(classes.Find(AttributeOf(graph, seed_relation, seed_column)));
	steps.push_back(first);
	depth_of[seed_relation] = 0;
	step_of[seed_relation] = 0;

	for (size_t i = 1; i < plan.steps.size(); i++) {
		const auto &step = plan.steps[i];
		const auto &edge = step.edges.front();
		const bool left_is_new = edge.left_relation == step.relation;
		const auto new_column = static_cast<size_t>(left_is_new ? edge.left_column : edge.right_column);
		const auto named_relation = left_is_new ? edge.right_relation : edge.left_relation;
		const auto named_column = static_cast<size_t>(left_is_new ? edge.right_column : edge.left_column);
		const auto named_attribute = AttributeOf(graph, named_relation, named_column);

		size_t attach_relation = named_relation;
		size_t attach_column = named_column;
		int attach_depth = depth_of.count(named_relation) ? depth_of[named_relation] : 0;
		for (size_t owner = 0; owner < graph.RelationCount(); owner++) {
			if (!depth_of.count(owner)) {
				continue;
			}
			for (size_t column = 0; column < graph.column_counts[owner]; column++) {
				if (!classes.SameClass(AttributeOf(graph, owner, column), named_attribute)) {
					continue;
				}
				if (depth_of[owner] < attach_depth) {
					attach_depth = depth_of[owner];
					attach_relation = owner;
					attach_column = column;
				}
			}
		}

		CostStep entry;
		entry.key = source.Stats(step.relation, new_column);
		entry.key_group = static_cast<int>(classes.Find(AttributeOf(graph, step.relation, new_column)));
		entry.parent_key = source.Stats(attach_relation, attach_column);
		entry.parent_step = static_cast<int>(step_of[attach_relation]);
		steps.push_back(entry);

		depth_of[step.relation] = attach_depth + 1;
		step_of[step.relation] = steps.size() - 1;
	}
	return steps;
}

ExecuteResult ExecuteCount(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                           PathStrategy strategy) {
	ExecuteResult result;
	if (!plan.complete) {
		result.error = plan.reason.empty() ? "no plan" : plan.reason;
		return result;
	}
	try {
		if (graph.column_types.size() != graph.RelationCount()) {
			throw std::runtime_error("QueryGraph::column_types must have one entry per relation");
		}
		AttributeTypes types;
		for (size_t relation = 0; relation < graph.RelationCount(); relation++) {
			if (graph.column_types[relation].size() != graph.column_counts[relation]) {
				throw std::runtime_error("QueryGraph::column_types[" + std::to_string(relation) +
				                         "] must have one entry per column");
			}
			for (size_t column = 0; column < graph.column_counts[relation]; column++) {
				types.emplace_back(AttributeOf(graph, relation, column), graph.column_types[relation][column]);
			}
		}

		auto make = [&](size_t relation) {
			std::vector<AttributeId> attributes;
			for (size_t column = 0; column < graph.column_counts[relation]; column++) {
				attributes.push_back(AttributeOf(graph, relation, column));
			}
			return MakeScan(attributes, types, source.Columns(relation));
		};

		EquivalenceClasses classes(graph);
		int64_t fused_count = -1;
		auto accumulated = make(plan.steps[0].relation);

		for (size_t i = 1; i < plan.steps.size(); i++) {
			const auto &step = plan.steps[i];
			// The accumulated tree must stay on top either way, otherwise the
			// new relation becomes the root and every previous one nests
			// beneath it -- a chain, in which nothing is independent.
			//
			// Top-insert puts the *probe* on top, bottom-insert the *build*, so
			// the two modes take their arguments swapped. That is the paper's
			// L (top-insert) R == R (bottom-insert) L: the shape is the same,
			// and what differs is which side gets the hash table.
			JoinKeys keys;
			std::vector<AttributeId> new_keys;
			std::vector<AttributeId> accumulated_keys;
			for (const auto &edge : step.edges) {
				const bool left_is_new = edge.left_relation == step.relation;
				new_keys.push_back(AttributeOf(graph, left_is_new ? edge.left_relation : edge.right_relation,
				                               static_cast<size_t>(left_is_new ? edge.left_column : edge.right_column)));
				const auto raw =
				    AttributeOf(graph, left_is_new ? edge.right_relation : edge.left_relation,
				                static_cast<size_t>(left_is_new ? edge.right_column : edge.left_column));
				accumulated_keys.push_back(ShallowestEquivalent(classes, accumulated.Tree(), raw));
			}

			// The aggregate is the topmost operator, so the final join's output
			// exists only to be counted (sections 4.2.2 and 4.5).
			const bool last_join = (i + 1 == plan.steps.size());
			if (mode == JoinMode::TOP_INSERT) {
				keys.build = new_keys;
				keys.probe = accumulated_keys;
				if (last_join) {
					fused_count = FactorizedCountJoin(make(step.relation), accumulated, keys, mode, strategy);
				} else {
					accumulated = FactorizedJoin(make(step.relation), accumulated, keys, mode, strategy);
				}
			} else {
				keys.build = accumulated_keys;
				keys.probe = new_keys;
				if (last_join) {
					fused_count = FactorizedCountJoin(accumulated, make(step.relation), keys, mode, strategy);
				} else {
					accumulated = FactorizedJoin(accumulated, make(step.relation), keys, mode, strategy);
				}
			}
		}

		result.count = fused_count >= 0 ? fused_count : accumulated.Count();
		result.records = accumulated.Rep().RecordCount();
		result.bytes = accumulated.Rep().BytesAllocated();
		result.ok = true;
	} catch (const std::exception &error) {
		result.error = error.what();
	}
	return result;
}

} // namespace factorize
