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

#include "../../src/core/join.hpp"
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
	Group scope("int32 baseline");

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
}

//! The bug this file exists to catch: a BIGINT join key whose values straddle
//! INT32_MAX. Before the fix, ExecuteCount hardcoded ValueType::INT32
//! regardless of what the caller declared, so MakeScan's `static_cast<int32_t>`
//! truncated every such value and this test would either miscount or, for
//! values that alias after truncation, undercount by merging distinct keys.
static void TestInt64BeyondInt32Range() {
	Group scope("int64 values beyond int32 range, including a low-32-bit-aliasing pair");

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
}

//! ExecuteCount must refuse to guess a missing or mismatched column_types
//! entry rather than defaulting it -- that default is exactly the bug above.
static void TestMissingColumnTypesRejected() {
	Group scope("missing column_types is rejected, not silently defaulted");

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
}

//! BuildPlan must refuse a disconnected join graph rather than producing a
//! plan that silently computes a cross product.
static void TestDisconnectedGraphRejected() {
	Group scope("disconnected join graph is rejected");

	QueryGraph graph;
	graph.column_counts = {1, 1, 1};
	graph.column_types = {{ValueType::INT32}, {ValueType::INT32}, {ValueType::INT32}};
	graph.predicates = {Predicate {0, 0, 1, 0}}; // relation 2 has no predicate at all

	const auto plan = BuildPlan(graph);
	Expect(!plan.complete, "disconnected graph: BuildPlan must not complete");
}

//! A caller-supplied relation count large enough to risk a stack overflow in
//! the unbounded recursion downstream (ftree.cpp, materialize.cpp) must be
//! refused before any of that recursion runs, not discovered by crashing.
static void TestExcessiveRelationCountRejected() {
	Group scope("an excessive relation count is rejected before any tree recursion runs");

	QueryGraph graph;
	const size_t huge = 100000;
	graph.column_counts.assign(huge, 1);
	graph.column_types.assign(huge, {ValueType::INT32});
	for (size_t i = 1; i < huge; i++) {
		graph.predicates.push_back(Predicate {i - 1, 0, i, 0});
	}

	const auto plan = BuildPlan(graph);
	Expect(!plan.complete, "100,000 relations: BuildPlan must refuse, not recurse");
}

//! A triangle cannot be arranged as an f-tree: its third relation reaches the
//! other two through two different equivalence classes at once, so its keys
//! cannot land on one level. The engine detects this halfway through executing
//! ("key attributes did not converge on one level"); planning has to detect it
//! first, or every caller's only signal is a query that dies mid-flight.
static void TestCyclicGraphRejected() {
	Group scope("cyclic join graph is rejected at planning time");

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
}

//! The same three relations joined on one shared key are *not* cyclic, however
//! many predicates say so: equality propagation collapses them into one class,
//! and a relation attaching through several edges of one class converges fine.
//! Counting predicates against relations would call this cyclic and refuse a
//! query the engine handles.
static void TestRedundantStarAccepted() {
	Group scope("one equivalence class stays acyclic however many predicates name it");

	QueryGraph graph;
	graph.column_counts = {1, 1, 1};
	graph.column_types = {{ValueType::INT32}, {ValueType::INT32}, {ValueType::INT32}};
	graph.predicates = {Predicate {0, 0, 1, 0}, Predicate {1, 0, 2, 0}, Predicate {0, 0, 2, 0}};

	const auto plan = BuildPlan(graph);
	Expect(plan.complete, "star with a redundant predicate: BuildPlan must complete");
}

