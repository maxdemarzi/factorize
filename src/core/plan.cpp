#include "plan.hpp"

#include <limits>
#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

namespace factorize {

namespace {

//! Attribute ids are assigned by walking relations in order, so a relation's
//! columns occupy a contiguous range. Callers only ever see the composed id.
size_t AttributeBase(const QueryGraph &graph, size_t relation) {
	size_t base = 0;
	for (size_t i = 0; i < relation && i < graph.column_counts.size(); i++) {
		base += graph.column_counts[i];
	}
	return base;
}

//! Every attribute of one relation, in column order -- the node a flat scan of
//! it becomes (FTree::Scan builds a single node holding all of them).
std::vector<AttributeId> AttributesOf(const QueryGraph &graph, size_t relation) {
	std::vector<AttributeId> attributes;
	const auto count = relation < graph.column_counts.size() ? graph.column_counts[relation] : 0;
	attributes.reserve(count);
	for (size_t column = 0; column < count; column++) {
		attributes.push_back(AttributeOf(graph, relation, column));
	}
	return attributes;
}

//! The storage width of one attribute, found by walking relations in order --
//! the inverse of AttributeOf, needed because the join measures key widths over
//! *attributes* and equality propagation can hand back an attribute belonging to
//! a different relation than the predicate named.
ValueType TypeOfAttribute(const QueryGraph &graph, AttributeId attribute) {
	size_t base = 0;
	for (size_t relation = 0; relation < graph.column_counts.size(); relation++) {
		const auto count = graph.column_counts[relation];
		if (attribute < base + count) {
			const size_t column = static_cast<size_t>(attribute) - base;
			if (relation < graph.column_types.size() && column < graph.column_types[relation].size()) {
				return graph.column_types[relation][column];
			}
			break;
		}
		base += count;
	}
	// Unknown width is charged as the wider of the two, so an attribute the
	// graph does not describe cannot smuggle a key past the cap.
	return ValueType::INT64;
}

size_t TotalAttributes(const QueryGraph &graph) {
	size_t total = 0;
	for (size_t count : graph.column_counts) {
		total += count;
	}
	return total;
}

} // namespace

AttributeId AttributeOf(const QueryGraph &graph, size_t relation, size_t column) {
	return static_cast<AttributeId>(AttributeBase(graph, relation) + column);
}

EquivalenceClasses::EquivalenceClasses(const QueryGraph &graph) {
	parent.resize(TotalAttributes(graph));
	for (size_t i = 0; i < parent.size(); i++) {
		parent[i] = i;
	}
	for (const auto &predicate : graph.predicates) {
		size_t a = Find(AttributeOf(graph, predicate.left_relation, static_cast<size_t>(predicate.left_column)));
		size_t b = Find(AttributeOf(graph, predicate.right_relation, static_cast<size_t>(predicate.right_column)));
		if (a != b) {
			parent[a] = b;
		}
	}
}

EquivalenceClasses::EquivalenceClasses(const QueryGraph &graph, Enforced) {
	parent.resize(TotalAttributes(graph));
	for (size_t i = 0; i < parent.size(); i++) {
		parent[i] = i;
	}
}

void EquivalenceClasses::Enforce(const QueryGraph &graph, const Predicate &predicate) {
	const size_t a = Find(AttributeOf(graph, predicate.left_relation, static_cast<size_t>(predicate.left_column)));
	const size_t b = Find(AttributeOf(graph, predicate.right_relation, static_cast<size_t>(predicate.right_column)));
	if (a != b) {
		parent[a] = b;
	}
}

size_t EquivalenceClasses::Find(size_t attribute) {
	while (parent[attribute] != attribute) {
		parent[attribute] = parent[parent[attribute]];
		attribute = parent[attribute];
	}
	return attribute;
}

bool EquivalenceClasses::SameClass(AttributeId a, AttributeId b) {
	return Find(a) == Find(b);
}

AttributeId ShallowestEquivalent(EquivalenceClasses &classes, const FTree &tree, AttributeId key) {
	AttributeId best = key;
	int best_depth = tree.DepthOfAttribute(key);
	for (size_t candidate = 0; candidate < classes.Size(); candidate++) {
		const auto attribute = static_cast<AttributeId>(candidate);
		if (attribute == key || !classes.SameClass(attribute, key)) {
			continue;
		}
		const int depth = tree.DepthOfAttribute(attribute);
		if (depth < 0) {
			continue; // not joined yet
		}
		if (best_depth < 0 || depth < best_depth) {
			best = attribute;
			best_depth = depth;
		}
	}
	return best;
}

//! Every f-tree/materialize traversal downstream of a plan (ftree.cpp,
//! materialize.cpp) is plain C++ recursion, one stack frame per tree level,
//! with no depth cap of its own -- a stack overflow crashes the whole process,
//! not just the offending query, which is a worse failure mode than any
//! std::runtime_error this codebase otherwise uses for bad input. Tree depth
//! is bounded by the number of relations in the worst case (a pure chain, no
//! equality propagation flattening it into a shallower star), so bounding
//! relation count here protects every downstream traversal at the one place
//! every caller's query already passes through, without touching each
//! traversal individually. 500 is generous relative to any real workload
//! (FINDINGS' own bytes-per-record table tops out at 12 relations) and
//! conservative relative to a 1MB thread stack even allowing several stacked
//! recursive calls per tree level.
static constexpr size_t kMaxRelations = 500;

bool IsAcyclic(const QueryGraph &graph) {
	if (graph.predicates.empty()) {
		return true;
	}
	// A predicate naming a relation the graph does not have would index past
	// `edges`. BuildPlan reports that shape as an error rather than trusting
	// it, and this is a public entry point, so the malformed graph answers
	// "not provably acyclic" -- the direction that declines and leaves the
	// stock plan to it, rather than crashing or guessing.
	for (const auto &predicate : graph.predicates) {
		if (predicate.left_relation >= graph.RelationCount() || predicate.right_relation >= graph.RelationCount()) {
			return false;
		}
	}
	EquivalenceClasses classes(graph);
	// Hyperedges over join variables only. A column no predicate touches sits
	// in exactly one hyperedge, so GYO's first rule would strip it anyway.
	std::vector<std::set<size_t>> edges(graph.RelationCount());
	for (const auto &predicate : graph.predicates) {
		edges[predicate.left_relation].insert(
		    classes.Find(AttributeOf(graph, predicate.left_relation, static_cast<size_t>(predicate.left_column))));
		edges[predicate.right_relation].insert(
		    classes.Find(AttributeOf(graph, predicate.right_relation, static_cast<size_t>(predicate.right_column))));
	}
	std::vector<std::set<size_t>> live;
	for (auto &edge : edges) {
		if (!edge.empty()) {
			live.push_back(std::move(edge));
		}
	}
	for (bool changed = true; changed && !live.empty();) {
		changed = false;
		std::map<size_t, size_t> occurrences;
		for (const auto &edge : live) {
			for (const auto vertex : edge) {
				occurrences[vertex]++;
			}
		}
		for (auto &edge : live) {
			for (auto it = edge.begin(); it != edge.end();) {
				if (occurrences[*it] == 1) {
					it = edge.erase(it);
					changed = true;
				} else {
					++it;
				}
			}
		}
		live.erase(std::remove_if(live.begin(), live.end(), [](const std::set<size_t> &edge) { return edge.empty(); }),
		           live.end());
		for (size_t i = 0; i < live.size(); i++) {
			bool contained = false;
			for (size_t j = 0; j < live.size() && !contained; j++) {
				contained = i != j && std::includes(live[j].begin(), live[j].end(), live[i].begin(), live[i].end());
			}
			if (contained) {
				live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
				changed = true;
				break;
			}
		}
	}
	return live.empty();
}

namespace {

//! splitmix64's finaliser. The keys are join keys: dense integers, often
//! consecutive, and the identity hash libstdc++ uses for `long` leaves linear
//! probing to deal with that clustering.
//!
//! A free function rather than a member because `ShardedKeySet` has to hash
//! before it knows which shard to ask, and hashing twice would undo the point
//! of sharding.
inline uint64_t Mix(int64_t key) {
	uint64_t x = static_cast<uint64_t>(key) + 0x9E3779B97F4A7C15ull;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
	return x ^ (x >> 31);
}

//! A set of int64 keys that allocates in blocks rather than per key.
//!
//! `std::unordered_set<int64_t>` was the obvious first choice and callgrind
//! priced it: on `job_acyclic_109_08`, 43.2% of the whole process's
//! instructions were in the allocator -- `_int_malloc`, `_int_free`,
//! `malloc_consolidate`, `unlink_chunk` -- against 17.3% in the reduction
//! itself, plus 3.8% more in `_M_rehash` and its prime-policy arithmetic. A
//! node-based set allocates once per distinct key and this reads millions of
//! them.
//!
//! Open addressing with linear probing, power-of-two capacity, and growth by
//! doubling: the table is one allocation however many keys arrive, and
//! rehashing is a linear scan rather than a walk of a chain of nodes.
//!
//! One array, not two. The first version kept the keys and an occupancy byte
//! side by side, which costs two cache misses a probe instead of one, on two
//! lines that are never near each other. A sampling profile of the reduction
//! put 34% of the function on the two loads that miss (D117). An empty slot
//! is a sentinel value instead, and the one key the sentinel cannot represent
//! is held in a flag beside the table.
class KeySet {
public:
	//! Insert a run of keys, which is how every caller actually arrives.
	//!
	//! One key at a time was a call per key once `Insert` had several callers
	//! and stopped being inlined, and the call is not the cost: outside the
	//! function the compiler kept `capacity_`, `count_` and the slot array in
	//! registers across the whole row loop, and across a call it cannot. A
	//! batch puts the loop back inside the table, and the fields are read
	//! once per run rather than once per key (D118).
	void InsertAll(const int64_t *keys, size_t count) {
		size_t at = 0;
		while (at < count) {
			// How many more keys can land before the next grow. Duplicates
			// do not consume room, so this is a floor, not a count, and the
			// outer loop simply comes round again.
			const size_t limit = capacity_ / 5 * 3;
			if (count_ + 1 >= limit) {
				Grow();
				continue;
			}
			const size_t stop = std::min(count, at + (limit - count_ - 1));
			int64_t *slots = slots_.data();
			const size_t mask = capacity_ - 1;
			size_t live = count_;
			bool empty_key = has_empty_key_;
			for (; at < stop; at++) {
				const int64_t key = keys[at];
				if (key == EMPTY) {
					empty_key = true;
					continue;
				}
				size_t slot = Mix(key) & mask;
				while (slots[slot] != EMPTY) {
					if (slots[slot] == key) {
						break;
					}
					slot = (slot + 1) & mask;
				}
				if (slots[slot] == EMPTY) {
					slots[slot] = key;
					live++;
				}
			}
			count_ = live;
			has_empty_key_ = empty_key;
		}
	}

	bool Contains(int64_t key, uint64_t hash) const {
		if (key == EMPTY) {
			return has_empty_key_;
		}
		if (count_ == 0) {
			return false;
		}
		size_t slot = hash & (capacity_ - 1);
		for (;;) {
			const int64_t occupant = slots_[slot];
			if (occupant == key) {
				return true;
			}
			if (occupant == EMPTY) {
				return false;
			}
			slot = (slot + 1) & (capacity_ - 1);
		}
	}

	bool Empty() const {
		return count_ == 0 && !has_empty_key_;
	}

private:
	//! The value an unoccupied slot holds. A join key equal to it is real and
	//! must still be answered for, so it is kept out of the table entirely.
	static constexpr int64_t EMPTY = std::numeric_limits<int64_t>::min();

	//! Three fifths, not seven tenths. Linear probing walks
	//! `(1 + 1/(1-load)^2)/2` slots on average to find a free one, which is
	//! 6.0 at 0.7 and 3.6 at 0.6; the profile found the collision step hot
	//! enough to be worth the fifth of a byte a key this costs (D117).
	void Grow() {
		const size_t wanted = capacity_ ? capacity_ * 2 : 1024;
		// Read into a local first: passing the static constexpr member
		// straight to the vector binds a reference to it, which odr-uses it,
		// and the standalone core still builds at C++14 where that wants an
		// out-of-line definition (scripts/core-test.sh).
		const int64_t empty = EMPTY;
		std::vector<int64_t> slots(wanted, empty);
		for (size_t i = 0; i < capacity_; i++) {
			const int64_t key = slots_[i];
			if (key == EMPTY) {
				continue;
			}
			size_t slot = Mix(key) & (wanted - 1);
			while (slots[slot] != EMPTY) {
				slot = (slot + 1) & (wanted - 1);
			}
			slots[slot] = key;
		}
		slots_.swap(slots);
		capacity_ = wanted;
	}

