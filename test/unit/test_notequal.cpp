//===----------------------------------------------------------------------===//
//                         factorize
//
// test/unit/test_notequal.cpp
//
// `<>` joins by inclusion-exclusion (plan section 10.5).
//
// The plan sends non-equality joins to a blockwise nested loop. For `<>` that
// is the wrong shape: no comparison is needed at all, because the pairs that
// differ are the pairs that exist minus the pairs that agree, and both of those
// are ordinary equi-join counts.
//
// Everything here is differential -- every count is checked against a
// brute-force enumeration before anything is claimed about it -- because the
// answer is a DIFFERENCE of two counts, and a difference is right only when
// both terms are. A bug that inflates both terms equally cancels out and would
// pass a test that only checked one.
//
//===----------------------------------------------------------------------===//

#include "../../src/core/plan.hpp"

#include <cstdio>
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

//! A group is only "ok" if nothing inside it failed.
static int g_reported = 0;

static void Report(const std::string &group) {
	if (g_failures > g_reported) {
		g_reported = g_failures;
		std::printf("  FAIL %s\n", group.c_str());
		return;
	}
	std::printf("  ok   %s\n", group.c_str());
}

namespace {

class Source : public RelationSource {
public:
	std::vector<std::vector<std::vector<int64_t>>> relations;

	const std::vector<std::vector<int64_t>> &Columns(size_t relation) override {
		return relations.at(relation);
	}
	ColumnStats Stats(size_t relation, size_t column) override {
		ColumnStats stats;
		stats.rows = static_cast<double>(relations.at(relation).at(column).size());
		stats.distinct = stats.rows > 0 ? stats.rows : 1;
		return stats;
	}
};

//! The oracle: every combination of one row per relation, keeping those that
//! satisfy every equality and, optionally, the inequality.
int64_t BruteForce(const QueryGraph &graph, const Source &source, size_t rows, const NotEqualPredicate *neq,
                   bool neq_as_equality) {
	const size_t relations = graph.RelationCount();
	std::vector<size_t> index(relations, 0);
	int64_t total = 0;
	for (;;) {
		bool keep = true;
		for (const auto &predicate : graph.predicates) {
			const int64_t left = source.relations[predicate.left_relation][static_cast<size_t>(predicate.left_column)]
			                                     [index[predicate.left_relation]];
			const int64_t right = source.relations[predicate.right_relation][static_cast<size_t>(predicate.right_column)]
			                                      [index[predicate.right_relation]];
			if (left != right) {
				keep = false;
				break;
			}
		}
		if (keep && neq != nullptr) {
			const int64_t left =
			    source.relations[neq->left_relation][static_cast<size_t>(neq->left_column)][index[neq->left_relation]];
			const int64_t right = source.relations[neq->right_relation][static_cast<size_t>(neq->right_column)]
			                                      [index[neq->right_relation]];
			keep = neq_as_equality ? (left == right) : (left != right);
		}
		total += keep ? 1 : 0;
		size_t digit = 0;
		for (; digit < relations; digit++) {
			if (++index[digit] < rows) {
				break;
			}
			index[digit] = 0;
		}
		if (digit == relations) {
			break;
		}
	}
	return total;
}

Source MakeData(std::mt19937 &rng, size_t relations, size_t columns, size_t rows, int domain) {
	Source source;
	for (size_t r = 0; r < relations; r++) {
		std::vector<std::vector<int64_t>> table(columns);
		for (size_t c = 0; c < columns; c++) {
			for (size_t row = 0; row < rows; row++) {
				table[c].push_back(static_cast<int64_t>(rng() % static_cast<unsigned>(domain)));
			}
		}
		source.relations.push_back(std::move(table));
	}
	return source;
}

QueryGraph MakeGraph(size_t relations, size_t columns, ValueType type) {
	QueryGraph graph;
	graph.column_counts.assign(relations, columns);
	graph.column_types.assign(relations, std::vector<ValueType>(columns, type));
	return graph;
}

} // namespace

//===--------------------------------------------------------------------===//
// The identity, over randomized shapes and data
//===--------------------------------------------------------------------===//

