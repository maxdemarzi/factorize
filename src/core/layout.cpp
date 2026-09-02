#include "layout.hpp"

#include <algorithm>
#include <cassert>
#include <map>
#include <stdexcept>

namespace factorize {

size_t ValueTypeSize(ValueType type) {
	switch (type) {
	case ValueType::INT32:
		return 4;
	case ValueType::INT64:
		return 8;
	}
	return 0;
}

const char *ValueTypeName(ValueType type) {
	switch (type) {
	case ValueType::INT32:
		return "INT32";
	case ValueType::INT64:
		return "INT64";
	}
	return "?";
}

namespace {

//! Rounds `offset` up to `alignment`, which is always a power of two here.
uint32_t AlignTo(uint32_t offset, uint32_t alignment) {
	return (offset + alignment - 1) & ~(alignment - 1);
}

//! Assigns a level id to every node, depth-first with the root at 0, and
//! records each node's children so sizes can be resolved bottom-up afterwards.
void Flatten(const FNode &node, std::vector<const FNode *> &nodes, std::vector<std::vector<LevelId>> &children) {
	const auto self = static_cast<LevelId>(nodes.size());
	nodes.push_back(&node);
	children.emplace_back();
	for (auto &child : node.Children()) {
		const auto child_level = static_cast<LevelId>(nodes.size());
		children[self].push_back(child_level);
		Flatten(*child, nodes, children);
	}
}

} // namespace

Layout Layout::FromFTree(const FTree &tree, const std::vector<std::pair<AttributeId, ValueType>> &types) {
	std::map<AttributeId, ValueType> type_of;
	for (auto &entry : types) {
		type_of[entry.first] = entry.second;
	}

	std::vector<const FNode *> nodes;
	std::vector<std::vector<LevelId>> children;
	Flatten(tree.Root(), nodes, children);

	if (nodes.size() > UINT16_MAX) {
		throw std::runtime_error("f-tree has more levels than a LevelId can address");
	}

	Layout layout;
	layout.levels.resize(nodes.size());
	for (size_t i = 0; i < nodes.size(); i++) {
		layout.levels[i].attributes = nodes[i]->Attributes();
		layout.levels[i].needs_lock = nodes[i]->RequiresLock();
	}

	// Sizes resolve bottom-up: a record inlines its first child of every slot,
	// so a parent's size depends on its children's. Flatten() emits parents
	// before children, so reverse order visits every child first.
	for (size_t i = nodes.size(); i-- > 0;) {
		auto &level = layout.levels[i];

		// The cached subtree cardinality leads the record: it is read on every
		// aggregate traversal, and putting it first keeps it on the same cache
		// line as the payload.
		uint32_t offset = 0;
		level.size_offset = offset;
		offset += static_cast<uint32_t>(sizeof(uint64_t));

		// Payload, widest first, so no padding is needed between columns.
		std::vector<std::pair<AttributeId, ValueType>> columns;
		columns.reserve(level.attributes.size());
		for (auto attribute : level.attributes) {
			auto entry = type_of.find(attribute);
			if (entry == type_of.end()) {
				throw std::runtime_error("no value type given for attribute " + std::to_string(attribute));
			}
			columns.emplace_back(attribute, entry->second);
		}
		std::stable_sort(columns.begin(), columns.end(), [](const auto &a, const auto &b) {
			return ValueTypeSize(a.second) > ValueTypeSize(b.second);
		});
		for (auto &column : columns) {
			const auto width = static_cast<uint32_t>(ValueTypeSize(column.second));
			offset = AlignTo(offset, width);
			level.payload.push_back(ValueDesc {column.first, column.second, offset});
			offset += width;
		}

		// One slot per child: the inlined first record (section 5.3) followed by
		// the overflow list header.
		for (auto child_level : children[i]) {
			const auto &child = layout.levels[child_level];
			assert(child.record_size > 0 && "children must be sized before their parent");

			SlotDesc slot {};
			slot.child_level = child_level;
			offset = AlignTo(offset, alignof(std::max_align_t));
			slot.inline_offset = offset;
			offset += child.record_size;
			offset = AlignTo(offset, static_cast<uint32_t>(alignof(ChildListHeader)));
			slot.header_offset = offset;
			offset += static_cast<uint32_t>(sizeof(ChildListHeader));
			level.slots.push_back(slot);
		}

		level.record_size = AlignTo(offset, alignof(std::max_align_t));
	}
	return layout;
}

int Layout::LevelOf(AttributeId attribute) const {
	for (size_t i = 0; i < levels.size(); i++) {
		const auto &attributes = levels[i].attributes;
		if (std::find(attributes.begin(), attributes.end(), attribute) != attributes.end()) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

std::string Layout::ToString(const AttributeNamer &namer) const {
	std::string result;
	for (size_t i = 0; i < levels.size(); i++) {
		const auto &level = levels[i];
		result += "L" + std::to_string(i) + " {";
		for (size_t j = 0; j < level.attributes.size(); j++) {
			if (j > 0) {
				result += ",";
			}
			result += namer(level.attributes[j]);
		}
		result += "} size=" + std::to_string(level.record_size);
		if (!level.slots.empty()) {
			result += " slots=[";
			for (size_t j = 0; j < level.slots.size(); j++) {
				if (j > 0) {
					result += ",";
				}
				result += "L" + std::to_string(level.slots[j].child_level);
			}
			result += "]";
		}
		if (level.needs_lock) {
			result += " lock";
		}
		result += "\n";
	}
	return result;
}

} // namespace factorize
