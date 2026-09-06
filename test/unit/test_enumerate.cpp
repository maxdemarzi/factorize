//===----------------------------------------------------------------------===//
//                         factorize
//
// test/unit/test_enumerate.cpp
//
// Flat tuples out of an f-representation (plan sections 10.2, 10.3).
//
// The property that matters is that enumeration and counting agree: the tuples
// emitted must be exactly the ones SubtreeSize counted, no more and no fewer.
// Section 4.6 names the way this goes wrong -- bottom-inserts leave records
// with empty child slots, and a record with an empty slot contributes nothing,
// so an enumerator that walks it anyway invents tuples that the count knows are
// not there.
//
//===----------------------------------------------------------------------===//

#include "../../src/core/enumerate.hpp"
#include "../../src/core/plan.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
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

//! A group is only "ok" if nothing inside it failed.
//!
//! Reporting from a destructor, against a failure count taken when the group
//! started, is what makes that line trustworthy. Two earlier shapes were not.
//! Printing "ok" unconditionally said ok for a group whose own checks had just
//! printed FAIL. Comparing against a global "last reported" watermark fixed
//! that but moved the FAIL onto the NEXT group's line whenever a group failed
//! and returned before reporting -- which several groups below do on purpose,
//! so that one broken invariant does not print a hundred FAIL lines. Both were
//! wrong the same way: the verdict was computed from state outside the group,
//! so it survived the group never reaching its own report.
class Group {
public:
	explicit Group(std::string name) : name(std::move(name)), failures_before(g_failures) {
	}
	Group(const Group &) = delete;
	Group &operator=(const Group &) = delete;

	//! Some groups only learn part of their own label by running -- how many
	//! cases they generated, say. A group that fails still has to be named, so
	//! the label is fixed up front and the detail appended to it afterwards.
	void Detail(const std::string &detail) {
		name += detail;
	}

	~Group() {
		std::printf("  %-4s %s\n", g_failures > failures_before ? "FAIL" : "ok", name.c_str());
	}

private:
	std::string name;
	const int failures_before;
};

class MemorySource : public RelationSource {
public:
	void Add(std::vector<std::vector<int64_t>> columns) {
		relations.push_back(std::move(columns));
	}
	const std::vector<std::vector<int64_t>> &Columns(size_t relation) override {
		return relations.at(relation);
	}
	ColumnStats Stats(size_t relation, size_t column) override {
		ColumnStats stats;
		stats.rows = static_cast<double>(relations.at(relation).at(column).size());
		stats.distinct = stats.rows;
		return stats;
	}

private:
	std::vector<std::vector<std::vector<int64_t>>> relations;
};

//! Builds a two-relation join and enumerates it, checking the tuples against
//! the ones a nested loop over the same inputs produces.
static void TestEnumerationMatchesTheJoin() {
	Group scope("enumeration emits exactly the tuples the count counted");

	MemorySource source;
	//     left            right
	//   k                k
	//   1 (twice)        1
	//   2                2 (twice)
	//   3                -        <- joins with nothing
	source.Add({{1, 1, 2, 3}});
	source.Add({{1, 2, 2}});

	QueryGraph graph;
	graph.column_counts = {1, 1};
	graph.column_types = {{ValueType::INT64}, {ValueType::INT64}};
	graph.predicates = {Predicate {0, 0, 1, 0}};
	const auto plan = BuildPlan(graph);

	// The join is 1x1 (twice) + 1x2 = 4 tuples over key values 1 and 2.
	const auto counted = ExecuteCount(graph, plan, source, JoinMode::BOTTOM_INSERT);
	Expect(counted.ok && counted.count == 4, "enumerate: the join has 4 tuples, count says " +
	                                             std::to_string(counted.count));

	auto materialized = ExecuteMaterialize(graph, plan, source, JoinMode::BOTTOM_INSERT, 0);
	Expect(materialized.ok, "enumerate: materialize succeeds (" + materialized.error + ")");
	Expect(materialized.tuples.size() == 4, "enumerate: emitted " + std::to_string(materialized.tuples.size()) +
	                                            " tuples, count says 4");

	// Both attributes are the same equivalence class, so every tuple has the two
	// columns equal -- and the multiset of values must be {1,1,2,2}.
	std::vector<int64_t> keys;
	for (const auto &tuple : materialized.tuples) {
		Expect(tuple.size() == 2, "enumerate: a tuple has one value per attribute");
		if (tuple.size() == 2) {
			Expect(tuple[0] == tuple[1], "enumerate: an equi-join's two key columns must agree, got " +
			                                 std::to_string(tuple[0]) + " and " + std::to_string(tuple[1]));
			keys.push_back(tuple[0]);
		}
	}
	std::sort(keys.begin(), keys.end());
	Expect(keys == std::vector<int64_t>({1, 1, 2, 2}), "enumerate: the key multiset is {1,1,2,2}");
}

