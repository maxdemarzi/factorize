#include "join.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace factorize {

//===--------------------------------------------------------------------===//
// FactorizedRelation
//===--------------------------------------------------------------------===//
namespace {
//! Thread-local, not a process global.
//!
//! As a plain global this was a data race waiting for the first thread: two
//! queries running at once, or two slices of one query, would read and write it
//! together, and the symptom would be one query silently running under
//! another's cap. Nothing was parallel yet, so nothing had happened -- which is
//! the worst kind of latent bug, because it looks fine until the change that
//! makes it fire is unrelated to it.
//!
//! The cost of thread-local is that a worker thread starts with no cap rather
//! than inheriting one, so anything that runs the engine on a thread it did not
//! set up has to set the limit there too.
thread_local size_t g_memory_limit = 0;
thread_local size_t g_estimate_budget = 0;
}

void SetGlobalMemoryLimit(size_t bytes) {
	g_memory_limit = bytes;
}

void SetGlobalEstimateBudget(size_t bytes) {
	g_estimate_budget = bytes;
}

size_t GetGlobalEstimateBudget() {
	return g_estimate_budget;
}

size_t GetGlobalMemoryLimit() {
	return g_memory_limit;
}

FactorizedRelation::FactorizedRelation(FTree tree_p, AttributeTypes types_p, bool with_weights)
    : tree(std::move(tree_p)), types(std::move(types_p)) {
	layout = std::make_unique<Layout>(Layout::FromFTree(tree, types, with_weights));
	frep = std::make_unique<FRepresentation>(*layout);
	frep->SetMemoryLimit(g_memory_limit);
	frep->SetEstimateBudget(g_estimate_budget);
}

namespace {

//! Hash of one row across every scanned column.
//!
//! Order-sensitive on purpose: mixing each column into the running hash means
//! two rows holding the same values in different columns land apart, which a
//! commutative combine like xor or addition would not do.
uint64_t HashRow(const std::vector<std::vector<int64_t>> &columns, size_t row) {
	uint64_t hash = 0;
	for (const auto &column : columns) {
		hash = HashKey(hash ^ static_cast<uint64_t>(column[row]));
	}
	return hash;
}

bool RowsEqual(const std::vector<std::vector<int64_t>> &columns, size_t a, size_t b) {
	for (const auto &column : columns) {
		if (column[a] != column[b]) {
			return false;
		}
	}
	return true;
}

//! Rows read before judging whether grouping is worth it.
const size_t kSampleRows = 1u << 16;
//! Group only when at most this percentage of the sampled rows were distinct.
const size_t kGroupBelowPercent = 90;

//! Roughly what percentage of a prefix of the rows are distinct.
//!
//! Approximate on purpose: it compares hashes rather than values, so a
//! collision undercounts distinct rows and biases the answer towards grouping.
//! Being wrong here costs a hash pass or some memory, never an answer -- the
//! multiplicity is a property of each record, so grouping is optional row by
//! row.
//!
//! A prefix rather than a spread sample because the rows arrive as vectors
//! already in storage order and this has to be cheap. It is fooled by a
//! relation whose duplicates all sit at one end, which costs a wrong guess and
//! nothing else.
size_t DistinctPercentOfSample(const std::vector<std::vector<int64_t>> &columns, size_t rows) {
	const size_t sample = rows < kSampleRows ? rows : kSampleRows;
	if (sample == 0 || columns.empty()) {
		return 100;
	}
	size_t capacity = 16;
	while (capacity < sample * 2) {
		capacity *= 2;
	}
	std::vector<uint64_t> seen(capacity, 0);
	std::vector<char> used(capacity, 0);
	const size_t mask = capacity - 1;
	size_t distinct = 0;
	for (size_t row = 0; row < sample; row++) {
		const uint64_t hash = HashRow(columns, row);
		size_t at = hash & mask;
		while (used[at] && seen[at] != hash) {
			at = (at + 1) & mask;
		}
		if (!used[at]) {
			used[at] = 1;
			seen[at] = hash;
			distinct++;
		}
	}
	return distinct * 100 / sample;
}

//! Collapses a scan's identical rows into one record apiece, tallied.
//!
//! A relation is a bag, and this engine scans only the columns a query's joins
//! mention (the DuckDB binder projects the rest away before the graph is even
//! built). Projecting a table onto its join columns is exactly what turns
//! distinct rows into identical ones -- `watdiv1052651` has 4,491,142 rows over
//! 39,781 distinct values of `s` -- and one record per row means every later
//! join re-pairs all 113 copies of a value instead of pairing it once. That is
//! multiplication where the shape called for addition, and it is what made a
//! seven-relation star hold 69,985,962 records for an answer of 39,891,620
//! (DECISIONS.md D35).
//!
//! Open addressing rather than the ChainingHashTable next door, which collects
//! entries and links them in Finalize(): grouping has to answer "have I seen
//! this row" while it is still reading, and that table cannot be probed until
//! it is closed.
//!
//! A match is confirmed by comparing the row's values, never by the hash alone.
//! The hash only says where to look. KeyReader above may compare packed keys
//! directly because it refuses anything wider than 64 bits; a scan has no such
//! cap, so trusting a hash here would silently merge two different rows and
//! undercount every query over the relation.
class RowGrouper {
public:
	//! `budget` caps what the table itself may occupy; 0 means uncapped. The
	//! table is heap, not arena, so the representation's own memory limit does
	//! not see it -- and the shape that defeats the sample below (dense
	//! duplicates early, distinct rows later) is exactly the one that would
	//! grow it to one entry per row behind that limit's back.
	RowGrouper(const std::vector<std::vector<int64_t>> &columns, size_t rows, size_t budget)
	    : columns(columns), budget(budget) {
		size_t capacity = 16;
		while (capacity < kMaxInitialSlots && capacity < rows * 2) {
			capacity *= 2;
		}
		slots.assign(capacity, Slot());
		mask = capacity - 1;
	}

