//===----------------------------------------------------------------------===//
//                         factorize
//
// test/unit/test_join.cpp
//
// Differential test: a factorized join must encode exactly the multiset of
// tuples a flat join produces -- not merely the same count.
//
// The f-representation is fully enumerated here and compared tuple-for-tuple
// against a brute-force flat join, over randomized shapes and data, in both
// insert modes and under both root-to-leaf strategies. That exercises the
// partial-flattening path (section 4.6) wherever the transformation had to
// merge nodes.
//
//===----------------------------------------------------------------------===//

#include "../../src/core/join.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace factorize;

static int g_failures = 0;
static int g_checks = 0;

static void Expect(bool condition, const std::string &what) {
	g_checks++;
	if (condition) {
		return;
	}
	g_failures++;
	std::printf("  FAIL %s\n", what.c_str());
}

static void Report(const std::string &group) {
	std::printf("  ok   %s\n", group.c_str());
}

//===--------------------------------------------------------------------===//
// Flat reference implementation
//===--------------------------------------------------------------------===//
struct FlatRelation {
	std::vector<AttributeId> attributes;
	std::vector<std::vector<int64_t>> rows;

	int Index(AttributeId attribute) const {
		for (size_t i = 0; i < attributes.size(); i++) {
			if (attributes[i] == attribute) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}
};

//! Straightforward nested-loop join. Slow on purpose: it is the oracle.
static FlatRelation FlatJoin(const FlatRelation &left, const FlatRelation &right,
                             const std::vector<AttributeId> &left_keys, const std::vector<AttributeId> &right_keys) {
	FlatRelation result;
	result.attributes = left.attributes;
	std::vector<int> carry;
	for (size_t i = 0; i < right.attributes.size(); i++) {
		if (left.Index(right.attributes[i]) < 0) {
			result.attributes.push_back(right.attributes[i]);
			carry.push_back(static_cast<int>(i));
		}
	}
	for (const auto &left_row : left.rows) {
		for (const auto &right_row : right.rows) {
			bool match = true;
			for (size_t k = 0; k < left_keys.size() && match; k++) {
				match = left_row[static_cast<size_t>(left.Index(left_keys[k]))] ==
				        right_row[static_cast<size_t>(right.Index(right_keys[k]))];
			}
			if (!match) {
				continue;
			}
			auto row = left_row;
			for (auto index : carry) {
				row.push_back(right_row[static_cast<size_t>(index)]);
			}
			result.rows.push_back(std::move(row));
		}
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Enumerating an f-representation
//
// Section 4.6: fix an entry at a node and iterate the required entries of its
// children, taking the cross product wherever a node has several children.
//===--------------------------------------------------------------------===//
static void EnumerateRecord(const FRepresentation &rep, Record record, std::map<AttributeId, int64_t> &assignment,
                            const std::function<void()> &emit);

static void EnumerateSlots(const FRepresentation &rep, Record record, size_t slot_index,
                           std::map<AttributeId, int64_t> &assignment, const std::function<void()> &emit) {
	const auto &level = rep.GetLayout().Level(record.Level());
	if (slot_index == level.slots.size()) {
		emit();
		return;
	}
	bool any = false;
	rep.ForEachChild(record, slot_index, [&](Record child) {
		any = true;
		EnumerateRecord(rep, child, assignment,
		                [&]() { EnumerateSlots(rep, record, slot_index + 1, assignment, emit); });
	});
	// An empty slot means this record encodes no tuples at all -- the case
	// bottom-inserts create and a flattening iterator has to skip.
	(void)any;
}

static void EnumerateRecord(const FRepresentation &rep, Record record, std::map<AttributeId, int64_t> &assignment,
                            const std::function<void()> &emit) {
	const auto &level = rep.GetLayout().Level(record.Level());
	std::vector<std::pair<AttributeId, bool>> restore;
	for (const auto &value : level.payload) {
		restore.emplace_back(value.attribute, assignment.count(value.attribute) > 0);
		assignment[value.attribute] = rep.GetValue(record, value.attribute);
	}
	EnumerateSlots(rep, record, 0, assignment, emit);
	for (const auto &entry : restore) {
		if (!entry.second) {
			assignment.erase(entry.first);
		}
	}
}

static std::vector<std::vector<int64_t>> Enumerate(const FactorizedRelation &relation,
                                                   const std::vector<AttributeId> &order) {
	std::vector<std::vector<int64_t>> rows;
	std::map<AttributeId, int64_t> assignment;
	relation.Rep().ForEachRoot([&](Record root) {
		EnumerateRecord(relation.Rep(), root, assignment, [&]() {
			std::vector<int64_t> row;
			row.reserve(order.size());
			for (auto attribute : order) {
				row.push_back(assignment.at(attribute));
			}
			rows.push_back(std::move(row));
		});
	});
	return rows;
}

static std::vector<std::vector<int64_t>> Sorted(std::vector<std::vector<int64_t>> rows) {
	std::sort(rows.begin(), rows.end());
	return rows;
}

//===--------------------------------------------------------------------===//
// Randomized differential test
//===--------------------------------------------------------------------===//
struct Relation {
	FlatRelation flat;
	std::vector<AttributeId> attributes;
};

static Relation MakeRandom(std::mt19937 &rng, AttributeId first_attribute, int columns, int rows, int domain) {
	Relation relation;
	std::uniform_int_distribution<int> value(0, domain - 1);
	for (int c = 0; c < columns; c++) {
		relation.attributes.push_back(first_attribute + static_cast<AttributeId>(c));
	}
	relation.flat.attributes = relation.attributes;
	for (int r = 0; r < rows; r++) {
		std::vector<int64_t> row;
		for (int c = 0; c < columns; c++) {
			row.push_back(value(rng));
		}
		relation.flat.rows.push_back(std::move(row));
	}
	return relation;
}

static FactorizedRelation ToFactorized(const Relation &relation, const AttributeTypes &types) {
	std::vector<std::vector<int64_t>> columns(relation.attributes.size());
	for (const auto &row : relation.flat.rows) {
		for (size_t c = 0; c < relation.attributes.size(); c++) {
			columns[c].push_back(row[c]);
		}
	}
	return MakeScan(relation.attributes, types, columns);
}

//! Runs one randomized query and compares the enumerated f-representation
//! against the flat oracle.
static bool RunCase(std::mt19937 &rng, int relations, int columns, int rows, int domain, JoinMode mode,
                    PathStrategy strategy, bool star, std::string &detail) {
	AttributeTypes types;
	std::vector<Relation> inputs;
	for (int i = 0; i < relations; i++) {
		auto relation = MakeRandom(rng, static_cast<AttributeId>(i * columns), columns, rows, domain);
		for (auto attribute : relation.attributes) {
			types.emplace_back(attribute, ValueType::INT32);
		}
		inputs.push_back(std::move(relation));
	}

	// Chain joins link consecutive relations; star joins all pivot on the first.
	FlatRelation flat = inputs[0].flat;
	FactorizedRelation factorized = ToFactorized(inputs[0], types);

	for (int i = 1; i < relations; i++) {
		const AttributeId right_key = static_cast<AttributeId>(i * columns);
		const AttributeId left_key = star ? static_cast<AttributeId>(0) : static_cast<AttributeId>((i - 1) * columns);

		JoinKeys join_keys;
		// The accumulated relation is the probe side; the new one is the build.
		join_keys.build = {right_key};
		join_keys.probe = {left_key};

		auto next_factorized =
		    FactorizedJoin(ToFactorized(inputs[static_cast<size_t>(i)], types), factorized, join_keys, mode, strategy);
		flat = FlatJoin(flat, inputs[static_cast<size_t>(i)].flat, {left_key}, {right_key});
		factorized = std::move(next_factorized);
	}

	std::vector<AttributeId> order = flat.attributes;
	std::sort(order.begin(), order.end());

	// Reorder the flat oracle's columns to match.
	std::vector<std::vector<int64_t>> flat_rows;
	for (const auto &row : flat.rows) {
		std::vector<int64_t> reordered;
		for (auto attribute : order) {
			reordered.push_back(row[static_cast<size_t>(flat.Index(attribute))]);
		}
		flat_rows.push_back(std::move(reordered));
	}

	const auto expected = Sorted(flat_rows);
	const auto actual = Sorted(Enumerate(factorized, order));
	const int64_t counted = factorized.Count();

	if (expected != actual || counted != static_cast<int64_t>(expected.size())) {
		detail = "relations=" + std::to_string(relations) + " columns=" + std::to_string(columns) +
		         " rows=" + std::to_string(rows) + " domain=" + std::to_string(domain) + (star ? " star" : " chain") +
		         (mode == JoinMode::TOP_INSERT ? " top" : " bottom") +
		         (strategy == PathStrategy::NAIVE ? " naive" : " levelwise") +
		         " expected=" + std::to_string(expected.size()) + " enumerated=" + std::to_string(actual.size()) +
		         " counted=" + std::to_string(counted);
		return false;
	}
	return true;
}

//! Long chains with keys taken from *varying* columns, which is what the CE
//! corpus actually looks like: `a.s = b.s and b.d = c.s and ...`.
static void TestLongChains() {
	std::printf("Long chains with mixed key columns\n");
	std::mt19937 rng(31337);
	int cases = 0;
	int failures = 0;
	std::string detail;

	for (int relations = 2; relations <= 6; relations++) {
		for (int seed = 0; seed < 6; seed++) {
			for (auto mode : {JoinMode::TOP_INSERT, JoinMode::BOTTOM_INSERT}) {
				AttributeTypes types;
				std::vector<Relation> inputs;
				for (int i = 0; i < relations; i++) {
					auto relation = MakeRandom(rng, static_cast<AttributeId>(i * 2), 2, 14, 3);
					for (auto attribute : relation.attributes) {
						types.emplace_back(attribute, ValueType::INT32);
					}
					inputs.push_back(std::move(relation));
				}
				// Alternate which column each link uses.
				std::uniform_int_distribution<int> pick(0, 1);
				FlatRelation flat = inputs[0].flat;
				FactorizedRelation factorized = ToFactorized(inputs[0], types);
				for (int i = 1; i < relations; i++) {
					const AttributeId left_key = static_cast<AttributeId>((i - 1) * 2 + pick(rng));
					const AttributeId right_key = static_cast<AttributeId>(i * 2 + pick(rng));
					JoinKeys keys;
					keys.build = {right_key};
					keys.probe = {left_key};
					factorized =
					    FactorizedJoin(ToFactorized(inputs[static_cast<size_t>(i)], types), factorized, keys, mode);
					flat = FlatJoin(flat, inputs[static_cast<size_t>(i)].flat, {left_key}, {right_key});
				}
				std::vector<AttributeId> order = flat.attributes;
				std::sort(order.begin(), order.end());
				std::vector<std::vector<int64_t>> flat_rows;
				for (const auto &row : flat.rows) {
					std::vector<int64_t> reordered;
					for (auto attribute : order) {
						reordered.push_back(row[static_cast<size_t>(flat.Index(attribute))]);
					}
					flat_rows.push_back(std::move(reordered));
				}
				cases++;
				const auto expected = Sorted(flat_rows);
				const auto actual = Sorted(Enumerate(factorized, order));
				if (expected != actual || factorized.Count() != static_cast<int64_t>(expected.size())) {
					failures++;
					if (detail.empty()) {
						detail = "relations=" + std::to_string(relations) +
						         (mode == JoinMode::TOP_INSERT ? " top" : " bottom") +
						         " expected=" + std::to_string(expected.size()) +
						         " enumerated=" + std::to_string(actual.size()) +
						         " counted=" + std::to_string(factorized.Count());
					}
				}
			}
		}
	}
	Expect(failures == 0, "long-chain mismatch: " + detail);
	if (failures == 0) {
		Report(std::to_string(cases) + " chains of up to 6 relations match the flat oracle");
	}
}

//! A star with N independent siblings, then a further join keyed on an
//! attribute inside one of those siblings -- so the new subtree attaches at
//! depth 1 rather than at the root.
//!
//! This is the shape equality propagation produces on the CE corpus, and it is
//! the one case the chain and star generators above both miss: they only ever
//! key on the root.
static void TestStarThenDeepJoin() {
	std::printf("Star with N siblings, then a join at depth 1\n");
	std::mt19937 rng(20260902);
	int failures = 0;
	std::string detail;

	for (int siblings = 1; siblings <= 4; siblings++) {
		const int relations = siblings + 2; // hub + siblings + the deep one
		AttributeTypes types;
		std::vector<Relation> inputs;
		for (int i = 0; i < relations; i++) {
			auto relation = MakeRandom(rng, static_cast<AttributeId>(i * 2), 2, 20, 4);
			for (auto attribute : relation.attributes) {
				types.emplace_back(attribute, ValueType::INT32);
			}
			inputs.push_back(std::move(relation));
		}

		// Hub is relation 0; every sibling joins on the hub's column 0.
		FlatRelation flat = inputs[0].flat;
		FactorizedRelation factorized = ToFactorized(inputs[0], types);
		for (int i = 1; i <= siblings; i++) {
			JoinKeys keys;
			keys.build = {static_cast<AttributeId>(i * 2)};
			keys.probe = {0};
			factorized = FactorizedJoin(ToFactorized(inputs[static_cast<size_t>(i)], types), factorized, keys,
			                            JoinMode::TOP_INSERT);
			flat = FlatJoin(flat, inputs[static_cast<size_t>(i)].flat, {0}, {static_cast<AttributeId>(i * 2)});
		}

		// Now join the last relation on column 1 of the *last* sibling, which
		// lives at depth 1 -- not at the root.
		const int last = siblings + 1;
		const AttributeId deep_key = static_cast<AttributeId>(siblings * 2 + 1);
		JoinKeys keys;
		keys.build = {static_cast<AttributeId>(last * 2)};
		keys.probe = {deep_key};
		factorized = FactorizedJoin(ToFactorized(inputs[static_cast<size_t>(last)], types), factorized, keys,
		                            JoinMode::TOP_INSERT);
		flat = FlatJoin(flat, inputs[static_cast<size_t>(last)].flat, {deep_key}, {static_cast<AttributeId>(last * 2)});

		std::vector<AttributeId> order = flat.attributes;
		std::sort(order.begin(), order.end());
		std::vector<std::vector<int64_t>> flat_rows;
		for (const auto &row : flat.rows) {
			std::vector<int64_t> reordered;
			for (auto attribute : order) {
				reordered.push_back(row[static_cast<size_t>(flat.Index(attribute))]);
			}
			flat_rows.push_back(std::move(reordered));
		}
		const auto expected = Sorted(flat_rows);
		const auto actual = Sorted(Enumerate(factorized, order));
		const int64_t counted = factorized.Count();
		const bool ok = expected == actual && counted == static_cast<int64_t>(expected.size());
		std::printf("       siblings=%d  tree=%s  expected=%zu counted=%lld  %s\n", siblings,
		            factorized.Tree().ToString(DefaultAttributeName).c_str(), expected.size(),
		            static_cast<long long>(counted), ok ? "ok" : "MISMATCH");
		if (!ok) {
			failures++;
			if (detail.empty()) {
				detail = "siblings=" + std::to_string(siblings) + " expected=" + std::to_string(expected.size()) +
				         " counted=" + std::to_string(counted);
			}
		}
	}
	Expect(failures == 0, "star-then-deep-join mismatch: " + detail);
}

static void TestDifferential() {
	std::printf("Randomized differential vs a flat oracle\n");
	std::mt19937 rng(20260901);
	int cases = 0;
	int failures = 0;
	std::string first_failure;

	for (int relations = 2; relations <= 4; relations++) {
		for (int columns = 1; columns <= 3; columns++) {
			for (int domain : {2, 5, 17}) {
				for (bool star : {false, true}) {
					for (auto mode : {JoinMode::TOP_INSERT, JoinMode::BOTTOM_INSERT}) {
						for (auto strategy : {PathStrategy::LEVELWISE, PathStrategy::NAIVE}) {
							std::string detail;
							cases++;
							if (!RunCase(rng, relations, columns, 12, domain, mode, strategy, star, detail)) {
								failures++;
								if (first_failure.empty()) {
									first_failure = detail;
								}
							}
						}
					}
				}
			}
		}
	}
	Expect(failures == 0, "differential mismatch: " + first_failure);
	if (failures == 0) {
		Report(std::to_string(cases) + " randomized queries match the flat oracle exactly");
	}
}

//===--------------------------------------------------------------------===//
// The equivalence the paper proves: L (top-insert) R == R (bottom-insert) L
//===--------------------------------------------------------------------===//
static void TestModeEquivalence() {
	std::printf("Insert-mode equivalence\n");
	std::mt19937 rng(7);
	AttributeTypes types;
	auto left = MakeRandom(rng, 0, 2, 40, 6);
	auto right = MakeRandom(rng, 10, 2, 40, 6);
	for (auto attribute : left.attributes) {
		types.emplace_back(attribute, ValueType::INT32);
	}
	for (auto attribute : right.attributes) {
		types.emplace_back(attribute, ValueType::INT32);
	}

	JoinKeys keys;
	keys.build = {0};
	keys.probe = {10};
	auto top = FactorizedJoin(ToFactorized(left, types), ToFactorized(right, types), keys, JoinMode::TOP_INSERT);

	JoinKeys swapped;
	swapped.build = {10};
	swapped.probe = {0};
	auto bottom =
	    FactorizedJoin(ToFactorized(right, types), ToFactorized(left, types), swapped, JoinMode::BOTTOM_INSERT);

	Expect(top.Tree().ToString(DefaultAttributeName) == bottom.Tree().ToString(DefaultAttributeName),
	       "top-insert(L,R) and bottom-insert(R,L) produce the same f-tree");
	Expect(top.Count() == bottom.Count(), "both modes count the same");

	std::vector<AttributeId> order = {0, 1, 10, 11};
	Expect(Sorted(Enumerate(top, order)) == Sorted(Enumerate(bottom, order)), "both modes encode the same tuples");
	Report("insert modes agree on shape, count and contents");
}

//===--------------------------------------------------------------------===//
// Partial flattening is genuinely exercised
//===--------------------------------------------------------------------===//
static void TestPartialFlattening() {
	std::printf("Partial flattening\n");
	std::mt19937 rng(99);

	// A bushy shape: join two independent pairs, then join the results on
	// attributes that sit on divergent paths of the accumulated tree. That is
	// exactly the case section 4.3 has to transform, and section 4.6 to flatten.
	AttributeTypes types;
	std::vector<Relation> inputs;
	for (int i = 0; i < 4; i++) {
		auto relation = MakeRandom(rng, static_cast<AttributeId>(i * 2), 2, 25, 4);
		for (auto attribute : relation.attributes) {
			types.emplace_back(attribute, ValueType::INT32);
		}
		inputs.push_back(std::move(relation));
	}

	JoinKeys k1;
	k1.build = {2};
	k1.probe = {0};
	auto left =
	    FactorizedJoin(ToFactorized(inputs[1], types), ToFactorized(inputs[0], types), k1, JoinMode::TOP_INSERT);
	auto flat_left = FlatJoin(inputs[0].flat, inputs[1].flat, {0}, {2});

	JoinKeys k2;
	k2.build = {6};
	k2.probe = {4};
	auto right =
	    FactorizedJoin(ToFactorized(inputs[3], types), ToFactorized(inputs[2], types), k2, JoinMode::TOP_INSERT);
	auto flat_right = FlatJoin(inputs[2].flat, inputs[3].flat, {4}, {6});

	// Join the two intermediates on one attribute from each side; the required
	// nodes of the left tree are on a single path, but the second predicate
	// forces a merge.
	JoinStats stats;
	JoinKeys k3;
	k3.build = {5};
	k3.probe = {1};
	auto joined = FactorizedJoin(right, left, k3, JoinMode::TOP_INSERT, PathStrategy::LEVELWISE, &stats);
	auto flat_joined = FlatJoin(flat_left, flat_right, {1}, {5});

	std::vector<AttributeId> order = {0, 1, 2, 3, 4, 5, 6, 7};
	std::vector<std::vector<int64_t>> flat_rows;
	for (const auto &row : flat_joined.rows) {
		std::vector<int64_t> reordered;
		for (auto attribute : order) {
			reordered.push_back(row[static_cast<size_t>(flat_joined.Index(attribute))]);
		}
		flat_rows.push_back(std::move(reordered));
	}
	Expect(Sorted(flat_rows) == Sorted(Enumerate(joined, order)), "bushy join matches the flat oracle");
	Expect(joined.Count() == static_cast<int64_t>(flat_rows.size()), "bushy join counts correctly");
	Report("bushy plan over four relations, " + std::to_string(flat_rows.size()) + " tuples");
	std::printf("       f-tree: %s\n", joined.Tree().ToString(DefaultAttributeName).c_str());
	std::printf("       %zu records for %lld tuples%s\n", stats.output_records, static_cast<long long>(joined.Count()),
	            stats.merged_nodes ? ", nodes were merged" : "");
}

//===--------------------------------------------------------------------===//
// The case that actually forces a merge: a diamond
//
// With a single-attribute key the required set is the key node plus its
// ancestors, which is already a root-to-leaf path -- so section 4.3 never has
// to transform anything. Merging is forced only when a predicate references
// attributes sitting on *divergent* branches, which is exactly what closing a
// cycle does. This is the shape section 4.3's Figure 7 is about, and the reason
// the paper reports cyclic queries as its weak case.
//
//   R0(x,y)  --x--  R1(k1,v)
//      |
//      +----y-----  R2(k2,w)
//
// then join R3 on  R1.v = R3.p  AND  R2.w = R3.q, closing the diamond.
//===--------------------------------------------------------------------===//
static void TestDiamondMerge() {
	std::printf("Diamond (cycle-closing) join -- forces node merging\n");
	std::mt19937 rng(4242);

	enum : AttributeId { X = 0, Y = 1, K1 = 2, V = 3, K2 = 4, W = 5, P = 6, Q = 7 };
	AttributeTypes types;
	for (AttributeId a = X; a <= Q; a++) {
		types.emplace_back(a, ValueType::INT32);
	}

	auto r0 = MakeRandom(rng, X, 2, 30, 4);
	auto r1 = MakeRandom(rng, K1, 2, 30, 4);
	auto r2 = MakeRandom(rng, K2, 2, 30, 4);
	auto r3 = MakeRandom(rng, P, 2, 30, 4);

	// Two independent children hanging off R0's node.
	JoinKeys j1;
	j1.build = {K1};
	j1.probe = {X};
	auto step1 = FactorizedJoin(ToFactorized(r1, types), ToFactorized(r0, types), j1, JoinMode::TOP_INSERT);
	auto flat1 = FlatJoin(r0.flat, r1.flat, {X}, {K1});

	JoinKeys j2;
	j2.build = {K2};
	j2.probe = {Y};
	auto step2 = FactorizedJoin(ToFactorized(r2, types), step1, j2, JoinMode::TOP_INSERT);
	auto flat2 = FlatJoin(flat1, r2.flat, {Y}, {K2});
	std::printf("       before: %s\n", step2.Tree().ToString(DefaultAttributeName).c_str());

	// Close the cycle: V and W are on different branches, so the required nodes
	// diverge and must be merged onto one root-to-leaf path.
	for (auto strategy : {PathStrategy::LEVELWISE, PathStrategy::NAIVE}) {
		JoinStats stats;
		JoinKeys j3;
		j3.build = {P, Q};
		j3.probe = {V, W};
		auto joined = FactorizedJoin(ToFactorized(r3, types), step2, j3, JoinMode::TOP_INSERT, strategy, &stats);
		auto flat3 = FlatJoin(flat2, r3.flat, {V, W}, {P, Q});

		const std::string label = strategy == PathStrategy::LEVELWISE ? "levelwise" : "naive";
		Expect(stats.merged_nodes, label + ": the transformation actually merged nodes");

		std::vector<AttributeId> order = {X, Y, K1, V, K2, W, P, Q};
		std::vector<std::vector<int64_t>> flat_rows;
		for (const auto &row : flat3.rows) {
			std::vector<int64_t> reordered;
			for (auto attribute : order) {
				reordered.push_back(row[static_cast<size_t>(flat3.Index(attribute))]);
			}
			flat_rows.push_back(std::move(reordered));
		}
		Expect(Sorted(flat_rows) == Sorted(Enumerate(joined, order)),
		       label + ": partially flattened result matches the flat oracle");
		Expect(joined.Count() == static_cast<int64_t>(flat_rows.size()), label + ": count matches");
		std::printf("       %-9s -> %s  (%lld tuples, %zu records)\n", label.c_str(),
		            joined.Tree().ToString(DefaultAttributeName).c_str(), static_cast<long long>(joined.Count()),
		            stats.output_records);
	}
	Report("cycle-closing join exercises partial flattening under both strategies");
}

//===--------------------------------------------------------------------===//
// The fused aggregate must agree exactly with materialize-then-count
//
// FactorizedCountJoin computes the result's cardinality without ever building
// it. The only acceptable evidence that it is right is that it returns exactly
// what materializing and counting returns, across shapes and both modes.
//===--------------------------------------------------------------------===//
static void TestFusedCount() {
	std::printf("Fused count vs materialize-then-count\n");
	std::mt19937 rng(20260903);
	int cases = 0;
	int failures = 0;
	std::string detail;

	for (int relations = 2; relations <= 4; relations++) {
		for (int domain : {2, 5, 11}) {
			for (bool star : {false, true}) {
				for (auto mode : {JoinMode::TOP_INSERT, JoinMode::BOTTOM_INSERT}) {
					AttributeTypes types;
					std::vector<Relation> inputs;
					for (int i = 0; i < relations; i++) {
						auto relation = MakeRandom(rng, static_cast<AttributeId>(i * 2), 2, 15, domain);
						for (auto attribute : relation.attributes) {
							types.emplace_back(attribute, ValueType::INT32);
						}
						inputs.push_back(std::move(relation));
					}

					FactorizedRelation accumulated = ToFactorized(inputs[0], types);
					int64_t fused = -1;
					int64_t materialized = -2;
					for (int i = 1; i < relations; i++) {
						const AttributeId probe_key =
						    star ? static_cast<AttributeId>(0) : static_cast<AttributeId>((i - 1) * 2);
						JoinKeys join_keys;
						join_keys.build = {static_cast<AttributeId>(i * 2)};
						join_keys.probe = {probe_key};
						if (i + 1 == relations) {
							fused = FactorizedCountJoin(ToFactorized(inputs[static_cast<size_t>(i)], types),
							                            accumulated, join_keys, mode);
							materialized = FactorizedJoin(ToFactorized(inputs[static_cast<size_t>(i)], types),
							                              accumulated, join_keys, mode)
							                   .Count();
						} else {
							accumulated = FactorizedJoin(ToFactorized(inputs[static_cast<size_t>(i)], types),
							                             accumulated, join_keys, mode);
						}
					}
					cases++;
					if (fused != materialized) {
						failures++;
						if (detail.empty()) {
							detail = "relations=" + std::to_string(relations) + " domain=" + std::to_string(domain) +
							         (star ? " star" : " chain") + (mode == JoinMode::TOP_INSERT ? " top" : " bottom") +
							         " fused=" + std::to_string(fused) +
							         " materialized=" + std::to_string(materialized);
						}
					}
				}
			}
		}
	}
	Expect(failures == 0, "fused count disagrees: " + detail);
	if (failures == 0) {
		Report(std::to_string(cases) + " queries: fused count == materialize-then-count");
	}
}

int main() {
	std::printf("factorize core: joins\n\n");
	TestDifferential();
	std::printf("\n");
	TestLongChains();
	std::printf("\n");
	TestStarThenDeepJoin();
	std::printf("\n");
	TestModeEquivalence();
	std::printf("\n");
	TestPartialFlattening();
	std::printf("\n");
	TestDiamondMerge();
	std::printf("\n");
	TestFusedCount();
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
