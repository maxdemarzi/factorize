#include "join.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace factorize {

//===--------------------------------------------------------------------===//
// FactorizedRelation
//===--------------------------------------------------------------------===//
namespace {
size_t g_memory_limit = 0;
}

void SetGlobalMemoryLimit(size_t bytes) {
	g_memory_limit = bytes;
}

size_t GetGlobalMemoryLimit() {
	return g_memory_limit;
}

FactorizedRelation::FactorizedRelation(FTree tree_p, AttributeTypes types_p)
    : tree(std::move(tree_p)), types(std::move(types_p)) {
	layout = std::make_unique<Layout>(Layout::FromFTree(tree, types));
	frep = std::make_unique<FRepresentation>(*layout);
	frep->SetMemoryLimit(g_memory_limit);
}

FactorizedRelation MakeScan(const std::vector<AttributeId> &attributes, const AttributeTypes &types,
                            const std::vector<std::vector<int64_t>> &columns) {
	if (columns.size() != attributes.size()) {
		throw std::runtime_error("MakeScan: one column per attribute expected");
	}
	FactorizedRelation relation(FTree::Scan(attributes), types);
	const size_t rows = columns.empty() ? 0 : columns[0].size();
	const auto &level = relation.GetLayout().Level(0);
	// Attributes are sorted inside the node and payload columns are ordered by
	// width, so map each input column to its payload slot once.
	std::vector<size_t> payload_of(attributes.size());
	for (size_t c = 0; c < attributes.size(); c++) {
		bool found = false;
		for (size_t p = 0; p < level.payload.size(); p++) {
			if (level.payload[p].attribute == attributes[c]) {
				payload_of[c] = p;
				found = true;
				break;
			}
		}
		if (!found) {
			throw std::runtime_error("MakeScan: attribute missing from layout");
		}
	}
	auto &rep = relation.Rep();
	for (size_t row = 0; row < rows; row++) {
		auto record = rep.AppendRoot();
		for (size_t c = 0; c < attributes.size(); c++) {
			const auto payload = payload_of[c];
			if (level.payload[payload].type == ValueType::INT32) {
				rep.SetInt32(record, payload, static_cast<int32_t>(columns[c][row]));
			} else {
				rep.SetInt64(record, payload, columns[c][row]);
			}
		}
	}
	return relation;
}

namespace {

//===--------------------------------------------------------------------===//
// Key packing
//===--------------------------------------------------------------------===//

//! Reads a join key out of an in-progress flattening.
//!
//! The key columns are packed bit-exactly into 64 bits, so the hash table's
//! chain walk can compare keys exactly rather than probabilistically -- the
//! `if (tuple.key != cur.key) continue;` of the paper's generated code. Wider
//! composite keys would need the values stored alongside for verification; the
//! join rejects them rather than risking a false match.
struct KeyReader {
	struct Column {
		uint32_t source_index;
		uint32_t payload_index;
		ValueType type;
	};
	uint32_t plan_index = 0;
	std::vector<Column> columns;

