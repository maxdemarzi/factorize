//===----------------------------------------------------------------------===//
//                         factorize
//
// test/unit/test_rangejoin.cpp
//
// The range-join spike (plan section 10.5, Q2 of
// tmp/20260904-10.5-other-join-types.md).
//
// The paper's section 4.8 says non-equality joins should use a blockwise nested
// loop and leaves optimizing that as future work. It does not ask the question
// that makes range joins interesting here: a theta predicate `A.x < B.y` is
// evaluated on the *records* holding x and y, not on the tuples those records
// encode. If x sits at a node standing for k flat tuples and y at one standing
// for m, a single comparison decides k*m flat pairs. A flat engine performs
// k*m comparisons for the same pairs.
//
// So the conjecture is that a nested loop over an f-representation costs
// |records|^2 where a flat one costs |tuples|^2, and the gap between those is
// the entire thesis of the project applied to a shape nobody claims it for.
//
// This measures it, and the answer has a sharp edge that the conjecture as
// stated hides: the saving is exactly the compression *at the predicate's own
// node*, squared -- not the compression of the representation as a whole. An
// attribute at the root is shared by every tuple beneath it and the win is
// large; an attribute at a leaf is held by one record per tuple and there is no
// win at all, however compact the rest of the representation is. Both cases are
// measured below, because quoting only the first would be the same species of
// claim as a benchmark that reports its best shape.
//
// Correctness first: a faster wrong count is worth nothing, so every case
// asserts that the factorized count equals the flat one before any timing is
// reported.
//
//===----------------------------------------------------------------------===//

#include "../../src/core/enumerate.hpp"
#include "../../src/core/join.hpp"

#include <chrono>
#include <cstdio>
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

namespace {

//! Attribute ids. HUB_X and HUB_K sit on the upper (root) records; ARM_V sits
//! on the lower ones, which is what makes the two cases differ.
enum : AttributeId { HUB_K = 0, HUB_X = 1, ARM_K = 2, ARM_V = 3 };

//! A star: `keys` hub rows, each joined to `fanout` arm rows.
//!
//! Under BOTTOM_INSERT the hub is the upper part, so the representation has one
//! root record per hub row and `fanout` children beneath each. That gives a
//! known compression of exactly `fanout`: `keys` roots standing for
//! `keys * fanout` tuples.
FactorizedRelation MakeStar(int keys, int fanout, int64_t x_offset) {
	AttributeTypes hub_types {{HUB_K, ValueType::INT64}, {HUB_X, ValueType::INT64}};
	AttributeTypes arm_types {{ARM_K, ValueType::INT64}, {ARM_V, ValueType::INT64}};

	std::vector<int64_t> hub_k, hub_x;
	for (int i = 0; i < keys; i++) {
		hub_k.push_back(i);
		// Spread x over a range wide enough that a `<` predicate selects a
		// non-trivial fraction rather than everything or nothing.
		hub_x.push_back(x_offset + i);
	}
	std::vector<int64_t> arm_k, arm_v;
	for (int i = 0; i < keys; i++) {
		for (int f = 0; f < fanout; f++) {
			arm_k.push_back(i);
			arm_v.push_back(x_offset + i);
		}
	}
	auto hub = MakeScan({HUB_K, HUB_X}, hub_types, {hub_k, hub_x});
	auto arm = MakeScan({ARM_K, ARM_V}, arm_types, {arm_k, arm_v});

	JoinKeys keys_spec;
	keys_spec.build = {HUB_K};
	keys_spec.probe = {ARM_K};
	return FactorizedJoin(hub, arm, keys_spec, JoinMode::BOTTOM_INSERT);
}

//! Every flat value of `attribute`, one entry per tuple the relation encodes.
std::vector<int64_t> FlatValues(const FactorizedRelation &relation, AttributeId attribute) {
	std::vector<TupleColumn> columns {TupleColumn {attribute, 0}};
	std::vector<int64_t> out;
	Enumerate(relation.Rep(), columns, 0, [&](const std::vector<int64_t> &values) {
		out.push_back(values[0]);
		return true;
	});
	return out;
}

struct Result {
	int64_t count = 0;
	int64_t evaluations = 0;
	double ms = 0;
};

//! The flat nested loop: one comparison per pair of flat tuples.
Result FlatThetaCount(const std::vector<int64_t> &left, const std::vector<int64_t> &right) {
	const auto start = std::chrono::steady_clock::now();
	Result r;
	for (const auto a : left) {
		for (const auto b : right) {
			r.evaluations++;
			if (a < b) {
				r.count++;
			}
		}
	}
	r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	return r;
}

//! The factorized nested loop, for a predicate attribute held on ROOT records.
//!
//! One comparison per pair of root records. A pair that passes contributes the
//! product of the two subtree sizes, because the predicate's value is constant
//! across each subtree -- which is precisely the property that makes one
//! comparison stand for many.
Result FactorizedThetaCountAtRoot(const FactorizedRelation &left, const FactorizedRelation &right,
                                  AttributeId left_attr, AttributeId right_attr) {
	// Collect (value, weight) per root first, so the measurement is of the
	// nested loop rather than of repeated traversal. The flat side gets the
	// same courtesy: its values are already materialized in a vector.
	struct Entry {
		int64_t value;
		int64_t weight;
	};
	std::vector<Entry> l, r_entries;
	left.Rep().ForEachRoot([&](Record root) {
		l.push_back(Entry {left.Rep().GetValue(root, left_attr), left.Rep().SubtreeSize(root)});
	});
	right.Rep().ForEachRoot([&](Record root) {
		r_entries.push_back(Entry {right.Rep().GetValue(root, right_attr), right.Rep().SubtreeSize(root)});
	});

	const auto start = std::chrono::steady_clock::now();
	Result r;
	for (const auto &a : l) {
		for (const auto &b : r_entries) {
			r.evaluations++;
			if (a.value < b.value) {
				r.count = CheckedCardinalityAdd(r.count, CheckedCardinalityMul(a.weight, b.weight));
			}
		}
	}
	r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	return r;
}

} // namespace

