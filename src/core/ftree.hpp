//===----------------------------------------------------------------------===//
//                         factorize
//
// core/ftree.hpp
//
// F-tree topology: the *shape* of a factorized representation.
//
// Implemented from Lehner & Neumann, "The Data World Is Not Flat" (PVLDB 19(11)),
// sections 2.1-2.2 (f-trees, path constraint), 4.2.3 (top- and bottom-insert
// joins) and 4.3 (root-to-leaf path generation).
//
// This header and its implementation must not depend on DuckDB (plan §4): the
// core is compiled and benchmarked standalone so the Phase 1 go/no-go
// measurement stays honest.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace factorize {

//! Identifies one attribute (column) within a query's factorized region.
using AttributeId = uint32_t;

//! Which input of a join becomes the upper part of the merged f-tree.
//!
//! Named for where the *probe* side lands (paper §4.2.3: "depending on the
//! insert position of the probe side"):
//!
//!   TOP_INSERT    -- probe tree on top; the build tree is inserted below it.
//!                    "we always place the build tree in top-inserts as a child
//!                     of the joined node from the probe side"
//!   BOTTOM_INSERT -- build tree on top; the probe tree is inserted below it.
//!                    "place the probe f-tree below the build side [...] the
//!                     build f-tree becomes the tree's upper part"
//!
//! Note this is the opposite of what DUCKDB_EXTENSION_PLAN.md §2.2 states; see
//! PLAN_ERRATA.md E1. The paper proves  L ⋈_TI R  ≡  R ⋈_BI L, so a top-insert
//! is a bottom-insert with the sides swapped. Having both decouples the f-tree's
//! shape from the choice of which side to build the hash table on, which is what
//! makes factorized joins freely reorderable.
enum class JoinMode : uint8_t { TOP_INSERT, BOTTOM_INSERT };

//! Strategy for §4.3's root-to-leaf path transformation.
enum class PathStrategy : uint8_t {
	//! Merge every required node into a single node. Simple, but it destroys the
	//! factorization of all of them.
	NAIVE,
	//! Merge only the required nodes that share a tree level, recursively. The
	//! paper's improved variant: 2.4% slower on geomean but far more robust,
	//! with >6.2x wins where it pays off (§6.2). Default.
	LEVELWISE
};

class FNode;
using FNodePtr = std::unique_ptr<FNode>;

//! Renders an attribute id for diagnostics and golden tests.
using AttributeNamer = std::function<std::string(AttributeId)>;

//! A node of an f-tree.
//!
//! A node holds a set of attributes. Its children denote *dependence* (the
//! tuples of a child are chosen relative to a fixed parent tuple); sibling
//! children denote *independence*, i.e. a Cartesian product (§2.2).
class FNode {
public:
	FNode() = default;
	explicit FNode(std::vector<AttributeId> attributes);

	const std::vector<AttributeId> &Attributes() const {
		return attributes;
	}
	const std::vector<FNodePtr> &Children() const {
		return children;
	}
	//! Mutable child list. Exposed because the section 4.3 transformations
	//! restructure a tree in place, moving whole subtrees between parents.
	std::vector<FNodePtr> &MutableChildren() {
		return children;
	}
	//! Bottom-inserts append to the child list of an already-materialized node
	//! from several threads at once, so only those nodes need synchronization
	//! (§5.2). Locking defensively everywhere would erase the parallel win.
	bool RequiresLock() const {
		return requires_lock;
	}
	void SetRequiresLock(bool value) {
		requires_lock = value;
	}

	bool HasAttribute(AttributeId attribute) const;
	void AddAttributes(const std::vector<AttributeId> &attrs);
	FNode &AddChild(FNodePtr child);

	//! Which nodes of the *input* tree this node draws its values from.
	//!
	//! Section 4.3's transformations merge nodes, so one output node can stand
	//! for several input nodes. Materializing it then means iterating the cross
	//! product of those inputs -- partial flattening, section 4.6. Recording
	//! provenance here is what lets that be planned once instead of rediscovered
	//! per record.
	const std::vector<uint32_t> &Sources() const {
		return sources;
	}
	void SetSources(std::vector<uint32_t> ids) {
		sources = std::move(ids);
	}
	void AddSources(const std::vector<uint32_t> &ids) {
		sources.insert(sources.end(), ids.begin(), ids.end());
	}

	//! Deep copy, including the requires_lock flags.
	FNodePtr Copy() const;

	//! Canonical rendering, e.g. "{A}({B},{C})". Children are emitted in a
	//! deterministic order so golden tests can compare strings.
	std::string ToString(const AttributeNamer &namer) const;

private:
	friend class FTree;

