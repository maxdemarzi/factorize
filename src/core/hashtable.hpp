//===----------------------------------------------------------------------===//
//                         factorize
//
// core/hashtable.hpp
//
// Chaining hash table whose values are handles into an f-representation.
//
// Shape follows the paper's section 4.4, which in turn follows Leis et al.'s
// morsel-driven build: entries are collected first, then a directory of exactly
// the right size is allocated once and the chains are linked. Nothing is
// rehashed, so entry addresses -- and therefore the chains -- are stable.
//
// The decisive design point (section 4.4): the table stores only the flat join
// key alongside a *handle* to the corresponding f-representation, never a
// materialized tuple. That is what keeps the join from flattening.
//
// Phase 1 is single-threaded, but the per-partition collection vectors are
// already the shape Phase 4 needs: each thread fills its own, and Finalize()
// links them all into one directory.
//
// No DuckDB headers (plan section 4).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "arena.hpp"
#include "frep.hpp"

#include <cstdint>
#include <vector>

namespace factorize {

//! Mixes a key so the *high* bits carry entropy: the directory slot is taken
//! from the top of the hash, following the paper's `hash >> shift`.
inline uint64_t HashKey(uint64_t key) {
	// splitmix64 finalizer.
	uint64_t x = key + 0x9e3779b97f4a7c15ull;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
	return x ^ (x >> 31);
}

//! `Value` is whatever a match needs to reach: for a top-insert that is a
//! snapshot of the build side's flattening context; for a bottom-insert it is
//! the already-materialized output record probes append to.
template <typename Value>
class ChainingHashTable {
public:
	struct Entry {
		Entry *next;
		//! The join key, kept in full so chain walks can reject hash collisions
		//! exactly, as the paper's generated code does.
		uint64_t key;
		//! Never a materialized tuple.
		Value value;
	};

	//! Collects one entry. Cheap and allocation-stable; no directory yet.
	void Insert(uint64_t key, Value value) {
		auto *entry = reinterpret_cast<Entry *>(arena.Allocate(sizeof(Entry)));
		entry->next = nullptr;
		entry->key = key;
		entry->value = value;
		entries.push_back(entry);
	}

	//! Builds the directory and links the chains. Idempotent per build phase.
	void Finalize() {
		// Section 2.6: capacity is size + size/8 rounded up to a power of two,
		// which keeps the load factor under ~0.9 without a resize path.
		const size_t wanted = entries.size() + entries.size() / 8 + 1;
		size_t capacity = 1;
		unsigned bits = 0;
		while (capacity < wanted) {
			capacity <<= 1;
			bits++;
		}
		shift = 64 - bits;
		directory.assign(capacity, nullptr);
		mask = capacity - 1;

		for (auto *entry : entries) {
			const auto slot = Slot(entry->key);
			entry->next = directory[slot];
			directory[slot] = entry;
		}
		finalized = true;
	}

	//! First candidate for `key`, or nullptr. Walk on with FindNext.
	const Entry *Find(uint64_t key) const {
		if (!finalized || directory.empty()) {
			return nullptr;
		}
		const auto *entry = directory[Slot(key)];
		while (entry && entry->key != key) {
			entry = entry->next;
		}
		return entry;
	}

	//! Next candidate after `current`, skipping hash collisions.
	static const Entry *FindNext(const Entry *current, uint64_t key) {
		const auto *entry = current->next;
		while (entry && entry->key != key) {
			entry = entry->next;
		}
		return entry;
	}

	//! Calls `fn(Record)` for every value matching `key`.
	template <typename Fn>
	void ForEachMatch(uint64_t key, Fn &&fn) const {
		for (const auto *entry = Find(key); entry; entry = FindNext(entry, key)) {
			fn(entry->value);
		}
	}

	size_t Size() const {
		return entries.size();
	}
	size_t Capacity() const {
		return directory.size();
	}
	bool IsFinalized() const {
		return finalized;
	}
	size_t BytesAllocated() const {
		return arena.BytesAllocated() + directory.capacity() * sizeof(Entry *);
	}

	//! Caps the entry arena the same way FRepresentation caps its own; 0 means
	//! unlimited. One `Entry` is allocated per `Insert`, so an unbounded build
	//! side previously grew this arena with no check regardless of how small
	//! the eventual output was.
	void SetMemoryLimit(size_t bytes) {
		arena.SetMemoryLimit(bytes);
	}

private:
	size_t Slot(uint64_t key) const {
		// Take the high bits, as the paper does. `mask` guards the bits == 0
		// case, where shift would be 64 and the shift itself undefined.
		return shift == 64 ? 0 : (HashKey(key) >> shift) & mask;
	}

	Arena arena;
	std::vector<Entry *> entries;
	std::vector<Entry *> directory;
	size_t mask = 0;
	unsigned shift = 64;
	bool finalized = false;
};

} // namespace factorize