//! The section 4.6 hazard, on a shape that produces it: a three-relation chain
//! where the middle relation has rows that join upward but not downward, so the
//! representation holds records whose subtree is empty.
static void TestEmptySubtreesAreNotEnumerated() {
	Group scope("records with empty subtrees are skipped, not enumerated");

	MemorySource source;
	// a.x joins b.x; b.y joins c.y. b has rows whose y matches nothing in c.
	source.Add({{1, 2}});                  // a(x)
	source.Add({{1, 1, 2}, {10, 99, 20}}); // b(x, y): (1,10) (1,99) (2,20)
	source.Add({{10, 20}});                // c(y): 99 is absent

	QueryGraph graph;
	graph.column_counts = {1, 2, 1};
	graph.column_types = {{ValueType::INT64}, {ValueType::INT64, ValueType::INT64}, {ValueType::INT64}};
	graph.predicates = {Predicate {0, 0, 1, 0}, Predicate {1, 1, 2, 0}};
	const auto plan = BuildPlan(graph);

	const auto counted = ExecuteCount(graph, plan, source, JoinMode::BOTTOM_INSERT);
	// (1,10) and (2,20) survive; (1,99) joins nothing downward.
	Expect(counted.ok && counted.count == 2, "enumerate: the chain has 2 tuples, count says " +
	                                             std::to_string(counted.count));

	auto materialized = ExecuteMaterialize(graph, plan, source, JoinMode::BOTTOM_INSERT, 0);
	Expect(materialized.ok, "enumerate: materialize succeeds (" + materialized.error + ")");
	Expect(materialized.tuples.size() == 2,
	       "enumerate: a record whose subtree is empty must contribute no tuples; emitted " +
	           std::to_string(materialized.tuples.size()) + " against a count of 2");
	for (const auto &tuple : materialized.tuples) {
		const bool y_is_real = std::find(tuple.begin(), tuple.end(), 99) == tuple.end();
		Expect(y_is_real, "enumerate: the value that joins nothing must never appear in a tuple");
	}
}

//! What LIMIT needs: the first k tuples without building the rest.
static void TestLimitStopsEarly() {
	Group scope("a limit stops enumeration early, and a limit past the end is harmless");

	MemorySource source;
	// 50 keys x 20 rows each on both sides: 20,000 tuples, of which we want 5.
	std::vector<int64_t> left, right;
	for (int64_t key = 0; key < 50; key++) {
		for (int i = 0; i < 20; i++) {
			left.push_back(key);
			right.push_back(key);
		}
	}
	source.Add({left});
	source.Add({right});

	QueryGraph graph;
	graph.column_counts = {1, 1};
	graph.column_types = {{ValueType::INT64}, {ValueType::INT64}};
	graph.predicates = {Predicate {0, 0, 1, 0}};
	const auto plan = BuildPlan(graph);

	const auto all = ExecuteCount(graph, plan, source, JoinMode::BOTTOM_INSERT);
	Expect(all.ok && all.count == 50 * 20 * 20, "limit: the whole join is 20000 tuples, count says " +
	                                                std::to_string(all.count));

	auto five = ExecuteMaterialize(graph, plan, source, JoinMode::BOTTOM_INSERT, 5);
	Expect(five.ok, "limit: materialize succeeds (" + five.error + ")");
	Expect(five.tuples.size() == 5, "limit: asked for 5, got " + std::to_string(five.tuples.size()));

	// A limit larger than the result is not an error, and does not invent rows.
	auto plenty = ExecuteMaterialize(graph, plan, source, JoinMode::BOTTOM_INSERT, 1000000);
	Expect(plenty.ok && plenty.tuples.size() == 20000,
	       "limit: a limit past the end yields the whole result, got " + std::to_string(plenty.tuples.size()));
}

