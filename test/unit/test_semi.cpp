//===----------------------------------------------------------------------===//
//                         factorize
//
// test/unit/test_semi.cpp
//
// Semi- and anti-joins in the count path (plan sections 10.4 and 10.5).
//
// The plan estimates semi-joins at 1-2 weeks and calls them "cheaper than
// counting: you need only non-emptiness". That is true of the question but not
// of this implementation, which is cheaper still: the marker an outer join
// already maintains is the entire mechanism, and the kinds differ only in what
// the matched weight becomes at one point in the walk.
//
//     inner   size *= matched                       (PRODUCT, preserve NEITHER)
//     outer   size *= (matched == 0 ? 1 : matched)  (PRODUCT, preserve a side)
//     semi    size  = (matched >  0 ? size : 0)
//     anti    size  = (matched == 0 ? size : 0)
//
// So the thing worth testing is not that a semi-join counts, but that four
// semantics sharing one insertion point have not been wired to each other's
// behaviour -- which a test of any one of them alone would not catch.
//
// Two invariants carry most of the weight here, because they relate the kinds
// rather than checking each against a number:
//
//   SEMI(side) + ANTI(side) == that side's own tuple count, exactly. Every
//   tuple either has a partner or has none, so the two kinds partition the
//   side. A test that got both wrong in the same direction still fails this.
//
//   SEMI(side) <= INNER, because the inner join counts each partnered tuple
//   once per partner and the semi-join counts it once. Equality exactly when
//   no tuple has two partners.
//
//===----------------------------------------------------------------------===//

#include "../../src/core/join.hpp"

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

static void Report(const std::string &group) {
	std::printf("  ok   %s\n", group.c_str());
}

namespace {

struct Rows {
	std::vector<AttributeId> attributes;
	std::vector<std::vector<int64_t>> rows;
	size_t Width() const {
		return attributes.size();
	}
};

AttributeTypes TypesOf(const Rows &r) {
	AttributeTypes t;
	for (auto a : r.attributes) {
		t.emplace_back(a, ValueType::INT64);
	}
	return t;
}

FactorizedRelation Scan(const Rows &r) {
	std::vector<std::vector<int64_t>> columns(r.Width());
	for (const auto &row : r.rows) {
		for (size_t c = 0; c < r.Width(); c++) {
			columns[c].push_back(row[c]);
		}
	}
	return MakeScan(r.attributes, TypesOf(r), columns);
}

Rows MakeRandom(std::mt19937 &rng, AttributeId base, size_t count, int domain) {
	Rows r;
	r.attributes = {base, static_cast<AttributeId>(base + 1)};
	std::uniform_int_distribution<int> value(0, domain - 1);
	for (size_t i = 0; i < count; i++) {
		r.rows.push_back({value(rng), value(rng)});
	}
	return r;
}

//! Reference answers, computed the obvious way over the flat rows.
struct Reference {
	int64_t inner = 0;
	int64_t semi_left = 0;
	int64_t anti_left = 0;
	int64_t semi_right = 0;
	int64_t anti_right = 0;
};

Reference BruteForce(const Rows &left, const Rows &right) {
	Reference ref;
	std::vector<bool> right_matched(right.rows.size(), false);
	for (const auto &l : left.rows) {
		bool any = false;
		for (size_t r = 0; r < right.rows.size(); r++) {
			if (l[0] == right.rows[r][0]) {
				ref.inner++;
				any = true;
				right_matched[r] = true;
			}
		}
		(any ? ref.semi_left : ref.anti_left)++;
	}
	for (size_t r = 0; r < right.rows.size(); r++) {
		(right_matched[r] ? ref.semi_right : ref.anti_right)++;
	}
	return ref;
}

int64_t Run(const Rows &build, const Rows &probe, JoinMode mode, JoinKind kind, Preserve preserve) {
	JoinKeys keys;
	keys.build = {build.attributes[0]};
	keys.probe = {probe.attributes[0]};
	return FactorizedCountJoin(Scan(build), Scan(probe), keys, mode, PathStrategy::LEVELWISE, nullptr, preserve, kind);
}

} // namespace

