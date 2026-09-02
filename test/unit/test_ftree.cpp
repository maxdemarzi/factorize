//===----------------------------------------------------------------------===//
//                         factorize
//
// test/unit/test_ftree.cpp
//
// Golden tests for the f-tree algebra, taken from the paper's own worked
// examples. Figures 6 and 7 are reproduced node-for-node; if these pass, the
// join and the root-to-leaf transformation agree with Lehner & Neumann.
//
// Standalone: this links only src/core and never touches DuckDB.
//
//===----------------------------------------------------------------------===//

#include "../../src/core/ftree.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace factorize;

//===--------------------------------------------------------------------===//
// Minimal harness
//===--------------------------------------------------------------------===//
static int g_failures = 0;
static int g_checks = 0;

static void ExpectEq(const std::string &actual, const std::string &expected, const char *what) {
	g_checks++;
	if (actual == expected) {
		std::printf("  ok   %s\n", what);
		return;
	}
	g_failures++;
	std::printf("  FAIL %s\n         expected: %s\n         actual:   %s\n", what, expected.c_str(), actual.c_str());
}

static void ExpectTrue(bool condition, const char *what) {
	g_checks++;
	if (condition) {
		std::printf("  ok   %s\n", what);
		return;
	}
	g_failures++;
	std::printf("  FAIL %s\n", what);
}

//! Names attributes from an explicit table, so tests can use the paper's own
//! labels (B1, C2, ...) rather than raw ids.
static AttributeNamer NamerFor(const std::map<AttributeId, std::string> &names) {
	return [names](AttributeId id) -> std::string {
		auto entry = names.find(id);
		return entry == names.end() ? std::string("?") : entry->second;
	};
}

//===--------------------------------------------------------------------===//
// Figure 6 -- top- and bottom-insert
//
// Build tree  A(B(C), D)      Probe tree  S(T(U))      Join  B.b = T.t
//
// (c) top-insert:    the probe tree is the upper part and the whole build tree
//                    is attached under T, the probe's joined node.
// (d) bottom-insert: the build tree is the upper part and the whole probe tree
//                    is attached under B, the build's joined node.
//===--------------------------------------------------------------------===//
static void TestFigure6() {
	std::printf("Figure 6 -- top- and bottom-insert\n");

	enum : AttributeId { A = 0, B = 1, C = 2, D = 3, S = 4, T = 5, U = 6 };
	auto namer = NamerFor({{A, "A"}, {B, "B"}, {C, "C"}, {D, "D"}, {S, "S"}, {T, "T"}, {U, "U"}});

	// Build tree: A with children B (-> C) and D.
	FTree build = FTree::Scan({A});
	auto &b_node = build.Root().AddChild(std::make_unique<FNode>(std::vector<AttributeId> {B}));
	b_node.AddChild(std::make_unique<FNode>(std::vector<AttributeId> {C}));
	build.Root().AddChild(std::make_unique<FNode>(std::vector<AttributeId> {D}));
	ExpectEq(build.ToString(namer), "{A}({B}({C}),{D})", "Figure 6b build tree");

	// Probe tree: the chain S -> T -> U.
	FTree probe = FTree::Scan({S});
	auto &t_node = probe.Root().AddChild(std::make_unique<FNode>(std::vector<AttributeId> {T}));
	t_node.AddChild(std::make_unique<FNode>(std::vector<AttributeId> {U}));
	ExpectEq(probe.ToString(namer), "{S}({T}({U}))", "Figure 6b probe tree");

	JoinKeys keys;
	keys.build = {B};
	keys.probe = {T};

	auto top = MergeTrees(build, probe, keys, JoinMode::TOP_INSERT);
	ExpectEq(top.ToString(namer), "{S}({T}({A}({B}({C}),{D}),{U}))", "Figure 6c top-insert");

	auto bottom = MergeTrees(build, probe, keys, JoinMode::BOTTOM_INSERT);
	ExpectEq(bottom.ToString(namer), "{A}({B}({C},{S}({T}({U}))),{D})", "Figure 6d bottom-insert");

	// Section 5.2: only the bottom-insert's insertion point is contended.
	const FNode *top_attached = top.Find(A);
	const FNode *bottom_attached = bottom.Find(S);
	ExpectTrue(top_attached != nullptr && !top_attached->RequiresLock(), "top-insert needs no lock");
	ExpectTrue(bottom_attached != nullptr && bottom_attached->RequiresLock(), "bottom-insert marks its insertion point");

	// The paper's equivalence: L (top-insert) R == R (bottom-insert) L.
	JoinKeys swapped;
	swapped.build = keys.probe;
	swapped.probe = keys.build;
	auto swapped_bi = MergeTrees(probe, build, swapped, JoinMode::BOTTOM_INSERT);
	ExpectEq(swapped_bi.ToString(namer), top.ToString(namer),
	         "top-insert(build,probe) has the shape of bottom-insert(probe,build)");
}

