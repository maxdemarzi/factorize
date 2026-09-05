//===----------------------------------------------------------------------===//
//                         factorize
//
// test/unit/test_composite.cpp
//
// Composite-key equi-joins: `A.k1 = B.k1 AND A.k2 = B.k2`.
//
// These were declined project-wide and reported as "cyclic join graph", which
// was wrong twice over: the graph is acyclic, and the engine underneath
// computes it correctly. The planner's convergence check tested whether the
// joined-side key attributes shared an equivalence CLASS, which is a proxy for
// the real requirement -- that they land on one f-tree NODE, since MakeKeyReader
// reads every key from a single level. Class-equality implies same-node and not
// the reverse, and every composite key lives in the gap.
//
// The planner now simulates the tree it is planning and asks the real question.
// So the cases worth writing are the ones where the proxy and the truth come
// apart, not the ones where they agree:
//
//   - two relations joined on two columns, where the keys are two classes on
//     one node: the proxy said no, the truth says yes;
//   - a third relation attaching on one of those two columns, which is where
//     "one node" survives a join and "one class" never applied;
//   - a key set the packed 64-bit key cannot hold, which must be a decline from
//     the planner and not an exception from inside the join.
//
//===----------------------------------------------------------------------===//

#include "../../src/core/enumerate.hpp"
#include "../../src/core/plan.hpp"

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

//! A group is only "ok" if nothing inside it failed.
//!
//! This printed ok unconditionally, so a group with failing checks showed
//! its FAIL lines and then said ok on the next line. The totals and the
//! exit code were right, so CI still caught it -- but a human scanning
//! output reads the last line of a group, and that line was a lie.
static int g_reported = 0;

static void Report(const std::string &what) {
	if (g_failures > g_reported) {
		g_reported = g_failures;
		std::printf("  FAIL %s\n", what.c_str());
		return;
	}
	std::printf("  ok   %s\n", what.c_str());
}

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

//! Rows of one relation, as a column-major block.
static std::vector<std::vector<int64_t>> Rows(const std::vector<std::vector<int64_t>> &rows, size_t columns) {
	std::vector<std::vector<int64_t>> block(columns);
	for (const auto &row : rows) {
		for (size_t c = 0; c < columns; c++) {
			block[c].push_back(row[c]);
		}
	}
	return block;
}

//! The answer nobody can get wrong: every combination, tested one at a time.
static int64_t BruteForceTwo(const std::vector<std::vector<int64_t>> &a, const std::vector<std::vector<int64_t>> &b,
                             const std::vector<std::pair<size_t, size_t>> &on) {
	int64_t count = 0;
	for (const auto &left : a) {
		for (const auto &right : b) {
			bool match = true;
			for (const auto &pair : on) {
				if (left[pair.first] != right[pair.second]) {
					match = false;
					break;
				}
			}
			count += match ? 1 : 0;
		}
	}
	return count;
}