//! GROUP BY on a key at the top of the f-tree: each root record is a group, and
//! the tuples belonging to it are the ones its subtree denotes. The groups must
//! sum to the count, and each one must equal what enumerating and tallying by
//! hand would give.
static void TestGroupCountMatchesEnumeration() {
	Group scope("grouping on a root attribute matches the count, group by group");

	MemorySource source;
	// key 1: 2 left x 3 right = 6 tuples
	// key 2: 1 left x 1 right = 1 tuple
	// key 3: 1 left x 0 right = no tuples, so no group
	source.Add({{1, 1, 2, 3}});
	source.Add({{1, 1, 1, 2}});

	QueryGraph graph;
	graph.column_counts = {1, 1};
	graph.column_types = {{ValueType::INT64}, {ValueType::INT64}};
	graph.predicates = {Predicate {0, 0, 1, 0}};
	const auto plan = BuildPlan(graph);

	auto grouped = ExecuteGroupCount(graph, plan, source, JoinMode::BOTTOM_INSERT, {GroupKey {0, 0}});
	Expect(grouped.ok, "group: succeeds (" + grouped.error + ")");

	std::map<int64_t, int64_t> got;
	for (const auto &entry : grouped.groups) {
		got[entry.first[0]] = entry.second[0];
	}
	Expect(got.size() == 2, "group: two keys join, got " + std::to_string(got.size()) + " groups");
	Expect(got[1] == 6, "group: key 1 has 6 tuples, got " + std::to_string(got[1]));
	Expect(got[2] == 1, "group: key 2 has 1 tuple, got " + std::to_string(got[2]));
	Expect(got.find(3) == got.end(), "group: a key that joins with nothing is not a group");

	// The groups have to sum to the count of the whole join, or one of the two
	// is wrong.
	int64_t summed = 0;
	for (const auto &entry : grouped.groups) {
		summed += entry.second[0];
	}
	const auto counted = ExecuteCount(graph, plan, source, JoinMode::BOTTOM_INSERT);
	Expect(counted.ok && summed == counted.count, "group: the groups sum to " + std::to_string(summed) +
	                                                  " against a count of " + std::to_string(counted.count));
}

//! A key below the root used to be declined; the fold now descends to it,
//! carrying the tuples the levels above stand for. Checked against enumeration
//! rather than against the arithmetic, since the weight it accumulates on the
//! way down is exactly what could be wrong.
static void TestGroupOnDeepKey() {
	Group scope("a grouping key below the root is descended to, not declined");

	MemorySource source;
	source.Add({{1, 2}});
	source.Add({{1, 1, 2}, {10, 99, 20}});
	source.Add({{10, 20}});

	QueryGraph graph;
	graph.column_counts = {1, 2, 1};
	graph.column_types = {{ValueType::INT64}, {ValueType::INT64, ValueType::INT64}, {ValueType::INT64}};
	graph.predicates = {Predicate {0, 0, 1, 0}, Predicate {1, 1, 2, 0}};
	const auto plan = BuildPlan(graph);

	// Relation 2's column is at the bottom of the chain, not the top.
	auto materialized = ExecuteMaterialize(graph, plan, source, JoinMode::BOTTOM_INSERT, 0);
	Expect(materialized.ok, "deep key: materialize succeeds (" + materialized.error + ")");
	// Attributes are a.x, b.x, b.y, c.y -- so relation 2's column is index 3.
	std::map<int64_t, int64_t> by_hand;
	for (const auto &tuple : materialized.tuples) {
		by_hand[tuple[3]]++;
	}

	auto grouped = ExecuteGroupCount(graph, plan, source, JoinMode::BOTTOM_INSERT, {GroupKey {2, 0}});
	Expect(grouped.ok, "deep key: fold succeeds (" + grouped.error + ")");
	Expect(grouped.groups.size() == by_hand.size(), "deep key: " + std::to_string(grouped.groups.size()) +
	                                                    " groups against " + std::to_string(by_hand.size()));
	for (const auto &entry : grouped.groups) {
		const auto found = by_hand.find(entry.first[0]);
		if (found == by_hand.end()) {
			Expect(false, "deep key: fold invented group " + std::to_string(entry.first[0]));
			continue;
		}
		Expect(entry.second[0] == found->second, "deep key: group " + std::to_string(entry.first[0]) + " counts " +
		                                             std::to_string(entry.second[0]) + ", enumeration says " +
		                                          std::to_string(found->second));
	}
}