	bool Active() const {
		return active;
	}

	//! The record already standing for a row identical to `row`, or null -- in
	//! which case the caller makes one and hands it to Added().
	Byte *Find(size_t row, uint64_t hash) {
		probe = hash & mask;
		while (slots[probe].data) {
			if (slots[probe].hash == hash && RowsEqual(columns, slots[probe].row, row)) {
				return slots[probe].data;
			}
			probe = (probe + 1) & mask;
		}
		return nullptr;
	}

	//! Takes the group Find() just failed to find; `probe` still points at the
	//! empty slot it stopped on.
	void Added(size_t row, uint64_t hash, Byte *data) {
		slots[probe] = Slot {data, hash, row};
		groups++;
		Seen();
		// Only while still grouping: Seen() may have just freed the table, and
		// growing an empty one underflows its mask.
		if (active && groups * 10 > slots.size() * 7) {
			Grow();
		}
	}

	//! Takes a hit, which changes no slot.
	void Matched() {
		Seen();
	}

	//! Stops grouping and releases the table. Public because MakeScan may
	//! decide against grouping before it reads a single row.
	void Abandon() {
		active = false;
		std::vector<Slot>().swap(slots);
	}

private:
	struct Slot {
		Byte *data = nullptr;
		uint64_t hash = 0;
		size_t row = 0;
	};

	//! Where the table starts, so a scan of ten rows does not allocate for a
	//! million. It grows from here on demand.
	static const size_t kMaxInitialSlots = 1024;

	void Grow() {
		if (budget != 0 && slots.size() * 2 * sizeof(Slot) > budget) {
			// Refusing to grow would leave the table full and every probe
			// walking it, so this abandons rather than caps.
			Abandon();
			return;
		}
		std::vector<Slot> bigger(slots.size() * 2);
		const size_t bigger_mask = bigger.size() - 1;
		for (const auto &slot : slots) {
			if (!slot.data) {
				continue;
			}
			size_t at = slot.hash & bigger_mask;
			while (bigger[at].data) {
				at = (at + 1) & bigger_mask;
			}
			bigger[at] = slot;
		}
		slots.swap(bigger);
		mask = bigger_mask;
	}

	//! Grouping is an optimization, and a relation whose rows are already
	//! distinct pays for it and gets nothing: a table as large as the input, to
	//! hold one entry per row. So it is abandoned once the rows read so far are
	//! nearly all distinct, which bounds what the attempt can cost to the
	//! sample.
	//!
	//! Abandoning is safe at any point precisely because the weights are a
	//! property of each record and not of the relation: what is already grouped
	//! stays grouped, every row after it gets a record of its own, and the two
	//! together denote the same bag either way. That is why this can be a
	//! guess -- being wrong costs time, never an answer.
	void Seen() {
		seen++;
		if (seen != kSampleRows) {
			return;
		}
		if (groups * 100 > seen * kGroupBelowPercent) {
			Abandon();
		}
	}

	const std::vector<std::vector<int64_t>> &columns;
	size_t budget = 0;
	std::vector<Slot> slots;
	size_t mask = 0;
	size_t probe = 0;
	size_t groups = 0;
	size_t seen = 0;
	bool active = true;
};

} // namespace