	std::vector<int64_t> slots_;
	size_t capacity_ = 0;
	size_t count_ = 0;
	bool has_empty_key_ = false;
	//! To exactly 64 bytes, which is a cache line.
	//!
	//! Not for the alignment -- nothing here promises the array starts on a
	//! line -- but for the stride. `count_` moves on every insert, and at any
	//! size under 64 two shards' counters share a line and the threads
	//! filling them fight over it. At exactly 64 the same field in two
	//! shards is 64 bytes apart wherever the array begins, so it never can.
	char padding_[23] = {};
};

//! The padding above is load-bearing and silent, so it is checked.
static_assert(sizeof(KeySet) == 64, "a shard has to be exactly one cache line wide");

//! Keys on their way to one shard, held in blocks so that growing never
//! copies what is already there.
//!
//! This was a `std::vector<int64_t>` and `push_back` reallocating it was 2.2%
//! of the whole run -- `_M_realloc_insert` under the probing round, visible
//! in the profile as its own frame (D121). Reserving instead would mean
//! reserving for the case where every row survives, which on a selective
//! join is a hundred times the memory actually used. Blocks cost one
//! allocation per 4096 keys and copy nothing.
class KeyBlocks {
public:
	void Add(int64_t key) {
		if (used_ == BLOCK) {
			// Through a local, because emplace_back binds a reference to its
			// argument and odr-using a static constexpr member wants an
			// out-of-line definition at C++14, which the standalone core
			// still builds at (scripts/core-test.sh).
			const size_t block = BLOCK;
			blocks_.emplace_back(block);
			used_ = 0;
		}
		blocks_.back()[used_++] = key;
	}

	size_t Blocks() const {
		return blocks_.size();
	}

	const int64_t *Block(size_t i) const {
		return blocks_[i].data();
	}

	//! Every block is full but the last, which holds what the last Add left.
	size_t BlockSize(size_t i) const {
		return i + 1 == blocks_.size() ? used_ : BLOCK;
	}

private:
	static constexpr size_t BLOCK = 4096;
	std::vector<std::vector<int64_t>> blocks_;
	//! Starts full, so the first Add is what allocates: a bucket that never
	//! receives a key never allocates at all, and there are workers squared
	//! of them per relation.
	size_t used_ = BLOCK;
};

//! The surviving keys of one relation, split into independent tables.
//!
//! A shard is chosen by the top byte of the hash and a slot by its low bits,
//! so the two are independent and a shard's occupancy does not depend on
//! which shard it is. With one shard this is the plain table above and costs
//! one extra indexed load; with N it is N tables that no two threads ever
//! write at once, which is what lets the reduction run on more than one core
//! without a lock (D118).
class ShardedKeySet {
public:
	void Reset(size_t shards) {
		split_.clear();
		if (shards > 1) {
			split_.resize(shards);
		}
		mask_ = shards - 1;
	}

	//! Which shard a hash belongs to. Shifting by a constant 56 rather than
	//! by `64 - log2(shards)` keeps one shard from shifting by 64, which is
	//! undefined, and 256 shards is more than any thread count here.
	size_t ShardOf(uint64_t hash) const {
		return static_cast<size_t>(hash >> 56) & mask_;
	}

	//! The shard array, as one pointer and one mask.
	//!
	//! The row loop resolves these once per relation rather than asking the
	//! set per row. Reaching the table through the set costs a second load,
	//! and asking whether it is split costs a third on a line the table
	//! itself is not on; both measured, together, at 0.87x on the corpus.
	//! This is D113's lesson a second time: what the reduction cannot afford
	//! is indirection inside the row loop.
	const KeySet *Tables() const {
		return split_.empty() ? &whole_ : split_.data();
	}

	KeySet *Writable() {
		return split_.empty() ? &whole_ : split_.data();
	}

	size_t Mask() const {
		return mask_;
	}

	//! Fill one named shard from a run of keys. The caller has already
	//! decided, by hash, that they all belong here; no other thread may be
	//! inside this shard.
	void FillShard(size_t shard, const KeyBlocks &keys) {
		for (size_t i = 0; i < keys.Blocks(); i++) {
			Writable()[shard].InsertAll(keys.Block(i), keys.BlockSize(i));
		}
	}