//! Summing is not counting, and the difference is a weight. A value in one
//! child slot appears once per combination of the *other* slots, so its
//! contribution is its slot's sum times how many tuples the rest make. Getting
//! that weight wrong is invisible on a two-relation join, where it is 1 -- so
//! this checks against enumeration, which cannot be fooled by it.
//! Sums of negative values.
//!
//! The overflow guards in frep.hpp are used for sums as well as counts, and a
//! sum can be negative. Written for cardinalities, which cannot be, they were
//! undefined behaviour on a negative operand: `max - b` overflows for b < 0.
//! This suite has run under UBSan the whole time, so the sanitizer was never
//! the missing part -- a negative number was. No generator produced one and no
//! test summed one, so the check that would have failed was never given
//! anything to fail on.
static void TestNegativeSums() {
	Group scope("a sum of negative values agrees with enumeration");

	MemorySource source;
	source.Add({{1, 1, 2}, {-5, -7, 3}}); // rel0: key, then the values to sum
	source.Add({{1, 1, 2}});              // rel1: key, two rows for key 1

	QueryGraph graph;
	graph.column_counts = {2, 1};
	graph.column_types = {{ValueType::INT64, ValueType::INT64}, {ValueType::INT64}};
	graph.predicates = {Predicate {0, 0, 1, 0}};
	const auto plan = BuildPlan(graph);
	Expect(plan.complete, "negative sums: plans (" + plan.reason + ")");
	if (!plan.complete) {
		return;
	}

	auto materialized = ExecuteMaterialize(graph, plan, source, JoinMode::BOTTOM_INSERT, 0);
	Expect(materialized.ok, "negative sums: materialize succeeds (" + materialized.error + ")");
	int64_t by_hand = 0;
	for (const auto &tuple : materialized.tuples) {
		by_hand += tuple[1];
	}
	// (sum_relation, sum_column): relation 0's second column holds the values.
	const auto folded = ExecuteSum(graph, plan, source, JoinMode::BOTTOM_INSERT, 0, 1);
	Expect(folded.ok, "negative sums: fold succeeds (" + folded.error + ")");
	Expect(folded.count == by_hand, "negative sums: fold gives " + std::to_string(folded.count) +
	                                    ", enumeration gives " + std::to_string(by_hand));
	// Without this the case could silently stop being about negatives.
	Expect(by_hand < 0, "negative sums: the total is " + std::to_string(by_hand) + ", which must be negative");
}