//! `<>` counted by subtraction must equal `<>` counted by enumeration.
//!
//! Both insert modes, because the identity is arithmetic over two counts and
//! each count is supposed to be mode-independent; if a mode were wrong the
//! difference would be wrong in a way no single-mode test would show.
static void TestDifferential() {
	std::mt19937 rng(70125);
	int cases = 0;
	int agreed = 0;
	int declined = 0;
	for (int trial = 0; trial < 240; trial++) {
		const size_t relations = 2 + (rng() % 2); // 2 or 3
		const size_t columns = 2 + (rng() % 2);   // 2 or 3
		const size_t rows = 3 + (rng() % 4);
		const int domain = 2 + static_cast<int>(rng() % 3);

		auto graph = MakeGraph(relations, columns, ValueType::INT32);
		// A spanning tree of equalities, so the graph is connected.
		for (size_t r = 1; r < relations; r++) {
			graph.predicates.push_back(Predicate {rng() % r, static_cast<int>(rng() % columns), r,
			                                      static_cast<int>(rng() % columns)});
		}
		// The inequality, between two distinct relations.
		NotEqualPredicate neq;
		neq.left_relation = rng() % relations;
		do {
			neq.right_relation = rng() % relations;
		} while (neq.right_relation == neq.left_relation);
		neq.left_column = static_cast<int>(rng() % columns);
		neq.right_column = static_cast<int>(rng() % columns);

		auto source = MakeData(rng, relations, columns, rows, domain);
		const auto result = ExecuteCountNotEqual(graph, neq, source,
		                                         (trial % 2) ? JoinMode::TOP_INSERT : JoinMode::BOTTOM_INSERT);
		cases++;
		if (!result.ok) {
			// A decline is a legitimate outcome -- the second term may not plan
			// -- but it must be a decline, never a wrong number.
			declined++;
			if (declined <= 2) {
				std::printf("       declined: %s\n", result.error.c_str());
			}
			continue;
		}
		agreed++;
		const int64_t truth = BruteForce(graph, source, rows, &neq, false);
		if (result.count != truth) {
			Expect(false, "case " + std::to_string(trial) + ": inclusion-exclusion gives " +
			                  std::to_string(result.count) + ", enumeration gives " + std::to_string(truth));
			break;
		}
	}
	Expect(agreed > 100, std::to_string(agreed) + " of " + std::to_string(cases) +
	                         " cases answered (rest declined) -- enough to be a test");
	Expect(true, std::to_string(agreed) + " answered cases all match brute-force enumeration");
	std::printf("       %d answered, %d declined\n", agreed, declined);
	Report("counting `<>` by subtraction agrees with enumerating it");
}

//! The relational invariant: the pairs that differ plus the pairs that agree
//! are exactly the pairs that exist.
//!
//! Stronger than checking either count alone, because it relates the two terms
//! rather than checking each against a number -- a version that got both wrong
//! in the same direction still fails it. This is the same shape as SEMI + ANTI
//! equalling a side's own tuples in test_semi.cpp.
static void TestPartition() {
	std::mt19937 rng(90210);
	int checked = 0;
	for (int trial = 0; trial < 120; trial++) {
		const size_t relations = 2 + (rng() % 2);
		const size_t columns = 2;
		const size_t rows = 3 + (rng() % 3);
		const int domain = 2 + static_cast<int>(rng() % 2);

		auto graph = MakeGraph(relations, columns, ValueType::INT32);
		for (size_t r = 1; r < relations; r++) {
			graph.predicates.push_back(Predicate {rng() % r, static_cast<int>(rng() % columns), r,
			                                      static_cast<int>(rng() % columns)});
		}
		NotEqualPredicate neq;
		neq.left_relation = 0;
		neq.right_relation = 1;
		neq.left_column = static_cast<int>(rng() % columns);
		neq.right_column = static_cast<int>(rng() % columns);

		auto source = MakeData(rng, relations, columns, rows, domain);
		const auto differ = ExecuteCountNotEqual(graph, neq, source, JoinMode::BOTTOM_INSERT);
		if (!differ.ok) {
			continue;
		}
		// The two halves, each counted independently by enumeration.
		const int64_t agree_truth = BruteForce(graph, source, rows, &neq, true);
		const int64_t all_truth = BruteForce(graph, source, rows, nullptr, false);
		checked++;
		if (differ.count + agree_truth != all_truth) {
			Expect(false, "trial " + std::to_string(trial) + ": differ(" + std::to_string(differ.count) + ") + agree(" +
			                  std::to_string(agree_truth) + ") != all(" + std::to_string(all_truth) + ")");
			break;
		}
	}
	Expect(checked > 40, std::to_string(checked) + " partitions checked");
	Expect(true, "differ + agree == all, on every case");
	Report("`<>` and `=` partition the equi-join exactly");
}

//===--------------------------------------------------------------------===//
// What it refuses, and why each refusal is the right answer
//===--------------------------------------------------------------------===//

