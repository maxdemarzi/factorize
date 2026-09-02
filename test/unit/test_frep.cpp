//===----------------------------------------------------------------------===//
//                         factorize
//
// test/unit/test_frep.cpp
//
// Layout, arena and runtime f-representation (plan Phase 1.2).
//
// The stability and random-bottom-insert cases here are the ones that decide
// whether bottom-inserts are possible at all -- which FINDINGS.md F4 shows is
// where essentially the entire benefit of factorization lives.
//
//===----------------------------------------------------------------------===//

#include "../../src/core/frep.hpp"
#include "../../src/core/layout.hpp"

#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace factorize;

static int g_failures = 0;
static int g_checks = 0;

static void ExpectTrue(bool condition, const std::string &what) {
	g_checks++;
	if (condition) {
		std::printf("  ok   %s\n", what.c_str());
		return;
	}
	g_failures++;
	std::printf("  FAIL %s\n", what.c_str());
}

template <typename T>
static void ExpectEq(const T &actual, const T &expected, const std::string &what) {
	g_checks++;
	if (actual == expected) {
		std::printf("  ok   %s\n", what.c_str());
		return;
	}
	g_failures++;
	std::printf("  FAIL %s (expected %lld, got %lld)\n", what.c_str(), static_cast<long long>(expected),
	            static_cast<long long>(actual));
}

static AttributeNamer NamerFor(const std::map<AttributeId, std::string> &names) {
	return [names](AttributeId id) -> std::string {
		auto entry = names.find(id);
		return entry == names.end() ? std::string("?") : entry->second;
	};
}

//===--------------------------------------------------------------------===//
// Layout
//===--------------------------------------------------------------------===//
static void TestLayout() {
	std::printf("Layout\n");
	enum : AttributeId { A = 0, B = 1, C = 2, D = 3 };
	auto namer = NamerFor({{A, "A"}, {B, "B"}, {C, "C"}, {D, "D"}});

	// The paper's Figure 5 tree: A with children D and B, and B with child C.
	FTree tree = FTree::Scan({A});
	tree.Root().AddChild(std::make_unique<FNode>(std::vector<AttributeId> {D}));
	auto &b = tree.Root().AddChild(std::make_unique<FNode>(std::vector<AttributeId> {B}));
	b.AddChild(std::make_unique<FNode>(std::vector<AttributeId> {C}));

	std::vector<std::pair<AttributeId, ValueType>> types = {
	    {A, ValueType::INT32}, {B, ValueType::INT32}, {C, ValueType::INT32}, {D, ValueType::INT32}};
	auto layout = Layout::FromFTree(tree, types);

	ExpectEq<size_t>(layout.LevelCount(), 4, "one level per f-tree node");
	ExpectEq<int>(layout.LevelOf(C), 3, "attribute C resolves to its level");

	// Every level's record inlines the first child of each slot, so a parent's
	// record is strictly larger than any of its children's.
	const auto &root = layout.Level(0);
	ExpectEq<size_t>(root.slots.size(), 2, "root has two slots");
	bool larger = true;
	for (auto &slot : root.slots) {
		larger = larger && root.record_size > layout.Level(slot.child_level).record_size;
	}
	ExpectTrue(larger, "a parent record subsumes its inlined children");

	// Slots must not overlap each other or the payload.
	bool disjoint = true;
	for (size_t i = 0; i < root.slots.size(); i++) {
		const auto &slot = root.slots[i];
		const auto child_size = layout.Level(slot.child_level).record_size;
		disjoint = disjoint && slot.header_offset >= slot.inline_offset + child_size;
		disjoint = disjoint && slot.inline_offset + child_size <= root.record_size;
		for (auto &value : root.payload) {
			disjoint = disjoint && value.offset < slot.inline_offset;
		}
	}
	ExpectTrue(disjoint, "payload and slots occupy disjoint ranges");
	std::printf("%s", layout.ToString(namer).c_str());
}

//===--------------------------------------------------------------------===//
// Address stability
//
// Plan Phase 1.2: append a million records and assert every handle taken along
// the way still resolves. Bottom-inserts depend on exactly this.
//===--------------------------------------------------------------------===//
static void TestStability() {
	std::printf("Address stability\n");
	enum : AttributeId { A = 0, B = 1 };

	FTree tree = FTree::Scan({A});
	tree.Root().AddChild(std::make_unique<FNode>(std::vector<AttributeId> {B}));
	auto layout = Layout::FromFTree(tree, {{A, ValueType::INT64}, {B, ValueType::INT64}});

	FRepresentation frep(layout);
	constexpr int kRoots = 1000000;
	std::vector<Record> handles;
	handles.reserve(kRoots);
	for (int i = 0; i < kRoots; i++) {
		auto root = frep.AppendRoot();
		frep.SetInt64(root, 0, i);
		handles.push_back(root);
	}
	bool all_valid = true;
	for (int i = 0; i < kRoots; i++) {
		if (frep.GetInt64(handles[static_cast<size_t>(i)], 0) != i) {
			all_valid = false;
			break;
		}
	}
	ExpectTrue(all_valid, "1,000,000 handles survive arena growth");

	// Now insert into arbitrary *earlier* records, which is the operation a
	// contiguous offset-based layout cannot do (plan section 2.3).
	std::mt19937 rng(12345);
	std::uniform_int_distribution<int> pick(0, kRoots - 1);
	for (int i = 0; i < 200000; i++) {
		auto parent = handles[static_cast<size_t>(pick(rng))];
		auto child = frep.InsertChild(parent, 0);
		frep.SetInt64(child, 0, i);
	}
	all_valid = true;
	for (int i = 0; i < kRoots; i++) {
		if (frep.GetInt64(handles[static_cast<size_t>(i)], 0) != i) {
			all_valid = false;
			break;
		}
	}
	ExpectTrue(all_valid, "random bottom-inserts do not disturb earlier records");

	int64_t children = 0;
	for (auto handle : handles) {
		children += frep.ChildCount(handle, 0);
	}
	ExpectEq<int64_t>(children, 200000, "every inserted child is reachable");
}