static void TestSumMatchesEnumeration() {
	Group scope("summing a column agrees with enumerating and adding up, at every position in the tree");

	MemorySource source;
	// A star: the hub joins two arms, so a hub value is multiplied by the
	// product of the arms' matching rows, and each arm's values are weighted by
	// the other arm's count.
	source.Add({{1, 2}});          // hub(k)
	source.Add({{1, 1, 2}});       // arm1(k): two rows for k=1
	source.Add({{1, 2, 2, 2}});    // arm2(k): one for k=1, three for k=2
	// Values to sum live on arm1, which is neither the root nor alone.
	QueryGraph graph;
	graph.column_counts = {1, 1, 1};
	graph.column_types = {{ValueType::INT64}, {ValueType::INT64}, {ValueType::INT64}};
	graph.predicates = {Predicate {0, 0, 1, 0}, Predicate {0, 0, 2, 0}};
	const auto plan = BuildPlan(graph);

	// Enumerate and add up by hand: whatever the fold says, this is the answer.
	auto materialized = ExecuteMaterialize(graph, plan, source, JoinMode::BOTTOM_INSERT, 0);
	Expect(materialized.ok, "sum: materialize succeeds (" + materialized.error + ")");
	for (size_t column = 0; column < 3; column++) {
		int64_t by_hand = 0;
		for (const auto &tuple : materialized.tuples) {
			by_hand += tuple[column];
		}
		const auto folded = ExecuteSum(graph, plan, source, JoinMode::BOTTOM_INSERT, column, 0);
		Expect(folded.ok, "sum: fold succeeds for relation " + std::to_string(column) + " (" + folded.error + ")");
		Expect(folded.count == by_hand, "sum: relation " + std::to_string(column) + " folds to " +
		                                    std::to_string(folded.count) + ", enumeration gives " +
		                                    std::to_string(by_hand));
	}
}

//! A column whose rows join with nothing must contribute nothing, and a group
//! of zeroes must still be a group.
static void TestSumIgnoresUnmatchedAndKeepsZeroGroups() {
	Group scope("unmatched rows contribute nothing, and a group summing to zero is still a group");

	MemorySource source;
	source.Add({{1, 2, 3}, {10, 20, 999}}); // left(k, v): k=3 joins nothing
	source.Add({{1, 2}});                   // right(k)

	QueryGraph graph;
	graph.column_counts = {2, 1};
	graph.column_types = {{ValueType::INT64, ValueType::INT64}, {ValueType::INT64}};
	graph.predicates = {Predicate {0, 0, 1, 0}};
	const auto plan = BuildPlan(graph);

	const auto folded = ExecuteSum(graph, plan, source, JoinMode::BOTTOM_INSERT, 0, 1);
	Expect(folded.ok, "sum: succeeds (" + folded.error + ")");
	Expect(folded.count == 30, "sum: 999 joins nothing and must not be summed; got " + std::to_string(folded.count));

	// Zeroes: the group exists, and its sum is 0.
	MemorySource zeroes;
	zeroes.Add({{1, 1}, {0, 0}});
	zeroes.Add({{1}});
	QueryGraph zero_graph;
	zero_graph.column_counts = {2, 1};
	zero_graph.column_types = {{ValueType::INT64, ValueType::INT64}, {ValueType::INT64}};
	zero_graph.predicates = {Predicate {0, 0, 1, 0}};
	const auto zero_plan = BuildPlan(zero_graph);
	auto grouped = ExecuteGroupSum(zero_graph, zero_plan, zeroes, JoinMode::BOTTOM_INSERT, {GroupKey {0, 0}}, 0, 1);
	Expect(grouped.ok, "sum: grouped sum succeeds (" + grouped.error + ")");
	Expect(grouped.groups.size() == 1, "sum: a group whose values are all zero is still a group, got " +
	                                       std::to_string(grouped.groups.size()));
	if (!grouped.groups.empty()) {
		Expect(grouped.groups[0].second[0] == 0, "sum: that group's sum is 0, got " +
		                                             std::to_string(grouped.groups[0].second[0]));
	}
}

