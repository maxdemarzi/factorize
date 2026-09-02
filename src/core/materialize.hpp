//===----------------------------------------------------------------------===//
//                         factorize
//
// core/materialize.hpp
//
// Partial flattening (paper section 4.6) and output construction.
//
// Section 4.3's transformations merge f-tree nodes, so one *output* node can
// stand for several *input* nodes. Materializing that output node means
// iterating the combinations of its inputs -- flattening exactly those nodes
// while everything else stays factorized:
//
//   "For some operators, it suffices to flatten only parts of the f-tree,
//    while keeping the remaining nodes unflattened."
//
// Because merged nodes are always siblings of a common ancestor, and siblings
// are independent, "combinations" means a nested loop over their child lists.
// The whole thing is planned once per query from the merge's provenance, then
// executed with no per-record decisions -- the same trade the level layouts
// make (plan section 0.3).
//
// The payoff is that the required path and the residual subtrees are handled by
// one uniform recursion: copying an input f-representation into the output
// shape is the same operation whether or not any merging happened.
//
// No DuckDB headers (plan section 4).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frep.hpp"
#include "ftree.hpp"
#include "layout.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace factorize {

//! Parent and child-slot of every node of a tree, in DFS order -- the same
//! order Layout assigns LevelIds, so an index here is a level there.
struct TreeTopology {
	std::vector<int32_t> parent;
	std::vector<int32_t> slot_in_parent;

	static TreeTopology Of(const FTree &tree);
	size_t size() const {
		return parent.size();
	}
};

//! One input node feeding an output level.
struct SourceRef {
	//! Node of the input tree (== level of the input layout).
	uint32_t input_level;
	//! Plan index whose context holds this node's parent record; -1 means the
	//! input root, which is iterated from the f-representation's root list.
	int32_t parent_plan_index;
	//! Position of the parent within that plan level's source list.
	int32_t parent_source_index;
	//! Slot of the parent *input* node leading to `input_level`.
	int32_t slot;
};

//! Moves one attribute's value from an input record to the output record.
struct PayloadCopy {
	uint32_t source_index;
	uint32_t input_payload;
	uint32_t output_payload;
	ValueType type;
};

struct PlanLevel {
	uint32_t output_level;
	//! Topologically ordered: a source whose parent sits in this same level
	//! always follows it.
	std::vector<SourceRef> sources;
	std::vector<PayloadCopy> payload;
	//! Output children this plan owns: (plan index, slot within output_level).
	std::vector<std::pair<uint32_t, uint32_t>> children;
	//! Offset of this level's records within the flat context array.
	uint32_t ctx_offset = 0;
};

//! How to build one side's contribution to a join output.
class MaterializePlan {
public:
	//! `owned` selects the output levels this plan is responsible for; `root`
	//! is the shallowest of them.
	static MaterializePlan Build(const FTree &input_tree, const Layout &input_layout, const FTree &output_tree,
	                             const Layout &output_layout, const std::vector<std::vector<uint32_t>> &sources,
	                             const std::vector<bool> &owned, uint32_t root_output_level);

	const std::vector<PlanLevel> &Levels() const {
		return levels;
	}
	const PlanLevel &Level(uint32_t index) const {
		return levels[index];
	}
	uint32_t ContextSize() const {
		return ctx_size;
	}
	//! Plan index for an output level, or -1.
	int32_t IndexOfOutputLevel(uint32_t output_level) const {
		auto entry = output_to_plan.find(output_level);
		return entry == output_to_plan.end() ? -1 : static_cast<int32_t>(entry->second);
	}
	std::string ToString() const;

private:
	std::vector<PlanLevel> levels;
	std::unordered_map<uint32_t, uint32_t> output_to_plan;
	uint32_t ctx_size = 0;
};

//! Records fixed by an in-progress flattening, indexed by ctx_offset.
using FlattenContext = std::vector<Record>;

//===--------------------------------------------------------------------===//
// Iteration
//===--------------------------------------------------------------------===//