	uint64_t Read(const MaterializePlan &plan, const FRepresentation &input, const FlattenContext &ctx) const {
		const auto &level = plan.Level(plan_index);
		uint64_t packed = 0;
		unsigned bit = 0;
		for (const auto &column : columns) {
			const Record record = ctx[level.ctx_offset + column.source_index];
			if (column.type == ValueType::INT32) {
				packed |= static_cast<uint64_t>(static_cast<uint32_t>(input.GetInt32(record, column.payload_index)))
				          << bit;
				bit += 32;
			} else {
				packed |= static_cast<uint64_t>(input.GetInt64(record, column.payload_index));
				bit += 64;
			}
		}
		return packed;
	}
};

//! Locates the output level holding `keys` on the given side, and builds the
//! reader that extracts them from that level's flattening context.
KeyReader MakeKeyReader(const MaterializePlan &plan, const Layout &input_layout, const std::vector<AttributeId> &keys) {
	KeyReader reader;
	unsigned bits = 0;
	for (auto key : keys) {
		bool found = false;
		for (size_t plan_index = 0; plan_index < plan.Levels().size() && !found; plan_index++) {
			const auto &level = plan.Level(static_cast<uint32_t>(plan_index));
			for (size_t source_index = 0; source_index < level.sources.size() && !found; source_index++) {
				const auto input_level = level.sources[source_index].input_level;
				const auto &in_layout = input_layout.Level(static_cast<LevelId>(input_level));
				for (size_t payload = 0; payload < in_layout.payload.size(); payload++) {
					if (in_layout.payload[payload].attribute != key) {
						continue;
					}
					if (!reader.columns.empty() && reader.plan_index != plan_index) {
						// Section 4.3 guarantees all keys land in one node; if
						// they did not, the transformation is wrong.
						throw std::runtime_error("join: key attributes did not converge on one level");
					}
					reader.plan_index = static_cast<uint32_t>(plan_index);
					reader.columns.push_back(
					    KeyReader::Column {static_cast<uint32_t>(source_index), static_cast<uint32_t>(payload),
					                       in_layout.payload[payload].type});
					bits += in_layout.payload[payload].type == ValueType::INT32 ? 32 : 64;
					found = true;
					break;
				}
			}
		}
		if (!found) {
			throw std::runtime_error("join: key attribute " + std::to_string(key) + " not present in input");
		}
	}
	if (bits > 64) {
		throw std::runtime_error("join: composite key wider than 64 bits is not supported");
	}
	return reader;
}

//! Marks the plan levels on the root-to-key path. Those levels' records are
//! fixed by the indexing pass and must not be re-iterated when the match is
//! materialized.
std::vector<bool> MarkKeyPath(const MaterializePlan &plan, uint32_t key_plan_index,
                              const std::vector<int32_t> &plan_parent) {
	std::vector<bool> on_path(plan.Levels().size(), false);
	int32_t current = static_cast<int32_t>(key_plan_index);
	while (current >= 0) {
		on_path[static_cast<size_t>(current)] = true;
		current = plan_parent[static_cast<size_t>(current)];
	}
	return on_path;
}

//! Parent plan index of every plan level, derived from the output tree.
std::vector<int32_t> PlanParents(const MaterializePlan &plan) {
	std::vector<int32_t> parent(plan.Levels().size(), -1);
	for (size_t i = 0; i < plan.Levels().size(); i++) {
		for (const auto &child : plan.Level(static_cast<uint32_t>(i)).children) {
			parent[child.first] = static_cast<int32_t>(i);
		}
	}
	return parent;
}

//! Builds the lower side's records beneath `target`, using the context already
//! fixed along the key path and iterating everything else.
void MaterializeLower(const MaterializePlan &plan, uint32_t plan_index, const std::vector<bool> &on_key_path,
                      const FRepresentation &input, FlattenContext &ctx, FRepresentation &output, Record target) {
	CopyPayload(plan, plan_index, input, ctx, output, target);
	for (const auto &child : plan.Level(plan_index).children) {
		const uint32_t child_plan = child.first;
		const uint32_t slot = child.second;
		if (on_key_path[child_plan]) {
			// Already fixed by the indexing pass: exactly one child.
			Record child_record = output.InsertChild(target, slot);
			MaterializeLower(plan, child_plan, on_key_path, input, ctx, output, child_record);
		} else {
			IterateLevel(plan, child_plan, input, ctx, 0, [&]() {
				Record child_record = output.InsertChild(target, slot);
				MaterializeLower(plan, child_plan, on_key_path, input, ctx, output, child_record);
			});
		}
	}
}

//! Iterates only the key path of a plan, invoking `fn()` with the context fixed
//! down to the key level. Used by whichever side is being indexed.
template <typename Fn>
void IterateKeyPath(const MaterializePlan &plan, uint32_t plan_index, uint32_t key_plan_index,
                    const std::vector<int32_t> &plan_parent, const FRepresentation &input, FlattenContext &ctx,
                    Fn &&fn) {
	IterateLevel(plan, plan_index, input, ctx, 0, [&]() {
		if (plan_index == key_plan_index) {
			fn();
			return;
		}
		// Descend towards the key level along the unique child that leads there.
		for (const auto &child : plan.Level(plan_index).children) {
			int32_t current = static_cast<int32_t>(key_plan_index);
			bool leads_there = false;
			while (current >= 0) {
				if (static_cast<uint32_t>(current) == child.first) {
					leads_there = true;
					break;
				}
				current = plan_parent[static_cast<size_t>(current)];
			}
			if (leads_there) {
				IterateKeyPath(plan, child.first, key_plan_index, plan_parent, input, ctx, fn);
			}
		}
	});
}

} // namespace

