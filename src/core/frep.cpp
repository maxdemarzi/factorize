#include "frep.hpp"

#include <cassert>
#include <cstring>
#include <stdexcept>

namespace factorize {

size_t FRepresentation::SegmentHeaderSize() {
	// Records that follow the header must keep max_align_t alignment.
	constexpr size_t align = alignof(std::max_align_t);
	return (sizeof(OverflowSegment) + align - 1) & ~(align - 1);
}

OverflowSegment *FRepresentation::AllocateSegment(uint32_t capacity, uint32_t record_size) {
	const size_t bytes = SegmentHeaderSize() + static_cast<size_t>(capacity) * record_size;
	// Segments are the only place the representation grows, and they are
	// allocated with doubling capacity, so checking here costs nothing
	// amortized and cannot be overshot by more than one segment.
	if (memory_limit != 0 && arena.BytesAllocated() + bytes > memory_limit) {
		throw std::runtime_error("f-representation exceeded its memory limit");
	}
	auto *segment = reinterpret_cast<OverflowSegment *>(arena.Allocate(bytes));
	segment->next = nullptr;
	segment->count = 0;
	segment->capacity = capacity;
	return segment;
}

Record FRepresentation::AppendRoot() {
	const auto record_size = layout->Level(0).record_size;
	if (!root_tail || root_tail->count == root_tail->capacity) {
		auto *segment = AllocateSegment(next_root_capacity, record_size);
		if (root_tail) {
			root_tail->next = segment;
		} else {
			root_head = segment;
		}
		root_tail = segment;
		// Root nodes usually have high cardinality (section 5.2), so grow fast.
		if (next_root_capacity < (1u << 16)) {
			next_root_capacity *= 2;
		}
	}
	auto *base = reinterpret_cast<Byte *>(root_tail) + SegmentHeaderSize();
	auto *data = base + static_cast<size_t>(root_tail->count) * record_size;
	root_tail->count++;
	roots++;
	records++;
	// The arena hands back zeroed memory, so every ChildListHeader in the
	// record -- including those of inlined children -- already reads as empty.
	return Record(0, data);
}

Record FRepresentation::InsertChild(Record parent, size_t slot_index) {
	assert(parent.IsValid());
	const auto &level = layout->Level(parent.Level());
	assert(slot_index < level.slots.size());
	const auto &slot = level.slots[slot_index];
	auto *header = HeaderOf(parent, slot);

	records++;

	if (header->count == 0) {
		// Section 5.3: the first child needs no indirection at all -- it is
		// already sitting inside the parent record.
		header->count = 1;
		return Record(slot.child_level, parent.Data() + slot.inline_offset);
	}

	const auto child_size = layout->Level(slot.child_level).record_size;
	auto *tail = static_cast<OverflowSegment *>(header->overflow_tail);
	if (!tail || tail->count == tail->capacity) {
		// Doubling capacities, as in the paper's FastDeque: chunk i holds 2^i
		// entries. Amortized O(1) appends, and no record is ever relocated.
		const uint32_t capacity = tail ? tail->capacity * 2 : 1;
		auto *segment = AllocateSegment(capacity, child_size);
		if (tail) {
			tail->next = segment;
		} else {
			header->overflow_head = segment;
		}
		header->overflow_tail = segment;
		tail = segment;
	}
	auto *base = reinterpret_cast<Byte *>(tail) + SegmentHeaderSize();
	auto *data = base + static_cast<size_t>(tail->count) * child_size;
	tail->count++;
	header->count++;
	return Record(slot.child_level, data);
}

//===--------------------------------------------------------------------===//
// Payload
//===--------------------------------------------------------------------===//
void FRepresentation::SetInt32(Record record, size_t payload_index, int32_t value) {
	const auto &desc = layout->Level(record.Level()).payload[payload_index];
	assert(desc.type == ValueType::INT32);
	std::memcpy(record.Data() + desc.offset, &value, sizeof(value));
}

void FRepresentation::SetInt64(Record record, size_t payload_index, int64_t value) {
	const auto &desc = layout->Level(record.Level()).payload[payload_index];
	assert(desc.type == ValueType::INT64);
	std::memcpy(record.Data() + desc.offset, &value, sizeof(value));
}

int32_t FRepresentation::GetInt32(Record record, size_t payload_index) const {
	const auto &desc = layout->Level(record.Level()).payload[payload_index];
	int32_t value;
	std::memcpy(&value, record.Data() + desc.offset, sizeof(value));
	return value;
}

int64_t FRepresentation::GetInt64(Record record, size_t payload_index) const {
	const auto &desc = layout->Level(record.Level()).payload[payload_index];
	int64_t value;
	std::memcpy(&value, record.Data() + desc.offset, sizeof(value));
	return value;
}

int64_t FRepresentation::GetValue(Record record, AttributeId attribute) const {
	const auto &level = layout->Level(record.Level());
	for (size_t i = 0; i < level.payload.size(); i++) {
		if (level.payload[i].attribute != attribute) {
			continue;
		}
		return level.payload[i].type == ValueType::INT32 ? static_cast<int64_t>(GetInt32(record, i))
		                                                 : GetInt64(record, i);
	}
	return 0;
}

uint32_t FRepresentation::ChildCount(Record parent, size_t slot_index) const {
	const auto &slot = layout->Level(parent.Level()).slots[slot_index];
	return HeaderOf(parent, slot)->count;
}

//===--------------------------------------------------------------------===//
// Aggregation (paper sections 2.5 and 5.4)
//===--------------------------------------------------------------------===//
int64_t FRepresentation::SubtreeSize(Record record) const {
	// The cache stores size + 1, so a zeroed record reads as "not computed"
	// while still letting a genuine size of 0 -- an empty subtree, which
	// bottom-inserts produce routinely (section 4.6) -- be memoized.
	const auto &level = layout->Level(record.Level());
	uint64_t cached;
	std::memcpy(&cached, record.Data() + level.size_offset, sizeof(cached));
	if (cached != 0) {
		return static_cast<int64_t>(cached - 1);
	}

	int64_t size = 1;
	for (size_t slot_index = 0; slot_index < level.slots.size(); slot_index++) {
		// Siblings are a Cartesian product, so slots multiply; the children
		// within one slot are alternatives, so they sum.
		int64_t slot_total = 0;
		ForEachChild(record, slot_index, [&](Record child) { slot_total = CheckedCardinalityAdd(slot_total, SubtreeSize(child)); });
		size = CheckedCardinalityMul(size, slot_total);
		if (size == 0) {
			// An empty slot makes the whole subtree contribute nothing; the
			// remaining slots cannot change that.
			break;
		}
	}

	const uint64_t to_store = static_cast<uint64_t>(size) + 1;
	std::memcpy(record.Data() + level.size_offset, &to_store, sizeof(to_store));
	return size;
}

int64_t FRepresentation::Count() const {
	int64_t total = 0;
	ForEachRoot([&](Record root) { total = CheckedCardinalityAdd(total, SubtreeSize(root)); });
	return total;
}

//===--------------------------------------------------------------------===//
// Diagnostics
//===--------------------------------------------------------------------===//
void FRepresentation::AppendString(std::string &out, Record record, const AttributeNamer &namer, int indent) const {
	const auto &level = layout->Level(record.Level());
	out.append(static_cast<size_t>(indent) * 2, ' ');
	out += "L" + std::to_string(record.Level()) + "(";
	for (size_t i = 0; i < level.payload.size(); i++) {
		if (i > 0) {
			out += ",";
		}
		out += namer(level.payload[i].attribute) + "=" + std::to_string(GetValue(record, level.payload[i].attribute));
	}
	out += ")\n";
	for (size_t slot_index = 0; slot_index < level.slots.size(); slot_index++) {
		ForEachChild(record, slot_index, [&](Record child) { AppendString(out, child, namer, indent + 1); });
	}
}

std::string FRepresentation::ToString(const AttributeNamer &namer) const {
	std::string out;
	ForEachRoot([&](Record root) { AppendString(out, root, namer, 0); });
	return out;
}

} // namespace factorize
