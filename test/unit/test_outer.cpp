//===----------------------------------------------------------------------===//
//                         factorize
//
// test/unit/test_outer.cpp
//
// Outer joins in the count path (plan section 10.5, paper section 4.8).
//
// The paper's sketch of outer joins is about representing null-extension: put
// nulls in the f-tree nodes, because "null is just another value in the
// domain". A count never looks at a value, so none of that is needed here and
// none of it is done -- what an unmatched tuple needs is to contribute 1
// instead of 0. That makes the whole feature arithmetic, which in turn makes
// the only convincing test a differential one: the count has to equal what a
// brute-force nested loop over the same rows produces, for every combination of
// which side is preserved and which side is indexed.
//
// Two things this is really testing, beyond "the number is right":
//
//   1. That Preserve is not silently swapped. It names the *arguments*, while
//      the counters work in upper and lower, and which is which depends on
//      JoinMode. A mapping that is inverted still passes every symmetric test,
//      so every case here is run under both modes and the asymmetric cases are
//      the point.
//
//   2. That preservation applies to tuples rather than to records. When the
//      preserved side is itself factorized, one record can stand for many
//      tuples, and an unmatched record must contribute as many output tuples as
//      it encodes -- not one.
//
//===----------------------------------------------------------------------===//

#include "../../src/core/join.hpp"

#include <cstdio>
#include <map>
#include <random>
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

static void Report(const std::string &group) {
	std::printf("  ok   %s\n", group.c_str());
}

namespace {

//! A flat relation as the test thinks of it: rows of values, one column per
//! attribute, so a reference implementation can be written as nested loops.
struct Rows {
	std::vector<AttributeId> attributes;
	std::vector<std::vector<int64_t>> rows;