//! Slicing has to be exact, not approximate: the count assembled from N
//! partitions must equal the count taken whole, for every N. Every output tuple
//! assigns one value to the sliced attribute, so bucketing on that value cuts
//! the output into disjoint pieces -- if that reasoning is wrong the sum comes
//! out short, and only comparing against the undivided answer catches it.
static void TestSlicingIsExact() {
	Group scope("a count assembled from slices equals the count taken whole");

	MemorySource source;
	// Skewed on purpose: one key appears far more often than the rest, so the
	// buckets are uneven and a slicing bug shows up as a specific shortfall
	// rather than a rounding-sized one.
	std::vector<int64_t> left, right;
	for (int64_t v = 0; v < 40; v++) {
		const int repeats = (v == 7) ? 50 : 2;
		for (int i = 0; i < repeats; i++) {
			left.push_back(v);
		}
		for (int i = 0; i < 3; i++) {
			right.push_back(v);
		}
	}
	source.Add({left});
	source.Add({right});

	QueryGraph graph;
	graph.column_counts = {1, 1};
	graph.column_types = {{ValueType::INT64}, {ValueType::INT64}};
	graph.predicates = {Predicate {0, 0, 1, 0}};

	const auto plan = BuildPlan(graph);
	const auto whole = ExecuteCount(graph, plan, source, JoinMode::BOTTOM_INSERT);
	Expect(whole.ok, "slicing: the undivided run succeeds (" + whole.error + ")");
	Expect(whole.count == 39 * 2 * 3 + 50 * 3,
	       "slicing: undivided count is " + std::to_string(39 * 2 * 3 + 50 * 3) + ", got " +
	           std::to_string(whole.count));

	for (size_t slices : {2u, 3u, 8u, 64u}) {
		const auto sliced = ExecuteCountSliced(graph, plan, source, JoinMode::BOTTOM_INSERT, slices);
		Expect(sliced.ok, "slicing: " + std::to_string(slices) + " slices succeed (" + sliced.error + ")");
		Expect(sliced.count == whole.count, "slicing: " + std::to_string(slices) + " slices give " +
		                                        std::to_string(sliced.count) + ", undivided gives " +
		                                        std::to_string(whole.count));
	}
}

//! The point of the exercise: a query that does not fit must still answer.
static void TestOutOfMemoryFallsBackToSlices() {
	Group scope("a count too large to fit whole is assembled from slices instead of failing");

	MemorySource source;
	std::vector<int64_t> keys;
	// Large enough that the data dwarfs the structures' fixed cost. The limit
	// below is a fraction of the measured need, and a fraction divides only the
	// data: every arena reserves a 64KB first chunk however little it holds, so
	// a need made mostly of fixed cost cannot be sliced under half of itself
	// however finely it is divided.
	for (int64_t v = 0; v < 40000; v++) {
		for (int i = 0; i < 4; i++) {
			keys.push_back(v);
		}
	}
	source.Add({keys});
	source.Add({keys});
	source.Add({keys});

	QueryGraph graph;
	graph.column_counts = {1, 1, 1};
	graph.column_types = {{ValueType::INT64}, {ValueType::INT64}, {ValueType::INT64}};
	graph.predicates = {Predicate {0, 0, 1, 0}, Predicate {1, 0, 2, 0}};

	const auto plan = BuildPlan(graph);
	SetGlobalMemoryLimit(0);
	const size_t baseline = ThreadBudget().used.load();
	const auto whole = ExecuteCount(graph, plan, source, JoinMode::BOTTOM_INSERT);
	const size_t need = ThreadBudget().peak.load() - baseline;
	Expect(whole.ok, "fallback: unlimited run succeeds (" + whole.error + ")");

	// Half of what the undivided run was measured to hold: by construction too
	// little for the whole, and with the data divided, enough for a slice.
	//
	// This was a constant, 220KB, and it stopped meaning anything once the
	// limit bounded the *sum* of what a slice holds rather than each structure
	// separately (D43). Several arenas are alive at once and each reserves a
	// 64KB first chunk, so 220KB fell below the structures' fixed cost: the
	// undivided run still failed, but so did every slice of it, and the test
	// reported that as slicing not working.
	SetGlobalMemoryLimit(need / 2);
	const auto refused = ExecuteCount(graph, plan, source, JoinMode::BOTTOM_INSERT);
	Expect(!refused.ok, "fallback: the undivided run must hit the cap");
	Expect(refused.out_of_memory, "fallback: hitting the cap must be distinguishable from any other error");

	const auto recovered = ExecuteCountWithinMemory(graph, plan, source, JoinMode::BOTTOM_INSERT);
	Expect(recovered.ok, "fallback: slicing must answer where the undivided run could not (" + recovered.error + ")");
	Expect(recovered.count == whole.count, "fallback: sliced count is " + std::to_string(recovered.count) +
	                                           ", unlimited count is " + std::to_string(whole.count));
	Expect(recovered.slices > 1, "fallback: the answer must be reported as assembled from slices");
	SetGlobalMemoryLimit(0);
}

