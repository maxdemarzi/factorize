#include "ftree.hpp"

#include <algorithm>
#include <cassert>
#include <unordered_set>

namespace factorize {

//===--------------------------------------------------------------------===//
// FNode
//===--------------------------------------------------------------------===//
FNode::FNode(std::vector<AttributeId> attributes_p) : attributes(std::move(attributes_p)) {
	std::sort(attributes.begin(), attributes.end());
	attributes.erase(std::unique(attributes.begin(), attributes.end()), attributes.end());
}

bool FNode::HasAttribute(AttributeId attribute) const {
	return std::binary_search(attributes.begin(), attributes.end(), attribute);
}

void FNode::AddAttributes(const std::vector<AttributeId> &attrs) {
	attributes.insert(attributes.end(), attrs.begin(), attrs.end());
	std::sort(attributes.begin(), attributes.end());
	attributes.erase(std::unique(attributes.begin(), attributes.end()), attributes.end());
}

FNode &FNode::AddChild(FNodePtr child) {
	children.push_back(std::move(child));
	return *children.back();
}

FNodePtr FNode::Copy() const {
	auto copy = std::make_unique<FNode>();
	copy->attributes = attributes;
	copy->sources = sources;
	copy->requires_lock = requires_lock;
	copy->children.reserve(children.size());
	for (auto &child : children) {
		copy->children.push_back(child->Copy());
	}
	return copy;
}

std::string FNode::ToString(const AttributeNamer &namer) const {
	std::string result = "{";
	for (size_t i = 0; i < attributes.size(); i++) {
		if (i > 0) {
			result += ",";
		}
		result += namer(attributes[i]);
	}
	result += "}";
	if (children.empty()) {
		return result;
	}
	// Siblings are a Cartesian product and therefore unordered; sort their
	// renderings so that the same shape always prints the same way.
	std::vector<std::string> rendered;
	rendered.reserve(children.size());
	for (auto &child : children) {
		rendered.push_back(child->ToString(namer));
	}
	std::sort(rendered.begin(), rendered.end());
	result += "(";
	for (size_t i = 0; i < rendered.size(); i++) {
		if (i > 0) {
			result += ",";
		}
		result += rendered[i];
	}
	result += ")";
	return result;
}

//===--------------------------------------------------------------------===//
// FTree
//===--------------------------------------------------------------------===//
FTree::FTree(FNodePtr root_p) : root(std::move(root_p)) {
	assert(root && "an f-tree always has a root");
}

FTree::FTree(const FTree &other) : root(other.root->Copy()) {
}

FTree &FTree::operator=(const FTree &other) {
	if (this != &other) {
		root = other.root->Copy();
	}
	return *this;
}

static void ResetSourcesIn(FNode &node, uint32_t &next) {
	node.SetSources({next++});
	for (auto &child : node.MutableChildren()) {
		ResetSourcesIn(*child, next);
	}
}

void FTree::ResetSources() {
	uint32_t next = 0;
	ResetSourcesIn(*root, next);
}

FTree FTree::Scan(std::vector<AttributeId> attributes) {
	return FTree(std::make_unique<FNode>(std::move(attributes)));
}

static const FNode *FindIn(const FNode &node, AttributeId attribute) {
	if (node.HasAttribute(attribute)) {
		return &node;
	}
	for (auto &child : node.Children()) {
		if (auto *found = FindIn(*child, attribute)) {
			return found;
		}
	}
	return nullptr;
}

const FNode *FTree::Find(AttributeId attribute) const {
	return FindIn(*root, attribute);
}

FNode *FTree::Find(AttributeId attribute) {
	return const_cast<FNode *>(FindIn(*root, attribute));
}

static int DepthOfAttributeIn(const FNode &node, AttributeId attribute, int depth) {
	if (node.HasAttribute(attribute)) {
		return depth;
	}
	for (auto &child : node.Children()) {
		const int found = DepthOfAttributeIn(*child, attribute, depth + 1);
		if (found >= 0) {
			return found;
		}
	}
	return -1;
}

int FTree::DepthOfAttribute(AttributeId attribute) const {
	return DepthOfAttributeIn(*root, attribute, 0);
}

static size_t CountNodes(const FNode &node) {
	size_t count = 1;
	for (auto &child : node.Children()) {
		count += CountNodes(*child);
	}
	return count;
}

size_t FTree::NodeCount() const {
	return CountNodes(*root);
}

static size_t DepthOf(const FNode &node) {
	size_t deepest = 0;
	for (auto &child : node.Children()) {
		deepest = std::max(deepest, DepthOf(*child));
	}
	return deepest + 1;
}

size_t FTree::Depth() const {
	return DepthOf(*root);
}

std::string FTree::ToString(const AttributeNamer &namer) const {
	return root->ToString(namer);
}

//===--------------------------------------------------------------------===//
// Root-to-leaf paths (paper section 4.3)
//===--------------------------------------------------------------------===//
namespace {

//! Marks a node required if it holds a join key or has a required descendant.
//! Section 4.2.3: "a node is required if it is either part of the join
//! condition or an ancestor of a required node".
bool MarkRequired(const FNode &node, const std::vector<AttributeId> &keys,
                  std::unordered_set<const FNode *> &required) {
	bool any = false;
	for (auto &child : node.Children()) {
		// Not short-circuiting: every required descendant must be marked.
		if (MarkRequired(*child, keys, required)) {
			any = true;
		}
	}
	if (!any) {
		for (auto key : keys) {
			if (node.HasAttribute(key)) {
				any = true;
				break;
			}
		}
	}
	if (any) {
		required.insert(&node);
	}
	return any;
}

//! The lowest common ancestor of the required set. Because that set is
//! ancestor-closed, the LCA is found by walking down for as long as exactly one
//! child is required: where two or more are, the paths diverge and the
//! transformation has to start.
FNode &FindLowestCommonAncestor(FNode &root, const std::unordered_set<const FNode *> &required) {
	FNode *current = &root;
	while (true) {
		FNode *only_required_child = nullptr;
		size_t required_children = 0;
		for (auto &child : current->Children()) {
			if (required.count(child.get()) > 0) {
				required_children++;
				only_required_child = child.get();
			}
		}
		if (required_children != 1) {
			// Either the required paths diverge here, or this is the deepest
			// required node and the path is already complete.
			return *current;
		}
		current = only_required_child;
	}
}

//! Section 4.3's recursive strategy: at each step, merge the LCA's *required*
//! children into one node -- taking their attributes and all of their children
//! -- while every other child is left untouched. The merged node becomes the
//! LCA of the next step, extending the required path one level at a time. Only
//! required nodes are flattened; the rest stay factorized.
FNode &MergeLevelwise(FNode &lca, const std::unordered_set<const FNode *> &required) {
	// Decide before moving anything: the loop below empties the child list, so
	// bailing out halfway would leave moved-from holes in the tree.
	bool merged_any = false;
	for (auto &child : lca.Children()) {
		if (required.count(child.get()) > 0) {
			merged_any = true;
			break;
		}
	}
	if (!merged_any) {
		// No required children: the required path already ends at the LCA.
		return lca;
	}

	auto merged = std::make_unique<FNode>();
	std::vector<FNodePtr> kept;
	for (auto &child : lca.MutableChildren()) {
		if (required.count(child.get()) > 0) {
			merged->AddAttributes(child->Attributes());
			merged->AddSources(child->Sources());
			merged->SetRequiresLock(merged->RequiresLock() || child->RequiresLock());
			for (auto &grandchild : child->MutableChildren()) {
				merged->MutableChildren().push_back(std::move(grandchild));
			}
		} else {
			kept.push_back(std::move(child));
		}
	}
	lca.MutableChildren() = std::move(kept);
	auto &attached = lca.AddChild(std::move(merged));
	return MergeLevelwise(attached, required);
}

//! Collapses every required node into a single node, discarding the mutual
//! factorization of all of them. Non-required subtrees are re-attached beneath
//! the result. Because the required set is ancestor-closed, its topmost member
//! is the tree root, so the merged node is the root.
FNode &MergeNaive(FNode &top, const std::unordered_set<const FNode *> &required) {
	std::vector<FNodePtr> kept;
	std::function<void(FNode &)> absorb = [&](FNode &node) {
		for (auto &child : node.MutableChildren()) {
			if (required.count(child.get()) > 0) {
				top.AddAttributes(child->Attributes());
				top.AddSources(child->Sources());
				top.SetRequiresLock(top.RequiresLock() || child->RequiresLock());
				absorb(*child);
			} else {
				kept.push_back(std::move(child));
			}
		}
		node.MutableChildren().clear();
	};
	absorb(top);
	top.MutableChildren() = std::move(kept);
	return top;
}

} // namespace

FNode &FTree::CreateRootToLeafPath(const std::vector<AttributeId> &keys, PathStrategy strategy) {
	std::unordered_set<const FNode *> required;
	MarkRequired(*root, keys, required);
	if (required.empty()) {
		// None of this join's keys live in this tree, so nothing constrains its
		// shape. Attaching at the root is the least-flattening choice.
		return *root;
	}
	if (strategy == PathStrategy::NAIVE) {
		return MergeNaive(*root, required);
	}
	auto &lca = FindLowestCommonAncestor(*root, required);
	return MergeLevelwise(lca, required);
}

//===--------------------------------------------------------------------===//
// Joins (paper section 4.2.3)
//===--------------------------------------------------------------------===//
FTree MergeTrees(const FTree &build, const FTree &probe, const JoinKeys &keys, JoinMode mode, PathStrategy strategy) {
	// Top-insert puts the probe tree on top; bottom-insert puts the build tree
	// on top. Nothing else differs between the two -- which is exactly the
	// symmetry behind  L (top-insert) R  ==  R (bottom-insert) L.
	const bool build_on_top = (mode == JoinMode::BOTTOM_INSERT);
	const FTree &upper_in = build_on_top ? build : probe;
	const FTree &lower_in = build_on_top ? probe : build;
	const std::vector<AttributeId> &upper_keys = build_on_top ? keys.build : keys.probe;
	const std::vector<AttributeId> &lower_keys = build_on_top ? keys.probe : keys.build;

	FTree result(upper_in);
	auto &insertion_point = result.CreateRootToLeafPath(upper_keys, strategy);

	// The lower tree needs a path of its own: joining across two of its
	// divergent branches makes those nodes interdependent (section 4.2.3).
	FTree lower(lower_in);
	lower.CreateRootToLeafPath(lower_keys, strategy);

	auto &attached = insertion_point.AddChild(lower.Root().Copy());
	if (mode == JoinMode::BOTTOM_INSERT) {
		// In a bottom-insert one upper (build) tuple collects probe matches
		// produced by many threads, so this insertion point is contended.
		// Top-inserts find all matches for a probe tuple in a single chain
		// traversal and need no lock here (section 5.2).
		attached.SetRequiresLock(true);
	}
	return result;
}

MergeInfo MergeTreesDetailed(const FTree &build, const FTree &probe, const JoinKeys &keys, JoinMode mode,
                             PathStrategy strategy) {
	const bool build_on_top = (mode == JoinMode::BOTTOM_INSERT);
	const FTree &upper_in = build_on_top ? build : probe;
	const FTree &lower_in = build_on_top ? probe : build;
	const std::vector<AttributeId> &upper_keys = build_on_top ? keys.build : keys.probe;
	const std::vector<AttributeId> &lower_keys = build_on_top ? keys.probe : keys.build;

	MergeInfo info;

	// Stamp both inputs so the transformation reports provenance against their
	// own DFS order, which is exactly the order Layout assigns LevelIds.
	FTree result(upper_in);
	result.ResetSources();
	auto &insertion_point = result.CreateRootToLeafPath(upper_keys, strategy);

	FTree lower(lower_in);
	lower.ResetSources();
	lower.CreateRootToLeafPath(lower_keys, strategy);

	auto &attached = insertion_point.AddChild(lower.Root().Copy());
	if (mode == JoinMode::BOTTOM_INSERT) {
		attached.SetRequiresLock(true);
	}

	// Walk the finished tree once in DFS order -- the same order Layout uses --
	// recording which side each level came from and where the seam is.
	const FNode *insertion_node = &insertion_point;
	const FNode *attached_node = &attached;
	uint32_t next = 0;
	std::function<void(const FNode &, bool)> visit = [&](const FNode &node, bool in_lower) {
		const uint32_t level = next++;
		const bool lower_here = in_lower || &node == attached_node;
		info.side.push_back(lower_here ? MergeSide::LOWER : MergeSide::UPPER);
		info.sources.push_back(node.Sources());
		if (&node == attached_node) {
			info.attach_level = level;
		}
		if (&node == insertion_node) {
			info.insertion_level = level;
			for (size_t i = 0; i < node.Children().size(); i++) {
				if (node.Children()[i].get() == attached_node) {
					info.attach_slot = static_cast<uint32_t>(i);
				}
			}
		}
		for (auto &child : node.Children()) {
			visit(*child, lower_here);
		}
	};
	visit(result.Root(), false);

	info.tree = std::move(result);
	return info;
}

//===--------------------------------------------------------------------===//
// Diagnostics
//===--------------------------------------------------------------------===//
std::string DefaultAttributeName(AttributeId attribute) {
	std::string name(1, static_cast<char>('A' + (attribute % 26)));
	if (attribute >= 26) {
		name += std::to_string(attribute / 26);
	}
	return name;
}

} // namespace factorize