//===--------------------------------------------------------------------===//
// Every kind, both sides, both modes, against a nested loop
//===--------------------------------------------------------------------===//
static void TestAgainstBruteForce() {
	std::printf("Semi and anti against a nested loop\n");
	std::mt19937 rng(20260905);
	int cases = 0, failures = 0;
	std::string detail;

	for (int left_domain : {2, 5, 11}) {
		for (int right_domain : {3, 7, 13}) {
			for (size_t left_rows : {size_t(0), size_t(1), size_t(14)}) {
				for (size_t right_rows : {size_t(0), size_t(1), size_t(19)}) {
					auto build = MakeRandom(rng, 0, left_rows, left_domain);
					auto probe = MakeRandom(rng, 10, right_rows, right_domain);
					const auto ref = BruteForce(build, probe);
					for (auto mode : {JoinMode::TOP_INSERT, JoinMode::BOTTOM_INSERT}) {
						struct Case {
							JoinKind kind;
							Preserve preserve;
							int64_t expected;
							const char *name;
						};
						const Case list[] = {
						    {JoinKind::PRODUCT, Preserve::NEITHER, ref.inner, "inner"},
						    {JoinKind::SEMI, Preserve::BUILD, ref.semi_left, "semi build"},
						    {JoinKind::ANTI, Preserve::BUILD, ref.anti_left, "anti build"},
						    {JoinKind::SEMI, Preserve::PROBE, ref.semi_right, "semi probe"},
						    {JoinKind::ANTI, Preserve::PROBE, ref.anti_right, "anti probe"},
						};
						for (const auto &c : list) {
							const auto got = Run(build, probe, mode, c.kind, c.preserve);
							cases++;
							if (got == c.expected) {
								continue;
							}
							failures++;
							if (detail.empty()) {
								detail = std::string(c.name) + " mode=" +
								         (mode == JoinMode::TOP_INSERT ? "top" : "bottom") + " " +
								         std::to_string(left_rows) + "x" + std::to_string(left_domain) + " vs " +
								         std::to_string(right_rows) + "x" + std::to_string(right_domain) + ": got " +
								         std::to_string(got) + ", expected " + std::to_string(c.expected);
							}
						}
					}
				}
			}
		}
	}
	Expect(failures == 0, std::to_string(failures) + " of " + std::to_string(cases) + " disagree (" + detail + ")");
	Expect(cases > 500, "the sweep covered " + std::to_string(cases) + " cases");
	Report("inner, semi and anti on both sides agree with a nested loop, both modes");
}

//===--------------------------------------------------------------------===//
// The relations between the kinds
//===--------------------------------------------------------------------===//
static void TestInvariants() {
	std::printf("Semi and anti partition the side they emit\n");
	std::mt19937 rng(99);
	int checked = 0;
	bool partition_ok = true, bound_ok = true;
	std::string detail;

	for (int trial = 0; trial < 60; trial++) {
		auto build = MakeRandom(rng, 0, 13, 4);
		auto probe = MakeRandom(rng, 10, 17, 6);
		for (auto mode : {JoinMode::TOP_INSERT, JoinMode::BOTTOM_INSERT}) {
			const auto semi_b = Run(build, probe, mode, JoinKind::SEMI, Preserve::BUILD);
			const auto anti_b = Run(build, probe, mode, JoinKind::ANTI, Preserve::BUILD);
			const auto semi_p = Run(build, probe, mode, JoinKind::SEMI, Preserve::PROBE);
			const auto anti_p = Run(build, probe, mode, JoinKind::ANTI, Preserve::PROBE);
			const auto inner = Run(build, probe, mode, JoinKind::PRODUCT, Preserve::NEITHER);
			checked++;
			if (semi_b + anti_b != static_cast<int64_t>(build.rows.size()) ||
			    semi_p + anti_p != static_cast<int64_t>(probe.rows.size())) {
				partition_ok = false;
				if (detail.empty()) {
					detail = "semi+anti=" + std::to_string(semi_b + anti_b) + " but the side has " +
					         std::to_string(build.rows.size());
				}
			}
			if (semi_b > inner || semi_p > inner) {
				bound_ok = false;
			}
		}
	}
	Expect(partition_ok, "semi + anti must equal the emitted side's tuple count (" + detail + ")");
	Expect(bound_ok, "a semi-join cannot exceed the inner join it filters");
	Expect(checked == 120, "checked " + std::to_string(checked) + " combinations");
	Report("semi + anti == the side's own tuples, and semi <= inner");
}