	bool Empty() const {
		if (split_.empty()) {
			return whole_.Empty();
		}
		for (const auto &shard : split_) {
			if (!shard.Empty()) {
				return false;
			}
		}
		return true;
	}

private:
	//! The whole set when there is one shard, and unused when there are
	//! several; `split_` being empty is what says which.
	KeySet whole_;
	std::vector<KeySet> split_;
	size_t mask_ = 0;
};

//! Run `body(0..workers-1)` on `workers` threads and wait for all of them.
//!
//! The calling thread takes slot 0 rather than sitting idle, so a run of two
//! workers spawns one thread. A worker that throws sets `failed` instead of
//! unwinding out of a thread, which would terminate the process: the only
//! throw reachable from the bodies here is a failed allocation, and the
//! reduction reports that as an error like any other.
bool RunInParallel(size_t workers, const std::function<void(size_t)> &body) {
	std::atomic<bool> failed(false);
	auto guarded = [&body, &failed](size_t worker) {
		try {
			body(worker);
		} catch (...) {
			failed.store(true);
		}
	};
	std::vector<std::thread> pool;
	pool.reserve(workers - 1);
	for (size_t worker = 1; worker < workers; worker++) {
		pool.emplace_back(guarded, worker);
	}
	guarded(0);
	for (auto &thread : pool) {
		thread.join();
	}
	return !failed.load();
}

//! Below this many rows a relation is reduced on one thread. Two rounds of
//! thread creation cost tens of microseconds and the corpus is mostly small
//! relations, so the floor is what keeps the parallel path from being a loss
//! on the queries it cannot help.
constexpr size_t PARALLEL_ROW_FLOOR = 1u << 16;

//! How many shards a thread budget turns into: a power of two, because a
//! shard is chosen by masking, and at most 64, because `ShardOf` reads the
//! top byte of the hash and 64 is already more threads than this reduction
//! has ever been given.
size_t ParallelWorkers(size_t threads) {
	size_t workers = 1;
	while (workers * 2 <= threads && workers < 64) {
		workers *= 2;
	}
	return workers;
}

//! One non-root node of the reduction tree: who it hangs from, and on which
//! columns. `own_column` is the key its parent probes it by.
struct ReductionNode {
	size_t parent = 0;
	size_t own_column = 0;
	size_t parent_column = 0;
};

} // namespace

bool ReducibleByExtremeReduction(const QueryGraph &graph) {
	return graph.predicates.size() + 1 == graph.RelationCount() && IsAcyclic(graph);
}

ExtremeResult ExecuteExtremeReduced(const QueryGraph &graph, RelationSource &source, size_t relation, size_t column,
                                    bool want_min, size_t threads) {
	ExtremeResult result;
	const size_t relations = graph.RelationCount();
	if (relation >= relations || column >= graph.column_counts[relation]) {
		result.error = "aggregated column is not a column of the query";
		return result;
	}
	if (!ReducibleByExtremeReduction(graph)) {
		result.error = graph.predicates.size() + 1 != relations
		                   ? "the predicates are not a spanning tree, so the reduction is not known to be sound"
		                   : "cyclic join graph";
		return result;
	}

	// Root at the relation holding the aggregated column and walk outwards.
	std::vector<ReductionNode> node(relations);
	std::vector<std::vector<size_t>> children(relations);
	std::vector<bool> placed(relations, false);
	std::vector<size_t> order;
	order.reserve(relations);
	order.push_back(relation);
	placed[relation] = true;
	std::vector<bool> used(graph.predicates.size(), false);
	for (size_t head = 0; head < order.size(); head++) {
		const size_t parent = order[head];
		for (size_t i = 0; i < graph.predicates.size(); i++) {
			if (used[i]) {
				continue;
			}
			const auto &predicate = graph.predicates[i];
			size_t child = 0;
			size_t child_column = 0;
			size_t parent_column = 0;
			if (predicate.left_relation == parent && !placed[predicate.right_relation]) {
				child = predicate.right_relation;
				child_column = static_cast<size_t>(predicate.right_column);
				parent_column = static_cast<size_t>(predicate.left_column);
			} else if (predicate.right_relation == parent && !placed[predicate.left_relation]) {
				child = predicate.left_relation;
				child_column = static_cast<size_t>(predicate.left_column);
				parent_column = static_cast<size_t>(predicate.right_column);
			} else {
				continue;
			}
			node[child] = ReductionNode {parent, child_column, parent_column};
			children[parent].push_back(child);
			placed[child] = true;
			used[i] = true;
			order.push_back(child);
		}
	}
	if (order.size() != relations) {
		result.error = "join graph is disconnected";
		return result;
	}

	// Children before parents, so a relation is reduced only once everything
	// below it has been. One relation is read at a time, because
	// RelationSource only guarantees its columns until the next one is asked
	// for -- the surviving keys are copied out before moving on.
	std::vector<ShardedKeySet> surviving(relations);
	bool extreme_set = false;
	for (size_t i = order.size(); i-- > 0;) {
		const size_t current = order[i];
		const auto &columns = source.Columns(current);
		const size_t rows = columns.empty() ? 0 : columns[0].size();
		result.rows_scanned += rows;
		// Resolved once, not per row. `columns[probe][row]` walks a vector of
		// vectors on every probe of every row, and the child loop between the
		// two indexings stops the compiler hoisting it. The profile put 79
		// instructions on a row that needs a hash and a lookup; this is where
		// a good part of the difference was (D113).
		struct Probe {
			const int64_t *column;
			const KeySet *tables;
			size_t mask;
		};
		std::vector<Probe> probes;
		probes.reserve(children[current].size());
		for (const size_t child : children[current]) {
			const ShardedKeySet &child_keys = surviving[child];
			probes.push_back(Probe {columns[node[child].parent_column].data(), child_keys.Tables(), child_keys.Mask()});
		}
		const int64_t *extreme_column = current == relation ? columns[column].data() : nullptr;
		const int64_t *key_column = current == relation ? nullptr : columns[node[current].own_column].data();

		const size_t workers = rows >= PARALLEL_ROW_FLOOR ? ParallelWorkers(threads) : 1;
		ShardedKeySet keys;
		keys.Reset(workers);

		auto survives = [&probes](size_t row) {
			for (const auto &probe : probes) {
				const int64_t key = probe.column[row];
				const uint64_t hash = Mix(key);
				if (!probe.tables[static_cast<size_t>(hash >> 56) & probe.mask].Contains(key, hash)) {
					return false;
				}
			}
			return true;
		};

		// Two rounds over the same rows, on one thread or on many; the one
		// thread case is the same code with one worker, because two code
		// paths over a reduction that has to be exactly right is one too
		// many.
		//
		// The first round probes, which only reads the children's tables and
		// so shares nothing, and sorts the keys it keeps into one bucket per
		// shard. The second gives each shard to one worker, which drains
		// every bucket holding keys for it: a shard has exactly one writer
		// and there is no lock anywhere.
		//
		// Buckets hold the key rather than the row, so the second round
		// reads what the first round wrote instead of walking the key column
		// again at random.
		std::vector<std::vector<KeyBlocks>> buckets(workers);
		std::vector<int64_t> best(workers, 0);
		std::vector<char> best_set(workers, 0);
		const size_t span = (rows + workers - 1) / workers;
		const bool probed = RunInParallel(workers, [&](size_t worker) {
			const size_t from = worker * span;
			const size_t to = std::min(rows, from + span);
			if (!extreme_column) {
				buckets[worker].resize(workers);
			}
			for (size_t row = from; row < to; row++) {
				if (!survives(row)) {
					continue;
				}
				if (extreme_column) {
					const int64_t value = extreme_column[row];
					if (!best_set[worker] || (want_min ? value < best[worker] : value > best[worker])) {
						best[worker] = value;
						best_set[worker] = 1;
					}
				} else {
					const int64_t key = key_column[row];
					buckets[worker][keys.ShardOf(Mix(key))].Add(key);
				}
			}
		});
		if (!probed) {
			result.error = "the reduction ran out of memory";
			return result;
		}
		if (extreme_column) {
			for (size_t worker = 0; worker < workers; worker++) {
				if (!best_set[worker]) {
					continue;
				}
				if (!extreme_set || (want_min ? best[worker] < result.value : best[worker] > result.value)) {
					result.value = best[worker];
					extreme_set = true;
				}
			}
		} else if (!RunInParallel(workers, [&](size_t shard) {
			           for (size_t worker = 0; worker < workers; worker++) {
				           keys.FillShard(shard, buckets[worker][shard]);
			           }
		           })) {
			result.error = "the reduction ran out of memory";
			return result;
		}

		if (current != relation) {
			// An empty relation empties the whole join, and continuing would
			// read every remaining input to discover it.
			if (keys.Empty()) {
				result.ok = true;
				return result;
			}
			surviving[current] = std::move(keys);
		}
	}
	result.ok = true;
	result.any = extreme_set;
	return result;
}

Plan BuildPlan(const QueryGraph &graph) {
	if (graph.RelationCount() > kMaxRelations) {
		Plan plan;
		plan.reason = "too many relations (" + std::to_string(graph.RelationCount()) + " > " +
		              std::to_string(kMaxRelations) + "); f-tree traversals recurse per level with no depth cap";
		return plan;
	}
	Plan plan;
	std::set<size_t> joined;
	std::vector<bool> used(graph.predicates.size(), false);
	// Only equalities a join has already applied. See EquivalenceClasses.
	EquivalenceClasses classes(graph, Enforced {});

	// A relation can only attach where every edge carrying it into the joined set
	// reaches attributes that end up on *one* f-tree level, and section 4.3
	// guarantees that happens exactly when the already-joined side of each of
	// those edges shares an equivalence class. A triangle is the smallest graph
	// that fails: its third relation reaches the other two through two different
	// classes at once.
	//
	// This is checked here rather than left to the engine because the engine's
	// answer is an exception thrown partway through a running query
	// ("key attributes did not converge on one level"). Planning is where a
	// caller can still do something about it -- the table function reports which
	// query it cannot run, and the optimizer rule quietly leaves the stock plan
	// alone.
	// The tree the plan will actually build, simulated as it is planned.
	//
	// An FTree is pure structure over attribute ids -- no data -- so the planner
	// can construct the very tree the engine will, and ask it the real question
	// instead of a proxy for it. That matters because the proxy was wrong in one
	// direction: sharing an equivalence class implies landing on one node, but
	// not the reverse, and every composite-key join lives in the gap. `A.k1 =
	// B.k1 AND A.k2 = B.k2` is what every composite foreign key writes, and it
	// was refused project-wide as "cyclic" -- which it is not.
	FTree simulated =
	    FTree::Scan(AttributesOf(graph, graph.predicates.empty() ? 0 : graph.predicates[0].left_relation));

	// The accumulated side's key attributes, as the join will name them: mapped
	// through equality propagation against the tree as it stands.
	auto accumulated_keys_for = [&](const std::vector<Predicate> &edges, size_t attaching) {
		std::vector<AttributeId> keys;
		for (const auto &edge : edges) {
			const bool left_is_new = edge.left_relation == attaching;
			const auto raw = AttributeOf(graph, left_is_new ? edge.right_relation : edge.left_relation,
			                             static_cast<size_t>(left_is_new ? edge.right_column : edge.left_column));
			keys.push_back(ShallowestEquivalent(classes, simulated, raw));
		}
		return keys;
	};

	// MakeKeyReader requires every key to be readable from ONE node, and throws
	// "key attributes did not converge on one level" otherwise -- mid-query,
	// where a caller can do nothing about it. So the same question is asked
	// here, by performing the transformation the join performs and looking at
	// where the keys ended up.
	//
	// LEVELWISE rather than NAIVE deliberately: NAIVE collapses every required
	// node into the root, so its keys always converge, which makes LEVELWISE the
	// stricter of the two and a plan accepted here safe under either.
	auto converges = [&](const std::vector<Predicate> &edges, size_t attaching) {
		const auto keys = accumulated_keys_for(edges, attaching);
		if (keys.size() < 2) {
			return true;
		}
		FTree probe_shape(simulated);
		const FNode &insertion = probe_shape.CreateRootToLeafPath(keys, PathStrategy::LEVELWISE);
		for (auto key : keys) {
			if (!insertion.HasAttribute(key)) {
				return false;
			}
		}
		return true;
	};

	// The packed composite key is one 64-bit word, so a key set wider than that
	// cannot be built at all (join.cpp: "composite key wider than 64 bits").
	// Refused here rather than thrown there, for the same reason as above.
	auto fits_key_width = [&](const std::vector<Predicate> &edges, size_t attaching) {
		// MakeKeyReader is built once per side and caps each at 64 bits, so both
		// sides are measured: a key can be INT32 on one relation and INT64 on
		// the other, and only the wider side would fail.
		//
		// The rule is the *sum* of the key columns' widths, not a count of them
		// and not "no INT64": one INT64 key fits at exactly 64, and INT32+INT64
		// is 96 and does not. That pairing -- a small discriminator beside a
		// bigint id -- is the common composite key in real schemas, so a rule
		// written as "not two INT64s" would let precisely the frequent case
		// through to the mid-join throw this exists to prevent.
		//
		// The accumulated side is measured over the keys AFTER equality
		// propagation, because those are the attributes MakeKeyReader will read.
		// Charging the raw predicate columns refuses queries the engine computes
		// perfectly well: when two edges put an INT64 column and an INT32 column
		// in one class, ShallowestEquivalent hands the join the INT32 one and it
		// packs 64 bits, while the raw columns add to 96. Measured over 4000
		// random graphs, that mistake declined 21 that the old planner answered.
		//
		// The invariant, since this function has now been wrong three ways: it
		// must measure exactly what MakeKeyReader measures.
		unsigned new_bits = 0;
		for (const auto &edge : edges) {
			const bool left_is_new = edge.left_relation == attaching;
			new_bits +=
			    TypeOfAttribute(graph,
			                    AttributeOf(graph, left_is_new ? edge.left_relation : edge.right_relation,
			                                static_cast<size_t>(left_is_new ? edge.left_column : edge.right_column))) ==
			            ValueType::INT32
			        ? 32u
			        : 64u;
		}
		unsigned joined_bits = 0;
		for (auto key : accumulated_keys_for(edges, attaching)) {
			joined_bits += TypeOfAttribute(graph, key) == ValueType::INT32 ? 32u : 64u;
		}
		return new_bits <= 64u && joined_bits <= 64u;
	};

	// Seed with the relation carrying the first predicate.
	const size_t seed = graph.predicates.empty() ? 0 : graph.predicates[0].left_relation;
	joined.insert(seed);
	plan.steps.push_back(PlanStep {seed, {}});

	while (joined.size() < graph.RelationCount()) {
		std::map<size_t, std::vector<Predicate>> candidates;
		for (size_t i = 0; i < graph.predicates.size(); i++) {
			if (used[i]) {
				continue;
			}
			const auto &predicate = graph.predicates[i];
			const bool left_in = joined.count(predicate.left_relation) > 0;
			const bool right_in = joined.count(predicate.right_relation) > 0;
			if (left_in == right_in) {
				continue; // both or neither joined
			}
			candidates[left_in ? predicate.right_relation : predicate.left_relation].push_back(predicate);
		}
		if (candidates.empty()) {
			plan.reason = "join graph is disconnected";
			return plan;
		}
		// A candidate whose edges do not converge is skipped rather than refused:
		// another relation may attach cleanly now, and the residual-predicate
		// check below still catches an edge that never gets used.
		auto best = candidates.end();
		bool too_wide = false;
		for (auto it = candidates.begin(); it != candidates.end(); ++it) {
			// Both tested, always, so the reason reported is the one a reader
			// can act on. Setting `too_wide` only where convergence had already
			// passed hid it behind the node-placement message for a candidate
			// that failed both -- sending someone hunting for a cycle when the
			// fix is casting a BIGINT to INTEGER. Reporting the right decision
			// for the wrong reason is the defect this message exists to end.
			const bool fits = fits_key_width(it->second, it->first);
			if (!fits) {
				too_wide = true;
			}
			if (!fits || !converges(it->second, it->first)) {
				continue;
			}
			if (best == candidates.end() || it->second.size() > best->second.size()) {
				best = it;
			}
		}
		if (best == candidates.end()) {
			// Named for what was actually measured rather than for the usual
			// cause. A cyclic graph reaches this, and so did every composite-key
			// join until the check above stopped guessing -- calling those
			// cyclic was wrong twice over, since the graph is acyclic and the
			// engine can compute it.
			plan.reason = too_wide ? "join key is wider than the 64-bit packed key"
			                       : "no relation attaches on keys that land on one f-tree node "
			                         "(a cyclic join graph is the usual cause)";
			return plan;
		}
		// Advance the simulation exactly as the join will: the accumulated tree
		// is the upper one under either insert mode -- bottom-insert puts the
		// build on top and names it `build`, top-insert puts the probe on top
		// and names it `probe`, and the accumulated side is whichever that is --
		// so the shape does not depend on the mode, only the locking does.
		{
			JoinKeys keys;
			keys.build = accumulated_keys_for(best->second, best->first);
			keys.probe.clear();
			for (const auto &edge : best->second) {
				const bool left_is_new = edge.left_relation == best->first;
				keys.probe.push_back(
				    AttributeOf(graph, left_is_new ? edge.left_relation : edge.right_relation,
				                static_cast<size_t>(left_is_new ? edge.left_column : edge.right_column)));
			}
			simulated = MergeTrees(simulated, FTree::Scan(AttributesOf(graph, best->first)), keys,
			                       JoinMode::BOTTOM_INSERT, PathStrategy::LEVELWISE);
		}
		plan.steps.push_back(PlanStep {best->first, best->second});
		joined.insert(best->first);
		// After the keys above were resolved, never before: this join is what
		// makes these equalities true, so resolving its own keys through them
		// would be assuming the conclusion.
		for (const auto &edge : best->second) {
			classes.Enforce(graph, edge);
		}
		for (size_t i = 0; i < graph.predicates.size(); i++) {
			const auto &predicate = graph.predicates[i];
			if (used[i]) {
				continue;
			}
			const bool covers = (predicate.left_relation == best->first && joined.count(predicate.right_relation)) ||
			                    (predicate.right_relation == best->first && joined.count(predicate.left_relation));
			if (covers) {
				used[i] = true;
			}
		}
	}

	for (size_t i = 0; i < graph.predicates.size(); i++) {
		if (!used[i]) {
			// A predicate between two already-joined relations is a selection on
			// the f-representation, which v1 does not implement.
			plan.reason = "residual predicate needs a selection operator";
			return plan;
		}
	}
	plan.complete = true;
	return plan;
}

std::vector<CostStep> BuildCostSteps(const QueryGraph &graph, const Plan &plan, RelationSource &source) {
	EquivalenceClasses classes(graph);
	std::vector<CostStep> steps;

	// Which column each relation joins on. For every relation but the first
	// this is the column its own attaching edge names; the first has no
	// attaching edge, so it takes the first edge that mentions it.
	std::map<size_t, size_t> join_column;
	for (size_t i = 1; i < plan.steps.size(); i++) {
		for (const auto &edge : plan.steps[i].edges) {
			join_column.emplace(edge.left_relation, static_cast<size_t>(edge.left_column));
			join_column.emplace(edge.right_relation, static_cast<size_t>(edge.right_column));
		}
	}

	// Mirrors the engine exactly, including equality propagation: a relation
	// attaches beneath the *shallowest* already-joined attribute of its
	// equivalence class, not beneath whichever relation the predicate happens to
	// name. That choice turns a chain into a star, and a star is the only shape
	// that compresses.
	std::map<size_t, int> depth_of;
	std::map<size_t, size_t> step_of;

	const size_t seed_relation = plan.steps[0].relation;
	const size_t seed_column = join_column.count(seed_relation) ? join_column[seed_relation] : 0;
	CostStep first;
	first.key = source.Stats(seed_relation, seed_column);
	first.key_group = static_cast<int>(classes.Find(AttributeOf(graph, seed_relation, seed_column)));
	steps.push_back(first);
	depth_of[seed_relation] = 0;
	step_of[seed_relation] = 0;

	for (size_t i = 1; i < plan.steps.size(); i++) {
		const auto &step = plan.steps[i];
		const auto &edge = step.edges.front();
		const bool left_is_new = edge.left_relation == step.relation;
		const auto new_column = static_cast<size_t>(left_is_new ? edge.left_column : edge.right_column);
		const auto named_relation = left_is_new ? edge.right_relation : edge.left_relation;
		const auto named_column = static_cast<size_t>(left_is_new ? edge.right_column : edge.left_column);
		const auto named_attribute = AttributeOf(graph, named_relation, named_column);

		size_t attach_relation = named_relation;
		size_t attach_column = named_column;
		int attach_depth = depth_of.count(named_relation) ? depth_of[named_relation] : 0;
		for (size_t owner = 0; owner < graph.RelationCount(); owner++) {
			if (!depth_of.count(owner)) {
				continue;
			}
			for (size_t column = 0; column < graph.column_counts[owner]; column++) {
				if (!classes.SameClass(AttributeOf(graph, owner, column), named_attribute)) {
					continue;
				}
				if (depth_of[owner] < attach_depth) {
					attach_depth = depth_of[owner];
					attach_relation = owner;
					attach_column = column;
				}
			}
		}

		CostStep entry;
		entry.key = source.Stats(step.relation, new_column);
		entry.key_group = static_cast<int>(classes.Find(AttributeOf(graph, step.relation, new_column)));
		entry.parent_key = source.Stats(attach_relation, attach_column);
		entry.parent_step = static_cast<int>(step_of[attach_relation]);
		steps.push_back(entry);

		depth_of[step.relation] = attach_depth + 1;
		step_of[step.relation] = steps.size() - 1;
	}
	return steps;
}

namespace {

//! What one materialized join left behind.
//! Whether anything will read the per-step census this query.
//!
//! The floors read `live`; a caller that wants the numbers themselves says so
//! through SetGlobalStepDetail. Checked rather than assumed, because the
//! walk costs a traversal of the whole representation.
bool StepDetailWanted() {
	return GetGlobalStepDetail() || GetGlobalMinCompression() > 0 || GetGlobalMinRate() > 0;
}

StepStats MeasureStep(size_t relation, const FactorizedRelation &accumulated) {
	StepStats step;
	step.elapsed_ms = ElapsedSliceMs();
	step.relation = relation;
	step.records = accumulated.Rep().RecordCount();
	step.bytes = accumulated.Rep().BytesAllocated();
	// The census is a full traversal of the representation, and cachegrind
	// puts 29.4% of this engine's last-level read misses inside it on
	// `watdiv_acyclic_217_01` -- more than any other function, and that query
	// is the largest regression in the corpus (D101).
	//
	// Nothing reads what it produces unless something asked. `live` feeds the
	// compression and rate floors, both of which default to 0 and return
	// immediately; `fanout_mean` and `fanout_max` feed a floor that D85
	// measured and reverted. Otherwise the fields reach only the
	// `explain_steps` diagnostic line and the `factorized_steps` table
	// function, which are requests rather than defaults. So the walk happens
	// when one of those is on, and not on the path a query normally takes.
	if (!StepDetailWanted()) {
		step.tuples = accumulated.Count();
		return step;
	}
	// One walk for both: the census counts the same live records
	// LiveRecordCount() would, and counting the children on the way costs an
	// increment per child list rather than a second traversal.
	size_t populated = 0;
	size_t children = 0;
	for (const auto &level : accumulated.Rep().Census()) {
		step.live += level.records;
		populated += level.populated_slots;
		children += level.children;
		if (level.populated_slots > 0) {
			const double level_fanout =
			    static_cast<double>(level.children) / static_cast<double>(level.populated_slots);
			step.fanout_max = level_fanout > step.fanout_max ? level_fanout : step.fanout_max;
		}
	}
	if (populated > 0) {
		step.fanout_mean = static_cast<double>(children) / static_cast<double>(populated);
	}
	// The join has just pruned, which computes every subtree size, so this
	// walks the roots over a warm cache rather than the representation.
	step.tuples = accumulated.Count();
	return step;
}

//! Abandons the query when the join that just ran did not compress.
//!
//! The gate that chose this plan predicted; this measures. Compression below
//! the floor means the representation is holding roughly a record per tuple,
//! which is a hash join built out of the wrong parts -- DuckDB's is tuned and
//! ours is not, so the remaining joins can only lose ground. Stopping here
//! throws away the steps already run, and that is the trade: those steps are
//! bounded by what has happened, while finishing is bounded by nothing.
//!
//! Deliberately not MemoryLimitExceeded. Slicing a query that is not
//! compressing makes several smaller copies of the same mistake.
//!
//! Two measured quantities, not one. A poor ratio alone is not a reason to
//! stop: most queries that compress badly also finish in milliseconds, which
//! is why a floor on its own abandoned 17 of 248 out-of-sample queries (D37).
//! The ones worth abandoning are those where the ratio is poor *and* real time
//! has already gone into it -- every catastrophic loss measured has that shape,
//! 300s against stock's 5.1s, 1.065s against 0.015s -- and both halves are
//! read off the run rather than predicted before it.
//! Abandons when the tuples are arriving too slowly to be worth the trouble.
//!
//! The rate, rather than the compression, because the rate is what the choice
//! is actually between: DuckDB's cost model is stated in tuples per
//! millisecond, so a measured rate can be compared to it and a measured ratio
//! cannot. Only at the last materialized join, where D52 measured the two
//! populations apart -- wins 1.11e5 to 6.28e7, losses 17 to 4.59e4 -- and
//! nowhere earlier, where the same measurement has them overlapping completely.
//!
//! The last materialized join is `total_steps - 2`: the final step of a count
//! is fused and materializes nothing, so the step before it is the last one
//! that leaves a representation to measure.
void CheckRate(const StepStats &step, size_t step_index, size_t total_steps) {
	const double min_rate = GetGlobalMinRate();
	if (min_rate <= 0 || step.live == 0 || step.tuples <= 0) {
		return;
	}
	if (step_index + 2 != total_steps) {
		return;
	}
	const double elapsed = ElapsedSliceMs();
	if (elapsed <= 0) {
		return;
	}
	const double rate = static_cast<double>(step.tuples) / elapsed;
	if (rate >= min_rate) {
		return;
	}
	throw std::runtime_error("join " + std::to_string(step_index) + " of " + std::to_string(total_steps - 1) +
	                         " had delivered " + std::to_string(step.tuples) + " tuples in " +
	                         std::to_string(static_cast<int64_t>(elapsed)) + "ms, a rate of " + std::to_string(rate) +
	                         " tuples/ms against a floor of " + std::to_string(min_rate) +
	                         ", so the stock plan is the better bet from here");
}

void CheckCompression(const StepStats &step, size_t step_index, size_t total_steps) {
	const double floor = GetGlobalMinCompression();
	if (floor <= 0 || step.live == 0) {
		return;
	}
	// Never on the first materialized join. Records grow by a sum over joins
	// and tuples by a product, so the product has not overtaken yet and a ratio
	// near 1 there says nothing: measured, the first step is 0.94 on a 142s win
	// and 0.50, 0.95, 0.99 on three queries worth abandoning. The second step is
	// what separates them -- 43.8 against 0.33, 0.97, 2.34 -- and judging at the
	// first is what made a floor abandon a query only this engine can answer,
	// and what a floor calibrated to 0.6 was working around (D37, D51).
	if (step_index <= 1) {
		return;
	}
	const double achieved = step.Compression();
	if (achieved >= floor) {
		return;
	}
	const double after = GetGlobalAbandonAfter();
	const double elapsed = ElapsedSliceMs();
	if (after > 0 && elapsed < after) {
		return;
	}
	throw std::runtime_error("join " + std::to_string(step_index) + " of " + std::to_string(total_steps - 1) +
	                         " left " + std::to_string(step.live) + " records standing for " +
	                         std::to_string(step.tuples) + " tuples, a compression of " + std::to_string(achieved) +
	                         " against a floor of " + std::to_string(floor) + " after " +
	                         std::to_string(static_cast<int64_t>(elapsed)) +
	                         "ms, so factorizing this query is not paying for itself");
}

} // namespace

ExecuteResult ExecuteCount(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                           PathStrategy strategy) {
	ExecuteResult result;
	if (!plan.complete) {
		result.error = plan.reason.empty() ? "no plan" : plan.reason;
		return result;
	}
	try {
		if (graph.column_types.size() != graph.RelationCount()) {
			throw std::runtime_error("QueryGraph::column_types must have one entry per relation");
		}
		AttributeTypes types;
		for (size_t relation = 0; relation < graph.RelationCount(); relation++) {
			if (graph.column_types[relation].size() != graph.column_counts[relation]) {
				throw std::runtime_error("QueryGraph::column_types[" + std::to_string(relation) +
				                         "] must have one entry per column");
			}
			for (size_t column = 0; column < graph.column_counts[relation]; column++) {
				types.emplace_back(AttributeOf(graph, relation, column), graph.column_types[relation][column]);
			}
		}

		auto make = [&](size_t relation) {
			std::vector<AttributeId> attributes;
			for (size_t column = 0; column < graph.column_counts[relation]; column++) {
				attributes.push_back(AttributeOf(graph, relation, column));
			}
			return MakeScan(attributes, types, source.Columns(relation));
		};

		// Enforced-only, and grown as the joins below apply their edges. The
		// planner accepted this graph under the same rule, so the two must agree
		// on what is substitutable or execution would resolve keys the plan
		// never validated.
		EquivalenceClasses classes(graph, Enforced {});
		int64_t fused_count = -1;
		auto accumulated = make(plan.steps[0].relation);

		for (size_t i = 1; i < plan.steps.size(); i++) {
			const auto &step = plan.steps[i];
			// The accumulated tree must stay on top either way, otherwise the
			// new relation becomes the root and every previous one nests
			// beneath it -- a chain, in which nothing is independent.
			//
			// Top-insert puts the *probe* on top, bottom-insert the *build*, so
			// the two modes take their arguments swapped. That is the paper's
			// L (top-insert) R == R (bottom-insert) L: the shape is the same,
			// and what differs is which side gets the hash table.
			JoinKeys keys;
			std::vector<AttributeId> new_keys;
			std::vector<AttributeId> accumulated_keys;
			for (const auto &edge : step.edges) {
				const bool left_is_new = edge.left_relation == step.relation;
				new_keys.push_back(
				    AttributeOf(graph, left_is_new ? edge.left_relation : edge.right_relation,
				                static_cast<size_t>(left_is_new ? edge.left_column : edge.right_column)));
				const auto raw = AttributeOf(graph, left_is_new ? edge.right_relation : edge.left_relation,
				                             static_cast<size_t>(left_is_new ? edge.right_column : edge.left_column));
				accumulated_keys.push_back(ShallowestEquivalent(classes, accumulated.Tree(), raw));
			}

			// The aggregate is the topmost operator, so the final join's output
			// exists only to be counted (sections 4.2.2 and 4.5).
			const bool last_join = (i + 1 == plan.steps.size());
			if (mode == JoinMode::TOP_INSERT) {
				keys.build = new_keys;
				keys.probe = accumulated_keys;
				if (last_join) {
					fused_count = FactorizedCountJoin(make(step.relation), accumulated, keys, mode, strategy);
				} else {
					accumulated = FactorizedJoin(make(step.relation), accumulated, keys, mode, strategy);
				}
			} else {
				keys.build = accumulated_keys;
				keys.probe = new_keys;
				if (last_join) {
					fused_count = FactorizedCountJoin(accumulated, make(step.relation), keys, mode, strategy);
				} else {
					accumulated = FactorizedJoin(accumulated, make(step.relation), keys, mode, strategy);
				}
			}
			if (!last_join) {
				result.steps.push_back(MeasureStep(step.relation, accumulated));
				CheckCompression(result.steps.back(), i, plan.steps.size());
				CheckRate(result.steps.back(), i, plan.steps.size());
			}
			// Applied by the join just issued, so substitutable from here on and
			// not before -- the same order BuildPlan validated the graph under.
			for (const auto &edge : step.edges) {
				classes.Enforce(graph, edge);
			}
		}

		result.count = fused_count >= 0 ? fused_count : accumulated.Count();
		result.records = accumulated.Rep().RecordCount();
		result.bytes = accumulated.Rep().BytesAllocated();
		result.ok = true;
	} catch (const MemoryLimitExceeded &error) {
		result.error = error.what();
		result.out_of_memory = true;
	} catch (const std::exception &error) {
		result.error = error.what();
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Slicing
//===--------------------------------------------------------------------===//

namespace {

//! Hands on one hash bucket of another source.
//!
//! Relations holding an attribute of the sliced class are filtered to the rows
//! whose key falls in the bucket; the rest are passed through untouched, since
//! their rows can only join with keys that survived anyway.
class SlicedSource : public RelationSource {
public:
	SlicedSource(RelationSource &inner, std::vector<int> key_column, size_t slice, size_t slices)
	    : inner(inner), key_column(std::move(key_column)), slice(slice), slices(slices) {
	}

	const std::vector<std::vector<int64_t>> &Columns(size_t relation) override {
		const auto &columns = inner.Columns(relation);
		if (key_column[relation] < 0) {
			return columns;
		}
		const auto &keys = columns[static_cast<size_t>(key_column[relation])];
		// Cleared and reserved, never re-assigned. `held.assign(n, {})` freed
		// these buffers on every relation, so each one re-grew from nothing
		// through push_back's doubling and faulted in every page again -- the
		// D36 bug, in the second place it was written and the more expensive of
		// the two: this runs once per relation *per slice*, so on eight threads
		// the whole input was re-allocated eight times over. Stack sampling put
		// 21 of 30 working samples right here while the storage scan it wraps
		// got two.
		//
		// The reserve is the expected share, not the whole relation: the point
		// of a bucket is that it holds about 1/slices of the rows, and reserving
		// for all of them would commit `slices` times the memory the slicing was
		// asked for. A bucket that runs over grows once, from a capacity that is
		// already close.
		if (held.size() != columns.size()) {
			held.assign(columns.size(), {});
		} else {
			for (auto &column : held) {
				column.clear();
			}
		}
		const size_t share = slices == 0 ? keys.size() : keys.size() / slices + 1;
		for (auto &column : held) {
			column.reserve(share);
		}
		for (size_t row = 0; row < keys.size(); row++) {
			if (HashKey(static_cast<uint64_t>(keys[row])) % slices != slice) {
				continue;
			}
			// Kept, and every column of the row is kept with it: the row
			// alignment MakeScan depends on is as load-bearing here as it is in
			// the scan.
			for (size_t column = 0; column < columns.size(); column++) {
				held[column].push_back(columns[column][row]);
			}
		}
		return held;
	}

	ColumnStats Stats(size_t relation, size_t column) override {
		return inner.Stats(relation, column);
	}

private:
	RelationSource &inner;
	//! The column of each relation to bucket on, or -1 to pass the relation
	//! through whole.
	std::vector<int> key_column;
	size_t slice;
	size_t slices;
	std::vector<std::vector<int64_t>> held;
};

//! Picks the equivalence class to slice on. Returns the per-relation column to
//! bucket on, or an empty vector if no class reaches more than one relation, in
//! which case slicing cannot help.
//!
//! Reaching the most relations is the obvious criterion and was the only one,
//! because filtering more relations makes a pass smaller. What it misses is
//! *which* relations, and the plan says which ones matter: a bucket of a key
//! that does not touch the relation the join order starts from does not shrink
//! it, so every slice builds the same first structure whole. Measured at eight
//! threads on `watdiv_acyclic_217_05`, that was eight slices each holding an
//! identical 361,282-record step and each peaking at 800MB (D54a).
//!
//! So the base relation comes first and reach breaks the tie. `plan` is
//! optional only because a caller without one still gets the old behaviour;
//! every caller in this file has one.
//!
//! Correctness does not depend on the choice. Slicing on *any* single class is
//! sound -- the closure argument below is about a class, not about a particular
//! one -- so this moves time and never the answer, which is what makes it safe
//! to decide on a measurement.
std::vector<int> ChooseSliceColumns(const QueryGraph &graph, const Plan *plan) {
	// The full closure is right here, and only here. Slicing partitions the
	// INPUT by hashing a column of each relation, and every relation whose
	// column is transitively equated must land in the same bucket or tuples that
	// join are separated and the count comes out short. That is a statement
	// about which rows can possibly match, which the whole graph decides -- not
	// about which substitutions a given join has earned.
	EquivalenceClasses classes(graph);
	std::map<size_t, std::vector<int>> by_class;
	std::map<size_t, size_t> reach;
	for (size_t relation = 0; relation < graph.RelationCount(); relation++) {
		for (size_t column = 0; column < graph.column_counts[relation]; column++) {
			const auto root = classes.Find(AttributeOf(graph, relation, column));
			auto &columns = by_class[root];
			if (columns.empty()) {
				columns.assign(graph.RelationCount(), -1);
			}
			if (columns[relation] < 0) {
				columns[relation] = static_cast<int>(column);
				reach[root]++;
			}
		}
	}
	// Whether a class touches the relation the plan starts from. Everything the
	// first join builds is made of that relation and one other, so a class that
	// misses it leaves the first structure at full size in every bucket.
	const bool have_base = plan != nullptr && !plan->steps.empty();
	const size_t base = have_base ? plan->steps[0].relation : 0;
	auto covers_base = [&](size_t root) {
		return have_base && base < by_class[root].size() && by_class[root][base] >= 0;
	};

	size_t best = 0;
	size_t best_reach = 0;
	bool best_covers = false;
	bool found = false;
	for (const auto &entry : reach) {
		if (entry.second <= 1) {
			// A class reaching one relation buckets that relation and leaves
			// every other one whole, so it cannot shrink the join.
			continue;
		}
		const bool covers = covers_base(entry.first);
		// Covering the base wins outright; reach only breaks the tie. A class
		// reaching six relations but not the first one still hands every bucket
		// the whole of the first structure to build.
		const bool better = !found || (covers && !best_covers) || (covers == best_covers && entry.second > best_reach);
		if (better) {
			best = entry.first;
			best_reach = entry.second;
			best_covers = covers;
			found = true;
		}
	}
	if (!found) {
		return {};
	}
	return by_class[best];
}

//! Every class that could be sliced on, widest reach first.
//!
//! `ChooseSliceColumns` returns the first of these and it is the right one to
//! start with: the more relations a key reaches, the more of the input a bucket
//! of it shrinks. What it cannot do is get out of the way when it is the wrong
//! key, and that is the case the README has been calling "no spilling" -- a
//! single value of the chosen class whose own subtree does not fit, which no
//! number of buckets separates from itself, because they all separate values
//! and it is one value.
//!
//! A *different* class is what separates it, and slicing on one is sound
//! whichever one it is: the closure argument is about a class, not about the
//! widest class. So the retries have somewhere left to go.
std::vector<std::vector<int>> RankSliceColumns(const QueryGraph &graph) {
	EquivalenceClasses classes(graph);
	std::map<size_t, std::vector<int>> by_class;
	std::map<size_t, size_t> reach;
	for (size_t relation = 0; relation < graph.RelationCount(); relation++) {
		for (size_t column = 0; column < graph.column_counts[relation]; column++) {
			const auto root = classes.Find(AttributeOf(graph, relation, column));
			auto &columns = by_class[root];
			if (columns.empty()) {
				columns.assign(graph.RelationCount(), -1);
			}
			if (columns[relation] < 0) {
				columns[relation] = static_cast<int>(column);
				reach[root]++;
			}
		}
	}
	std::vector<std::pair<size_t, size_t>> ranked;
	for (const auto &entry : reach) {
		if (entry.second > 1) {
			// A class reaching one relation buckets that relation and leaves
			// every other one whole, so it cannot shrink the join.
			ranked.emplace_back(entry.second, entry.first);
		}
	}
	std::sort(ranked.begin(), ranked.end(), [](const std::pair<size_t, size_t> &a, const std::pair<size_t, size_t> &b) {
		return a.first != b.first ? a.first > b.first : a.second < b.second;
	});
	std::vector<std::vector<int>> out;
	out.reserve(ranked.size());
	for (const auto &entry : ranked) {
		out.push_back(by_class[entry.second]);
	}
	return out;
}

} // namespace

ExecuteResult ExecuteCountNotEqual(const QueryGraph &graph, const NotEqualPredicate &neq, RelationSource &source,
                                   JoinMode mode, PathStrategy strategy) {
	ExecuteResult result;
	const auto relations = graph.RelationCount();
	if (neq.left_relation >= relations || neq.right_relation >= relations) {
		result.error = "not-equal: relation out of range";
		return result;
	}
	if (neq.left_relation == neq.right_relation) {
		// `A.x <> A.y` is a filter on one relation, not a join between two, and
		// its complement `A.x = A.y` is a filter too. Neither is an edge, so
		// neither term is expressible as a join graph -- and answering it would
		// mean silently ignoring the predicate.
		result.error = "not-equal: both columns are on one relation, which is a filter rather than a join";
		return result;
	}
	if (neq.left_column < 0 || neq.right_column < 0 ||
	    static_cast<size_t>(neq.left_column) >= graph.column_counts[neq.left_relation] ||
	    static_cast<size_t>(neq.right_column) >= graph.column_counts[neq.right_relation]) {
		result.error = "not-equal: column out of range";
		return result;
	}

	// Term one: the equalities alone.
	const auto all_plan = BuildPlan(graph);
	if (!all_plan.complete) {
		result.error = "not-equal: the equality-only term does not plan: " + all_plan.reason;
		return result;
	}
	const auto all = ExecuteCount(graph, all_plan, source, mode, strategy);
	if (!all.ok) {
		result.error = "not-equal: the equality-only term failed: " + all.error;
		result.out_of_memory = all.out_of_memory;
		return result;
	}

	// Term two: the same graph with `<>` promoted to `=`. This is the term that
	// needs composite keys, since it adds a second edge between a pair the graph
	// may already join.
	QueryGraph equal_graph = graph;
	equal_graph.predicates.push_back(
	    Predicate {neq.left_relation, neq.left_column, neq.right_relation, neq.right_column});
	const auto equal_plan = BuildPlan(equal_graph);
	if (!equal_plan.complete) {
		result.error = "not-equal: the equality term does not plan: " + equal_plan.reason;
		return result;
	}
	const auto equal = ExecuteCount(equal_graph, equal_plan, source, mode, strategy);
	if (!equal.ok) {
		result.error = "not-equal: the equality term failed: " + equal.error;
		result.out_of_memory = equal.out_of_memory;
		return result;
	}

	// The equality term counts a subset of the equality-only term, so the
	// difference cannot be negative and cannot overflow. If it ever is negative
	// the two terms disagreed about something and the answer is worthless, so
	// say so rather than return it.
	if (equal.count > all.count) {
		result.error = "not-equal: the equality term counted more than the equality-only term (" +
		               std::to_string(equal.count) + " > " + std::to_string(all.count) + ")";
		return result;
	}
	result.count = all.count - equal.count;
	result.tuples = result.count;
	result.records = all.records + equal.records;
	result.bytes = all.bytes + equal.bytes;
	result.ok = true;
	return result;
}

ExecuteResult ExecuteCountSlice(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                size_t slice, size_t slices, PathStrategy strategy) {
	if (slices <= 1) {
		return ExecuteCount(graph, plan, source, mode, strategy);
	}
	auto key_column = ChooseSliceColumns(graph, &plan);
	if (key_column.empty()) {
		ExecuteResult result;
		result.error = "no join key reaches more than one relation, so slicing cannot shrink anything";
		return result;
	}
	SlicedSource sliced(source, key_column, slice, slices);
	auto result = ExecuteCount(graph, plan, sliced, mode, strategy);
	result.slices = slices;
	return result;
}

//! Counts one bucket by partitioning it on a key other than the one it is a
//! bucket of. Not ok when no other key can be found or none of them fits.
//!
//! The case this exists for is a bucket whose contents are a single value of the
//! chosen class: refining that class puts the same rows in the same bucket
//! however fine the modulus, because a modulus separates values and there is
//! only one. A different class separates them, and slicing on one is sound
//! whichever one it is -- the closure argument in `ChooseSliceColumns` is about
//! a class, not about the widest class (D55).
//!
//! Nested rather than swapped: the inner source is still this bucket of the
//! original key, so the pieces partition exactly what the caller was given and
//! no other bucket's work is touched. That is the property the modulus
//! refinement relies on too, reached a different way.
static ExecuteResult CountBySecondKey(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                      size_t slice, size_t slices, PathStrategy strategy) {
	ExecuteResult failed;
	if (!GetGlobalSecondKey()) {
		failed.error = "this bucket does not fit and splitting it on another key is off";
		return failed;
	}
	// The outer bucket has to be the *caller's* bucket, which means the key the
	// caller's `slice` and `slices` were computed against. Taking the widest
	// class instead happened to be the same thing while ChooseSliceColumns
	// preferred width, and would have silently stopped being the same thing the
	// moment it preferred anything else -- re-bucketing the input under a
	// different key while keeping the old bucket number is a wrong answer, not
	// a slow one. So it is asked for rather than inferred.
	const auto primary = ChooseSliceColumns(graph, &plan);
	if (primary.empty()) {
		failed.error = "no join key reaches more than one relation, so slicing cannot shrink anything";
		return failed;
	}
	auto ranked = RankSliceColumns(graph);
	// Every class except the one already being bucketed on.
	std::vector<std::vector<int>> others;
	for (auto &candidate : ranked) {
		if (candidate != primary) {
			others.push_back(candidate);
		}
	}
	if (others.empty()) {
		failed.error = "only one join key reaches more than one relation, so there is no other key to partition on";
		return failed;
	}
	// One alternative key, and two moduli. Every piece re-scans the whole input
	// to find its own rows, so an attempt at `parts` pieces costs `parts` passes
	// -- and this runs only after the modulus refinement has already spent
	// 8 + 64 + 512 + 4096 of them failing.
	//
	// The first version of this tried every class at every modulus up to 4096,
	// and it turned `watdiv_acyclic_217_10` from a query that failed in 23s and
	// was answered by the stock plan into one that did not finish in 300s. The
	// bound is not a tuning knob: a second key either separates the value that
	// is carrying the bucket or it does not, and 64 pieces is enough to find out
	// (the regression fixture recovers at 8).
	static const size_t kOtherKeys = 1;
	static const size_t kMaxParts = 64;
	const size_t tries = others.size() < kOtherKeys ? others.size() : kOtherKeys;
	for (size_t rank = 0; rank < tries; rank++) {
		SlicedSource bucket(source, primary, slice, slices);
		for (size_t parts = 8; parts <= kMaxParts; parts *= 8) {
			int64_t total = 0;
			size_t records = 0;
			size_t bytes = 0;
			std::vector<StepStats> steps;
			bool fits = true;
			for (size_t part = 0; part < parts && fits; part++) {
				SlicedSource inner(bucket, others[rank], part, parts);
				auto piece = ExecuteCount(graph, plan, inner, mode, strategy);
				if (!piece.ok) {
					if (!piece.out_of_memory) {
						// Something other than size, which a different
						// partition will not fix.
						return piece;
					}
					fits = false;
					break;
				}
				total = CheckedCardinalityAdd(total, piece.count);
				records = records > piece.records ? records : piece.records;
				bytes = bytes > piece.bytes ? bytes : piece.bytes;
				if (steps.empty()) {
					steps = std::move(piece.steps);
				}
			}
			if (fits) {
				ExecuteResult refined;
				refined.ok = true;
				refined.count = total;
				refined.records = records;
				refined.bytes = bytes;
				refined.slices = slices * parts;
				refined.steps = std::move(steps);
				return refined;
			}
		}
	}
	failed.out_of_memory = true;
	failed.error = "no partition of any join key made this bucket fit";
	return failed;
}

ExecuteResult ExecuteCountSliceWithinMemory(const QueryGraph &graph, const Plan &plan, RelationSource &source,
                                            JoinMode mode, size_t slice, size_t slices, PathStrategy strategy) {
	auto result = ExecuteCountSlice(graph, plan, source, mode, slice, slices, strategy);
	if (result.ok || !result.out_of_memory) {
		return result;
	}
	// Refine this bucket, and only this bucket. A modulus of `slices * factor`
	// splits bucket `slice` into the buckets congruent to it, and touches no
	// other bucket -- which is what makes this safe to do on one thread while
	// others are working on theirs.
	for (size_t factor = 8; factor <= 4096; factor *= 8) {
		const size_t finer = slices * factor;
		int64_t total = 0;
		size_t records = 0;
		size_t bytes = 0;
		std::vector<StepStats> steps;
		bool fits = true;
		for (size_t part = 0; part < factor && fits; part++) {
			auto piece = ExecuteCountSlice(graph, plan, source, mode, slice + part * slices, finer, strategy);
			if (!piece.ok) {
				if (!piece.out_of_memory) {
					return piece;
				}
				fits = false;
				result = piece;
				break;
			}
			total = CheckedCardinalityAdd(total, piece.count);
			records = records > piece.records ? records : piece.records;
			bytes = bytes > piece.bytes ? bytes : piece.bytes;
			if (steps.empty()) {
				steps = std::move(piece.steps);
			}
		}
		if (fits) {
			ExecuteResult refined;
			refined.ok = true;
			refined.count = total;
			refined.records = records;
			refined.bytes = bytes;
			refined.slices = finer;
			refined.steps = std::move(steps);
			return refined;
		}
	}

	// Refining the same key has run out, which means the bucket is not merely
	// big but skewed: one value of the chosen class is carrying it, and no
	// modulus separates a value from itself. A second class can.
	auto second = CountBySecondKey(graph, plan, source, mode, slice, slices, strategy);
	if (second.ok) {
		return second;
	}
	// This slice is out of ideas, and the buckets are all the same query. Every
	// sibling is about to spend what this one just spent to reach the same
	// verdict, so they are told instead (D54b).
	RaiseSharedAbandon();
	return result;
}

ExecuteResult ExecuteCountSliced(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                 size_t slices, PathStrategy strategy) {
	ExecuteResult result;
	if (slices <= 1) {
		return ExecuteCount(graph, plan, source, mode, strategy);
	}

	int64_t total = 0;
	size_t records = 0;
	size_t bytes = 0;
	for (size_t slice = 0; slice < slices; slice++) {
		auto part = ExecuteCountSlice(graph, plan, source, mode, slice, slices, strategy);
		if (!part.ok) {
			part.slices = slices;
			return part;
		}
		total = CheckedCardinalityAdd(total, part.count);
		// The largest slice is what had to fit, so that is what is reported.
		records = records > part.records ? records : part.records;
		bytes = bytes > part.bytes ? bytes : part.bytes;
		if (result.steps.empty()) {
			// From one slice, not from all of them: per-step numbers do not add
			// up across slices the way a count does, and one bucket's shape is
			// what the compression check inside it actually saw.
			result.steps = std::move(part.steps);
		}
	}
	result.ok = true;
	result.count = total;
	result.records = records;
	result.bytes = bytes;
	result.slices = slices;
	return result;
}

namespace {

//! Runs every join in `plan`, fusing none, and hands back what they built.
//!
//! ExecuteCount fuses the last join because its output exists only to be
//! counted. Everything that wants the *result* -- tuples, groups -- needs the
//! representation to exist, so it needs this instead.
struct BuiltRelation {
	bool ok = false;
	std::string error;
	bool out_of_memory = false;
	std::unique_ptr<FactorizedRelation> relation;
	std::unique_ptr<EquivalenceClasses> classes;
};

BuiltRelation BuildRepresentation(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                  PathStrategy strategy) {
	BuiltRelation built;
	try {
		if (graph.column_types.size() != graph.RelationCount()) {
			throw std::runtime_error("QueryGraph::column_types must have one entry per relation");
		}
		AttributeTypes types;
		for (size_t relation = 0; relation < graph.RelationCount(); relation++) {
			if (graph.column_types[relation].size() != graph.column_counts[relation]) {
				throw std::runtime_error("QueryGraph::column_types[" + std::to_string(relation) +
				                         "] must have one entry per column");
			}
			for (size_t column = 0; column < graph.column_counts[relation]; column++) {
				types.emplace_back(AttributeOf(graph, relation, column), graph.column_types[relation][column]);
			}
		}

		auto make = [&](size_t relation) {
			std::vector<AttributeId> attributes;
			for (size_t column = 0; column < graph.column_counts[relation]; column++) {
				attributes.push_back(AttributeOf(graph, relation, column));
			}
			return MakeScan(attributes, types, source.Columns(relation));
		};

		built.classes.reset(new EquivalenceClasses(graph, Enforced {}));
		auto accumulated = make(plan.steps[0].relation);

		for (size_t i = 1; i < plan.steps.size(); i++) {
			const auto &step = plan.steps[i];
			JoinKeys keys;
			std::vector<AttributeId> new_keys;
			std::vector<AttributeId> accumulated_keys;
			for (const auto &edge : step.edges) {
				const bool left_is_new = edge.left_relation == step.relation;
				new_keys.push_back(
				    AttributeOf(graph, left_is_new ? edge.left_relation : edge.right_relation,
				                static_cast<size_t>(left_is_new ? edge.left_column : edge.right_column)));
				const auto raw = AttributeOf(graph, left_is_new ? edge.right_relation : edge.left_relation,
				                             static_cast<size_t>(left_is_new ? edge.right_column : edge.left_column));
				accumulated_keys.push_back(ShallowestEquivalent(*built.classes, accumulated.Tree(), raw));
			}
			if (mode == JoinMode::TOP_INSERT) {
				keys.build = new_keys;
				keys.probe = accumulated_keys;
				accumulated = FactorizedJoin(make(step.relation), accumulated, keys, mode, strategy);
			} else {
				keys.build = accumulated_keys;
				keys.probe = new_keys;
				accumulated = FactorizedJoin(accumulated, make(step.relation), keys, mode, strategy);
			}
			// Every join here materializes -- nothing is fused, that is what
			// this function is for -- so the last one is checked like the rest
			// except that there is nothing left to save by abandoning after it.
			if (i + 1 < plan.steps.size()) {
				CheckCompression(MeasureStep(step.relation, accumulated), i, plan.steps.size());
			}
			// After the join, for the same reason as everywhere else: an equality
			// becomes substitutable when a join has made it true, not when a
			// predicate has asked for it.
			for (const auto &edge : step.edges) {
				built.classes->Enforce(graph, edge);
			}
		}
		built.relation.reset(new FactorizedRelation(std::move(accumulated)));
		built.ok = true;
	} catch (const MemoryLimitExceeded &error) {
		built.error = error.what();
		built.out_of_memory = true;
	} catch (const std::exception &error) {
		built.error = error.what();
	}
	return built;
}

} // namespace

MaterializeResult ExecuteMaterialize(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                     size_t limit, PathStrategy strategy) {
	MaterializeResult result;
	if (!plan.complete) {
		result.error = plan.reason.empty() ? "no plan" : plan.reason;
		return result;
	}
	auto built = BuildRepresentation(graph, plan, source, mode, strategy);
	if (!built.ok) {
		result.error = built.error;
		result.out_of_memory = built.out_of_memory;
		return result;
	}
	try {
		auto &accumulated = *built.relation;
		auto &classes = *built.classes;

		// An equi-join's two key attributes are always equal, so the merge is
		// free to keep only one of them. A tuple still has to carry both, and
		// the value of the missing one is by definition the value of whichever
		// equivalent attribute survived.
		const auto &layout = accumulated.GetLayout();
		auto present = [&](AttributeId attribute) {
			for (size_t level = 0; level < layout.LevelCount(); level++) {
				for (const auto &entry : layout.Level(static_cast<LevelId>(level)).payload) {
					if (entry.attribute == attribute) {
						return true;
					}
				}
			}
			return false;
		};

		std::vector<TupleColumn> columns;
		size_t position = 0;
		for (size_t relation = 0; relation < graph.RelationCount(); relation++) {
			for (size_t column = 0; column < graph.column_counts[relation]; column++) {
				const auto wanted = AttributeOf(graph, relation, column);
				AttributeId source_attribute = wanted;
				if (!present(wanted)) {
					bool found = false;
					for (size_t other = 0; other < graph.RelationCount() && !found; other++) {
						for (size_t c = 0; c < graph.column_counts[other] && !found; c++) {
							const auto candidate = AttributeOf(graph, other, c);
							if (present(candidate) && classes.SameClass(candidate, wanted)) {
								source_attribute = candidate;
								found = true;
							}
						}
					}
					if (!found) {
						throw std::runtime_error("attribute " + std::to_string(wanted) +
						                         " is neither stored nor equal to anything stored");
					}
				}
				columns.push_back(TupleColumn {source_attribute, position});
				position++;
			}
		}

		Enumerate(accumulated.Rep(), columns, limit, [&](const std::vector<int64_t> &values) {
			result.tuples.push_back(values);
			return true;
		});
		result.records = accumulated.Rep().RecordCount();
		result.bytes = accumulated.Rep().BytesAllocated();
		result.ok = true;
	} catch (const MemoryLimitExceeded &error) {
		result.error = error.what();
		result.out_of_memory = true;
	} catch (const std::exception &error) {
		result.error = error.what();
	}
	return result;
}

MaterializeResult ExecuteMaterializeWithinMemory(const QueryGraph &graph, const Plan &plan, RelationSource &source,
                                                 JoinMode mode, size_t limit, PathStrategy strategy) {
	auto result = ExecuteMaterialize(graph, plan, source, mode, limit, strategy);
	if (result.ok || !result.out_of_memory) {
		return result;
	}
	auto key_column = ChooseSliceColumns(graph, &plan);
	if (key_column.empty()) {
		return result;
	}
	static const size_t kSliceSteps[] = {8, 64, 512, 4096};
	for (auto slices : kSliceSteps) {
		MaterializeResult gathered;
		bool fits = true;
		for (size_t slice = 0; slice < slices && fits; slice++) {
			SlicedSource sliced(source, key_column, slice, slices);
			// Each bucket only has to supply what the limit still wants.
			const size_t remaining = limit == 0 ? 0 : limit - gathered.tuples.size();
			auto part = ExecuteMaterialize(graph, plan, sliced, mode, remaining, strategy);
			if (!part.ok) {
				if (!part.out_of_memory) {
					return part;
				}
				fits = false;
				result = part;
				break;
			}
			for (auto &tuple : part.tuples) {
				gathered.tuples.push_back(std::move(tuple));
			}
			gathered.records = gathered.records > part.records ? gathered.records : part.records;
			gathered.bytes = gathered.bytes > part.bytes ? gathered.bytes : part.bytes;
			if (limit != 0 && gathered.tuples.size() >= limit) {
				// The prefix is complete; the remaining buckets are never built,
				// which is the whole point of asking for a limit.
				break;
			}
		}
		if (fits) {
			gathered.ok = true;
			return gathered;
		}
	}
	return result;
}

ExecuteResult ExecuteProductSum(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                size_t x_relation, size_t x_column, size_t y_relation, size_t y_column,
                                PathStrategy strategy) {
	ExecuteResult result;
	auto built = BuildRepresentation(graph, plan, source, mode, strategy);
	if (!built.ok) {
		result.error = built.error;
		result.out_of_memory = built.out_of_memory;
		return result;
	}
	try {
		const auto &rep = built.relation->Rep();
		const auto x = AttributeOf(graph, x_relation, x_column);
		const auto y = AttributeOf(graph, y_relation, y_column);
		// Roots are alternatives, so their quantities add. Only the product term
		// is answered; the other three exist so that the recursion below a root
		// can combine, and are of no interest once it has.
		Int128 total;
		int64_t tuples = 0;
		rep.ForEachRoot([&](Record root) {
			const auto sums = rep.SubtreeProductSum(root, x, y);
			total = CheckedAdd128(total, sums.sum_xy);
			tuples = CheckedCardinalityAdd(tuples, sums.count);
		});
		result.product_sum = total;
		// `count` keeps reporting the answer when it fits, so every caller
		// that predates the widening still works; the 128-bit field is what
		// the operator reads.
		result.count =
		    (total.high == 0 && total.low <= 0x7FFFFFFFFFFFFFFFULL)
		        ? static_cast<int64_t>(total.low)
		        : (total.high == -1 && total.low >= 0x8000000000000000ULL ? static_cast<int64_t>(total.low) : 0);
		result.tuples = tuples;
		result.records = rep.RecordCount();
		result.bytes = rep.BytesAllocated();
		result.ok = true;
	} catch (const MemoryLimitExceeded &error) {
		result.error = error.what();
		result.out_of_memory = true;
	} catch (const std::exception &error) {
		result.error = error.what();
	}
	return result;
}

ExecuteResult ExecuteSum(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                         size_t sum_relation, size_t sum_column, PathStrategy strategy) {
	ExecuteResult result;
	if (!plan.complete) {
		result.error = plan.reason.empty() ? "no plan" : plan.reason;
		return result;
	}
	if (sum_relation >= graph.RelationCount() || sum_column >= graph.column_counts[sum_relation]) {
		result.error = "summed column is not a column of the query";
		return result;
	}
	// The last join cannot be fused: fusing counts the output as it goes, and a
	// sum needs the column's values, which only exist once the representation
	// does.
	auto built = BuildRepresentation(graph, plan, source, mode, strategy);
	if (!built.ok) {
		result.error = built.error;
		result.out_of_memory = built.out_of_memory;
		return result;
	}
	try {
		const auto &rep = built.relation->Rep();
		const auto attribute = AttributeOf(graph, sum_relation, sum_column);
		int64_t total = 0;
		int64_t tuples = 0;
		rep.ForEachRoot([&](Record root) {
			total = CheckedCardinalityAdd(total, rep.SubtreeSum(root, attribute));
			// Counted alongside, because a total of zero says nothing about
			// whether anything was summed, and SQL answers NULL when nothing was.
			tuples = CheckedCardinalityAdd(tuples, static_cast<int64_t>(rep.SubtreeSize(root)));
		});
		result.count = total;
		result.tuples = tuples;
		result.records = rep.RecordCount();
		result.bytes = rep.BytesAllocated();
		result.ok = true;
	} catch (const MemoryLimitExceeded &error) {
		result.error = error.what();
		result.out_of_memory = true;
	} catch (const std::exception &error) {
		result.error = error.what();
	}
	return result;
}

namespace {

//! Where each grouping column lives in the representation, and where its value
//! goes in the emitted row.
struct GroupSite {
	AttributeId attribute = 0;
	LevelId level = 0;
	size_t position = 0;
};

//! Groups a representation by several columns at once, without enumerating a
//! tuple.
//!
//! The single-key case is easy because a root record *is* a group. Several keys
//! are not, and the plan (§10.1) expects the hard shape to be declined: keys
//! scattered across independent sibling branches need the cross product of
//! those branches. But that cross product is exactly what the representation is
//! made of -- siblings are independent, which is the entire premise -- so the
//! branches can be walked together rather than flattened.
//!
//! The walk fixes a key's value as it descends past it and multiplies by any
//! slot that holds no key at all, because such a slot only decides *how many*
//! tuples a combination stands for. When every key below has been fixed, what
//! remains is one group and the subtree size beneath it.
//!
//! The cost is the number of groups, which is the size of the answer the caller
//! asked for -- not the size of the join.
class GroupFold {
public:
	//! What a set of tuples contributes: how many there are, and what each
	//! summed column sums to over them. The counts and the sums are carried
	//! together because every sum needs the count.
	//!
	//! Independent sets combine by the semiring product, not by multiplying
	//! twice: (c1,s1) x (c2,s2) = (c1*c2, s1*c2 + c1*s2). Each side's values
	//! appear once per tuple of the other, which is why a sum cannot be carried
	//! as a scalar weight -- propagating only the counts loses every value in
	//! every branch but the last.
	//!
	//! Several sums ride along in one walk. The product above is per column and
	//! the columns do not interact, so `sums[i]` needs no more than the shared
	//! counts to be exact. A fixed array rather than a vector: one of these is
	//! made per record visited, and an allocation there would cost more than the
	//! fold it belongs to.
	struct Fold {
		int64_t count = 1;
		int64_t sums[kMaxAggregates] = {};
		//! Per aggregate, the extreme so far, at the identity when nothing has
		//! been seen. Set up by `Identity()` rather than default-initialised,
		//! since min and max have different identities.
		int64_t extremes[kMaxAggregates] = {};
	};

	//! A fold standing for one tuple carrying nothing yet.
	Fold Identity() const {
		Fold fold;
		for (size_t i = 0; i < aggregates.size(); i++) {
			fold.extremes[i] = ExtremeIdentity(i);
		}
		return fold;
	}
	int64_t ExtremeIdentity(size_t i) const {
		return aggregates[i].kind == Aggregate::MAX ? std::numeric_limits<int64_t>::min()
		                                            : std::numeric_limits<int64_t>::max();
	}
	int64_t Better(size_t i, int64_t a, int64_t b) const {
		return aggregates[i].kind == Aggregate::MAX ? std::max(a, b) : std::min(a, b);
	}
	static bool IsExtreme(Aggregate kind) {
		return kind == Aggregate::MIN || kind == Aggregate::MAX;
	}
	//! AVG folds exactly as SUM does; only the emission differs.
	static bool IsSum(Aggregate kind) {
		return kind == Aggregate::SUM || kind == Aggregate::AVG;
	}

	Fold Combine(const Fold &a, const Fold &b) const {
		Fold result;
		result.count = CheckedCardinalityMul(a.count, b.count);
		for (size_t i = 0; i < aggregates.size(); i++) {
			if (IsExtreme(aggregates[i].kind)) {
				// The product of two branches is empty if either is, and then
				// no value from the other belongs to any tuple. Taking the
				// better of the two here regardless is the §4.6 hazard in its
				// min/max form: it would report a value no tuple carries.
				result.extremes[i] =
				    (a.count == 0 || b.count == 0) ? ExtremeIdentity(i) : Better(i, a.extremes[i], b.extremes[i]);
				continue;
			}
			result.sums[i] = CheckedCardinalityAdd(CheckedCardinalityMul(a.sums[i], b.count),
			                                       CheckedCardinalityMul(a.count, b.sums[i]));
		}
		return result;
	}

	Fold Alternatives(const Fold &a, const Fold &b) const {
		Fold result;
		result.count = CheckedCardinalityAdd(a.count, b.count);
		for (size_t i = 0; i < aggregates.size(); i++) {
			if (IsExtreme(aggregates[i].kind)) {
				// A union needs no emptiness check: an empty alternative holds
				// the identity, which loses to anything real.
				result.extremes[i] = Better(i, a.extremes[i], b.extremes[i]);
				continue;
			}
			result.sums[i] = CheckedCardinalityAdd(a.sums[i], b.sums[i]);
		}
		return result;
	}

	GroupFold(const FRepresentation &rep, std::vector<GroupSite> sites, size_t width,
	          std::vector<GroupAggregate> aggregates, std::vector<AttributeId> sum_attributes)
	    : rep(rep), sites(std::move(sites)), width(width), aggregates(std::move(aggregates)),
	      sum_attributes(std::move(sum_attributes)) {
		// Which levels have a key at or below them, so a slot leading nowhere
		// useful can be collapsed into a multiplier instead of walked.
		const auto &layout = rep.GetLayout();
		carries.assign(layout.LevelCount(), false);
		for (size_t level = layout.LevelCount(); level-- > 0;) {
			for (const auto &site : this->sites) {
				if (site.level == level) {
					carries[level] = true;
				}
			}
			for (const auto &slot : layout.Level(static_cast<LevelId>(level)).slots) {
				if (carries[slot.child_level]) {
					carries[level] = true;
				}
			}
		}
	}

	void Run(std::map<std::vector<int64_t>, Fold> &out) {
		std::vector<int64_t> row(width, 0);
		const Fold identity = Identity();
		rep.ForEachRoot([&](Record root) { Descend(root, row, identity, out); });
	}

private:
	//! What a slot's children stand for, taken together: they are alternatives,
	//! so they add.
	Fold SlotTotals(Record record, size_t slot_index) const {
		Fold total = Identity();
		total.count = 0;
		rep.ForEachChild(record, slot_index, [&](Record child) {
			Fold child_fold = Identity();
			child_fold.count = rep.SubtreeSize(child);
			for (size_t i = 0; i < aggregates.size(); i++) {
				if (IsSum(aggregates[i].kind)) {
					child_fold.sums[i] = rep.SubtreeSum(child, sum_attributes[i]);
				} else if (IsExtreme(aggregates[i].kind)) {
					child_fold.extremes[i] =
					    rep.SubtreeExtreme(child, sum_attributes[i], aggregates[i].kind == Aggregate::MAX);
				}
			}
			total = Alternatives(total, child_fold);
		});
		return total;
	}

	//! This record's own contribution before any child is considered: one tuple
	//! so far, carrying its own value if the summed column lives here.
	Fold Own(Record record) const {
		Fold fold = Identity();
		// Its multiplicity, so a grouped record contributes the tuples it
		// stands for rather than one of them.
		fold.count = rep.HasWeights() ? rep.GetWeight(record) : 1;
		const auto &level = rep.GetLayout().Level(record.Level());
		for (size_t i = 0; i < aggregates.size(); i++) {
			const bool extreme = IsExtreme(aggregates[i].kind);
			if (!IsSum(aggregates[i].kind) && !extreme) {
				continue;
			}
			for (const auto &entry : level.payload) {
				if (entry.attribute == sum_attributes[i]) {
					if (extreme) {
						// Multiplicity is irrelevant to an extreme: one copy of
						// the value and a thousand have the same minimum.
						fold.extremes[i] = rep.GetValue(record, sum_attributes[i]);
					} else {
						// Each of the tuples this record stands for carries the
						// value, so the sum is the value that many times.
						fold.sums[i] = CheckedCardinalityMul(rep.GetValue(record, sum_attributes[i]), fold.count);
					}
					break;
				}
			}
		}
		return fold;
	}

	void Descend(Record record, std::vector<int64_t> &row, const Fold &incoming,
	             std::map<std::vector<int64_t>, Fold> &out) {
		if (incoming.count == 0 || rep.SubtreeSize(record) == 0) {
			// An empty subtree denotes no tuples, so it names no group.
			return;
		}
		for (const auto &site : sites) {
			if (site.level == record.Level()) {
				row[site.position] = rep.GetValue(record, site.attribute);
			}
		}
		const auto &level = rep.GetLayout().Level(record.Level());

		// Slots with no key beneath them cannot split a group, so they fold in
		// now: they decide how many tuples a combination stands for, and what
		// those tuples contribute.
		Fold here = Combine(incoming, Own(record));
		std::vector<size_t> walk;
		for (size_t slot_index = 0; slot_index < level.slots.size(); slot_index++) {
			if (carries[level.slots[slot_index].child_level]) {
				walk.push_back(slot_index);
				continue;
			}
			here = Combine(here, SlotTotals(record, slot_index));
		}
		if (here.count == 0) {
			return;
		}
		if (walk.empty()) {
			// Every key is fixed, so this subtree is one group's worth of tuples.
			//
			// Inserted with an explicit zero rather than through operator[]:
			// Fold's default is the *multiplicative* identity, count 1, because
			// that is what Combine needs -- and using it as an accumulator would
			// add a tuple to every group that nothing counted.
			//
			// The extremes need the same care for the same reason, and are the
			// one case where zero is not a safe default: an accumulator holding
			// 0 wins every min against a positive column and loses every max
			// against a negative one. `Identity()` gives each aggregate the one
			// that loses to everything.
			auto found = out.find(row);
			if (found == out.end()) {
				Fold empty = Identity();
				empty.count = 0;
				found = out.emplace(row, empty).first;
			}
			found->second = Alternatives(found->second, here);
			return;
		}
		Cross(record, walk, 0, row, here, out);
	}

	//! The cross product over the slots that do carry keys.
	//!
	//! Each contributes its own groups and the combinations are the product of
	//! them, which is precisely the case §10.1 expected to be declined. It is
	//! computable because siblings are independent -- the property the whole
	//! representation is built on -- so a branch can be grouped on its own and
	//! its groups combined with the others afterwards.
	void Cross(Record record, const std::vector<size_t> &walk, size_t index, std::vector<int64_t> &row,
	           const Fold &incoming, std::map<std::vector<int64_t>, Fold> &out) {
		const bool last = (index + 1 == walk.size());
		rep.ForEachChild(record, walk[index], [&](Record child) {
			if (last) {
				Descend(child, row, incoming, out);
				return;
			}
			// Group this branch by itself, then carry each of its groups into
			// the remaining branches. Combining with the semiring product is
			// what keeps a branch's values from being lost behind another
			// branch's counts.
			std::map<std::vector<int64_t>, Fold> nested;
			std::vector<int64_t> child_row = row;
			Fold identity;
			Descend(child, child_row, identity, nested);
			for (const auto &entry : nested) {
				std::vector<int64_t> merged = entry.first;
				Cross(record, walk, index + 1, merged, Combine(incoming, entry.second), out);
			}
		});
	}

	const FRepresentation &rep;
	std::vector<GroupSite> sites;
	size_t width;
	std::vector<GroupAggregate> aggregates;
	//! Parallel to `aggregates`; meaningful only where the kind is SUM.
	std::vector<AttributeId> sum_attributes;
	std::vector<bool> carries;
};

GroupCountResult GroupBy(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                         const std::vector<GroupKey> &keys, const std::vector<GroupAggregate> &aggregates,
                         PathStrategy strategy) {
	GroupCountResult result;
	if (!plan.complete) {
		result.error = plan.reason.empty() ? "no plan" : plan.reason;
		return result;
	}
	if (aggregates.empty()) {
		result.error = "no aggregate to compute";
		return result;
	}
	if (aggregates.size() > kMaxAggregates) {
		result.error = "more than " + std::to_string(kMaxAggregates) + " aggregates in one query";
		return result;
	}
	for (const auto &aggregate : aggregates) {
		if (aggregate.kind == Aggregate::SUM_PRODUCT) {
			// Refused rather than mishandled: IsSum() would treat it as a plain
			// sum of x and answer a different question (D80).
			result.error = "sum of a product is not grouped; use ExecuteProductSum";
			return result;
		}
		if (aggregate.kind != Aggregate::COUNT && (aggregate.relation >= graph.RelationCount() ||
		                                           aggregate.column >= graph.column_counts[aggregate.relation])) {
			result.error = "summed column is not a column of the query";
			return result;
		}
	}
	for (const auto &key : keys) {
		if (key.relation >= graph.RelationCount() || key.column >= graph.column_counts[key.relation]) {
			result.error = "grouping column is not a column of the query";
			return result;
		}
	}
	auto built = BuildRepresentation(graph, plan, source, mode, strategy);
	if (!built.ok) {
		result.error = built.error;
		result.out_of_memory = built.out_of_memory;
		return result;
	}
	try {
		const auto &accumulated = *built.relation;
		auto &classes = *built.classes;
		const auto &layout = accumulated.GetLayout();

		// Find where each key's values are actually stored. An equi-join's keys
		// are equal by definition, so grouping on either side of one names the
		// same groups -- and the merge is free to have kept only one of them.
		std::vector<GroupSite> sites;
		for (size_t position = 0; position < keys.size(); position++) {
			const auto wanted = AttributeOf(graph, keys[position].relation, keys[position].column);
			bool found = false;
			for (size_t level = 0; level < layout.LevelCount() && !found; level++) {
				for (const auto &entry : layout.Level(static_cast<LevelId>(level)).payload) {
					if (entry.attribute == wanted || classes.SameClass(entry.attribute, wanted)) {
						sites.push_back(GroupSite {entry.attribute, static_cast<LevelId>(level), position});
						found = true;
						break;
					}
				}
			}
			if (!found) {
				result.error = "grouping column is not stored anywhere in the representation";
				return result;
			}
		}

		std::vector<AttributeId> sum_attributes(aggregates.size(), AttributeId(0));
		for (size_t i = 0; i < aggregates.size(); i++) {
			if (aggregates[i].kind != Aggregate::COUNT) {
				sum_attributes[i] = AttributeOf(graph, aggregates[i].relation, aggregates[i].column);
			}
		}
		GroupFold fold(accumulated.Rep(), std::move(sites), keys.size(), aggregates, std::move(sum_attributes));
		std::map<std::vector<int64_t>, GroupFold::Fold> totals;
		fold.Run(totals);

		for (const auto &entry : totals) {
			// A combination no tuple satisfies is not a group. Emptiness is
			// judged by the count even when the answer is a sum, or a group
			// whose values happen to total zero would vanish.
			if (entry.second.count <= 0) {
				continue;
			}
			std::vector<int64_t> values;
			values.reserve(aggregates.size());
			for (size_t i = 0; i < aggregates.size(); i++) {
				switch (aggregates[i].kind) {
				case Aggregate::SUM:
				case Aggregate::AVG:
					values.push_back(entry.second.sums[i]);
					break;
				case Aggregate::MIN:
				case Aggregate::MAX:
					values.push_back(entry.second.extremes[i]);
					break;
				default:
					values.push_back(entry.second.count);
					break;
				}
			}
			result.groups.emplace_back(entry.first, std::move(values));
			result.counts.push_back(entry.second.count);
		}
		result.records = accumulated.Rep().RecordCount();
		result.bytes = accumulated.Rep().BytesAllocated();
		result.ok = true;
	} catch (const MemoryLimitExceeded &error) {
		result.error = error.what();
		result.out_of_memory = true;
	} catch (const std::exception &error) {
		result.error = error.what();
	}
	return result;
}

} // namespace

GroupCountResult ExecuteGroupBy(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                const std::vector<GroupKey> &keys, const std::vector<GroupAggregate> &aggregates,
                                PathStrategy strategy) {
	return GroupBy(graph, plan, source, mode, keys, aggregates, strategy);
}

GroupCountResult ExecuteGroupCount(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                   const std::vector<GroupKey> &keys, PathStrategy strategy) {
	return GroupBy(graph, plan, source, mode, keys, {GroupAggregate {Aggregate::COUNT, 0, 0}}, strategy);
}

GroupCountResult ExecuteGroupSum(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                 const std::vector<GroupKey> &keys, size_t sum_relation, size_t sum_column,
                                 PathStrategy strategy) {
	return GroupBy(graph, plan, source, mode, keys, {GroupAggregate {Aggregate::SUM, sum_relation, sum_column}},
	               strategy);
}

ExecuteResult ExecuteCountAtMost(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                 size_t limit, PathStrategy strategy) {
	ExecuteResult result;
	if (limit == 0) {
		// `LIMIT 0` keeps no tuples, and reading the input to learn that would
		// be the one case where this operator is strictly worse than not
		// existing. Said as zero buckets read, since none were.
		result.ok = true;
		result.count = 0;
		result.slices = 0;
		return result;
	}
	// Enough buckets that reaching the limit early is worth something, few
	// enough that a join too small to reach it does not pay for many passes to
	// learn that. The buckets are examined in order and nothing about the
	// answer depends on which one supplies the tuples: their counts are added,
	// and the total is what the limit clamps.
	static const size_t kProbeSlices = 16;
	auto key_column = ChooseSliceColumns(graph, &plan);
	const size_t slices = key_column.empty() ? 1 : kProbeSlices;

	// Sixteen, and not the much finer partition that looks obviously better.
	//
	// The measured problem is granularity: DuckDB streams its probe and stops at
	// the first *tuple*, while this stops at the first non-empty *bucket*, and a
	// sixteenth of a dense join is still an enormous join to build in order to
	// learn it is not empty. So looking first in two buckets of a partition 64
	// times finer was tried, expecting the dense case to end there for two
	// passes over the input.
	//
	// It made things worse and was reverted (D53). Gated over the corpus it cost
	// 4.74s against 4.60s for this; on watdiv, the dataset it was aimed at, it
	// bought 10% (46.8s to 42.1s) because what is expensive there is the pass
	// and not the bucket -- while on epinions, where the gate actually fires, it
	// was 2.6x worse for exactly the same reason. A partition finer than the
	// key's distinct count also mostly produces empty buckets, and the execution
	// path has no cheap distinct count to size one against.
	int64_t total = 0;
	for (size_t slice = 0; slice < slices; slice++) {
		auto part = ExecuteCountSliceWithinMemory(graph, plan, source, mode, slice, slices, strategy);
		if (!part.ok) {
			return part;
		}
		total = CheckedCardinalityAdd(total, part.count);
		// The high-water mark across the buckets, not their sum: they are built
		// one at a time and the space is reused.
		result.records = part.records > result.records ? part.records : result.records;
		result.bytes = part.bytes > result.bytes ? part.bytes : result.bytes;
		if (static_cast<uint64_t>(total) >= static_cast<uint64_t>(limit)) {
			result.ok = true;
			result.count = static_cast<int64_t>(limit);
			// How much of the partition was read before the answer was known.
			result.slices = slice + 1;
			return result;
		}
	}
	result.ok = true;
	result.count = total;
	result.slices = slices;
	return result;
}

ExecuteResult ExecuteExists(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                            PathStrategy strategy) {
	// One tuple is all the evidence EXISTS needs, and stopping at it is the
	// whole difference from counting.
	return ExecuteCountAtMost(graph, plan, source, mode, 1, strategy);
}

ExecuteResult ExecuteCountWithinMemory(const QueryGraph &graph, const Plan &plan, RelationSource &source, JoinMode mode,
                                       PathStrategy strategy) {
	auto result = ExecuteCount(graph, plan, source, mode, strategy);
	if (result.ok || !result.out_of_memory) {
		return result;
	}
	// Geometric, so the retries together cost about as much as the pass that
	// finally works rather than a multiple of it. The ceiling is where slicing
	// has stopped being the answer: past it the query is not too big overall but
	// too skewed, with one key value whose own subtree does not fit, and no
	// number of buckets separates a value from itself.
	static const size_t kSliceSteps[] = {8, 64, 512, 4096};
	for (auto slices : kSliceSteps) {
		auto sliced = ExecuteCountSliced(graph, plan, source, mode, slices, strategy);
		if (sliced.ok || !sliced.out_of_memory) {
			return sliced;
		}
		result = sliced;
	}
	// Past the ceiling, which is where the query is not too big overall but too
	// skewed -- one key value whose own subtree does not fit. A partition of a
	// different key is what separates it, and taking the whole input as one
	// bucket is how this path asks for that (D55).
	auto second = CountBySecondKey(graph, plan, source, mode, 0, 1, strategy);
	if (second.ok) {
		return second;
	}
	return result;
}

} // namespace factorize
