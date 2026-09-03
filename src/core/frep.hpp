//===----------------------------------------------------------------------===//
//                         factorize
//
// core/frep.hpp
//
// The runtime f-representation: records laid out per the plan-time Layout,
// nested by value with the first child of every slot inlined (section 5.3).
//
// Two construction primitives, exactly as the paper states in section 5.2
// ("we can construct the f-representations with only two functions"):
//
//     AppendRoot()             -- creates the upper part, T_upper
//     InsertChild(parent, s)   -- creates the lower part, T_lower
//
// Top- versus bottom-insert is only a question of which input tree maps to
// which primitive; there is no separate code path.
//
// No DuckDB headers (plan section 4).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "arena.hpp"
#include "layout.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace factorize {

//! Cardinality arithmetic, checked. Sizes and counts here are never negative
//! by construction (they count records or tuples), so the only failure mode
//! is "too large to represent," and these throw rather than wrap.
//!
//! `SubtreeSize`/`Count` and their join.cpp counterparts (LowerSizeCounter,
//! OutputCounter -- the arithmetic FactorizedCountJoin actually runs for the
//! final join of every query) used to multiply and sum plain int64_t with no
//! check at all: a handful of joins with moderate fan-out compounds past
//! INT64_MAX while the f-representation itself stays tiny -- factorization
//! keeping the *representation* small is exactly what removes the flat
//! result's own size as a natural ceiling on the count. Deliberately not
//! __builtin_mul_overflow/__builtin_add_overflow: those are GCC/Clang
//! extensions, and this project's CI builds a plain-MSVC Windows target
//! (`windows_amd64`, distinct from `windows_amd64_mingw`) that does not
//! support them.
inline int64_t CheckedCardinalityAdd(int64_t a, int64_t b) {
	if (a > std::numeric_limits<int64_t>::max() - b) {
		throw std::runtime_error("factorized count exceeds the representable range (int64 overflow)");
	}
	return a + b;
}

inline int64_t CheckedCardinalityMul(int64_t a, int64_t b) {
	if (a != 0 && b > std::numeric_limits<int64_t>::max() / a) {
		throw std::runtime_error("factorized count exceeds the representable range (int64 overflow)");
	}
	return a * b;
}

//! A pointer to one record, tagged with the level whose layout describes it.
//!
//! Addresses are stable (see arena.hpp), so a raw pointer is a valid handle and
//! costs the same 8 bytes a packed {level, chunk, index} handle would, without
//! the decode. The level travels alongside rather than inside it.
class Record {
public:
	Record() = default;
	Record(LevelId level, Byte *data) : data(data), level(level) {
	}

	bool IsValid() const {
		return data != nullptr;
	}
	LevelId Level() const {
		return level;
	}
	Byte *Data() const {
		return data;
	}

	bool operator==(const Record &other) const {
		return data == other.data;
	}

private:
	Byte *data = nullptr;
	LevelId level = 0;
};

//! One overflow segment of a child list. Children beyond the inlined first one
//! live here. Segment capacities double, so appends are amortized O(1) and no
//! record ever moves.
struct OverflowSegment {
	OverflowSegment *next;
	uint32_t count;
	uint32_t capacity;
	// Followed by `capacity` child records, contiguously.
};

class FRepresentation {
public:
	explicit FRepresentation(const Layout &layout) : layout(&layout) {
	}

	const Layout &GetLayout() const {
		return *layout;
	}

	//! Caps how much this representation may allocate; 0 means unlimited.
	//!
	//! Checked where memory actually grows rather than between operators: a
	//! single join can allocate far more than the whole budget before it
	//! returns, so a between-joins check is no protection at all. This is the
	//! standalone form of what plan Phase 7 item 4 requires against DuckDB's
	//! `memory_limit` -- an f-representation that silently exhausts memory is
	//! worse than one that declines.
	void SetMemoryLimit(size_t bytes) {
		memory_limit = bytes;
	}

	//===------------------------------------------------------------------===//
	// Construction
	//===------------------------------------------------------------------===//

	//! Appends a new root record (T_upper).
	Record AppendRoot();

	//! Appends a child to `slot_index` of `parent` and returns it (T_lower).
	//!
	//! The first child of a slot goes into the inlined space already present in
	//! the parent record, so a tuple joining with exactly one other tuple costs
	//! no indirection and the layout degenerates to a flat row -- section 5.3's
	//! "adaptive factorization". Later children go to overflow segments.
	Record InsertChild(Record parent, size_t slot_index);

	//===------------------------------------------------------------------===//
	// Payload access
	//===------------------------------------------------------------------===//

	void SetInt32(Record record, size_t payload_index, int32_t value);
	void SetInt64(Record record, size_t payload_index, int64_t value);
	int32_t GetInt32(Record record, size_t payload_index) const;
	int64_t GetInt64(Record record, size_t payload_index) const;

	//! Reads a payload column by attribute, widening to int64 regardless of
	//! stored width. Convenience for tests and diagnostics.
	int64_t GetValue(Record record, AttributeId attribute) const;

	//===------------------------------------------------------------------===//
	// Traversal
	//===------------------------------------------------------------------===//

	//! Number of children in a slot, including the inlined first one.
	uint32_t ChildCount(Record parent, size_t slot_index) const;

