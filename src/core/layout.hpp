//===----------------------------------------------------------------------===//
//                         factorize
//
// core/layout.hpp
//
// Plan-time record layout: turns an FTree into per-level descriptors.
//
// This is the substitute for code generation (plan section 0.3). The paper
// bakes the f-tree's shape into generated C++ structs:
//
//     struct NodeB { B b; NodeC frontC; vec<NodeC> tailC; };
//     struct NodeA { A a; NodeD frontD; NodeB frontB;
//                    vec<NodeD> tailD; vec<NodeB> tailB; };
//
// -- one level of indirection, with the first child of each list inlined into
// the parent (section 5.3). We cannot emit structs, but the f-tree is fixed for
// a whole query and known at *plan* time, so the same layout can be computed
// once into a descriptor and read once per level instead of once per node.
//
// A "level" here is one node of the f-tree, not one depth. Every runtime
// record at a given level therefore has an identical shape by construction,
// which is what restores branch predictability.
//
// No DuckDB headers (plan section 4).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "ftree.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace factorize {

//! Index of a level, i.e. of one f-tree node. Level 0 is the root.
using LevelId = uint16_t;

//! Value types a payload column may hold.
//!
//! Restricted to integers for v1 (DECISIONS.md D10): floating point breaks the
//! bit-exact `auto` == `off` invariant the Phase 7 fuzzer depends on, because
//! factorized and parallel summation both reassociate additions.
enum class ValueType : uint8_t { INT32, INT64 };

size_t ValueTypeSize(ValueType type);
const char *ValueTypeName(ValueType type);

//! One payload column inside a record.
struct ValueDesc {
	AttributeId attribute;
	ValueType type;
	//! Byte offset within the record.
	uint32_t offset;
};

//! One child list hanging off a record.
//!
//! Following section 5.3, the first child is stored *inline* in the parent, so
//! a tuple that joins with exactly one other tuple costs no indirection at all
//! and the layout degenerates to a flat row. Children beyond the first go to an
//! overflow list.
struct SlotDesc {
	//! Level of the records this slot holds.
	LevelId child_level;
	//! Offset of the inlined first child record.
	uint32_t inline_offset;
	//! Offset of the ChildList header (count + overflow chain).
	uint32_t header_offset;
};

//! The shape shared by every record at one level.
struct LevelLayout {
	//! The f-tree node this level was derived from, for diagnostics.
	std::vector<AttributeId> attributes;
	//! Bytes per record, including inlined first children (so this is the size
	//! of the whole nested subtree's fixed part).
	uint32_t record_size = 0;
	//! Offset of the cached subtree cardinality (section 5.4). Counting walks
	//! the f-representation multiplying child subtree sizes, so this field is
	//! what makes count(*) cost O(|f-rep|) instead of O(|flat result|).
	uint32_t size_offset = 0;
	std::vector<ValueDesc> payload;
	std::vector<SlotDesc> slots;
	//! Set for the insertion point of a bottom-insert: many threads append to
	//! one upper-tree record there. Top-insert records need no lock (5.2).
	bool needs_lock = false;
};

//! Runtime header of one child list, embedded in the parent record.
//!
//! `count` is the number of children *including* the inlined first one, so
//! count == 0 means empty, count == 1 means the inline slot alone is live and
//! `overflow` is untouched.
struct ChildListHeader {
	uint32_t count;
	uint32_t reserved;
	//! First overflow segment; null while count <= 1.
	void *overflow_head;
	//! Last overflow segment, so appends stay O(1).
	void *overflow_tail;
};

//! The complete plan-time description of a query's f-representation.
class Layout {
public:
	//! Derives the layout from a finished f-tree. `types` supplies the value
	//! type of every attribute mentioned in the tree.
	static Layout FromFTree(const FTree &tree, const std::vector<std::pair<AttributeId, ValueType>> &types);

	const LevelLayout &Level(LevelId level) const {
		return levels[level];
	}
	size_t LevelCount() const {
		return levels.size();
	}
	//! Bytes of one root record, including everything inlined beneath it.
	uint32_t RootRecordSize() const {
		return levels.empty() ? 0 : levels[0].record_size;
	}
	//! Level holding a given attribute, or -1.
	int LevelOf(AttributeId attribute) const;

	//! Human-readable dump, used by tests and by EXPLAIN later.
	std::string ToString(const AttributeNamer &namer) const;

private:
	std::vector<LevelLayout> levels;
};

} // namespace factorize
