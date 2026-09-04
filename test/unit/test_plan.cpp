//===----------------------------------------------------------------------===//
//                         factorize
//
// test/unit/test_plan.cpp
//
// Regression coverage for src/core/plan.cpp -- previously zero, flagged by
// review as the gap that let ExecuteCount's ValueType::INT32 hardcoding ship
// unnoticed for the whole of Phase 2 (every attribute was silently truncated
// to 32 bits; a BIGINT/UBIGINT join key outside int32 range produced a wrong
// count with no error, no test catching it, since no test in this module
// existed at all).
//
//===----------------------------------------------------------------------===//

#include "../../src/core/plan.hpp"

#include <cstdio>
#include <map>
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

//! An in-memory RelationSource: relations are supplied whole, one column list
//! per relation. Real callers scan; this hands back what it was given.
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
		const auto &values = relations.at(relation).at(column);
		stats.rows = static_cast<double>(values.size());
		stats.distinct = static_cast<double>(values.size());
		return stats;
	}

private:
	std::vector<std::vector<std::vector<int64_t>>> relations;
};

//! A two-relation star on one INT32 key: 10 distinct values, N rows each ->
//! N*N tuples. The baseline case, which worked before and must keep working.
static void TestInt32Baseline() {
	MemorySource source;
	// Relation 0: key column only (10 values, 3 rows each -> 30 rows).
	std::vector<int64_t> keys_a;
	for (int64_t v = 0; v < 10; v++) {
		for (int i = 0; i < 3; i++) {
			keys_a.push_back(v);
		}
	}
	source.Add({keys_a});
	source.Add({keys_a});

	QueryGraph graph;
	graph.column_counts = {1, 1};
	graph.column_types = {{ValueType::INT32}, {ValueType::INT32}};
	graph.predicates = {Predicate {0, 0, 1, 0}};

	const auto plan = BuildPlan(graph);
	Expect(plan.complete, "int32 baseline: plan completes");
	const auto result = ExecuteCount(graph, plan, source, JoinMode::BOTTOM_INSERT);
	Expect(result.ok, "int32 baseline: executes without error (" + result.error + ")");
	Expect(result.count == 10 * 3 * 3, "int32 baseline: count is 10*3*3 = 90, got " + std::to_string(result.count));
	Report("int32 baseline");
}

//! The bug this file exists to catch: a BIGINT join key whose values straddle
//! INT32_MAX. Before the fix, ExecuteCount hardcoded ValueType::INT32
//! regardless of what the caller declared, so MakeScan's `static_cast<int32_t>`
//! truncated every such value and this test would either miscount or, for
//! values that alias after truncation, undercount by merging distinct keys.
static void TestInt64BeyondInt32Range() {
	MemorySource source;
	const int64_t base = static_cast<int64_t>(2) * 1000 * 1000 * 1000; // > INT32_MAX (2147483647)
	std::vector<int64_t> keys_a, keys_b;
	std::map<int64_t, int> want;
	for (int64_t v = 0; v < 5; v++) {
		const int64_t key = base + v; // five distinct values, all > INT32_MAX
		for (int i = 0; i < 2; i++) {
			keys_a.push_back(key);
		}
		for (int i = 0; i < 3; i++) {
			keys_b.push_back(key);
		}
		want[key] = 2 * 3;
	}
	source.Add({keys_a});
	source.Add({keys_b});

	QueryGraph graph;
	graph.column_counts = {1, 1};
	graph.column_types = {{ValueType::INT64}, {ValueType::INT64}};
	graph.predicates = {Predicate {0, 0, 1, 0}};

	const auto plan = BuildPlan(graph);
	Expect(plan.complete, "int64 beyond int32 range: plan completes");
	const auto result = ExecuteCount(graph, plan, source, JoinMode::BOTTOM_INSERT);
	Expect(result.ok, "int64 beyond int32 range: executes without error (" + result.error + ")");
	int64_t expected = 0;
	for (const auto &entry : want) {
		expected += entry.second;
	}
	Expect(result.count == expected,
	      "int64 beyond int32 range: count is " + std::to_string(expected) + ", got " + std::to_string(result.count));

	// The adversarial-truncation case: two distinct BIGINT values that alias to
	// the same int32_t after a naive `static_cast<int32_t>` (they differ only
	// above bit 31) must NOT be merged into one key.
	MemorySource alias_source;
	const int64_t v1 = base;
	const int64_t v2 = base + (static_cast<int64_t>(1) << 32); // same low 32 bits as v1
	alias_source.Add({{v1, v1, v2}});
	alias_source.Add({{v1, v2, v2}});
	QueryGraph alias_graph;
	alias_graph.column_counts = {1, 1};
	alias_graph.column_types = {{ValueType::INT64}, {ValueType::INT64}};
	alias_graph.predicates = {Predicate {0, 0, 1, 0}};
	const auto alias_plan = BuildPlan(alias_graph);
	const auto alias_result = ExecuteCount(alias_graph, alias_plan, alias_source, JoinMode::BOTTOM_INSERT);
	// v1 (2 rows in relation 0) x v1 (1 row in relation 1) = 2
	// v2 (1 row in relation 0) x v2 (2 rows in relation 1) = 2
	// total = 4, NOT 3*3=9 (which is what merging v1/v2 into one key would give)
	Expect(alias_result.ok, "int64 alias case: executes without error (" + alias_result.error + ")");
	Expect(alias_result.count == 4,
	      "int64 alias case: distinct high-32-bit values must not collide, got " + std::to_string(alias_result.count));
	Report("int64 values beyond int32 range, including a low-32-bit-aliasing pair");
}