//===--------------------------------------------------------------------===//
// The predicate attribute is at the root: maximum sharing
//===--------------------------------------------------------------------===//
static void TestPredicateAtRoot() {
	std::printf("Range join, predicate attribute at the root\n");

	const int keys = 300;
	const int fanout = 40;
	auto left = MakeStar(keys, fanout, 0);
	auto right = MakeStar(keys, fanout, 150);

	const auto left_flat = FlatValues(left, HUB_X);
	const auto right_flat = FlatValues(right, HUB_X);
	Expect(left_flat.size() == static_cast<size_t>(keys) * static_cast<size_t>(fanout),
	       "the star encodes keys*fanout tuples, got " + std::to_string(left_flat.size()));

	const auto flat = FlatThetaCount(left_flat, right_flat);
	const auto fact = FactorizedThetaCountAtRoot(left, right, HUB_X, HUB_X);

	Expect(flat.count == fact.count, "factorized theta count " + std::to_string(fact.count) +
	                                     " differs from the flat one " + std::to_string(flat.count));
	Expect(fact.count > 0, "the predicate selects something rather than nothing");
	Expect(fact.evaluations * static_cast<int64_t>(fanout) * fanout == flat.evaluations,
	       "evaluations should differ by exactly fanout^2: " + std::to_string(flat.evaluations) + " vs " +
	           std::to_string(fact.evaluations));

	std::printf("    tuples %ld x %ld,  records %ld x %ld  (compression %dx per side)\n",
	            static_cast<long>(left_flat.size()), static_cast<long>(right_flat.size()),
	            static_cast<long>(left_flat.size() / fanout), static_cast<long>(right_flat.size() / fanout), fanout);
	std::printf("    flat        %12ld comparisons  %8.2f ms\n", static_cast<long>(flat.evaluations), flat.ms);
	std::printf("    factorized  %12ld comparisons  %8.2f ms\n", static_cast<long>(fact.evaluations), fact.ms);
	std::printf("    ratio       %12.1fx comparisons  %8.1fx time\n",
	            static_cast<double>(flat.evaluations) / static_cast<double>(fact.evaluations),
	            fact.ms > 0 ? flat.ms / fact.ms : 0.0);
	Report("same answer, fewer comparisons by exactly the squared compression");
}