//! Two relations joined on two columns at once. The keys are two different
//! equivalence classes, which is exactly why the old check refused this, and
//! they are on one node, which is why the engine can do it.
static void TestTwoRelationsCompositeKey() {
	const std::vector<std::vector<int64_t>> a_rows = {{1, 10}, {1, 11}, {2, 10}, {2, 12}, {3, 10}};
	const std::vector<std::vector<int64_t>> b_rows = {{1, 10}, {1, 12}, {2, 10}, {2, 10}, {4, 10}};

	MemorySource source;
	source.Add(Rows(a_rows, 2));
	source.Add(Rows(b_rows, 2));

	QueryGraph graph;
	graph.column_counts = {2, 2};
	graph.column_types = {{ValueType::INT32, ValueType::INT32}, {ValueType::INT32, ValueType::INT32}};
	graph.predicates = {Predicate {0, 0, 1, 0}, Predicate {0, 1, 1, 1}};

	const auto plan = BuildPlan(graph);
	Expect(plan.complete, "composite: two-column join plans (" + plan.reason + ")");
	if (!plan.complete) {
		return;
	}
	const auto counted = ExecuteCount(graph, plan, source, JoinMode::BOTTOM_INSERT);
	const auto truth = BruteForceTwo(a_rows, b_rows, {{0, 0}, {1, 1}});
	Expect(counted.ok, "composite: two-column join runs (" + counted.error + ")");
	Expect(counted.count == truth, "composite: two-column count is " + std::to_string(counted.count) +
	                                   ", brute force says " + std::to_string(truth));

	// The same shape on one column only, so the test says the second column
	// changes the answer rather than merely being accepted.
	QueryGraph single;
	single.column_counts = {2, 2};
	single.column_types = graph.column_types;
	single.predicates = {Predicate {0, 0, 1, 0}};
	const auto single_plan = BuildPlan(single);
	const auto single_count = ExecuteCount(single, single_plan, source, JoinMode::BOTTOM_INSERT);
	const auto single_truth = BruteForceTwo(a_rows, b_rows, {{0, 0}});
	Expect(single_plan.complete && single_count.ok && single_count.count == single_truth,
	       "composite: the one-column join of the same tables still agrees");
	Expect(single_truth != truth, "composite: the second key column actually narrows the join (" +
	                                  std::to_string(single_truth) + " vs " + std::to_string(truth) + ")");
	Report("two relations joined on two columns at once");
}

//! Both insert modes, because the tree is built from opposite ends in each and
//! the planner simulates only one of them.
static void TestCompositeUnderBothModes() {
	const std::vector<std::vector<int64_t>> a_rows = {{1, 5}, {1, 6}, {2, 5}, {2, 5}, {3, 7}};
	const std::vector<std::vector<int64_t>> b_rows = {{1, 5}, {2, 5}, {2, 5}, {3, 8}};

	MemorySource source;
	source.Add(Rows(a_rows, 2));
	source.Add(Rows(b_rows, 2));

	QueryGraph graph;
	graph.column_counts = {2, 2};
	graph.column_types = {{ValueType::INT32, ValueType::INT32}, {ValueType::INT32, ValueType::INT32}};
	graph.predicates = {Predicate {0, 0, 1, 0}, Predicate {0, 1, 1, 1}};

	const auto plan = BuildPlan(graph);
	Expect(plan.complete, "modes: plans (" + plan.reason + ")");
	if (!plan.complete) {
		return;
	}
	const auto truth = BruteForceTwo(a_rows, b_rows, {{0, 0}, {1, 1}});
	for (auto mode : {JoinMode::BOTTOM_INSERT, JoinMode::TOP_INSERT}) {
		const auto counted = ExecuteCount(graph, plan, source, mode);
		Expect(counted.ok && counted.count == truth,
		       std::string("modes: ") + (mode == JoinMode::BOTTOM_INSERT ? "bottom" : "top") + "-insert gives " +
		           std::to_string(counted.count) + ", brute force says " + std::to_string(truth));
	}
	Report("a composite key counts the same under both insert modes");
}