	//! Sorted and duplicate-free, so equal attribute sets render identically.
	std::vector<AttributeId> attributes;
	std::vector<FNodePtr> children;
	//! Input-tree node ids this node draws from; see Sources().
	std::vector<uint32_t> sources;
	bool requires_lock = false;
};

//! The shape of an f-representation: a single-rooted tree of attribute sets.
class FTree {
public:
	FTree() : root(std::make_unique<FNode>()) {
	}
	explicit FTree(FNodePtr root);

	FTree(const FTree &other);
	FTree &operator=(const FTree &other);
	FTree(FTree &&) = default;
	FTree &operator=(FTree &&) = default;

	FNode &Root() {
		return *root;
	}
	const FNode &Root() const {
		return *root;
	}

	//! A base table scan is a trivial f-representation: one node holding every
	//! attribute, which is exactly the flat layout (§4.2.1). This is what makes
	//! the flat/factorized boundary free.
	static FTree Scan(std::vector<AttributeId> attributes);

	//! Stamps every node with its own DFS index as its sole source, defining
	//! the identity that a later transformation reports provenance against.
	//! Call this on an input tree before merging it.
	void ResetSources();

	//! Locates the node holding an attribute, or nullptr.
	const FNode *Find(AttributeId attribute) const;
	FNode *Find(AttributeId attribute);

	//! Depth of the node holding an attribute (root == 0), or -1 if absent.
	//!
	//! Used to choose *where* to attach a join. Equality is transitive, so a
	//! predicate may be rewritten onto any attribute of the same equivalence
	//! class; picking the shallowest one makes the new subtree a sibling of the
	//! existing ones rather than nesting beneath them, which is the difference
	//! between a Cartesian product and a chain.
	int DepthOfAttribute(AttributeId attribute) const;

	//! Number of nodes.
	size_t NodeCount() const;
	//! Length of the longest root-to-leaf path, in nodes.
	size_t Depth() const;

	std::string ToString(const AttributeNamer &namer) const;

	//! Transforms the tree in place so that every node required by `keys` lies
	//! on one root-to-leaf path (§4.3), and returns the deepest such node --
	//! the point where a joined subtree is attached.
	//!
	//! A node is *required* if it holds one of `keys` or is an ancestor of a
	//! required node (§4.2.3). If the required nodes already form a path this is
	//! a no-op.
	FNode &CreateRootToLeafPath(const std::vector<AttributeId> &keys, PathStrategy strategy);

private:
	FNodePtr root;
};

//! The attributes a join predicate references, split by which input holds them.
struct JoinKeys {
	std::vector<AttributeId> build;
	std::vector<AttributeId> probe;
};

//! Merges two f-trees across an equi-join (§4.2.3).
//!
//! Both inputs are first given a root-to-leaf path over their own join keys --
//! the upper one so there is a single deepest node to attach to, the lower one
//! because a join across divergent paths makes those nodes interdependent. The
//! lower tree is then attached as a child of the upper tree's deepest required
//! node.
//!
//! `mode` selects which input becomes the upper tree; see JoinMode.
FTree MergeTrees(const FTree &build, const FTree &probe, const JoinKeys &keys, JoinMode mode,
                 PathStrategy strategy = PathStrategy::LEVELWISE);

//! Which input a merged output node draws from.
enum class MergeSide : uint8_t { UPPER, LOWER };

//! MergeTrees plus the provenance needed to actually materialize the result.
//!
//! Node ids in `sources` index the *input* tree in DFS order, which is also the
//! order Layout assigns LevelIds -- so a source id is directly a level of the
//! corresponding input's layout.
struct MergeInfo {
	FTree tree;
	//! Per output level, in the output tree's DFS order.
	std::vector<MergeSide> side;
	std::vector<std::vector<uint32_t>> sources;
	//! Output level where the lower tree's root was attached.
	uint32_t attach_level = 0;
	//! Output level of the upper tree's deepest required node, i.e. the parent
	//! of `attach_level`. The join key is readable here, and for a bottom-insert
	//! this is the record probes append to.
	uint32_t insertion_level = 0;
	//! Slot index of `attach_level` within `insertion_level`.
	uint32_t attach_slot = 0;
};

MergeInfo MergeTreesDetailed(const FTree &build, const FTree &probe, const JoinKeys &keys, JoinMode mode,
                             PathStrategy strategy = PathStrategy::LEVELWISE);

//! Names attributes 0..25 as A..Z (then A1, B1, ...) for tests and diagnostics.
std::string DefaultAttributeName(AttributeId attribute);

} // namespace factorize