//===--------------------------------------------------------------------===//
// The join
//===--------------------------------------------------------------------===//
FactorizedRelation FactorizedJoin(const FactorizedRelation &build, const FactorizedRelation &probe,
                                  const JoinKeys &keys, JoinMode mode, PathStrategy strategy, JoinStats *stats) {
	const bool build_on_top = (mode == JoinMode::BOTTOM_INSERT);
	const FactorizedRelation &upper = build_on_top ? build : probe;
	const FactorizedRelation &lower = build_on_top ? probe : build;
	const std::vector<AttributeId> &upper_keys = build_on_top ? keys.build : keys.probe;
	const std::vector<AttributeId> &lower_keys = build_on_top ? keys.probe : keys.build;

	auto merge = MergeTreesDetailed(build.Tree(), probe.Tree(), keys, mode, strategy);

	AttributeTypes out_types = build.Types();
	for (const auto &entry : probe.Types()) {
		if (std::find_if(out_types.begin(), out_types.end(),
		                 [&](const std::pair<AttributeId, ValueType> &existing) {
			                 return existing.first == entry.first;
		                 }) == out_types.end()) {
			out_types.push_back(entry);
		}
	}

	FactorizedRelation result(merge.tree, out_types);
	auto &out = result.Rep();

	std::vector<bool> upper_owned(merge.side.size(), false);
	std::vector<bool> lower_owned(merge.side.size(), false);
	for (size_t i = 0; i < merge.side.size(); i++) {
		(merge.side[i] == MergeSide::UPPER ? upper_owned : lower_owned)[i] = true;
	}

	auto upper_plan = MaterializePlan::Build(upper.Tree(), upper.GetLayout(), merge.tree, result.GetLayout(),
	                                         merge.sources, upper_owned, 0);
	auto lower_plan = MaterializePlan::Build(lower.Tree(), lower.GetLayout(), merge.tree, result.GetLayout(),
	                                         merge.sources, lower_owned, merge.attach_level);

	const auto upper_key = MakeKeyReader(upper_plan, upper.GetLayout(), upper_keys);
	const auto lower_key = MakeKeyReader(lower_plan, lower.GetLayout(), lower_keys);
	const auto lower_parents = PlanParents(lower_plan);
	const auto lower_on_key_path = MarkKeyPath(lower_plan, lower_key.plan_index, lower_parents);

	if (stats) {
		for (const auto &sources : merge.sources) {
			if (sources.size() > 1) {
				stats->merged_nodes = true;
			}
		}
	}

	FlattenContext upper_ctx(upper_plan.ContextSize());
	FlattenContext lower_ctx(lower_plan.ContextSize());

	if (mode == JoinMode::TOP_INSERT) {
		// The build side is the lower part. Index its flattening contexts by
		// key; each match replays one of them beneath a probe record.
		Arena snapshots;
		ChainingHashTable<const Record *> table;
		const auto lower_parents_local = lower_parents;
		IterateKeyPath(lower_plan, 0, lower_key.plan_index, lower_parents_local, lower.Rep(), lower_ctx, [&]() {
			const uint64_t key = lower_key.Read(lower_plan, lower.Rep(), lower_ctx);
			auto *snapshot = reinterpret_cast<Record *>(snapshots.Allocate(sizeof(Record) * lower_ctx.size()));
			for (size_t i = 0; i < lower_ctx.size(); i++) {
				snapshot[i] = lower_ctx[i];
			}
			table.Insert(key, snapshot);
		});
		table.Finalize();
		if (stats) {
			stats->build_keys = table.Size();
		}

		// Probe side drives, materializing the upper tree as it goes and
		// splicing matches in at the insertion point (Figure 9).
		FlattenContext replay(lower_plan.ContextSize());
		IterateLevel(upper_plan, 0, upper.Rep(), upper_ctx, 0, [&]() {
			Record root = out.AppendRoot();
			MaterializeSubtree(upper_plan, 0, upper.Rep(), upper_ctx, out, root,
			                   static_cast<int32_t>(merge.insertion_level), [&](Record insertion_record) {
				                   if (stats) {
					                   stats->probe_rows++;
				                   }
				                   const uint64_t key = upper_key.Read(upper_plan, upper.Rep(), upper_ctx);
				                   table.ForEachMatch(key, [&](const Record *snapshot) {
					                   if (stats) {
						                   stats->matches++;
					                   }
					                   std::memcpy(replay.data(), snapshot, sizeof(Record) * replay.size());
					                   Record attached = out.InsertChild(insertion_record, merge.attach_slot);
					                   MaterializeLower(lower_plan, 0, lower_on_key_path, lower.Rep(), replay, out,
					                                    attached);
				                   });
			                   });
		});
	} else {
		// Bottom-insert: the build side is the upper part, so it is materialized
		// first and the table points at the records probes append to (Figure 11).
		ChainingHashTable<Record> table;
		IterateLevel(upper_plan, 0, upper.Rep(), upper_ctx, 0, [&]() {
			Record root = out.AppendRoot();
			MaterializeSubtree(upper_plan, 0, upper.Rep(), upper_ctx, out, root,
			                   static_cast<int32_t>(merge.insertion_level), [&](Record insertion_record) {
				                   const uint64_t key = upper_key.Read(upper_plan, upper.Rep(), upper_ctx);
				                   table.Insert(key, insertion_record);
			                   });
		});
		table.Finalize();
		if (stats) {
			stats->build_keys = table.Size();
		}

		IterateKeyPath(lower_plan, 0, lower_key.plan_index, lower_parents, lower.Rep(), lower_ctx, [&]() {
			if (stats) {
				stats->probe_rows++;
			}
			const uint64_t key = lower_key.Read(lower_plan, lower.Rep(), lower_ctx);
			table.ForEachMatch(key, [&](Record insertion_record) {
				if (stats) {
					stats->matches++;
				}
				Record attached = out.InsertChild(insertion_record, merge.attach_slot);
				MaterializeLower(lower_plan, 0, lower_on_key_path, lower.Rep(), lower_ctx, out, attached);
			});
		});
	}

	// Drop the records left behind by probe tuples that matched nothing, so the
	// next join copies only what still encodes tuples. Without this the
	// representation grows while the result shrinks.
	out.PruneEmptySubtrees();

	if (stats) {
		stats->output_records = out.RecordCount();
		stats->output_bytes = out.BytesAllocated();
		stats->live_records = out.LiveRecordCount();
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Counting without materializing (paper sections 4.2.2 and 4.5)
//===--------------------------------------------------------------------===//
namespace {

//! Cardinality of the subtree one side would contribute, computed from the
//! plan instead of from records that are never built.
//!
//! Mirrors MaterializeLower exactly, with "create a record" replaced by
//! "multiply in its size": slots are a Cartesian product so they multiply, and
//! the instances within a slot are alternatives so they sum.
struct LowerSizeCounter {
	const MaterializePlan &plan;
	const std::vector<bool> &on_key_path;
	const FRepresentation &input;
	FlattenContext &ctx;

	int64_t Of(uint32_t plan_index) {
		int64_t size = 1;
		for (const auto &child : plan.Level(plan_index).children) {
			int64_t slot_total = 0;
			if (on_key_path[child.first]) {
				// Fixed by the indexing pass: exactly one instance.
				slot_total = Of(child.first);
			} else {
				IterateLevel(plan, child.first, input, ctx, 0, [&]() { slot_total += Of(child.first); });
			}
			size *= slot_total;
			if (size == 0) {
				return 0;
			}
		}
		return size;
	}
};

//! Cardinality of the whole join output, walking the upper side's plan and
//! folding the matching lower subtree sizes in at the insertion level.
struct OutputCounter {
	const MaterializePlan &plan;
	const FRepresentation &input;
	FlattenContext &ctx;
	int32_t insertion_level;
	const KeyReader &key;
	const ChainingHashTable<int64_t> &table;
	size_t *matches;

	int64_t Of(uint32_t plan_index) {
		const auto &level = plan.Level(plan_index);
		int64_t size = 1;
		for (const auto &child : level.children) {
			int64_t slot_total = 0;
			IterateLevel(plan, child.first, input, ctx, 0, [&]() { slot_total += Of(child.first); });
			size *= slot_total;
			if (size == 0) {
				return 0;
			}
		}
		if (static_cast<int32_t>(level.output_level) == insertion_level) {
			// The slot the other side would have been attached to contributes
			// the summed size of everything that matches this key.
			int64_t matched = 0;
			table.ForEachMatch(key.Read(plan, input, ctx), [&](int64_t subtree) {
				matched += subtree;
				if (matches) {
					(*matches)++;
				}
			});
			size *= matched;
		}
		return size;
	}
};

} // namespace

int64_t FactorizedCountJoin(const FactorizedRelation &build, const FactorizedRelation &probe, const JoinKeys &keys,
                            JoinMode mode, PathStrategy strategy, JoinStats *stats) {
	const bool build_on_top = (mode == JoinMode::BOTTOM_INSERT);
	const FactorizedRelation &upper = build_on_top ? build : probe;
	const FactorizedRelation &lower = build_on_top ? probe : build;
	const std::vector<AttributeId> &upper_keys = build_on_top ? keys.build : keys.probe;
	const std::vector<AttributeId> &lower_keys = build_on_top ? keys.probe : keys.build;

	auto merge = MergeTreesDetailed(build.Tree(), probe.Tree(), keys, mode, strategy);

	AttributeTypes out_types = build.Types();
	for (const auto &entry : probe.Types()) {
		if (std::find_if(out_types.begin(), out_types.end(), [&](const std::pair<AttributeId, ValueType> &existing) {
			    return existing.first == entry.first;
		    }) == out_types.end()) {
			out_types.push_back(entry);
		}
	}
	// The layout is still needed to plan the traversal; no representation is.
	const Layout out_layout = Layout::FromFTree(merge.tree, out_types);

	std::vector<bool> upper_owned(merge.side.size(), false);
	std::vector<bool> lower_owned(merge.side.size(), false);
	for (size_t i = 0; i < merge.side.size(); i++) {
		(merge.side[i] == MergeSide::UPPER ? upper_owned : lower_owned)[i] = true;
	}

	auto upper_plan =
	    MaterializePlan::Build(upper.Tree(), upper.GetLayout(), merge.tree, out_layout, merge.sources, upper_owned, 0);
	auto lower_plan = MaterializePlan::Build(lower.Tree(), lower.GetLayout(), merge.tree, out_layout, merge.sources,
	                                         lower_owned, merge.attach_level);

	const auto upper_key = MakeKeyReader(upper_plan, upper.GetLayout(), upper_keys);
	const auto lower_key = MakeKeyReader(lower_plan, lower.GetLayout(), lower_keys);
	const auto lower_parents = PlanParents(lower_plan);
	const auto lower_on_key_path = MarkKeyPath(lower_plan, lower_key.plan_index, lower_parents);

	FlattenContext upper_ctx(upper_plan.ContextSize());
	FlattenContext lower_ctx(lower_plan.ContextSize());

	// Index the lower side by key, storing the size each match would contribute
	// rather than a handle to records that are never created.
	ChainingHashTable<int64_t> table;
	IterateKeyPath(lower_plan, 0, lower_key.plan_index, lower_parents, lower.Rep(), lower_ctx, [&]() {
		LowerSizeCounter counter {lower_plan, lower_on_key_path, lower.Rep(), lower_ctx};
		table.Insert(lower_key.Read(lower_plan, lower.Rep(), lower_ctx), counter.Of(0));
	});
	table.Finalize();
	if (stats) {
		stats->build_keys = table.Size();
	}

	int64_t total = 0;
	size_t matches = 0;
	OutputCounter counter {upper_plan,           upper.Rep(), upper_ctx, static_cast<int32_t>(merge.insertion_level),
	                       upper_key,            table,       &matches};
	IterateLevel(upper_plan, 0, upper.Rep(), upper_ctx, 0, [&]() {
		if (stats) {
			stats->probe_rows++;
		}
		total += counter.Of(0);
	});
	if (stats) {
		stats->matches = matches;
	}
	return total;
}

} // namespace factorize