FactorizedRelation MakeScan(const std::vector<AttributeId> &attributes, const AttributeTypes &types,
                            const std::vector<std::vector<int64_t>> &columns) {
	if (columns.size() != attributes.size()) {
		throw std::runtime_error("MakeScan: one column per attribute expected");
	}
	const size_t rows = columns.empty() ? 0 : columns[0].size();
	// Decided before the relation exists, because the layout fixes whether a
	// record has room for a multiplicity at all, and that room is not free:
	// records align to 16 bytes, so the field costs 16 of them. Reserving it on
	// a relation whose rows are already distinct measured +40% memory for
	// nothing (DECISIONS.md D35).
	const bool group = DistinctPercentOfSample(columns, rows) <= kGroupBelowPercent;
	FactorizedRelation relation(FTree::Scan(attributes), types, group);
	// One caller (table_function.cpp) used to build `columns` by filtering NULLs
	// independently per column, which desynchronizes rows the moment one column
	// has a NULL and a sibling doesn't -- a relation's several columns must
	// describe the same rows in the same order, or this zips unrelated values
	// together. Catch it here, at the one place that assumes it, rather than
	// trusting every future caller to get it right.
	for (size_t c = 0; c < columns.size(); c++) {
		if (columns[c].size() != rows) {
			throw std::runtime_error("MakeScan: column " + std::to_string(c) + " has " +
			                         std::to_string(columns[c].size()) + " rows, expected " + std::to_string(rows) +
			                         " (columns of one relation must be row-aligned)");
		}
	}
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
	auto write = [&](Record record, size_t row) {
		for (size_t c = 0; c < attributes.size(); c++) {
			const auto payload = payload_of[c];
			if (level.payload[payload].type == ValueType::INT32) {
				rep.SetInt32(record, payload, static_cast<int32_t>(columns[c][row]));
			} else {
				rep.SetInt64(record, payload, columns[c][row]);
			}
		}
	};

	// Identical rows become one record that counts them. Roots come out in
	// first-occurrence order: the input's order, with each row's later
	// duplicates pulled up to the first of them.
	RowGrouper grouper(columns, group ? rows : 0, g_memory_limit == 0 ? 0 : g_memory_limit / 8);
	if (!group) {
		grouper.Abandon();
	}
	for (size_t row = 0; row < rows; row++) {
		if (grouper.Active()) {
			const uint64_t hash = HashRow(columns, row);
			if (auto *seen = grouper.Find(row, hash)) {
				const Record record(0, seen);
				rep.SetWeight(record, CheckedCardinalityAdd(rep.GetWeight(record), 1));
				grouper.Matched();
				continue;
			}
			auto record = rep.AppendRoot();
			write(record, row);
			grouper.Added(row, hash, record.Data());
			continue;
		}
		auto record = rep.AppendRoot();
		write(record, row);
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
//!
//! That 64-bit cap is a CORRECTNESS guarantee for the packing below, not only a
//! limit on what is supported. An INT64 column consumes the whole word, so the
//! cap is what makes it necessarily the only column and therefore necessarily
//! at offset zero -- which is why the INT64 case can write the value straight
//! in. Widen the cap without giving that case a real bit offset and every
//! multi-column key containing an INT64 collides into one hash key: wrong
//! counts, silently, with no throw anywhere. Adding a `<< bit` there is not the
//! fix either, since shifting a 64-bit value by 32 discards half of it; a wider
//! key needs the values stored for verification, as above.
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
				// Necessarily the only column, so necessarily at offset zero:
				// MakeKeyReader has already refused anything that would put a
				// 64-bit column beside another. Assigned rather than or-ed to
				// say so at the point it is relied on.
				packed = static_cast<uint64_t>(input.GetInt64(record, column.payload_index));
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
					reader.columns.push_back(KeyReader::Column {static_cast<uint32_t>(source_index),
					                                            static_cast<uint32_t>(payload),
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
	// Also what keeps KeyReader::Read's packing correct: rejecting anything over
	// 64 bits is what leaves a 64-bit column alone in the word. See the comment
	// on KeyReader before relaxing this.
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
	// The lower side builds its records here rather than through
	// MaterializeSubtree, so it needs the multiplicity carried over too --
	// otherwise a materialized join silently drops every duplicate the scan
	// grouped, and disagrees with the fused count over the same inputs.
	const int64_t weight = WeightOf(plan, plan_index, input, ctx);
	if (weight != 1) {
		output.SetWeight(target, weight);
	}
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
		if (std::find_if(out_types.begin(), out_types.end(), [&](const std::pair<AttributeId, ValueType> &existing) {
			    return existing.first == entry.first;
		    }) == out_types.end()) {
			out_types.push_back(entry);
		}
	}

	// The output can only carry multiplicities if one of the inputs already
	// does; a join never invents them.
	FactorizedRelation result(merge.tree, out_types, upper.Rep().HasWeights() || lower.Rep().HasWeights());
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
		snapshots.SetMemoryLimit(GetGlobalMemoryLimit());
		ChainingHashTable<const Record *> table;
		table.SetMemoryLimit(GetGlobalMemoryLimit());
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
		table.SetMemoryLimit(GetGlobalMemoryLimit());
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
		// The multiplicity the output record would have carried, had one been
		// built. This walk exists precisely because it is not.
		int64_t size = WeightOf(plan, plan_index, input, ctx);
		for (const auto &child : plan.Level(plan_index).children) {
			int64_t slot_total = 0;
			if (on_key_path[child.first]) {
				// Fixed by the indexing pass: exactly one instance.
				slot_total = Of(child.first);
			} else {
				IterateLevel(plan, child.first, input, ctx, 0,
				            [&]() { slot_total = CheckedCardinalityAdd(slot_total, Of(child.first)); });
			}
			size = CheckedCardinalityMul(size, slot_total);
			if (size == 0) {
				return 0;
			}
		}
		return size;
	}
};

//! One indexed lower-side instance: the number of tuples it would contribute,
//! and whether any upper tuple ever reached it.
//!
//! The flag is the paper's build-side "marker" (section 4.8). It costs a byte
//! per entry and one store per match, and it is only read when the lower side
//! is preserved.
struct LowerEntry {
	int64_t size;
	bool matched;
};

//! What the upper walk does with the matched weight once it has it.
//!
//! MARK_ONLY exists because a semi- or anti-join that emits the *lower* side
//! gets its answer from the sweep over the hash entries, not from the upper
//! walk -- but the walk still has to run, because it is what sets the marks.
//! It deliberately leaves `size` alone rather than returning zero: returning
//! zero would make an ancestor's slot product zero and short-circuit the rest
//! of the walk, and the branches it skipped would then never mark theirs.
enum class UpperFold : uint8_t { PRODUCT, PRESERVE, SEMI, ANTI, MARK_ONLY };

//! Cardinality of the whole join output, walking the upper side's plan and
//! folding the matching lower subtree sizes in at the insertion level.
struct OutputCounter {
	const MaterializePlan &plan;
	const FRepresentation &input;
	FlattenContext &ctx;
	int32_t insertion_level;
	const KeyReader &key;
	ChainingHashTable<LowerEntry> &table;
	size_t *matches;
	//! What the matched weight becomes at the insertion level. This is the only
	//! place the four join kinds differ.
	UpperFold fold;
	//! Whether the marker has to be maintained at all.
	bool mark_lower;

	int64_t Of(uint32_t plan_index) {
		const auto &level = plan.Level(plan_index);
		int64_t size = WeightOf(plan, plan_index, input, ctx);
		for (const auto &child : level.children) {
			int64_t slot_total = 0;
			IterateLevel(plan, child.first, input, ctx, 0,
			            [&]() { slot_total = CheckedCardinalityAdd(slot_total, Of(child.first)); });
			size = CheckedCardinalityMul(size, slot_total);
			if (size == 0) {
				// This upper record encodes no tuples at all, so there is
				// nothing here for an outer join to preserve and nothing that
				// may mark a lower entry as matched. Preservation is about
				// tuples that exist and found no partner.
				return 0;
			}
		}
		if (static_cast<int32_t>(level.output_level) == insertion_level) {
			// The slot the other side would have been attached to contributes
			// the summed size of everything that matches this key.
			int64_t matched = 0;
			table.ForEachMatchMutable(key.Read(plan, input, ctx), [&](LowerEntry &entry) {
				matched = CheckedCardinalityAdd(matched, entry.size);
				if (mark_lower) {
					entry.matched = true;
				}
				if (matches) {
					(*matches)++;
				}
			});
			switch (fold) {
			case UpperFold::PRODUCT:
				size = CheckedCardinalityMul(size, matched);
				break;
			case UpperFold::PRESERVE:
				// Null-extension, counted rather than represented: one output
				// tuple per upper tuple, whatever the lower side's columns
				// would have been.
				size = CheckedCardinalityMul(size, matched == 0 ? 1 : matched);
				break;
			case UpperFold::SEMI:
				// The partner is a test, not a factor. These upper tuples
				// appear once each or not at all, however many partners they
				// have -- which is why a semi-join cannot overflow where the
				// inner join it filters would.
				size = matched > 0 ? size : 0;
				break;
			case UpperFold::ANTI:
				size = matched == 0 ? size : 0;
				break;
			case UpperFold::MARK_ONLY:
				// Marks are set above; `size` is discarded by the caller.
				break;
			}
		}
		return size;
	}
};

} // namespace