//! Grouping on keys in *sibling* branches -- the case plan §10.1 expected to be
//! declined, because the groups are the cross product of the branches.
//!
//! It is computable precisely because siblings are independent, which is the
//! property the representation exists to preserve. The check is against
//! enumeration: group the flat tuples by hand and demand the same table, since
//! a fold that lost a branch's values behind another branch's counts would
//! still produce plausible numbers.
static void TestGroupOnSiblingBranches() {
	Group scope("grouping across independent sibling branches matches enumeration");

	MemorySource source;
	// A star: hub joins two arms, and the group keys are one column from each
	// arm, so neither is an ancestor of the other.
	source.Add({{1, 1, 2}});             // hub(k)
	source.Add({{1, 1, 2}, {7, 8, 9}});  // arm1(k, a)
	source.Add({{1, 2, 2}, {70, 80, 90}}); // arm2(k, b)

	QueryGraph graph;
	graph.column_counts = {1, 2, 2};
	graph.column_types = {{ValueType::INT64},
	                      {ValueType::INT64, ValueType::INT64},
	                      {ValueType::INT64, ValueType::INT64}};
	graph.predicates = {Predicate {0, 0, 1, 0}, Predicate {0, 0, 2, 0}};
	const auto plan = BuildPlan(graph);

	auto materialized = ExecuteMaterialize(graph, plan, source, JoinMode::BOTTOM_INSERT, 0);
	Expect(materialized.ok, "sibling groups: materialize succeeds (" + materialized.error + ")");
	// Attribute order is relation 0's columns, then 1's, then 2's: hub.k, arm1.k,
	// arm1.a, arm2.k, arm2.b -- so a is index 2 and b is index 4.
	std::map<std::pair<int64_t, int64_t>, int64_t> by_hand;
	for (const auto &tuple : materialized.tuples) {
		by_hand[{tuple[2], tuple[4]}]++;
	}

	auto grouped = ExecuteGroupCount(graph, plan, source, JoinMode::BOTTOM_INSERT,
	                                 {GroupKey {1, 1}, GroupKey {2, 1}});
	Expect(grouped.ok, "sibling groups: fold succeeds (" + grouped.error + ")");
	Expect(grouped.groups.size() == by_hand.size(), "sibling groups: " + std::to_string(grouped.groups.size()) +
	                                                    " groups against " + std::to_string(by_hand.size()) +
	                                                    " from enumeration");
	for (const auto &entry : grouped.groups) {
		const auto key = std::make_pair(entry.first[0], entry.first[1]);
		const auto found = by_hand.find(key);
		if (found == by_hand.end()) {
			Expect(false, "sibling groups: fold invented the group (" + std::to_string(key.first) + "," +
			                  std::to_string(key.second) + ")");
			continue;
		}
		Expect(entry.second[0] == found->second, "sibling groups: (" + std::to_string(key.first) + "," +
		                                             std::to_string(key.second) + ") counts " +
		                                             std::to_string(entry.second[0]) + ", enumeration says " +
		                                          std::to_string(found->second));
	}
}

//! The same shape, summing. This is where carrying only counts through the
//! cross product would show up: every branch but the last would lose its
//! values, and the totals would still look reasonable.
static void TestGroupSumOnSiblingBranches() {
	Group scope("summing a column from one branch while grouping on another matches enumeration");

	MemorySource source;
	source.Add({{1, 1, 2}});
	source.Add({{1, 1, 2}, {7, 8, 9}});
	source.Add({{1, 2, 2}, {70, 80, 90}});

	QueryGraph graph;
	graph.column_counts = {1, 2, 2};
	graph.column_types = {{ValueType::INT64},
	                      {ValueType::INT64, ValueType::INT64},
	                      {ValueType::INT64, ValueType::INT64}};
	graph.predicates = {Predicate {0, 0, 1, 0}, Predicate {0, 0, 2, 0}};
	const auto plan = BuildPlan(graph);

	auto materialized = ExecuteMaterialize(graph, plan, source, JoinMode::BOTTOM_INSERT, 0);
	Expect(materialized.ok, "sibling sums: materialize succeeds (" + materialized.error + ")");
	// Group by arm1.a, and sum arm2.b -- the summed column lives in the *other*
	// branch from the grouping key.
	std::map<int64_t, int64_t> by_hand;
	for (const auto &tuple : materialized.tuples) {
		by_hand[tuple[2]] += tuple[4];
	}

	auto grouped = ExecuteGroupSum(graph, plan, source, JoinMode::BOTTOM_INSERT, {GroupKey {1, 1}}, 2, 1);
	Expect(grouped.ok, "sibling sums: fold succeeds (" + grouped.error + ")");
	Expect(grouped.groups.size() == by_hand.size(), "sibling sums: " + std::to_string(grouped.groups.size()) +
	                                                    " groups against " + std::to_string(by_hand.size()));
	for (const auto &entry : grouped.groups) {
		const auto found = by_hand.find(entry.first[0]);
		if (found == by_hand.end()) {
			Expect(false, "sibling sums: fold invented group " + std::to_string(entry.first[0]));
			continue;
		}
		Expect(entry.second[0] == found->second, "sibling sums: group " + std::to_string(entry.first[0]) + " sums to " +
		                                             std::to_string(entry.second[0]) + ", enumeration says " +
		                                          std::to_string(found->second));
	}
}