	size_t Width() const {
		return attributes.size();
	}
	int Column(AttributeId attribute) const {
		for (size_t i = 0; i < attributes.size(); i++) {
			if (attributes[i] == attribute) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}
};

AttributeTypes TypesOf(const Rows &relation) {
	AttributeTypes types;
	for (auto attribute : relation.attributes) {
		types.emplace_back(attribute, ValueType::INT64);
	}
	return types;
}

FactorizedRelation Scan(const Rows &relation) {
	std::vector<std::vector<int64_t>> columns(relation.Width());
	for (const auto &row : relation.rows) {
		for (size_t c = 0; c < relation.Width(); c++) {
			columns[c].push_back(row[c]);
		}
	}
	return MakeScan(relation.attributes, TypesOf(relation), columns);
}

//! The answer, computed the obvious way. Two flat relations, one join
//! attribute each, every combination of preservation.
int64_t BruteForce(const Rows &left, AttributeId left_key, const Rows &right, AttributeId right_key, bool preserve_left,
                   bool preserve_right) {
	const auto lc = static_cast<size_t>(left.Column(left_key));
	const auto rc = static_cast<size_t>(right.Column(right_key));
	int64_t total = 0;
	std::vector<bool> right_matched(right.rows.size(), false);
	for (const auto &l : left.rows) {
		bool matched = false;
		for (size_t r = 0; r < right.rows.size(); r++) {
			if (l[lc] == right.rows[r][rc]) {
				total++;
				matched = true;
				right_matched[r] = true;
			}
		}
		if (!matched && preserve_left) {
			total++;
		}
	}
	if (preserve_right) {
		for (size_t r = 0; r < right.rows.size(); r++) {
			if (!right_matched[r]) {
				total++;
			}
		}
	}
	return total;
}

Rows MakeRandom(std::mt19937 &rng, AttributeId base, size_t width, size_t count, int domain) {
	Rows relation;
	for (size_t i = 0; i < width; i++) {
		relation.attributes.push_back(static_cast<AttributeId>(base + i));
	}
	std::uniform_int_distribution<int> value(0, domain - 1);
	for (size_t r = 0; r < count; r++) {
		std::vector<int64_t> row;
		for (size_t i = 0; i < width; i++) {
			row.push_back(value(rng));
		}
		relation.rows.push_back(std::move(row));
	}
	return relation;
}

const char *NameOf(Preserve preserve) {
	switch (preserve) {
	case Preserve::NEITHER:
		return "NEITHER";
	case Preserve::BUILD:
		return "BUILD";
	case Preserve::PROBE:
		return "PROBE";
	default:
		return "BOTH";
	}
}

} // namespace

//===--------------------------------------------------------------------===//
// Two flat relations, every preservation, both modes
//===--------------------------------------------------------------------===//
static void TestFlatOuterJoins() {
	std::printf("Outer joins against a nested loop\n");
	std::mt19937 rng(20260904);
	int cases = 0;
	int failures = 0;
	std::string detail;

	// Domains deliberately small and unequal in size, so that both sides
	// genuinely have unmatched rows rather than the case never arising.
	for (int build_domain : {2, 5, 9}) {
		for (int probe_domain : {3, 5, 13}) {
			for (size_t build_rows : {size_t(0), size_t(1), size_t(12)}) {
				for (size_t probe_rows : {size_t(0), size_t(1), size_t(17)}) {
					auto build = MakeRandom(rng, 0, 2, build_rows, build_domain);
					auto probe = MakeRandom(rng, 10, 2, probe_rows, probe_domain);
					JoinKeys keys;
					keys.build = {0};
					keys.probe = {10};
					for (auto preserve :
					     {Preserve::NEITHER, Preserve::BUILD, Preserve::PROBE, Preserve::BOTH}) {
						const auto expected =
						    BruteForce(build, 0, probe, 10,
						               preserve == Preserve::BUILD || preserve == Preserve::BOTH,
						               preserve == Preserve::PROBE || preserve == Preserve::BOTH);
						for (auto mode : {JoinMode::TOP_INSERT, JoinMode::BOTTOM_INSERT}) {
							const auto actual = FactorizedCountJoin(Scan(build), Scan(probe), keys, mode,
							                                        PathStrategy::LEVELWISE, nullptr, preserve);
							cases++;
							if (actual == expected) {
								continue;
							}
							failures++;
							if (detail.empty()) {
								detail = std::string(NameOf(preserve)) + " mode=" +
								         (mode == JoinMode::TOP_INSERT ? "top" : "bottom") + " build=" +
								         std::to_string(build_rows) + "x" + std::to_string(build_domain) +
								         " probe=" + std::to_string(probe_rows) + "x" +
								         std::to_string(probe_domain) + ": got " + std::to_string(actual) +
								         ", expected " + std::to_string(expected);
							}
						}
					}
				}
			}
		}
	}
	Expect(failures == 0, "outer join count differs from a nested loop in " + std::to_string(failures) + " of " +
	                          std::to_string(cases) + " cases (" + detail + ")");
	Expect(cases > 200, "the sweep covered " + std::to_string(cases) + " cases");
	Report("every preservation, both modes, agrees with a nested loop");
}

//===--------------------------------------------------------------------===//
// An inner join is what Preserve::NEITHER means
//===--------------------------------------------------------------------===//
static void TestNeitherIsUnchanged() {
	std::printf("Preserve::NEITHER is the inner join it was\n");
	std::mt19937 rng(7);
	int mismatches = 0;
	for (int trial = 0; trial < 40; trial++) {
		auto build = MakeRandom(rng, 0, 2, 9, 4);
		auto probe = MakeRandom(rng, 10, 2, 11, 6);
		JoinKeys keys;
		keys.build = {0};
		keys.probe = {10};
		for (auto mode : {JoinMode::TOP_INSERT, JoinMode::BOTTOM_INSERT}) {
			const auto without = FactorizedCountJoin(Scan(build), Scan(probe), keys, mode);
			const auto with_neither = FactorizedCountJoin(Scan(build), Scan(probe), keys, mode,
			                                             PathStrategy::LEVELWISE, nullptr, Preserve::NEITHER);
			if (without != with_neither) {
				mismatches++;
			}
		}
	}
	Expect(mismatches == 0, "the defaulted call and an explicit NEITHER disagree " + std::to_string(mismatches) +
	                            " times");
	Report("adding the parameter did not change what existing callers get");
}

//===--------------------------------------------------------------------===//
// Preservation counts tuples, not records
//===--------------------------------------------------------------------===//
//
// The preserved side here is the result of an inner join, so one of its records
// stands for several flat tuples. An unmatched record has to contribute as many
// output tuples as it encodes. Counting it once -- the obvious bug, and the one
// a flat-only test cannot see -- shows up as a count that is too small.
static void TestPreservedSideIsFactorized() {
	std::printf("A preserved side that is itself factorized\n");

	// r(a, b) joined with s(b, c) on b, then left-joined with u(a, d) on a.
	Rows r;
	r.attributes = {0, 1};
	r.rows = {{1, 100}, {1, 101}, {2, 100}, {3, 102}};
	Rows s;
	s.attributes = {1, 2};
	s.rows = {{100, 7}, {100, 8}, {101, 9}, {102, 9}};
	Rows u;
	u.attributes = {0, 3};
	u.rows = {{1, 55}, {9, 56}};

	// The flat inner join of r and s, computed the obvious way, is the relation
	// the outer join must preserve.
	Rows rs;
	rs.attributes = {0, 1, 2};
	for (const auto &lr : r.rows) {
		for (const auto &ls : s.rows) {
			if (lr[1] == ls[0]) {
				rs.rows.push_back({lr[0], lr[1], ls[1]});
			}
		}
	}
	Expect(rs.rows.size() == 6, "the inner join underneath has 6 tuples, got " + std::to_string(rs.rows.size()));

	JoinKeys inner_keys;
	inner_keys.build = {1};
	inner_keys.probe = {1};
	JoinKeys outer_keys;
	outer_keys.build = {0};
	outer_keys.probe = {0};

	for (auto mode : {JoinMode::TOP_INSERT, JoinMode::BOTTOM_INSERT}) {
		auto accumulated = FactorizedJoin(Scan(r), Scan(s), inner_keys, mode);
		Expect(accumulated.Count() == 6, "the factorized inner join encodes 6 tuples");

		for (auto preserve : {Preserve::NEITHER, Preserve::BUILD, Preserve::PROBE, Preserve::BOTH}) {
			const auto expected = BruteForce(rs, 0, u, 0, preserve == Preserve::BUILD || preserve == Preserve::BOTH,
			                                 preserve == Preserve::PROBE || preserve == Preserve::BOTH);
			const auto actual = FactorizedCountJoin(accumulated, Scan(u), outer_keys, mode, PathStrategy::LEVELWISE,
			                                        nullptr, preserve);
			Expect(actual == expected, std::string("factorized preserved side, ") + NameOf(preserve) + " mode=" +
			                               (mode == JoinMode::TOP_INSERT ? "top" : "bottom") + ": got " +
			                               std::to_string(actual) + ", expected " + std::to_string(expected));
		}
	}
	Report("an unmatched record contributes every tuple it encodes, not one");
}

//===--------------------------------------------------------------------===//
// The degenerate sides, which are where an off-by-one lives
//===--------------------------------------------------------------------===//
static void TestEmptySides() {
	std::printf("Empty sides\n");
	Rows filled;
	filled.attributes = {0, 1};
	filled.rows = {{1, 10}, {2, 20}, {2, 21}};
	Rows empty;
	empty.attributes = {10, 11};

	JoinKeys keys;
	keys.build = {0};
	keys.probe = {10};

	for (auto mode : {JoinMode::TOP_INSERT, JoinMode::BOTTOM_INSERT}) {
		const std::string where = mode == JoinMode::TOP_INSERT ? " (top)" : " (bottom)";
		// Nothing on the probe side: an inner join is empty, preserving the
		// build side gives one output tuple per build tuple.
		Expect(FactorizedCountJoin(Scan(filled), Scan(empty), keys, mode) == 0,
		       "inner join with an empty probe side is empty" + where);
		Expect(FactorizedCountJoin(Scan(filled), Scan(empty), keys, mode, PathStrategy::LEVELWISE, nullptr,
		                           Preserve::BUILD) == 3,
		       "preserving the build side against an empty probe side gives every build tuple" + where);
		Expect(FactorizedCountJoin(Scan(filled), Scan(empty), keys, mode, PathStrategy::LEVELWISE, nullptr,
		                           Preserve::PROBE) == 0,
		       "preserving an empty probe side gives nothing" + where);
		Expect(FactorizedCountJoin(Scan(filled), Scan(empty), keys, mode, PathStrategy::LEVELWISE, nullptr,
		                           Preserve::BOTH) == 3,
		       "a full outer join with one side empty is the other side" + where);

		// And the same with the sides exchanged, which is the check that would
		// fail if Preserve were mapped onto upper/lower the wrong way round.
		JoinKeys flipped;
		flipped.build = {10};
		flipped.probe = {0};
		Expect(FactorizedCountJoin(Scan(empty), Scan(filled), flipped, mode, PathStrategy::LEVELWISE, nullptr,
		                           Preserve::PROBE) == 3,
		       "preserving the probe side against an empty build side gives every probe tuple" + where);
		Expect(FactorizedCountJoin(Scan(empty), Scan(filled), flipped, mode, PathStrategy::LEVELWISE, nullptr,
		                           Preserve::BUILD) == 0,
		       "preserving an empty build side gives nothing" + where);
	}
	Report("empty sides, in both directions");
}

int main() {
	std::printf("factorize core: outer joins\n\n");
	TestFlatOuterJoins();
	std::printf("\n");
	TestNeitherIsUnchanged();
	std::printf("\n");
	TestPreservedSideIsFactorized();
	std::printf("\n");
	TestEmptySides();
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