//! A third relation attaching on ONE of the two composite columns.
//!
//! This is the case the equivalence-class proxy could never express: after the
//! composite join, k1 and k2 are on one node, and a third relation joining on
//! k1 alone attaches to that node. The classes involved were never equal and
//! never become so.
static void TestThirdRelationOnOneOfTheKeys() {
	const std::vector<std::vector<int64_t>> a_rows = {{1, 10}, {1, 11}, {2, 10}, {2, 12}, {3, 10}};
	const std::vector<std::vector<int64_t>> b_rows = {{1, 10}, {1, 12}, {2, 10}, {2, 10}, {4, 10}};
	const std::vector<std::vector<int64_t>> c_rows = {{1}, {1}, {2}, {5}};

	MemorySource source;
	source.Add(Rows(a_rows, 2));
	source.Add(Rows(b_rows, 2));
	source.Add(Rows(c_rows, 1));

	QueryGraph graph;
	graph.column_counts = {2, 2, 1};
	graph.column_types = {{ValueType::INT32, ValueType::INT32},
	                      {ValueType::INT32, ValueType::INT32},
	                      {ValueType::INT32}};
	graph.predicates = {Predicate {0, 0, 1, 0}, Predicate {0, 1, 1, 1}, Predicate {0, 0, 2, 0}};

	const auto plan = BuildPlan(graph);
	Expect(plan.complete, "third: three-relation composite plans (" + plan.reason + ")");
	if (!plan.complete) {
		return;
	}
	int64_t truth = 0;
	for (const auto &left : a_rows) {
		for (const auto &right : b_rows) {
			if (left[0] != right[0] || left[1] != right[1]) {
				continue;
			}
			for (const auto &third : c_rows) {
				truth += left[0] == third[0] ? 1 : 0;
			}
		}
	}
	for (auto mode : {JoinMode::BOTTOM_INSERT, JoinMode::TOP_INSERT}) {
		const auto counted = ExecuteCount(graph, plan, source, mode);
		Expect(counted.ok && counted.count == truth,
		       std::string("third: ") + (mode == JoinMode::BOTTOM_INSERT ? "bottom" : "top") + "-insert gives " +
		           std::to_string(counted.count) + ", brute force says " + std::to_string(truth));
	}
	Report("a third relation attaching on one column of a composite key");
}

//! The packed key is one 64-bit word, and the rule is the SUM of the key
//! columns' widths -- not a count of them and not "no INT64".
//!
//! INT32+INT64 is 96 bits and must be refused. It is the shape that matters:
//! a small discriminator beside a bigint id is the ordinary composite key, so a
//! rule written as "not two INT64s" would let exactly the common case through
//! to an exception thrown from inside a running join.
static void TestKeyWidthIsDeclinedNotThrown() {
	struct Case {
		ValueType first;
		ValueType second;
		bool fits;
		const char *what;
	};
	const Case cases[] = {
	    {ValueType::INT32, ValueType::INT32, true, "INT32+INT32 is 64 bits and fits"},
	    {ValueType::INT32, ValueType::INT64, false, "INT32+INT64 is 96 bits and does not"},
	    {ValueType::INT64, ValueType::INT32, false, "INT64+INT32 is 96 bits and does not"},
	    {ValueType::INT64, ValueType::INT64, false, "INT64+INT64 is 128 bits and does not"},
	};
	for (const auto &c : cases) {
		QueryGraph graph;
		graph.column_counts = {2, 2};
		graph.column_types = {{c.first, c.second}, {c.first, c.second}};
		graph.predicates = {Predicate {0, 0, 1, 0}, Predicate {0, 1, 1, 1}};
		const auto plan = BuildPlan(graph);
		Expect(plan.complete == c.fits, std::string("width: ") + c.what);
		if (!c.fits) {
			Expect(plan.reason.find("64-bit") != std::string::npos,
			       std::string("width: the reason names the key width, not cyclicity -- got \"") + plan.reason + "\"");
		}
	}

	// A single INT64 key is exactly 64 bits and is not composite at all, so it
	// has to keep working: the cap is on the sum, and one column reaches it.
	QueryGraph single;
	single.column_counts = {1, 1};
	single.column_types = {{ValueType::INT64}, {ValueType::INT64}};
	single.predicates = {Predicate {0, 0, 1, 0}};
	Expect(BuildPlan(single).complete, "width: one INT64 key is exactly 64 bits and still plans");
	Report("a key wider than the packed word is declined by the planner, not thrown by the join");
}