//! Invokes `fn()` once per combination of the input records feeding one plan
//! level, having written them into `ctx`.
//!
//! This is the nested loop that section 4.6 describes: fix an entry at a node,
//! iterate the required entries of its children, repeat. Sources with no parent
//! iterate the input's root list; the rest iterate a child slot of an
//! already-fixed record.
template <typename Fn>
void IterateLevel(const MaterializePlan &plan, uint32_t plan_index, const FRepresentation &input, FlattenContext &ctx,
                  size_t source_index, Fn &&fn) {
	const auto &level = plan.Level(plan_index);
	if (source_index == level.sources.size()) {
		fn();
		return;
	}
	const auto &ref = level.sources[source_index];
	const size_t slot_in_ctx = level.ctx_offset + source_index;
	if (ref.parent_plan_index < 0) {
		input.ForEachRoot([&](Record record) {
			ctx[slot_in_ctx] = record;
			IterateLevel(plan, plan_index, input, ctx, source_index + 1, fn);
		});
		return;
	}
	const auto &parent_level = plan.Level(static_cast<uint32_t>(ref.parent_plan_index));
	const Record parent = ctx[parent_level.ctx_offset + static_cast<size_t>(ref.parent_source_index)];
	input.ForEachChild(parent, static_cast<size_t>(ref.slot), [&](Record record) {
		ctx[slot_in_ctx] = record;
		IterateLevel(plan, plan_index, input, ctx, source_index + 1, fn);
	});
}

//! Copies the payload of one plan level from the fixed input records.
inline void CopyPayload(const MaterializePlan &plan, uint32_t plan_index, const FRepresentation &input,
                        const FlattenContext &ctx, FRepresentation &output, Record target) {
	const auto &level = plan.Level(plan_index);
	for (const auto &copy : level.payload) {
		const Record source = ctx[level.ctx_offset + copy.source_index];
		if (copy.type == ValueType::INT32) {
			output.SetInt32(target, copy.output_payload, input.GetInt32(source, copy.input_payload));
		} else {
			output.SetInt64(target, copy.output_payload, input.GetInt64(source, copy.input_payload));
		}
	}
}

//===--------------------------------------------------------------------===//
// Construction
//===--------------------------------------------------------------------===//

//! Fills `target` -- the freshly created output record for `plan_index` -- and
//! recursively builds every output child this plan owns.
//!
//! `stop_at` lets a join interrupt the recursion at the seam where the other
//! side attaches; `on_stop(Record parent, uint32_t slot)` is invoked there
//! instead of descending. Pass -1 to copy the whole subtree.
template <typename OnStop>
void MaterializeSubtree(const MaterializePlan &plan, uint32_t plan_index, const FRepresentation &input,
                        FlattenContext &ctx, FRepresentation &output, Record target, int32_t stop_at_output_level,
                        OnStop &&on_stop) {
	CopyPayload(plan, plan_index, input, ctx, output, target);
	const auto &level = plan.Level(plan_index);
	for (const auto &child : level.children) {
		const uint32_t child_plan = child.first;
		const uint32_t slot = child.second;
		IterateLevel(plan, child_plan, input, ctx, 0, [&]() {
			Record child_record = output.InsertChild(target, slot);
			MaterializeSubtree(plan, child_plan, input, ctx, output, child_record, stop_at_output_level, on_stop);
		});
	}
	if (stop_at_output_level >= 0 && static_cast<int32_t>(level.output_level) == stop_at_output_level) {
		on_stop(target);
	}
}

//! Copies an entire input f-representation into `output` under `plan`.
//! Used to reshape a relation when no join is involved.
inline void MaterializeAll(const MaterializePlan &plan, const FRepresentation &input, FRepresentation &output) {
	FlattenContext ctx(plan.ContextSize());
	IterateLevel(plan, 0, input, ctx, 0, [&]() {
		Record root = output.AppendRoot();
		MaterializeSubtree(plan, 0, input, ctx, output, root, -1, [](Record) {});
	});
}

} // namespace factorize