//===--------------------------------------------------------------------===//
// Figure 7 -- generating a common root-to-leaf path
//
// Original:            A(B1(C1, C2(E)), B2(D))
// Inserting R as a dependent node of B2 and C2, which lie on divergent paths.
//
// naive     -> every required node collapses into one:  {A,B1,B2,C2}
// levelwise -> only nodes sharing a level are merged:    A / {B1,B2} / {C2}
//===--------------------------------------------------------------------===//
static void TestFigure7() {
	std::printf("Figure 7 -- root-to-leaf path generation\n");

	enum : AttributeId { A = 0, B1 = 1, B2 = 2, C1 = 3, C2 = 4, D = 5, E = 6, R = 7 };
	auto namer = NamerFor({{A, "A"}, {B1, "B1"}, {B2, "B2"}, {C1, "C1"}, {C2, "C2"}, {D, "D"}, {E, "E"}, {R, "R"}});

	auto make_original = []() {
		FTree tree = FTree::Scan({A});
		auto &b1 = tree.Root().AddChild(std::make_unique<FNode>(std::vector<AttributeId> {B1}));
		b1.AddChild(std::make_unique<FNode>(std::vector<AttributeId> {C1}));
		auto &c2 = b1.AddChild(std::make_unique<FNode>(std::vector<AttributeId> {C2}));
		c2.AddChild(std::make_unique<FNode>(std::vector<AttributeId> {E}));
		auto &b2 = tree.Root().AddChild(std::make_unique<FNode>(std::vector<AttributeId> {B2}));
		b2.AddChild(std::make_unique<FNode>(std::vector<AttributeId> {D}));
		return tree;
	};

	ExpectEq(make_original().ToString(namer), "{A}({B1}({C1},{C2}({E})),{B2}({D}))", "Figure 7 original tree");

	const std::vector<AttributeId> required_keys = {B2, C2};

	// The join is inserted as a child of the path's deepest node, which is what
	// CreateRootToLeafPath returns.
	auto naive = make_original();
	auto &naive_tip = naive.CreateRootToLeafPath(required_keys, PathStrategy::NAIVE);
	naive_tip.AddChild(std::make_unique<FNode>(std::vector<AttributeId> {R}));
	ExpectEq(naive.ToString(namer), "{A,B1,B2,C2}({C1},{D},{E},{R})", "Figure 7 naive strategy");

	auto levelwise = make_original();
	auto &levelwise_tip = levelwise.CreateRootToLeafPath(required_keys, PathStrategy::LEVELWISE);
	levelwise_tip.AddChild(std::make_unique<FNode>(std::vector<AttributeId> {R}));
	ExpectEq(levelwise.ToString(namer), "{A}({B1,B2}({C1},{C2}({E},{R}),{D}))", "Figure 7 levelwise strategy");

	// The paper's point: the levelwise strategy keeps A factorized, so it holds
	// strictly more nodes than the naive one.
	ExpectTrue(levelwise.NodeCount() > naive.NodeCount(), "levelwise preserves more factorization than naive");
}

//===--------------------------------------------------------------------===//
// Invariants that hold for every f-tree
//===--------------------------------------------------------------------===//
static void TestInvariants() {
	std::printf("Invariants\n");

	enum : AttributeId { A = 0, B = 1, C = 2 };
	auto namer = NamerFor({{A, "A"}, {B, "B"}, {C, "C"}});

	// Section 4.2.1: a table scan is a single node, i.e. the flat layout.
	auto scan = FTree::Scan({A, B, C});
	ExpectEq(scan.ToString(namer), "{A,B,C}", "a scan is one node holding every attribute");
	ExpectTrue(scan.Depth() == 1 && scan.NodeCount() == 1, "a scan is flat");

	// Attributes are deduplicated and ordered, so equal sets render equally.
	auto unordered = FTree::Scan({C, A, B, A});
	ExpectEq(unordered.ToString(namer), "{A,B,C}", "attribute sets are canonical");

	// A required set that already forms a path must not be restructured.
	FTree chain = FTree::Scan({A});
	auto &b = chain.Root().AddChild(std::make_unique<FNode>(std::vector<AttributeId> {B}));
	b.AddChild(std::make_unique<FNode>(std::vector<AttributeId> {C}));
	const auto before = chain.ToString(namer);
	auto &tip = chain.CreateRootToLeafPath({B}, PathStrategy::LEVELWISE);
	ExpectEq(chain.ToString(namer), before, "an existing root-to-leaf path is left alone");
	ExpectTrue(tip.HasAttribute(B), "the returned insertion point is the deepest required node");

	// Joining two flat scans is the base case every query starts from.
	auto left = FTree::Scan({A, B});
	auto right = FTree::Scan({B, C});
	JoinKeys keys;
	keys.build = {B};
	keys.probe = {B};
	auto joined = MergeTrees(left, right, keys, JoinMode::TOP_INSERT);
	ExpectEq(joined.ToString(namer), "{B,C}({A,B})", "top-insert of two scans nests build under probe");
}

int main() {
	std::printf("factorize core: f-tree golden tests\n\n");
	TestFigure6();
	std::printf("\n");
	TestFigure7();
	std::printf("\n");
	TestInvariants();
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