//! ExecuteCount must refuse to guess a missing or mismatched column_types
//! entry rather than defaulting it -- that default is exactly the bug above.
static void TestMissingColumnTypesRejected() {
	MemorySource source;
	source.Add({{1, 2, 3}});
	source.Add({{1, 2, 3}});

	QueryGraph graph;
	graph.column_counts = {1, 1};
	// column_types deliberately left empty.
	graph.predicates = {Predicate {0, 0, 1, 0}};

	const auto plan = BuildPlan(graph);
	const auto result = ExecuteCount(graph, plan, source, JoinMode::BOTTOM_INSERT);
	Expect(!result.ok, "missing column_types: ExecuteCount must fail, not default to INT32");
	Report("missing column_types is rejected, not silently defaulted");
}

//! BuildPlan must refuse a disconnected join graph rather than producing a
//! plan that silently computes a cross product.
static void TestDisconnectedGraphRejected() {
	QueryGraph graph;
	graph.column_counts = {1, 1, 1};
	graph.column_types = {{ValueType::INT32}, {ValueType::INT32}, {ValueType::INT32}};
	graph.predicates = {Predicate {0, 0, 1, 0}}; // relation 2 has no predicate at all

	const auto plan = BuildPlan(graph);
	Expect(!plan.complete, "disconnected graph: BuildPlan must not complete");
	Report("disconnected join graph is rejected");
}

//! A caller-supplied relation count large enough to risk a stack overflow in
//! the unbounded recursion downstream (ftree.cpp, materialize.cpp) must be
//! refused before any of that recursion runs, not discovered by crashing.
static void TestExcessiveRelationCountRejected() {
	QueryGraph graph;
	const size_t huge = 100000;
	graph.column_counts.assign(huge, 1);
	graph.column_types.assign(huge, {ValueType::INT32});
	for (size_t i = 1; i < huge; i++) {
		graph.predicates.push_back(Predicate {i - 1, 0, i, 0});
	}

	const auto plan = BuildPlan(graph);
	Expect(!plan.complete, "100,000 relations: BuildPlan must refuse, not recurse");
	Report("an excessive relation count is rejected before any tree recursion runs");
}

//! A triangle cannot be arranged as an f-tree: its third relation reaches the
//! other two through two different equivalence classes at once, so its keys
//! cannot land on one level. The engine detects this halfway through executing
//! ("key attributes did not converge on one level"); planning has to detect it
//! first, or every caller's only signal is a query that dies mid-flight.
static void TestCyclicGraphRejected() {
	QueryGraph graph;
	graph.column_counts = {2, 2, 2};
	graph.column_types = {{ValueType::INT32, ValueType::INT32},
	                      {ValueType::INT32, ValueType::INT32},
	                      {ValueType::INT32, ValueType::INT32}};
	// a.dst = b.src, b.dst = c.src, c.dst = a.src -- three relations, three
	// classes, no relation attachable on a single key.
	graph.predicates = {Predicate {0, 1, 1, 0}, Predicate {1, 1, 2, 0}, Predicate {2, 1, 0, 0}};

	const auto plan = BuildPlan(graph);
	Expect(!plan.complete, "triangle: BuildPlan must not complete");
	Report("cyclic join graph is rejected at planning time");
}

//! The same three relations joined on one shared key are *not* cyclic, however
//! many predicates say so: equality propagation collapses them into one class,
//! and a relation attaching through several edges of one class converges fine.
//! Counting predicates against relations would call this cyclic and refuse a
//! query the engine handles.
static void TestRedundantStarAccepted() {
	QueryGraph graph;
	graph.column_counts = {1, 1, 1};
	graph.column_types = {{ValueType::INT32}, {ValueType::INT32}, {ValueType::INT32}};
	graph.predicates = {Predicate {0, 0, 1, 0}, Predicate {1, 0, 2, 0}, Predicate {0, 0, 2, 0}};

	const auto plan = BuildPlan(graph);
	Expect(plan.complete, "star with a redundant predicate: BuildPlan must complete");
	Report("one equivalence class stays acyclic however many predicates name it");
}

int main() {
	std::printf("factorize core: plan\n\n");
	TestInt32Baseline();
	std::printf("\n");
	TestInt64BeyondInt32Range();
	std::printf("\n");
	TestMissingColumnTypesRejected();
	std::printf("\n");
	TestDisconnectedGraphRejected();
	std::printf("\n");
	TestExcessiveRelationCountRejected();
	std::printf("\n");
	TestCyclicGraphRejected();
	std::printf("\n");
	TestRedundantStarAccepted();
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