//! Counts three relations by trying every combination against every predicate.
static int64_t BruteForceThree(const std::vector<std::vector<std::vector<int64_t>>> &rows,
                               const std::vector<Predicate> &predicates) {
	int64_t count = 0;
	for (const auto &a : rows[0]) {
		for (const auto &b : rows[1]) {
			for (const auto &c : rows[2]) {
				const std::vector<const std::vector<int64_t> *> tuple = {&a, &b, &c};
				bool match = true;
				for (const auto &p : predicates) {
					if ((*tuple[p.left_relation])[static_cast<size_t>(p.left_column)] !=
					    (*tuple[p.right_relation])[static_cast<size_t>(p.right_column)]) {
						match = false;
						break;
					}
				}
				count += match ? 1 : 0;
			}
		}
	}
	return count;
}

//! An equality that no join has enforced must never be substituted.
//!
//! EquivalenceClasses used to merge every predicate of the graph before any join
//! ran, so ShallowestEquivalent could resolve a key through an equality that was
//! merely *implied* by predicates not yet applied. When that happened, the two
//! keys of one join collapsed onto a single attribute, the join checked one
//! constraint where the query wrote two, and the count came out too high with
//! nothing to indicate it. Measured over 3000 random graphs, 132 of the 2599 the
//! planner accepted answered wrongly.
//!
//! The property asserted here is the only one that matters and is weaker than
//! "computes the answer" on purpose: declining is always safe, and answering
//! wrongly never is. Both shapes below returned a wrong count before the fix.
static void TestUnenforcedEqualityIsNeverSubstituted() {
	struct Shape {
		const char *what;
		std::vector<std::vector<std::vector<int64_t>>> rows;
		std::vector<size_t> columns;
		std::vector<ValueType> types;
		std::vector<Predicate> predicates;
	};
	const std::vector<Shape> shapes = {
	    // Two edges attaching one relation, whose accumulated sides are equal
	    // only *because* of those two edges. Answered 5 against a truth of 3.
	    {"two edges imply an equality between the sides they attach to",
	     {{{1}, {2}, {3}}, {{1, 1}, {2, 2}, {2, 3}}, {{1}, {2}, {2}}},
	     {1, 2, 1},
	     {ValueType::INT32, ValueType::INT32, ValueType::INT32, ValueType::INT32},
	     {Predicate {0, 0, 1, 0}, Predicate {1, 1, 2, 0}, Predicate {2, 0, 0, 0}}},
	    // A triangle in which no relation has two of its own columns in one
	    // class, so "a self-equality within one relation" does not describe it.
	    // Answered 12 against a truth of 8.
	    {"a triangle implying no self-equality within any relation",
	     {{{1, 2}, {2, 1}, {2, 2}}, {{1, 1}, {2, 2}, {1, 2}}, {{1, 1}, {2, 2}, {2, 1}}},
	     {2, 2, 2},
	     {ValueType::INT32, ValueType::INT32, ValueType::INT32, ValueType::INT32, ValueType::INT32, ValueType::INT32},
	     {Predicate {0, 0, 1, 1}, Predicate {1, 0, 2, 1}, Predicate {0, 1, 2, 1}}},
	};

	for (const auto &shape : shapes) {
		MemorySource source;
		QueryGraph graph;
		size_t next_type = 0;
		for (size_t r = 0; r < shape.rows.size(); r++) {
			source.Add(Rows(shape.rows[r], shape.columns[r]));
			graph.column_counts.push_back(shape.columns[r]);
			std::vector<ValueType> types;
			for (size_t c = 0; c < shape.columns[r]; c++) {
				types.push_back(shape.types[next_type++]);
			}
			graph.column_types.push_back(types);
		}
		graph.predicates = shape.predicates;

		const auto truth = BruteForceThree(shape.rows, shape.predicates);
		const auto plan = BuildPlan(graph);
		if (!plan.complete) {
			Expect(true, std::string("unenforced: declined, which is safe -- ") + shape.what);
			continue;
		}
		for (auto mode : {JoinMode::BOTTOM_INSERT, JoinMode::TOP_INSERT}) {
			const auto counted = ExecuteCount(graph, plan, source, mode);
			Expect(counted.ok && counted.count == truth,
			       std::string("unenforced: ") + shape.what + " -- " +
			           (mode == JoinMode::BOTTOM_INSERT ? "bottom" : "top") + "-insert gives " +
			           std::to_string(counted.count) + ", brute force says " + std::to_string(truth));
		}
	}
	Report("an equality no join has enforced is never substituted");
}