//===--------------------------------------------------------------------===//
// Inlining (section 5.3)
//===--------------------------------------------------------------------===//
static void TestInlining() {
	std::printf("Inlining\n");
	enum : AttributeId { A = 0, B = 1 };

	FTree tree = FTree::Scan({A});
	tree.Root().AddChild(std::make_unique<FNode>(std::vector<AttributeId> {B}));
	auto layout = Layout::FromFTree(tree, {{A, ValueType::INT64}, {B, ValueType::INT64}});
	const auto &slot = layout.Level(0).slots[0];

	FRepresentation frep(layout);
	auto root = frep.AppendRoot();
	auto first = frep.InsertChild(root, 0);
	frep.SetInt64(first, 0, 111);

	// The single-child case must live inside the parent and leave the overflow
	// chain untouched: that is what makes the layout degrade to a flat row.
	ExpectTrue(first.Data() == root.Data() + slot.inline_offset, "the first child is inlined into the parent");
	const auto *header = reinterpret_cast<const ChildListHeader *>(root.Data() + slot.header_offset);
	ExpectTrue(header->overflow_head == nullptr, "a one-child slot never allocates overflow");

	auto second = frep.InsertChild(root, 0);
	frep.SetInt64(second, 0, 222);
	ExpectTrue(header->overflow_head != nullptr, "the second child starts the overflow chain");
	ExpectEq<int64_t>(frep.GetInt64(first, 0), 111, "the inlined child survives overflow allocation");

	std::vector<int64_t> seen;
	frep.ForEachChild(root, 0, [&](Record child) { seen.push_back(frep.GetInt64(child, 0)); });
	ExpectTrue(seen == std::vector<int64_t> {111, 222}, "iteration yields the inline child first");
}

//===--------------------------------------------------------------------===//
// Counting without flattening (section 2.5)
//===--------------------------------------------------------------------===//
static void TestCount() {
	std::printf("Counting\n");
	enum : AttributeId { A = 0, B = 1, C = 2 };
	auto namer = NamerFor({{A, "A"}, {B, "B"}, {C, "C"}});

	// A with independent children B and C: the count is the Cartesian product
	// of the two child lists, summed over roots.
	FTree tree = FTree::Scan({A});
	tree.Root().AddChild(std::make_unique<FNode>(std::vector<AttributeId> {B}));
	tree.Root().AddChild(std::make_unique<FNode>(std::vector<AttributeId> {C}));
	auto layout = Layout::FromFTree(tree, {{A, ValueType::INT32}, {B, ValueType::INT32}, {C, ValueType::INT32}});

	FRepresentation frep(layout);
	auto root = frep.AppendRoot();
	frep.SetInt32(root, 0, 1);
	for (int i = 0; i < 3; i++) {
		frep.SetInt32(frep.InsertChild(root, 0), 0, i);
	}
	for (int i = 0; i < 4; i++) {
		frep.SetInt32(frep.InsertChild(root, 1), 0, i);
	}
	ExpectEq<int64_t>(frep.Count(), 12, "siblings multiply: 3 x 4 = 12");

	// A second root adds to the first: alternatives sum.
	auto other = frep.AppendRoot();
	frep.SetInt32(other, 0, 2);
	for (int i = 0; i < 2; i++) {
		frep.SetInt32(frep.InsertChild(other, 0), 0, i);
	}
	frep.SetInt32(frep.InsertChild(other, 1), 0, 9);
	ExpectEq<int64_t>(frep.Count(), 14, "roots sum: 12 + (2 x 1) = 14");

	// An empty slot contributes nothing. Bottom-inserts produce such subtrees
	// routinely (section 4.6), and a flattening iterator must skip them.
	auto empty = frep.AppendRoot();
	frep.SetInt32(empty, 0, 3);
	frep.SetInt32(frep.InsertChild(empty, 0), 0, 7);
	ExpectEq<int64_t>(frep.Count(), 14, "a record with an empty slot contributes 0");

	// The exponential win: 20 roots x 1000 x 1000 children is 20,000,000 flat
	// tuples held in 20,001 records.
	FRepresentation big(layout);
	for (int r = 0; r < 20; r++) {
		auto record = big.AppendRoot();
		for (int i = 0; i < 1000; i++) {
			big.InsertChild(record, 0);
		}
		for (int i = 0; i < 1000; i++) {
			big.InsertChild(record, 1);
		}
	}
	ExpectEq<int64_t>(big.Count(), 20000000, "20,000,000 flat tuples counted without flattening");
	ExpectTrue(big.RecordCount() == 20 + 20 * 2000, "held in 40,020 records");
	std::printf("       %zu records, %zu bytes for 20,000,000 flat tuples\n", big.RecordCount(),
	            big.BytesAllocated());
}

int main() {
	std::printf("factorize core: layout / arena / f-representation\n\n");
	TestLayout();
	std::printf("\n");
	TestStability();
	std::printf("\n");
	TestInlining();
	std::printf("\n");
	TestCount();
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