	//! Invokes `fn(Record)` for each child of a slot, inline child first.
	template <typename Fn>
	void ForEachChild(Record parent, size_t slot_index, Fn &&fn) const {
		const auto &level = layout->Level(parent.Level());
		const auto &slot = level.slots[slot_index];
		const auto *header = HeaderOf(parent, slot);
		if (header->count == 0) {
			return;
		}
		Record first(slot.child_level, parent.Data() + slot.inline_offset);
		if (!IsPruned(first)) {
			fn(first);
		}
		if (header->count == 1) {
			return;
		}
		const auto child_size = layout->Level(slot.child_level).record_size;
		for (auto *segment = static_cast<OverflowSegment *>(header->overflow_head); segment; segment = segment->next) {
			auto *base = reinterpret_cast<Byte *>(segment) + SegmentHeaderSize();
			for (uint32_t i = 0; i < segment->count; i++) {
				Record child(slot.child_level, base + static_cast<size_t>(i) * child_size);
				if (!IsPruned(child)) {
					fn(child);
				}
			}
		}
	}

	//! Invokes `fn(Record)` for each root record.
	template <typename Fn>
	void ForEachRoot(Fn &&fn) const {
		if (layout->LevelCount() == 0) {
			return;
		}
		const auto record_size = layout->Level(0).record_size;
		for (auto *segment = root_head; segment; segment = segment->next) {
			auto *base = reinterpret_cast<Byte *>(segment) + SegmentHeaderSize();
			for (uint32_t i = 0; i < segment->count; i++) {
				Record root(0, base + static_cast<size_t>(i) * record_size);
				if (!IsPruned(root)) {
					fn(root);
				}
			}
		}
	}

	//! Computes every subtree size, then starts skipping the empty ones.
	//!
	//! An inner join drops a probe tuple that matches nothing, but a factorized
	//! join still leaves a record behind for it -- one whose subtree encodes no
	//! tuples and therefore contributes 0. Those records are harmless to
	//! correctness and ruinous to cost: the next join copies them, the one after
	//! copies the copies, and the representation grows while the result shrinks.
	//!
	//! Skipping a zero-size subtree is always correct for an inner join, since
	//! it encodes no tuples at all. Sizes must be computed *before* pruning is
	//! enabled, because an uncomputed cache is indistinguishable from a size of
	//! zero.
	void PruneEmptySubtrees() {
		prune_empty = false;
		ForEachRoot([&](Record root) { SubtreeSize(root); });
		prune_empty = true;
	}

	//===------------------------------------------------------------------===//
	// Aggregation
	//===------------------------------------------------------------------===//

	//! Cardinality of the flat relation this f-representation encodes.
	//!
	//! Section 2.5: size(node) = product over slots of (sum over that slot's
	//! children of size(child)), with size(leaf) = 1. Cost is O(|f-rep|), not
	//! O(|flat result|) -- which is the entire point of counting without
	//! flattening. Subtree sizes are cached in each record as they are computed
	//! (section 5.4), so a subtree reached more than once is only counted once.
	int64_t Count() const;

	//! Cardinality of one subtree, memoized in the record.
	int64_t SubtreeSize(Record record) const;

	//===------------------------------------------------------------------===//
	// Diagnostics
	//===------------------------------------------------------------------===//

	size_t RootCount() const {
		return roots;
	}
	size_t RecordCount() const {
		return records;
	}
	//! Records still reachable once empty subtrees are skipped. The gap between
	//! this and RecordCount() is the dead weight a join leaves behind.
	size_t LiveRecordCount() const {
		size_t live = 0;
		ForEachRoot([&](Record root) { live += CountLive(root); });
		return live;
	}
	size_t BytesAllocated() const {
		return arena.BytesAllocated();
	}
	//! Renders the whole f-representation. Tests only -- this flattens.
	std::string ToString(const AttributeNamer &namer) const;

private:
	static size_t SegmentHeaderSize();

	ChildListHeader *HeaderOf(Record parent, const SlotDesc &slot) const {
		return reinterpret_cast<ChildListHeader *>(parent.Data() + slot.header_offset);
	}

	size_t CountLive(Record record) const {
		size_t live = 1;
		const auto &level = layout->Level(record.Level());
		for (size_t slot = 0; slot < level.slots.size(); slot++) {
			ForEachChild(record, slot, [&](Record child) { live += CountLive(child); });
		}
		return live;
	}

	//! True once pruning is armed and this record's cached size is 0. The cache
	//! stores size + 1, so 1 means a genuine size of zero.
	bool IsPruned(Record record) const {
		if (!prune_empty) {
			return false;
		}
		uint64_t cached;
		std::memcpy(&cached, record.Data() + layout->Level(record.Level()).size_offset, sizeof(cached));
		return cached == 1;
	}

	//! Allocates a segment able to hold `capacity` records of `record_size`.
	OverflowSegment *AllocateSegment(uint32_t capacity, uint32_t record_size);

	void AppendString(std::string &out, Record record, const AttributeNamer &namer, int indent) const;

	const Layout *layout;
	mutable Arena arena;
	//! Root records live in their own segment chain, mirroring the paper's
	//! `vector<NodeA> root`. Under Phase 4 parallelism each thread gets its own
	//! chain and Combine() links them (plan Phase 4).
	OverflowSegment *root_head = nullptr;
	OverflowSegment *root_tail = nullptr;
	//! Armed by PruneEmptySubtrees(); makes iteration skip zero-size records.
	bool prune_empty = false;
	size_t memory_limit = 0;
	uint32_t next_root_capacity = 8;
	size_t roots = 0;
	size_t records = 0;
};

} // namespace factorize