//! A genuinely cyclic graph must still be refused -- the point was never to
//! accept everything, and a triangle's third relation reaches the other two
//! through nodes that no transformation brings together.
static void TestCyclicStillRefused() {
	QueryGraph graph;
	graph.column_counts = {2, 2, 2};
	graph.column_types = {{ValueType::INT32, ValueType::INT32},
	                      {ValueType::INT32, ValueType::INT32},
	                      {ValueType::INT32, ValueType::INT32}};
	// A-B, B-C, C-A on distinct columns: a triangle.
	graph.predicates = {Predicate {0, 0, 1, 0}, Predicate {1, 1, 2, 0}, Predicate {2, 1, 0, 1}};
	const auto plan = BuildPlan(graph);
	Expect(!plan.complete, "cyclic: a triangle is still refused");
	Report("a cyclic graph is still refused, and says so without claiming to be about keys");
}

//! A sweep, because the interesting failures are structural rather than
//! arithmetic and one hand-written shape proves little. Every combination of
//! key-column count and row content, counted against a nested loop.
static void TestSweepAgainstBruteForce() {
	int cases = 0;
	for (int domain = 2; domain <= 4; domain++) {
		for (int a_rows_n = 1; a_rows_n <= 6; a_rows_n++) {
			for (int b_rows_n = 1; b_rows_n <= 6; b_rows_n++) {
				std::vector<std::vector<int64_t>> a_rows;
				std::vector<std::vector<int64_t>> b_rows;
				for (int i = 0; i < a_rows_n; i++) {
					a_rows.push_back({i % domain, (i * 3) % domain});
				}
				for (int i = 0; i < b_rows_n; i++) {
					b_rows.push_back({(i * 2) % domain, i % domain});
				}
				MemorySource source;
				source.Add(Rows(a_rows, 2));
				source.Add(Rows(b_rows, 2));

				QueryGraph graph;
				graph.column_counts = {2, 2};
				graph.column_types = {{ValueType::INT32, ValueType::INT32},
				                      {ValueType::INT32, ValueType::INT32}};
				graph.predicates = {Predicate {0, 0, 1, 0}, Predicate {0, 1, 1, 1}};
				const auto plan = BuildPlan(graph);
				if (!plan.complete) {
					Expect(false, "sweep: plan failed (" + plan.reason + ")");
					return;
				}
				const auto truth = BruteForceTwo(a_rows, b_rows, {{0, 0}, {1, 1}});
				for (auto mode : {JoinMode::BOTTOM_INSERT, JoinMode::TOP_INSERT}) {
					const auto counted = ExecuteCount(graph, plan, source, mode);
					if (!counted.ok || counted.count != truth) {
						Expect(false, "sweep: domain " + std::to_string(domain) + " " + std::to_string(a_rows_n) +
						                  "x" + std::to_string(b_rows_n) + " gave " + std::to_string(counted.count) +
						                  " against " + std::to_string(truth) + " (" + counted.error + ")");
						return;
					}
					cases++;
				}
			}
		}
	}
	Expect(cases == 3 * 6 * 6 * 2, "sweep: ran every case, " + std::to_string(cases));
	Report("every composite shape in the sweep agrees with a nested loop");
}

int main() {
	std::printf("factorize core: composite keys\n\n");
	TestTwoRelationsCompositeKey();
	TestCompositeUnderBothModes();
	TestThirdRelationOnOneOfTheKeys();
	TestKeyWidthIsDeclinedNotThrown();
	TestUnenforcedEqualityIsNeverSubstituted();
	TestCyclicStillRefused();
	TestSweepAgainstBruteForce();
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