int64_t FactorizedCountJoin(const FactorizedRelation &build, const FactorizedRelation &probe, const JoinKeys &keys,
                            JoinMode mode, PathStrategy strategy, JoinStats *stats, Preserve preserve, JoinKind kind) {
	// Reject the combinations that have no meaning rather than picking one.
	// A semi-join over "neither side" or "both sides" is not a conservative
	// reading of an ambiguous request, it is a caller bug, and the count it
	// would return looks perfectly ordinary.
	const bool filtering = (kind == JoinKind::SEMI || kind == JoinKind::ANTI);
	if (filtering && (preserve == Preserve::NEITHER || preserve == Preserve::BOTH)) {
		throw std::runtime_error("join: a semi- or anti-join must emit exactly one side");
	}
	const bool build_on_top = (mode == JoinMode::BOTTOM_INSERT);
	// `preserve` names the arguments; the counters think in upper and lower.
	// Which argument is which depends on the mode, so the translation happens
	// once, here, rather than at each use.
	const bool preserve_build = (preserve == Preserve::BUILD || preserve == Preserve::BOTH);
	const bool preserve_probe = (preserve == Preserve::PROBE || preserve == Preserve::BOTH);
	const bool preserve_upper = build_on_top ? preserve_build : preserve_probe;
	const bool preserve_lower = build_on_top ? preserve_probe : preserve_build;

	// For SEMI/ANTI, "preserved" reads as "emitted". When the emitted side is
	// the lower one the upper walk contributes nothing and only sets marks, and
	// the answer comes from the sweep.
	UpperFold fold = UpperFold::PRODUCT;
	if (kind == JoinKind::PRODUCT && preserve_upper) {
		fold = UpperFold::PRESERVE;
	} else if (kind == JoinKind::SEMI) {
		fold = preserve_upper ? UpperFold::SEMI : UpperFold::MARK_ONLY;
	} else if (kind == JoinKind::ANTI) {
		fold = preserve_upper ? UpperFold::ANTI : UpperFold::MARK_ONLY;
	}
	const bool emit_upper = !(filtering && preserve_lower);
	const bool mark_lower = preserve_lower;
	// Which entries the sweep collects: an outer or anti join wants the ones
	// nothing reached, a semi join wants the ones something did.
	const bool sweep_lower = mark_lower;
	const bool sweep_matched = (kind == JoinKind::SEMI);
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
	ChainingHashTable<LowerEntry> table;
	table.SetMemoryLimit(GetGlobalMemoryLimit());
	IterateKeyPath(lower_plan, 0, lower_key.plan_index, lower_parents, lower.Rep(), lower_ctx, [&]() {
		LowerSizeCounter counter {lower_plan, lower_on_key_path, lower.Rep(), lower_ctx};
		table.Insert(lower_key.Read(lower_plan, lower.Rep(), lower_ctx), LowerEntry {counter.Of(0), false});
	});
	table.Finalize();
	if (stats) {
		stats->build_keys = table.Size();
	}

	int64_t total = 0;
	size_t matches = 0;
	OutputCounter counter {upper_plan, upper.Rep(), upper_ctx, static_cast<int32_t>(merge.insertion_level),
	                       upper_key,  table,       &matches,  fold,
	                       mark_lower};
	IterateLevel(upper_plan, 0, upper.Rep(), upper_ctx, 0, [&]() {
		if (stats) {
			stats->probe_rows++;
		}
		const auto contributed = counter.Of(0);
		if (emit_upper) {
			total = CheckedCardinalityAdd(total, contributed);
		}
	});

	if (sweep_lower) {
		// The other half of the paper's sketch, and the same sweep serves three
		// kinds. An outer join adds every lower instance nothing reached, since
		// each is null-extended on the upper columns and contributes its own
		// tuples and no more. An anti-join emits exactly those same instances,
		// and a semi-join emits their complement. One pass over the entries the
		// build phase already created.
		table.ForEachValue([&](const LowerEntry &entry) {
			if (entry.matched == sweep_matched) {
				total = CheckedCardinalityAdd(total, entry.size);
			}
		});
	}

	if (stats) {
		stats->matches = matches;
	}
	return total;
}

} // namespace factorize