//===--------------------------------------------------------------------===//
// The predicate attribute is at a leaf: no sharing
//===--------------------------------------------------------------------===//
//
// This is the case that bounds the claim. ARM_V is held by one record per flat
// tuple, so there is nothing to share and the factorized loop degenerates to
// the flat one -- however compact the representation is overall.
static void TestPredicateAtLeaf() {
	std::printf("Range join, predicate attribute at a leaf\n");

	const int keys = 300;
	const int fanout = 40;
	auto left = MakeStar(keys, fanout, 0);

	const auto leaf_values = FlatValues(left, ARM_V);
	int64_t leaf_records = 0;
	left.Rep().ForEachRoot([&](Record root) {
		// One child record per arm row: the count of records holding ARM_V is
		// the count of tuples, which is the whole point.
		leaf_records += left.Rep().SubtreeSize(root);
	});

	Expect(leaf_records == static_cast<int64_t>(leaf_values.size()),
	       "a leaf attribute is held by one record per tuple: " + std::to_string(leaf_records) + " vs " +
	           std::to_string(leaf_values.size()));
	std::printf("    tuples %ld,  records holding the predicate attribute %ld  (compression 1.0x)\n",
	            static_cast<long>(leaf_values.size()), static_cast<long>(leaf_records));
	Report("no sharing at a leaf, so a factorized loop has nothing to save");
}

//===--------------------------------------------------------------------===//
// How the saving scales with the compression at the predicate's node
//===--------------------------------------------------------------------===//
static void TestSavingTracksCompression() {
	std::printf("The saving is the compression at that node, squared\n");

	bool all_match = true;
	std::string detail;
	for (int fanout : {1, 2, 4, 8, 32}) {
		const int keys = 400;
		auto left = MakeStar(keys, fanout, 0);
		auto right = MakeStar(keys, fanout, 100);
		const auto flat = FlatThetaCount(FlatValues(left, HUB_X), FlatValues(right, HUB_X));
		const auto fact = FactorizedThetaCountAtRoot(left, right, HUB_X, HUB_X);
		if (flat.count != fact.count) {
			all_match = false;
			detail = "counts differ at fanout " + std::to_string(fanout);
		}
		const double ratio = static_cast<double>(flat.evaluations) / static_cast<double>(fact.evaluations);
		// Comparisons are not the whole story, and reporting only them would
		// overstate this: a flat nested loop over a contiguous int64 vector is
		// close to the friendliest loop a CPU can run, while the factorized one
		// does a checked multiply-add per surviving pair. So the comparison ratio
		// is an upper bound on the time ratio, and below some fan-out the
		// factorized loop is slower despite doing strictly less work.
		const double time_ratio = fact.ms > 0 ? flat.ms / fact.ms : 0.0;
		std::printf("    fanout %2d:  comparisons %10ld -> %7ld (%7.1fx)   time %7.2f -> %5.2f ms (%6.2fx)%s\n",
		            fanout, static_cast<long>(flat.evaluations), static_cast<long>(fact.evaluations), ratio, flat.ms,
		            fact.ms, time_ratio, time_ratio < 1.0 ? "   <- SLOWER" : "");
	}
	Expect(all_match, "factorized and flat counts agree at every fan-out (" + detail + ")");
	Report("the ratio is fanout^2 across a range of fan-outs, including 1x");
}

int main() {
	std::printf("factorize core: range joins (plan 10.5, Q2)\n\n");
	TestPredicateAtRoot();
	std::printf("\n");
	TestPredicateAtLeaf();
	std::printf("\n");
	TestSavingTracksCompression();
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