//! EXISTS has to agree with COUNT on emptiness, for a join with tuples and for
//! one without. Stopping at the first witness is only worth anything if it
//! stops at a *correct* one, and the failure mode of a partitioned search is
//! answering false because the witness was in a bucket never examined.
static void TestExistsAgreesWithCount() {
	Group scope("exists agrees with count on emptiness, including when the witness is sparse");

	// Non-empty, and deliberately sparse: only one key value joins, so most
	// buckets of the partition are empty and the answer lives in one of them.
	MemorySource present;
	present.Add({{7, 1, 2, 3}});
	present.Add({{7, 4, 5, 6}});

	QueryGraph graph;
	graph.column_counts = {1, 1};
	graph.column_types = {{ValueType::INT64}, {ValueType::INT64}};
	graph.predicates = {Predicate {0, 0, 1, 0}};
	const auto plan = BuildPlan(graph);

	const auto counted = ExecuteCount(graph, plan, present, JoinMode::BOTTOM_INSERT);
	Expect(counted.ok && counted.count == 1, "exists: the sparse join has exactly one tuple, got " +
	                                             std::to_string(counted.count));
	const auto found = ExecuteExists(graph, plan, present, JoinMode::BOTTOM_INSERT);
	Expect(found.ok, "exists: succeeds (" + found.error + ")");
	Expect(found.count == 1, "exists: must find the witness wherever its bucket falls");

	// Empty: every bucket has to be examined before answering, and the answer
	// still has to be no.
	MemorySource absent;
	absent.Add({{1, 2, 3}});
	absent.Add({{4, 5, 6}});
	const auto none = ExecuteExists(graph, plan, absent, JoinMode::BOTTOM_INSERT);
	Expect(none.ok, "exists: succeeds on an empty join (" + none.error + ")");
	Expect(none.count == 0, "exists: a join with no tuples must answer no");
}