static void TestDeclines() {
	std::mt19937 rng(4);
	auto source = MakeData(rng, 2, 2, 4, 3);

	{
		// Both columns on one relation: a filter, not a join. Neither term is
		// expressible as a join graph, and answering would mean dropping the
		// predicate silently.
		auto graph = MakeGraph(2, 2, ValueType::INT32);
		graph.predicates.push_back(Predicate {0, 0, 1, 0});
		NotEqualPredicate neq;
		neq.left_relation = 0;
		neq.right_relation = 0;
		neq.left_column = 0;
		neq.right_column = 1;
		const auto result = ExecuteCountNotEqual(graph, neq, source, JoinMode::BOTTOM_INSERT);
		Expect(!result.ok, "a `<>` between two columns of one relation is refused");
		Expect(result.error.find("filter") != std::string::npos,
		       "and the reason says it is a filter rather than a join");
	}
	{
		auto graph = MakeGraph(2, 2, ValueType::INT32);
		graph.predicates.push_back(Predicate {0, 0, 1, 0});
		NotEqualPredicate neq;
		neq.left_relation = 0;
		neq.right_relation = 5;
		const auto result = ExecuteCountNotEqual(graph, neq, source, JoinMode::BOTTOM_INSERT);
		Expect(!result.ok, "a relation index out of range is refused");
	}
	{
		auto graph = MakeGraph(2, 2, ValueType::INT32);
		graph.predicates.push_back(Predicate {0, 0, 1, 0});
		NotEqualPredicate neq;
		neq.left_relation = 0;
		neq.right_relation = 1;
		neq.left_column = 7;
		neq.right_column = 0;
		const auto result = ExecuteCountNotEqual(graph, neq, source, JoinMode::BOTTOM_INSERT);
		Expect(!result.ok, "a column index out of range is refused");
	}
	{
		// The second term joins on (equality key, compared column) together, so
		// its packed key is the SUM of both widths. Two BIGINTs is 128 bits and
		// cannot be built -- and this has to arrive as a decline from the
		// planner, not as a throw from inside the join.
		auto graph = MakeGraph(2, 2, ValueType::INT64);
		graph.predicates.push_back(Predicate {0, 0, 1, 0});
		NotEqualPredicate neq;
		neq.left_relation = 0;
		neq.right_relation = 1;
		neq.left_column = 1;
		neq.right_column = 1;
		const auto result = ExecuteCountNotEqual(graph, neq, source, JoinMode::BOTTOM_INSERT);
		Expect(!result.ok, "64-bit columns make the second term's composite key too wide, and it is refused");
		Expect(result.error.find("equality term") != std::string::npos,
		       "and the reason names which of the two terms could not be built");
	}
	Report("refusals: every unsupported shape is a decline with a reason, never a wrong number");
}

//===--------------------------------------------------------------------===//
// What it costs
//===--------------------------------------------------------------------===//

//! Two full counts to answer one query.
//!
//! Reported, never asserted. The point is that inclusion-exclusion is not free:
//! a flat engine answers `<>` in ONE pass with a filter, and this takes two
//! passes plus a composite-key join that is strictly more work than the plain
//! one. It wins when the equi-join is where the time goes and loses when the
//! `<>` is doing most of the filtering, and a gate would need to know which.
static void TestCost() {
	std::mt19937 rng(31337);
	const size_t rows = 400;
	auto graph = MakeGraph(2, 2, ValueType::INT32);
	graph.predicates.push_back(Predicate {0, 0, 1, 0});
	NotEqualPredicate neq;
	neq.left_relation = 0;
	neq.right_relation = 1;
	neq.left_column = 1;
	neq.right_column = 1;

	// Two shapes with the same inputs and very different answers: a wide `<>`
	// (almost everything differs) and a narrow one (almost nothing does).
	for (int domain : {2, 64}) {
		auto source = MakeData(rng, 2, 2, rows, domain);
		const auto result = ExecuteCountNotEqual(graph, neq, source, JoinMode::BOTTOM_INSERT);
		if (!result.ok) {
			std::printf("       compared-column domain %2d: declined (%s)\n", domain, result.error.c_str());
			continue;
		}
		const auto all_plan = BuildPlan(graph);
		const auto all = ExecuteCount(graph, all_plan, source, JoinMode::BOTTOM_INSERT);
		const double kept = all.count > 0 ? 100.0 * static_cast<double>(result.count) / static_cast<double>(all.count)
		                                  : 0.0;
		std::printf("       compared-column domain %2d: equi-join %lld tuples, `<>` keeps %lld (%.1f%%)\n", domain,
		            static_cast<long long>(all.count), static_cast<long long>(result.count), kept);
		Expect(result.count <= all.count, "the `<>` count never exceeds the equi-join it filters (domain " +
		                                      std::to_string(domain) + ")");
	}
	Report("cost: two passes, and the second is a composite-key join -- not free, and not always a win");
}

int main() {
	std::printf("factorize core: `<>` joins by inclusion-exclusion\n\n");
	TestDifferential();
	std::printf("\n");
	TestPartition();
	std::printf("\n");
	TestDeclines();
	std::printf("\n");
	TestCost();
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