//===--------------------------------------------------------------------===//
// The emitted side is itself factorized
//===--------------------------------------------------------------------===//
//
// One record then stands for many tuples, and a semi-join has to emit every
// tuple that record encodes rather than the record once. This is the same trap
// the outer-join work had, arriving through a different fold.
static void TestFactorizedSide() {
	std::printf("A factorized side, where one record is many tuples\n");

	Rows r;
	r.attributes = {0, 1};
	r.rows = {{1, 100}, {1, 101}, {2, 100}, {3, 102}};
	Rows s;
	s.attributes = {1, 2};
	s.rows = {{100, 7}, {100, 8}, {101, 9}, {102, 9}};
	Rows u;
	u.attributes = {0, 3};
	u.rows = {{1, 55}, {9, 56}};

	// The flat inner join underneath, so the reference is computed on tuples.
	Rows rs;
	rs.attributes = {0, 1, 2};
	for (const auto &a : r.rows) {
		for (const auto &b : s.rows) {
			if (a[1] == b[0]) {
				rs.rows.push_back({a[0], a[1], b[1]});
			}
		}
	}
	const auto ref = BruteForce(rs, u);

	JoinKeys inner_keys;
	inner_keys.build = {1};
	inner_keys.probe = {1};
	JoinKeys outer_keys;
	outer_keys.build = {0};
	outer_keys.probe = {0};

	for (auto mode : {JoinMode::TOP_INSERT, JoinMode::BOTTOM_INSERT}) {
		auto accumulated = FactorizedJoin(Scan(r), Scan(s), inner_keys, mode);
		const std::string where = mode == JoinMode::TOP_INSERT ? " (top)" : " (bottom)";
		const auto semi = FactorizedCountJoin(accumulated, Scan(u), outer_keys, mode, PathStrategy::LEVELWISE, nullptr,
		                                      Preserve::BUILD, JoinKind::SEMI);
		const auto anti = FactorizedCountJoin(accumulated, Scan(u), outer_keys, mode, PathStrategy::LEVELWISE, nullptr,
		                                      Preserve::BUILD, JoinKind::ANTI);
		Expect(semi == ref.semi_left, "semi over a factorized side: got " + std::to_string(semi) + ", expected " +
		                                  std::to_string(ref.semi_left) + where);
		Expect(anti == ref.anti_left, "anti over a factorized side: got " + std::to_string(anti) + ", expected " +
		                                  std::to_string(ref.anti_left) + where);
		Expect(semi + anti == accumulated.Count(),
		       "semi + anti must equal the factorized side's tuple count" + where);
	}
	Report("a record standing for many tuples emits all of them, not one");
}

//===--------------------------------------------------------------------===//
// The combinations that have no meaning are refused
//===--------------------------------------------------------------------===//
static void TestInvalidCombinations() {
	std::printf("Meaningless combinations are refused rather than answered\n");
	Rows a;
	a.attributes = {0, 1};
	a.rows = {{1, 1}};
	Rows b;
	b.attributes = {10, 11};
	b.rows = {{1, 1}};

	auto throws = [&](JoinKind kind, Preserve preserve) {
		try {
			Run(a, b, JoinMode::BOTTOM_INSERT, kind, preserve);
			return false;
		} catch (const std::exception &) {
			return true;
		}
	};
	Expect(throws(JoinKind::SEMI, Preserve::NEITHER), "a semi-join emitting neither side must throw");
	Expect(throws(JoinKind::SEMI, Preserve::BOTH), "a semi-join emitting both sides must throw");
	Expect(throws(JoinKind::ANTI, Preserve::NEITHER), "an anti-join emitting neither side must throw");
	Expect(throws(JoinKind::ANTI, Preserve::BOTH), "an anti-join emitting both sides must throw");
	// PRODUCT is valid with every Preserve: NEITHER is an inner join, one side is
	// an outer join, BOTH is a full outer join. Only SEMI and ANTI constrain it,
	// which is why there is no separate OUTER kind to disagree with Preserve.
	Expect(!throws(JoinKind::PRODUCT, Preserve::NEITHER), "PRODUCT with NEITHER is the inner join");
	Expect(!throws(JoinKind::PRODUCT, Preserve::BUILD), "PRODUCT with one side is the outer join");
	Expect(!throws(JoinKind::PRODUCT, Preserve::BOTH), "PRODUCT with BOTH is the full outer join");
	Report("only semi and anti constrain which side is named");
}

int main() {
	std::printf("factorize core: semi and anti joins\n\n");
	TestAgainstBruteForce();
	std::printf("\n");
	TestInvariants();
	std::printf("\n");
	TestFactorizedSide();
	std::printf("\n");
	TestInvalidCombinations();
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