//! The gate is a prediction; this is a measurement, and they disagree.
//!
//! Two graphs of the same shape and size, differing only in how the keys
//! repeat: a chain of unique keys, where every record stands for one tuple and
//! factorizing is pure overhead, and a star where each record stands for
//! hundreds. Both are accepted with the check off. With the floor set, exactly
//! the first is abandoned -- and abandoned as a plain failure rather than as a
//! memory problem, since slicing a query that is not compressing only makes
//! several smaller copies of the same mistake (D37).
static void TestCompressionFloorAbandons() {
	Group scope("a join that does not compress is abandoned, one that does is not");

	// Chain: keys are unique, so the join is a permutation and the whole
	// representation holds one tuple per record or worse.
	MemorySource chain_source;
	std::vector<int64_t> unique;
	for (int64_t v = 0; v < 2000; v++) {
		unique.push_back(v);
	}
	chain_source.Add({unique});
	chain_source.Add({unique});
	chain_source.Add({unique});
	chain_source.Add({unique});
	// Four relations, so two joins materialize and one of them is past the
	// first. The floor exempts the first: records grow by a sum there while
	// tuples grow by a product, so a ratio near 1 says nothing yet, and judging
	// on it abandons queries only this engine can answer (D51).
	QueryGraph chain;
	chain.column_counts = {1, 1, 1, 1};
	chain.column_types = {{ValueType::INT64}, {ValueType::INT64}, {ValueType::INT64}, {ValueType::INT64}};
	chain.predicates = {Predicate {0, 0, 1, 0}, Predicate {1, 0, 2, 0}, Predicate {2, 0, 3, 0}};

	// Star: one centre value per group, twenty partners in each of three arms,
	// so a group of 61 records stands for 8000 tuples.
	MemorySource star_source;
	std::vector<int64_t> centres;
	std::vector<int64_t> arm;
	for (int64_t v = 0; v < 100; v++) {
		centres.push_back(v);
		for (int i = 0; i < 20; i++) {
			arm.push_back(v);
		}
	}
	star_source.Add({centres});
	star_source.Add({arm});
	star_source.Add({arm});
	star_source.Add({arm});
	QueryGraph star;
	star.column_counts = {1, 1, 1, 1};
	star.column_types = {{ValueType::INT64}, {ValueType::INT64}, {ValueType::INT64}, {ValueType::INT64}};
	star.predicates = {Predicate {0, 0, 1, 0}, Predicate {0, 0, 2, 0}, Predicate {0, 0, 3, 0}};

	const auto chain_plan = BuildPlan(chain);
	const auto star_plan = BuildPlan(star);

	SetGlobalMinCompression(0);
	const auto chain_off = ExecuteCount(chain, chain_plan, chain_source, JoinMode::BOTTOM_INSERT);
	const auto star_off = ExecuteCount(star, star_plan, star_source, JoinMode::BOTTOM_INSERT);
	Expect(chain_off.ok, "compression floor: the chain answers with the check off (" + chain_off.error + ")");
	Expect(star_off.ok, "compression floor: the star answers with the check off (" + star_off.error + ")");
	Expect(chain_off.count == 2000, "compression floor: chain counts 2000, got " + std::to_string(chain_off.count));
	Expect(star_off.count == 100 * 20 * 20 * 20,
	       "compression floor: star counts 800000, got " + std::to_string(star_off.count));

	// Every intermediate join is measured, and the numbers are the ones the
	// representation actually holds rather than anything predicted.
	// n relations means n-1 joins, of which the last is fused into the count and
	// materializes nothing -- so n-2 steps, and a two-relation query has none.
	Expect(chain_off.steps.size() == chain_plan.steps.size() - 2,
	       "compression floor: one step recorded per materialized join, got " +
	           std::to_string(chain_off.steps.size()));
	for (const auto &step : chain_off.steps) {
		Expect(step.live > 0 && step.tuples > 0, "compression floor: a step reports what it built");
		Expect(step.Compression() < 2.0,
		       "compression floor: a chain of unique keys barely compresses, got " +
		           std::to_string(step.Compression()));
	}
	for (const auto &step : star_off.steps) {
		Expect(step.Compression() > 2.0, "compression floor: the star compresses, got " +
		                                     std::to_string(step.Compression()));
	}

	// The same two queries with the floor between them.
	SetGlobalMinCompression(2.0);
	const auto chain_on = ExecuteCountWithinMemory(chain, chain_plan, chain_source, JoinMode::BOTTOM_INSERT);
	const auto star_on = ExecuteCountWithinMemory(star, star_plan, star_source, JoinMode::BOTTOM_INSERT);
	SetGlobalMinCompression(0);

	Expect(!chain_on.ok, "compression floor: the chain is abandoned");
	Expect(!chain_on.out_of_memory,
	       "compression floor: abandoning is not a memory problem, so it must not be retried by slicing");
	Expect(chain_on.error.find("compression") != std::string::npos,
	       "compression floor: the error says what was measured, got '" + chain_on.error + "'");
	Expect(chain_on.slices == 1, "compression floor: the chain was not sliced, it ran " +
	                                 std::to_string(chain_on.slices) + " slices");
	// The exemption, pinned: three relations leave exactly one materialized
	// join -- the first -- and the floor must not judge the query on it.
	MemorySource short_source;
	short_source.Add({unique});
	short_source.Add({unique});
	short_source.Add({unique});
	QueryGraph short_chain;
	short_chain.column_counts = {1, 1, 1};
	short_chain.column_types = {{ValueType::INT64}, {ValueType::INT64}, {ValueType::INT64}};
	short_chain.predicates = {Predicate {0, 0, 1, 0}, Predicate {1, 0, 2, 0}};
	const auto short_plan = BuildPlan(short_chain);
	SetGlobalMinCompression(2.0);
	const auto short_on = ExecuteCountWithinMemory(short_chain, short_plan, short_source, JoinMode::BOTTOM_INSERT);
	SetGlobalMinCompression(0);
	Expect(short_on.ok, "compression floor: a query whose only materialized join is the first is not judged on it (" +
	                        short_on.error + ")");
	Expect(short_on.count == 2000, "compression floor: the exempt chain still counts 2000, got " +
	                                   std::to_string(short_on.count));

	Expect(star_on.ok, "compression floor: the star still answers (" + star_on.error + ")");
	Expect(star_on.count == star_off.count, "compression floor: the star's count is unchanged, " +
	                                            std::to_string(star_on.count) + " against " +
	                                            std::to_string(star_off.count));
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
	std::printf("\n");
	TestSlicingIsExact();
	std::printf("\n");
	TestOutOfMemoryFallsBackToSlices();
	std::printf("\n");
	TestExistsAgreesWithCount();
	std::printf("\n");
	TestCompressionFloorAbandons();
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