//! Several aggregates in one walk. The point of doing them together is that
//! they are folds over the same tuples, so the walk is shared -- but sharing it
//! is only sound if each aggregate's arithmetic is untouched by the others'.
//! Two sums over columns in *different* branches is where that would break:
//! each is weighted by how many tuples the other branches make, and those
//! weights differ per aggregate.
static void TestSeveralAggregatesInOneWalk() {
	Group scope("several aggregates folded in one walk agree with each folded alone");

	MemorySource source;
	source.Add({{1, 1, 2}});
	source.Add({{1, 1, 2}, {7, 8, 9}});
	source.Add({{1, 2, 2}, {70, 80, 90}});

	QueryGraph graph;
	graph.column_counts = {1, 2, 2};
	graph.column_types = {{ValueType::INT64},
	                      {ValueType::INT64, ValueType::INT64},
	                      {ValueType::INT64, ValueType::INT64}};
	graph.predicates = {Predicate {0, 0, 1, 0}, Predicate {0, 0, 2, 0}};
	const auto plan = BuildPlan(graph);

	auto materialized = ExecuteMaterialize(graph, plan, source, JoinMode::BOTTOM_INSERT, 0);
	Expect(materialized.ok, "several: materialize succeeds (" + materialized.error + ")");

	// Group by the hub key, and ask for count(*), sum(arm1.b) and sum(arm2.b)
	// at once -- one aggregate over each branch, plus one over neither.
	std::map<int64_t, std::vector<int64_t>> by_hand;
	for (const auto &tuple : materialized.tuples) {
		auto &row = by_hand[tuple[0]];
		if (row.empty()) {
			row.assign(3, 0);
		}
		row[0] += 1;
		row[1] += tuple[2];
		row[2] += tuple[4];
	}

	const std::vector<GroupAggregate> aggregates = {GroupAggregate {Aggregate::COUNT, 0, 0},
	                                                GroupAggregate {Aggregate::SUM, 1, 1},
	                                                GroupAggregate {Aggregate::SUM, 2, 1}};
	auto grouped = ExecuteGroupBy(graph, plan, source, JoinMode::BOTTOM_INSERT, {GroupKey {0, 0}}, aggregates);
	Expect(grouped.ok, "several: fold succeeds (" + grouped.error + ")");
	Expect(grouped.groups.size() == by_hand.size(), "several: " + std::to_string(grouped.groups.size()) +
	                                                    " groups against " + std::to_string(by_hand.size()));
	for (const auto &entry : grouped.groups) {
		const auto found = by_hand.find(entry.first[0]);
		if (found == by_hand.end()) {
			Expect(false, "several: fold invented group " + std::to_string(entry.first[0]));
			continue;
		}
		Expect(entry.second.size() == 3, "several: three values per group, got " +
		                                     std::to_string(entry.second.size()));
		for (size_t i = 0; i < found->second.size() && i < entry.second.size(); i++) {
			Expect(entry.second[i] == found->second[i],
			       "several: group " + std::to_string(entry.first[0]) + " aggregate " + std::to_string(i) +
			           " gives " + std::to_string(entry.second[i]) + ", enumeration says " +
			           std::to_string(found->second[i]));
		}
	}

	// The same aggregates asked for one at a time have to agree with the shared
	// walk, or sharing it changed an answer.
	for (size_t i = 0; i < aggregates.size(); i++) {
		auto alone = ExecuteGroupBy(graph, plan, source, JoinMode::BOTTOM_INSERT, {GroupKey {0, 0}},
		                            {aggregates[i]});
		Expect(alone.ok && alone.groups.size() == grouped.groups.size(),
		       "several: aggregate " + std::to_string(i) + " alone gives the same groups");
		for (size_t g = 0; g < alone.groups.size() && g < grouped.groups.size(); g++) {
			Expect(alone.groups[g].first == grouped.groups[g].first && alone.groups[g].second.size() == 1 &&
			           alone.groups[g].second[0] == grouped.groups[g].second[i],
			       "several: aggregate " + std::to_string(i) + " alone agrees with it in company");
		}
	}
}

