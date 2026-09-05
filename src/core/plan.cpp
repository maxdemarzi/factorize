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

//! Every attribute of one relation, in column order -- the node a flat scan of
//! it becomes (FTree::Scan builds a single node holding all of them).
std::vector<AttributeId> AttributesOf(const QueryGraph &graph, size_t relation) {
	std::vector<AttributeId> attributes;
	const auto count = relation < graph.column_counts.size() ? graph.column_counts[relation] : 0;
	attributes.reserve(count);
	for (size_t column = 0; column < count; column++) {
		attributes.push_back(AttributeOf(graph, relation, column));
	}
	return attributes;
}

//! The storage width of one attribute, found by walking relations in order --
//! the inverse of AttributeOf, needed because the join measures key widths over
//! *attributes* and equality propagation can hand back an attribute belonging to
//! a different relation than the predicate named.
ValueType TypeOfAttribute(const QueryGraph &graph, AttributeId attribute) {
	size_t base = 0;
	for (size_t relation = 0; relation < graph.column_counts.size(); relation++) {
		const auto count = graph.column_counts[relation];
		if (attribute < base + count) {
			const size_t column = static_cast<size_t>(attribute) - base;
			if (relation < graph.column_types.size() && column < graph.column_types[relation].size()) {
				return graph.column_types[relation][column];
			}
			break;
		}
		base += count;
	}
	// Unknown width is charged as the wider of the two, so an attribute the
	// graph does not describe cannot smuggle a key past the cap.
	return ValueType::INT64;
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

EquivalenceClasses::EquivalenceClasses(const QueryGraph &graph, Enforced) {
	parent.resize(TotalAttributes(graph));
	for (size_t i = 0; i < parent.size(); i++) {
		parent[i] = i;
	}
}

void EquivalenceClasses::Enforce(const QueryGraph &graph, const Predicate &predicate) {
	const size_t a = Find(AttributeOf(graph, predicate.left_relation, static_cast<size_t>(predicate.left_column)));
	const size_t b = Find(AttributeOf(graph, predicate.right_relation, static_cast<size_t>(predicate.right_column)));
	if (a != b) {
		parent[a] = b;
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
	// Only equalities a join has already applied. See EquivalenceClasses.
	EquivalenceClasses classes(graph, Enforced {});

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
	// The tree the plan will actually build, simulated as it is planned.
	//
	// An FTree is pure structure over attribute ids -- no data -- so the planner
	// can construct the very tree the engine will, and ask it the real question
	// instead of a proxy for it. That matters because the proxy was wrong in one
	// direction: sharing an equivalence class implies landing on one node, but
	// not the reverse, and every composite-key join lives in the gap. `A.k1 =
	// B.k1 AND A.k2 = B.k2` is what every composite foreign key writes, and it
	// was refused project-wide as "cyclic" -- which it is not.
	FTree simulated = FTree::Scan(AttributesOf(graph, graph.predicates.empty() ? 0 : graph.predicates[0].left_relation));

	// The accumulated side's key attributes, as the join will name them: mapped
	// through equality propagation against the tree as it stands.
	auto accumulated_keys_for = [&](const std::vector<Predicate> &edges, size_t attaching) {
		std::vector<AttributeId> keys;
		for (const auto &edge : edges) {
			const bool left_is_new = edge.left_relation == attaching;
			const auto raw = AttributeOf(graph, left_is_new ? edge.right_relation : edge.left_relation,
			                             static_cast<size_t>(left_is_new ? edge.right_column : edge.left_column));
			keys.push_back(ShallowestEquivalent(classes, simulated, raw));
		}
		return keys;
	};

	// MakeKeyReader requires every key to be readable from ONE node, and throws
	// "key attributes did not converge on one level" otherwise -- mid-query,
	// where a caller can do nothing about it. So the same question is asked
	// here, by performing the transformation the join performs and looking at
	// where the keys ended up.
	//
	// LEVELWISE rather than NAIVE deliberately: NAIVE collapses every required
	// node into the root, so its keys always converge, which makes LEVELWISE the
	// stricter of the two and a plan accepted here safe under either.
	auto converges = [&](const std::vector<Predicate> &edges, size_t attaching) {
		const auto keys = accumulated_keys_for(edges, attaching);
		if (keys.size() < 2) {
			return true;
		}
		FTree probe_shape(simulated);
		const FNode &insertion = probe_shape.CreateRootToLeafPath(keys, PathStrategy::LEVELWISE);
		for (auto key : keys) {
			if (!insertion.HasAttribute(key)) {
				return false;
			}
		}
		return true;
	};

	// The packed composite key is one 64-bit word, so a key set wider than that
	// cannot be built at all (join.cpp: "composite key wider than 64 bits").
	// Refused here rather than thrown there, for the same reason as above.
	auto fits_key_width = [&](const std::vector<Predicate> &edges, size_t attaching) {
		// MakeKeyReader is built once per side and caps each at 64 bits, so both
		// sides are measured: a key can be INT32 on one relation and INT64 on
		// the other, and only the wider side would fail.
		//
		// The rule is the *sum* of the key columns' widths, not a count of them
		// and not "no INT64": one INT64 key fits at exactly 64, and INT32+INT64
		// is 96 and does not. That pairing -- a small discriminator beside a
		// bigint id -- is the common composite key in real schemas, so a rule
		// written as "not two INT64s" would let precisely the frequent case
		// through to the mid-join throw this exists to prevent.
		//
		// The accumulated side is measured over the keys AFTER equality
		// propagation, because those are the attributes MakeKeyReader will read.
		// Charging the raw predicate columns refuses queries the engine computes
		// perfectly well: when two edges put an INT64 column and an INT32 column
		// in one class, ShallowestEquivalent hands the join the INT32 one and it
		// packs 64 bits, while the raw columns add to 96. Measured over 4000
		// random graphs, that mistake declined 21 that the old planner answered.
		//
		// The invariant, since this function has now been wrong three ways: it
		// must measure exactly what MakeKeyReader measures.
		unsigned new_bits = 0;
		for (const auto &edge : edges) {
			const bool left_is_new = edge.left_relation == attaching;
			new_bits += TypeOfAttribute(graph, AttributeOf(graph, left_is_new ? edge.left_relation : edge.right_relation,
			                                               static_cast<size_t>(left_is_new ? edge.left_column
			                                                                               : edge.right_column))) ==
			                    ValueType::INT32
			                ? 32u
			                : 64u;
		}
		unsigned joined_bits = 0;
		for (auto key : accumulated_keys_for(edges, attaching)) {
			joined_bits += TypeOfAttribute(graph, key) == ValueType::INT32 ? 32u : 64u;
		}
		return new_bits <= 64u && joined_bits <= 64u;
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
		bool too_wide = false;
		for (auto it = candidates.begin(); it != candidates.end(); ++it) {
			// Both tested, always, so the reason reported is the one a reader
			// can act on. Setting `too_wide` only where convergence had already
			// passed hid it behind the node-placement message for a candidate
			// that failed both -- sending someone hunting for a cycle when the
			// fix is casting a BIGINT to INTEGER. Reporting the right decision
			// for the wrong reason is the defect this message exists to end.
			const bool fits = fits_key_width(it->second, it->first);
			if (!fits) {
				too_wide = true;
			}
			if (!fits || !converges(it->second, it->first)) {
				continue;
			}
			if (best == candidates.end() || it->second.size() > best->second.size()) {
				best = it;
			}
		}
		if (best == candidates.end()) {
			// Named for what was actually measured rather than for the usual
			// cause. A cyclic graph reaches this, and so did every composite-key
			// join until the check above stopped guessing -- calling those
			// cyclic was wrong twice over, since the graph is acyclic and the
			// engine can compute it.
			plan.reason = too_wide ? "join key is wider than the 64-bit packed key"
			                       : "no relation attaches on keys that land on one f-tree node "
			                         "(a cyclic join graph is the usual cause)";
			return plan;
		}
		// Advance the simulation exactly as the join will: the accumulated tree
		// is the upper one under either insert mode -- bottom-insert puts the
		// build on top and names it `build`, top-insert puts the probe on top
		// and names it `probe`, and the accumulated side is whichever that is --
		// so the shape does not depend on the mode, only the locking does.
		{
			JoinKeys keys;
			keys.build = accumulated_keys_for(best->second, best->first);
			keys.probe.clear();
			for (const auto &edge : best->second) {
				const bool left_is_new = edge.left_relation == best->first;
				keys.probe.push_back(AttributeOf(graph, left_is_new ? edge.left_relation : edge.right_relation,
				                                 static_cast<size_t>(left_is_new ? edge.left_column : edge.right_column)));
			}
			simulated = MergeTrees(simulated, FTree::Scan(AttributesOf(graph, best->first)), keys,
			                       JoinMode::BOTTOM_INSERT, PathStrategy::LEVELWISE);
		}
		plan.steps.push_back(PlanStep {best->first, best->second});
		joined.insert(best->first);
		// After the keys above were resolved, never before: this join is what
		// makes these equalities true, so resolving its own keys through them
		// would be assuming the conclusion.
		for (const auto &edge : best->second) {
			classes.Enforce(graph, edge);
		}
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

		// Enforced-only, and grown as the joins below apply their edges. The
		// planner accepted this graph under the same rule, so the two must agree
		// on what is substitutable or execution would resolve keys the plan
		// never validated.
		EquivalenceClasses classes(graph, Enforced {});
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
			// Applied by the join just issued, so substitutable from here on and
			// not before -- the same order BuildPlan validated the graph under.
			for (const auto &edge : step.edges) {
				classes.Enforce(graph, edge);
			}
		}

		result.count = fused_count >= 0 ? fused_count : accumulated.Count();
		result.records = accumulated.Rep().RecordCount();
		result.bytes = accumulated.Rep().BytesAllocated();
		result.ok = true;
	} catch (const MemoryLimitExceeded &error) {
		result.error = error.what();
		result.out_of_memory = true;
	} catch (const std::exception &error) {
		result.error = error.what();
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Slicing
//===--------------------------------------------------------------------===//

namespace {

//! Hands on one hash bucket of another source.
//!
//! Relations holding an attribute of the sliced class are filtered to the rows
//! whose key falls in the bucket; the rest are passed through untouched, since
//! their rows can only join with keys that survived anyway.
class SlicedSource : public RelationSource {
public:
	SlicedSource(RelationSource &inner, std::vector<int> key_column, size_t slice, size_t slices)
	    : inner(inner), key_column(std::move(key_column)), slice(slice), slices(slices) {
	}

	const std::vector<std::vector<int64_t>> &Columns(size_t relation) override {
		const auto &columns = inner.Columns(relation);
		if (key_column[relation] < 0) {
			return columns;
		}
		const auto &keys = columns[static_cast<size_t>(key_column[relation])];
		held.assign(columns.size(), {});
		for (size_t row = 0; row < keys.size(); row++) {
			if (HashKey(static_cast<uint64_t>(keys[row])) % slices != slice) {
				continue;
			}
			// Kept, and every column of the row is kept with it: the row
			// alignment MakeScan depends on is as load-bearing here as it is in
			// the scan.
			for (size_t column = 0; column < columns.size(); column++) {
				held[column].push_back(columns[column][row]);
			}
		}
		return held;
	}

	ColumnStats Stats(size_t relation, size_t column) override {
		return inner.Stats(relation, column);
	}

private:
	RelationSource &inner;
	//! The column of each relation to bucket on, or -1 to pass the relation
	//! through whole.
	std::vector<int> key_column;
	size_t slice;
	size_t slices;
	std::vector<std::vector<int64_t>> held;
};

//! Picks the equivalence class to slice on: the one reaching the most
//! relations, since filtering those is what makes a pass small. Returns the
//! per-relation column to bucket on, or an empty vector if no class reaches
//! more than one relation, in which case slicing cannot help.
std::vector<int> ChooseSliceColumns(const QueryGraph &graph) {
	// The full closure is right here, and only here. Slicing partitions the
	// INPUT by hashing a column of each relation, and every relation whose
	// column is transitively equated must land in the same bucket or tuples that
	// join are separated and the count comes out short. That is a statement
	// about which rows can possibly match, which the whole graph decides -- not
	// about which substitutions a given join has earned.
	EquivalenceClasses classes(graph);
	std::map<size_t, std::vector<int>> by_class;
	std::map<size_t, size_t> reach;
	for (size_t relation = 0; relation < graph.RelationCount(); relation++) {
		for (size_t column = 0; column < graph.column_counts[relation]; column++) {
			const auto root = classes.Find(AttributeOf(graph, relation, column));
			auto &columns = by_class[root];
			if (columns.empty()) {
				columns.assign(graph.RelationCount(), -1);
			}
			if (columns[relation] < 0) {
				columns[relation] = static_cast<int>(column);
				reach[root]++;
			}
		}
	}
	size_t best = 0;
	size_t best_reach = 1;
	bool found = false;
	for (const auto &entry : reach) {
		if (entry.second > best_reach) {
			best = entry.first;
			best_reach = entry.second;
			found = true;
		}
	}
	if (!found) {
		return {};
	}
	return by_class[best];
}

} // namespace

ExecuteResult ExecuteCountSlice(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                size_t slice, size_t slices, PathStrategy strategy) {
	if (slices <= 1) {
		return ExecuteCount(graph, plan, source, mode, strategy);
	}
	auto key_column = ChooseSliceColumns(graph);
	if (key_column.empty()) {
		ExecuteResult result;
		result.error = "no join key reaches more than one relation, so slicing cannot shrink anything";
		return result;
	}
	SlicedSource sliced(source, key_column, slice, slices);
	auto result = ExecuteCount(graph, plan, sliced, mode, strategy);
	result.slices = slices;
	return result;
}

ExecuteResult ExecuteCountSliceWithinMemory(const QueryGraph &graph, const Plan &plan, RelationSource &source,
                                            JoinMode mode, size_t slice, size_t slices, PathStrategy strategy) {
	auto result = ExecuteCountSlice(graph, plan, source, mode, slice, slices, strategy);
	if (result.ok || !result.out_of_memory) {
		return result;
	}
	// Refine this bucket, and only this bucket. A modulus of `slices * factor`
	// splits bucket `slice` into the buckets congruent to it, and touches no
	// other bucket -- which is what makes this safe to do on one thread while
	// others are working on theirs.
	for (size_t factor = 8; factor <= 4096; factor *= 8) {
		const size_t finer = slices * factor;
		int64_t total = 0;
		size_t records = 0;
		size_t bytes = 0;
		bool fits = true;
		for (size_t part = 0; part < factor && fits; part++) {
			auto piece = ExecuteCountSlice(graph, plan, source, mode, slice + part * slices, finer, strategy);
			if (!piece.ok) {
				if (!piece.out_of_memory) {
					return piece;
				}
				fits = false;
				result = piece;
				break;
			}
			total = CheckedCardinalityAdd(total, piece.count);
			records = records > piece.records ? records : piece.records;
			bytes = bytes > piece.bytes ? bytes : piece.bytes;
		}
		if (fits) {
			ExecuteResult refined;
			refined.ok = true;
			refined.count = total;
			refined.records = records;
			refined.bytes = bytes;
			refined.slices = finer;
			return refined;
		}
	}
	return result;
}

ExecuteResult ExecuteCountSliced(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                 size_t slices, PathStrategy strategy) {
	ExecuteResult result;
	if (slices <= 1) {
		return ExecuteCount(graph, plan, source, mode, strategy);
	}

	int64_t total = 0;
	size_t records = 0;
	size_t bytes = 0;
	for (size_t slice = 0; slice < slices; slice++) {
		auto part = ExecuteCountSlice(graph, plan, source, mode, slice, slices, strategy);
		if (!part.ok) {
			part.slices = slices;
			return part;
		}
		total = CheckedCardinalityAdd(total, part.count);
		// The largest slice is what had to fit, so that is what is reported.
		records = records > part.records ? records : part.records;
		bytes = bytes > part.bytes ? bytes : part.bytes;
	}
	result.ok = true;
	result.count = total;
	result.records = records;
	result.bytes = bytes;
	result.slices = slices;
	return result;
}

namespace {

//! Runs every join in `plan`, fusing none, and hands back what they built.
//!
//! ExecuteCount fuses the last join because its output exists only to be
//! counted. Everything that wants the *result* -- tuples, groups -- needs the
//! representation to exist, so it needs this instead.
struct BuiltRelation {
	bool ok = false;
	std::string error;
	bool out_of_memory = false;
	std::unique_ptr<FactorizedRelation> relation;
	std::unique_ptr<EquivalenceClasses> classes;
};

BuiltRelation BuildRepresentation(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                  PathStrategy strategy) {
	BuiltRelation built;
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

		built.classes.reset(new EquivalenceClasses(graph, Enforced {}));
		auto accumulated = make(plan.steps[0].relation);

		for (size_t i = 1; i < plan.steps.size(); i++) {
			const auto &step = plan.steps[i];
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
				accumulated_keys.push_back(ShallowestEquivalent(*built.classes, accumulated.Tree(), raw));
			}
			if (mode == JoinMode::TOP_INSERT) {
				keys.build = new_keys;
				keys.probe = accumulated_keys;
				accumulated = FactorizedJoin(make(step.relation), accumulated, keys, mode, strategy);
			} else {
				keys.build = accumulated_keys;
				keys.probe = new_keys;
				accumulated = FactorizedJoin(accumulated, make(step.relation), keys, mode, strategy);
			}
			// After the join, for the same reason as everywhere else: an equality
			// becomes substitutable when a join has made it true, not when a
			// predicate has asked for it.
			for (const auto &edge : step.edges) {
				built.classes->Enforce(graph, edge);
			}
		}
		built.relation.reset(new FactorizedRelation(std::move(accumulated)));
		built.ok = true;
	} catch (const MemoryLimitExceeded &error) {
		built.error = error.what();
		built.out_of_memory = true;
	} catch (const std::exception &error) {
		built.error = error.what();
	}
	return built;
}

} // namespace

MaterializeResult ExecuteMaterialize(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                     size_t limit, PathStrategy strategy) {
	MaterializeResult result;
	if (!plan.complete) {
		result.error = plan.reason.empty() ? "no plan" : plan.reason;
		return result;
	}
	auto built = BuildRepresentation(graph, plan, source, mode, strategy);
	if (!built.ok) {
		result.error = built.error;
		result.out_of_memory = built.out_of_memory;
		return result;
	}
	try {
		auto &accumulated = *built.relation;
		auto &classes = *built.classes;

		// An equi-join's two key attributes are always equal, so the merge is
		// free to keep only one of them. A tuple still has to carry both, and
		// the value of the missing one is by definition the value of whichever
		// equivalent attribute survived.
		const auto &layout = accumulated.GetLayout();
		auto present = [&](AttributeId attribute) {
			for (size_t level = 0; level < layout.LevelCount(); level++) {
				for (const auto &entry : layout.Level(static_cast<LevelId>(level)).payload) {
					if (entry.attribute == attribute) {
						return true;
					}
				}
			}
			return false;
		};

		std::vector<TupleColumn> columns;
		size_t position = 0;
		for (size_t relation = 0; relation < graph.RelationCount(); relation++) {
			for (size_t column = 0; column < graph.column_counts[relation]; column++) {
				const auto wanted = AttributeOf(graph, relation, column);
				AttributeId source_attribute = wanted;
				if (!present(wanted)) {
					bool found = false;
					for (size_t other = 0; other < graph.RelationCount() && !found; other++) {
						for (size_t c = 0; c < graph.column_counts[other] && !found; c++) {
							const auto candidate = AttributeOf(graph, other, c);
							if (present(candidate) && classes.SameClass(candidate, wanted)) {
								source_attribute = candidate;
								found = true;
							}
						}
					}
					if (!found) {
						throw std::runtime_error("attribute " + std::to_string(wanted) +
						                         " is neither stored nor equal to anything stored");
					}
				}
				columns.push_back(TupleColumn {source_attribute, position});
				position++;
			}
		}

		Enumerate(accumulated.Rep(), columns, limit, [&](const std::vector<int64_t> &values) {
			result.tuples.push_back(values);
			return true;
		});
		result.records = accumulated.Rep().RecordCount();
		result.bytes = accumulated.Rep().BytesAllocated();
		result.ok = true;
	} catch (const MemoryLimitExceeded &error) {
		result.error = error.what();
		result.out_of_memory = true;
	} catch (const std::exception &error) {
		result.error = error.what();
	}
	return result;
}

MaterializeResult ExecuteMaterializeWithinMemory(const QueryGraph &graph, const Plan &plan, RelationSource &source,
                                                 JoinMode mode, size_t limit, PathStrategy strategy) {
	auto result = ExecuteMaterialize(graph, plan, source, mode, limit, strategy);
	if (result.ok || !result.out_of_memory) {
		return result;
	}
	auto key_column = ChooseSliceColumns(graph);
	if (key_column.empty()) {
		return result;
	}
	static const size_t kSliceSteps[] = {8, 64, 512, 4096};
	for (auto slices : kSliceSteps) {
		MaterializeResult gathered;
		bool fits = true;
		for (size_t slice = 0; slice < slices && fits; slice++) {
			SlicedSource sliced(source, key_column, slice, slices);
			// Each bucket only has to supply what the limit still wants.
			const size_t remaining = limit == 0 ? 0 : limit - gathered.tuples.size();
			auto part = ExecuteMaterialize(graph, plan, sliced, mode, remaining, strategy);
			if (!part.ok) {
				if (!part.out_of_memory) {
					return part;
				}
				fits = false;
				result = part;
				break;
			}
			for (auto &tuple : part.tuples) {
				gathered.tuples.push_back(std::move(tuple));
			}
			gathered.records = gathered.records > part.records ? gathered.records : part.records;
			gathered.bytes = gathered.bytes > part.bytes ? gathered.bytes : part.bytes;
			if (limit != 0 && gathered.tuples.size() >= limit) {
				// The prefix is complete; the remaining buckets are never built,
				// which is the whole point of asking for a limit.
				break;
			}
		}
		if (fits) {
			gathered.ok = true;
			return gathered;
		}
	}
	return result;
}

ExecuteResult ExecuteSum(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                         size_t sum_relation, size_t sum_column, PathStrategy strategy) {
	ExecuteResult result;
	if (!plan.complete) {
		result.error = plan.reason.empty() ? "no plan" : plan.reason;
		return result;
	}
	if (sum_relation >= graph.RelationCount() || sum_column >= graph.column_counts[sum_relation]) {
		result.error = "summed column is not a column of the query";
		return result;
	}
	// The last join cannot be fused: fusing counts the output as it goes, and a
	// sum needs the column's values, which only exist once the representation
	// does.
	auto built = BuildRepresentation(graph, plan, source, mode, strategy);
	if (!built.ok) {
		result.error = built.error;
		result.out_of_memory = built.out_of_memory;
		return result;
	}
	try {
		const auto &rep = built.relation->Rep();
		const auto attribute = AttributeOf(graph, sum_relation, sum_column);
		int64_t total = 0;
		int64_t tuples = 0;
		rep.ForEachRoot([&](Record root) {
			total = CheckedCardinalityAdd(total, rep.SubtreeSum(root, attribute));
			// Counted alongside, because a total of zero says nothing about
			// whether anything was summed, and SQL answers NULL when nothing was.
			tuples = CheckedCardinalityAdd(tuples, static_cast<int64_t>(rep.SubtreeSize(root)));
		});
		result.count = total;
		result.tuples = tuples;
		result.records = rep.RecordCount();
		result.bytes = rep.BytesAllocated();
		result.ok = true;
	} catch (const MemoryLimitExceeded &error) {
		result.error = error.what();
		result.out_of_memory = true;
	} catch (const std::exception &error) {
		result.error = error.what();
	}
	return result;
}

namespace {

//! Where each grouping column lives in the representation, and where its value
//! goes in the emitted row.
struct GroupSite {
	AttributeId attribute = 0;
	LevelId level = 0;
	size_t position = 0;
};

//! Groups a representation by several columns at once, without enumerating a
//! tuple.
//!
//! The single-key case is easy because a root record *is* a group. Several keys
//! are not, and the plan (§10.1) expects the hard shape to be declined: keys
//! scattered across independent sibling branches need the cross product of
//! those branches. But that cross product is exactly what the representation is
//! made of -- siblings are independent, which is the entire premise -- so the
//! branches can be walked together rather than flattened.
//!
//! The walk fixes a key's value as it descends past it and multiplies by any
//! slot that holds no key at all, because such a slot only decides *how many*
//! tuples a combination stands for. When every key below has been fixed, what
//! remains is one group and the subtree size beneath it.
//!
//! The cost is the number of groups, which is the size of the answer the caller
//! asked for -- not the size of the join.
class GroupFold {
public:
	//! What a set of tuples contributes: how many there are, and what each
	//! summed column sums to over them. The counts and the sums are carried
	//! together because every sum needs the count.
	//!
	//! Independent sets combine by the semiring product, not by multiplying
	//! twice: (c1,s1) x (c2,s2) = (c1*c2, s1*c2 + c1*s2). Each side's values
	//! appear once per tuple of the other, which is why a sum cannot be carried
	//! as a scalar weight -- propagating only the counts loses every value in
	//! every branch but the last.
	//!
	//! Several sums ride along in one walk. The product above is per column and
	//! the columns do not interact, so `sums[i]` needs no more than the shared
	//! counts to be exact. A fixed array rather than a vector: one of these is
	//! made per record visited, and an allocation there would cost more than the
	//! fold it belongs to.
	struct Fold {
		int64_t count = 1;
		int64_t sums[kMaxAggregates] = {};
	};

	Fold Combine(const Fold &a, const Fold &b) const {
		Fold result;
		result.count = CheckedCardinalityMul(a.count, b.count);
		for (size_t i = 0; i < aggregates.size(); i++) {
			result.sums[i] = CheckedCardinalityAdd(CheckedCardinalityMul(a.sums[i], b.count),
			                                       CheckedCardinalityMul(a.count, b.sums[i]));
		}
		return result;
	}

	Fold Alternatives(const Fold &a, const Fold &b) const {
		Fold result;
		result.count = CheckedCardinalityAdd(a.count, b.count);
		for (size_t i = 0; i < aggregates.size(); i++) {
			result.sums[i] = CheckedCardinalityAdd(a.sums[i], b.sums[i]);
		}
		return result;
	}

	GroupFold(const FRepresentation &rep, std::vector<GroupSite> sites, size_t width,
	          std::vector<GroupAggregate> aggregates, std::vector<AttributeId> sum_attributes)
	    : rep(rep), sites(std::move(sites)), width(width), aggregates(std::move(aggregates)),
	      sum_attributes(std::move(sum_attributes)) {
		// Which levels have a key at or below them, so a slot leading nowhere
		// useful can be collapsed into a multiplier instead of walked.
		const auto &layout = rep.GetLayout();
		carries.assign(layout.LevelCount(), false);
		for (size_t level = layout.LevelCount(); level-- > 0;) {
			for (const auto &site : this->sites) {
				if (site.level == level) {
					carries[level] = true;
				}
			}
			for (const auto &slot : layout.Level(static_cast<LevelId>(level)).slots) {
				if (carries[slot.child_level]) {
					carries[level] = true;
				}
			}
		}
	}

	void Run(std::map<std::vector<int64_t>, Fold> &out) {
		std::vector<int64_t> row(width, 0);
		Fold identity;
		rep.ForEachRoot([&](Record root) { Descend(root, row, identity, out); });
	}

private:
	//! What a slot's children stand for, taken together: they are alternatives,
	//! so they add.
	Fold SlotTotals(Record record, size_t slot_index) const {
		Fold total;
		total.count = 0;
		rep.ForEachChild(record, slot_index, [&](Record child) {
			Fold child_fold;
			child_fold.count = rep.SubtreeSize(child);
			for (size_t i = 0; i < aggregates.size(); i++) {
				if (aggregates[i].kind == Aggregate::SUM) {
					child_fold.sums[i] = rep.SubtreeSum(child, sum_attributes[i]);
				}
			}
			total = Alternatives(total, child_fold);
		});
		return total;
	}

	//! This record's own contribution before any child is considered: one tuple
	//! so far, carrying its own value if the summed column lives here.
	Fold Own(Record record) const {
		Fold fold;
		const auto &level = rep.GetLayout().Level(record.Level());
		for (size_t i = 0; i < aggregates.size(); i++) {
			if (aggregates[i].kind != Aggregate::SUM) {
				continue;
			}
			for (const auto &entry : level.payload) {
				if (entry.attribute == sum_attributes[i]) {
					fold.sums[i] = rep.GetValue(record, sum_attributes[i]);
					break;
				}
			}
		}
		return fold;
	}

	void Descend(Record record, std::vector<int64_t> &row, const Fold &incoming,
	             std::map<std::vector<int64_t>, Fold> &out) {
		if (incoming.count == 0 || rep.SubtreeSize(record) == 0) {
			// An empty subtree denotes no tuples, so it names no group.
			return;
		}
		for (const auto &site : sites) {
			if (site.level == record.Level()) {
				row[site.position] = rep.GetValue(record, site.attribute);
			}
		}
		const auto &level = rep.GetLayout().Level(record.Level());

		// Slots with no key beneath them cannot split a group, so they fold in
		// now: they decide how many tuples a combination stands for, and what
		// those tuples contribute.
		Fold here = Combine(incoming, Own(record));
		std::vector<size_t> walk;
		for (size_t slot_index = 0; slot_index < level.slots.size(); slot_index++) {
			if (carries[level.slots[slot_index].child_level]) {
				walk.push_back(slot_index);
				continue;
			}
			here = Combine(here, SlotTotals(record, slot_index));
		}
		if (here.count == 0) {
			return;
		}
		if (walk.empty()) {
			// Every key is fixed, so this subtree is one group's worth of tuples.
			//
			// Inserted with an explicit zero rather than through operator[]:
			// Fold's default is the *multiplicative* identity, count 1, because
			// that is what Combine needs -- and using it as an accumulator would
			// add a tuple to every group that nothing counted.
			auto found = out.find(row);
			if (found == out.end()) {
				Fold empty;
				empty.count = 0;
				found = out.emplace(row, empty).first;
			}
			found->second = Alternatives(found->second, here);
			return;
		}
		Cross(record, walk, 0, row, here, out);
	}

	//! The cross product over the slots that do carry keys.
	//!
	//! Each contributes its own groups and the combinations are the product of
	//! them, which is precisely the case §10.1 expected to be declined. It is
	//! computable because siblings are independent -- the property the whole
	//! representation is built on -- so a branch can be grouped on its own and
	//! its groups combined with the others afterwards.
	void Cross(Record record, const std::vector<size_t> &walk, size_t index, std::vector<int64_t> &row,
	           const Fold &incoming, std::map<std::vector<int64_t>, Fold> &out) {
		const bool last = (index + 1 == walk.size());
		rep.ForEachChild(record, walk[index], [&](Record child) {
			if (last) {
				Descend(child, row, incoming, out);
				return;
			}
			// Group this branch by itself, then carry each of its groups into
			// the remaining branches. Combining with the semiring product is
			// what keeps a branch's values from being lost behind another
			// branch's counts.
			std::map<std::vector<int64_t>, Fold> nested;
			std::vector<int64_t> child_row = row;
			Fold identity;
			Descend(child, child_row, identity, nested);
			for (const auto &entry : nested) {
				std::vector<int64_t> merged = entry.first;
				Cross(record, walk, index + 1, merged, Combine(incoming, entry.second), out);
			}
		});
	}

	const FRepresentation &rep;
	std::vector<GroupSite> sites;
	size_t width;
	std::vector<GroupAggregate> aggregates;
	//! Parallel to `aggregates`; meaningful only where the kind is SUM.
	std::vector<AttributeId> sum_attributes;
	std::vector<bool> carries;
};

GroupCountResult GroupBy(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                         const std::vector<GroupKey> &keys, const std::vector<GroupAggregate> &aggregates,
                         PathStrategy strategy) {
	GroupCountResult result;
	if (!plan.complete) {
		result.error = plan.reason.empty() ? "no plan" : plan.reason;
		return result;
	}
	if (aggregates.empty()) {
		result.error = "no aggregate to compute";
		return result;
	}
	if (aggregates.size() > kMaxAggregates) {
		result.error = "more than " + std::to_string(kMaxAggregates) + " aggregates in one query";
		return result;
	}
	for (const auto &aggregate : aggregates) {
		if (aggregate.kind == Aggregate::SUM && (aggregate.relation >= graph.RelationCount() ||
		                                         aggregate.column >= graph.column_counts[aggregate.relation])) {
			result.error = "summed column is not a column of the query";
			return result;
		}
	}
	for (const auto &key : keys) {
		if (key.relation >= graph.RelationCount() || key.column >= graph.column_counts[key.relation]) {
			result.error = "grouping column is not a column of the query";
			return result;
		}
	}
	auto built = BuildRepresentation(graph, plan, source, mode, strategy);
	if (!built.ok) {
		result.error = built.error;
		result.out_of_memory = built.out_of_memory;
		return result;
	}
	try {
		const auto &accumulated = *built.relation;
		auto &classes = *built.classes;
		const auto &layout = accumulated.GetLayout();

		// Find where each key's values are actually stored. An equi-join's keys
		// are equal by definition, so grouping on either side of one names the
		// same groups -- and the merge is free to have kept only one of them.
		std::vector<GroupSite> sites;
		for (size_t position = 0; position < keys.size(); position++) {
			const auto wanted = AttributeOf(graph, keys[position].relation, keys[position].column);
			bool found = false;
			for (size_t level = 0; level < layout.LevelCount() && !found; level++) {
				for (const auto &entry : layout.Level(static_cast<LevelId>(level)).payload) {
					if (entry.attribute == wanted || classes.SameClass(entry.attribute, wanted)) {
						sites.push_back(GroupSite {entry.attribute, static_cast<LevelId>(level), position});
						found = true;
						break;
					}
				}
			}
			if (!found) {
				result.error = "grouping column is not stored anywhere in the representation";
				return result;
			}
		}

		std::vector<AttributeId> sum_attributes(aggregates.size(), AttributeId(0));
		for (size_t i = 0; i < aggregates.size(); i++) {
			if (aggregates[i].kind == Aggregate::SUM) {
				sum_attributes[i] = AttributeOf(graph, aggregates[i].relation, aggregates[i].column);
			}
		}
		GroupFold fold(accumulated.Rep(), std::move(sites), keys.size(), aggregates, std::move(sum_attributes));
		std::map<std::vector<int64_t>, GroupFold::Fold> totals;
		fold.Run(totals);

		for (const auto &entry : totals) {
			// A combination no tuple satisfies is not a group. Emptiness is
			// judged by the count even when the answer is a sum, or a group
			// whose values happen to total zero would vanish.
			if (entry.second.count <= 0) {
				continue;
			}
			std::vector<int64_t> values;
			values.reserve(aggregates.size());
			for (size_t i = 0; i < aggregates.size(); i++) {
				values.push_back(aggregates[i].kind == Aggregate::SUM ? entry.second.sums[i] : entry.second.count);
			}
			result.groups.emplace_back(entry.first, std::move(values));
		}
		result.records = accumulated.Rep().RecordCount();
		result.bytes = accumulated.Rep().BytesAllocated();
		result.ok = true;
	} catch (const MemoryLimitExceeded &error) {
		result.error = error.what();
		result.out_of_memory = true;
	} catch (const std::exception &error) {
		result.error = error.what();
	}
	return result;
}

} // namespace

GroupCountResult ExecuteGroupBy(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                               const std::vector<GroupKey> &keys, const std::vector<GroupAggregate> &aggregates,
                               PathStrategy strategy) {
	return GroupBy(graph, plan, source, mode, keys, aggregates, strategy);
}

GroupCountResult ExecuteGroupCount(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                   const std::vector<GroupKey> &keys, PathStrategy strategy) {
	return GroupBy(graph, plan, source, mode, keys, {GroupAggregate {Aggregate::COUNT, 0, 0}}, strategy);
}

GroupCountResult ExecuteGroupSum(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                 const std::vector<GroupKey> &keys, size_t sum_relation, size_t sum_column,
                                 PathStrategy strategy) {
	return GroupBy(graph, plan, source, mode, keys, {GroupAggregate {Aggregate::SUM, sum_relation, sum_column}},
	               strategy);
}

ExecuteResult ExecuteExists(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                            PathStrategy strategy) {
	// Enough buckets that finding a witness early is worth something, few enough
	// that a genuinely empty join does not pay for many passes to learn it. The
	// buckets are examined in order and nothing about the answer depends on
	// which one answers first.
	static const size_t kProbeSlices = 16;
	auto key_column = ChooseSliceColumns(graph);
	const size_t slices = key_column.empty() ? 1 : kProbeSlices;

	ExecuteResult result;
	for (size_t slice = 0; slice < slices; slice++) {
		auto part = ExecuteCountSliceWithinMemory(graph, plan, source, mode, slice, slices, strategy);
		if (!part.ok) {
			return part;
		}
		if (part.count > 0) {
			result.ok = true;
			result.count = 1;
			result.records = part.records;
			result.bytes = part.bytes;
			// How much of the partition was read before the answer was known.
			result.slices = slice + 1;
			return result;
		}
	}
	result.ok = true;
	result.count = 0;
	result.slices = slices;
	return result;
}

ExecuteResult ExecuteCountWithinMemory(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                       PathStrategy strategy) {
	auto result = ExecuteCount(graph, plan, source, mode, strategy);
	if (result.ok || !result.out_of_memory) {
		return result;
	}
	// Geometric, so the retries together cost about as much as the pass that
	// finally works rather than a multiple of it. The ceiling is where slicing
	// has stopped being the answer: past it the query is not too big overall but
	// too skewed, with one key value whose own subtree does not fit, and no
	// number of buckets separates a value from itself.
	static const size_t kSliceSteps[] = {8, 64, 512, 4096};
	for (auto slices : kSliceSteps) {
		auto sliced = ExecuteCountSliced(graph, plan, source, mode, slices, strategy);
		if (sliced.ok || !sliced.out_of_memory) {
			return sliced;
		}
		result = sliced;
	}
	return result;
}

} // namespace factorize