//! No grouping columns at all: the whole join is one group, which is how the
//! ungrouped answer is computed once several aggregates are asked for.
static void TestSeveralAggregatesUngrouped() {
	Group scope("with no grouping columns the whole join is a single group");

	MemorySource source;
	source.Add({{1, 1, 2}});
	source.Add({{1, 1, 2}, {7, 8, 9}});
	source.Add({{1, 2, 2}, {70, 80, 90}});

	QueryGraph graph;
	graph.column_counts = {1, 2, 2};
	graph.column_types = {{ValueType::INT64},
	                      {ValueType::INT64, ValueType::INT64},
	                      {ValueType::INT64, ValueType::INT64}};
	graph.predicates = {Predicate {0, 0, 1, 0}, Predicate {0, 0, 2, 0}};
	const auto plan = BuildPlan(graph);

	auto materialized = ExecuteMaterialize(graph, plan, source, JoinMode::BOTTOM_INSERT, 0);
	Expect(materialized.ok, "ungrouped: materialize succeeds (" + materialized.error + ")");
	int64_t tuples = 0;
	int64_t first = 0;
	int64_t second = 0;
	for (const auto &tuple : materialized.tuples) {
		tuples += 1;
		first += tuple[2];
		second += tuple[4];
	}

	auto grouped = ExecuteGroupBy(graph, plan, source, JoinMode::BOTTOM_INSERT, {},
	                              {GroupAggregate {Aggregate::COUNT, 0, 0}, GroupAggregate {Aggregate::SUM, 1, 1},
	                               GroupAggregate {Aggregate::SUM, 2, 1}});
	Expect(grouped.ok, "ungrouped: fold succeeds (" + grouped.error + ")");
	Expect(grouped.groups.size() == 1, "ungrouped: the whole join is one group, got " +
	                                       std::to_string(grouped.groups.size()));
	if (!grouped.groups.empty()) {
		Expect(grouped.groups[0].first.empty(), "ungrouped: that group has no key values");
		Expect(grouped.groups[0].second[0] == tuples, "ungrouped: count is " +
		                                                  std::to_string(grouped.groups[0].second[0]) +
		                                                  ", enumeration says " + std::to_string(tuples));
		Expect(grouped.groups[0].second[1] == first, "ungrouped: first sum is " +
		                                                 std::to_string(grouped.groups[0].second[1]) +
		                                                 ", enumeration says " + std::to_string(first));
		Expect(grouped.groups[0].second[2] == second, "ungrouped: second sum is " +
		                                                  std::to_string(grouped.groups[0].second[2]) +
		                                                  ", enumeration says " + std::to_string(second));
	}
}

int main() {
	std::printf("factorize core: enumerate\n\n");
	TestEnumerationMatchesTheJoin();
	std::printf("\n");
	TestEmptySubtreesAreNotEnumerated();
	std::printf("\n");
	TestLimitStopsEarly();
	std::printf("\n");
	TestGroupCountMatchesEnumeration();
	std::printf("\n");
	TestGroupOnDeepKey();
	std::printf("\n");
	TestSumMatchesEnumeration();
	std::printf("\n");
	TestNegativeSums();
	std::printf("\n");
	TestSumIgnoresUnmatchedAndKeepsZeroGroups();
	std::printf("\n");
	TestGroupOnSiblingBranches();
	std::printf("\n");
	TestGroupSumOnSiblingBranches();
	TestSeveralAggregatesInOneWalk();
	TestSeveralAggregatesUngrouped();
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
