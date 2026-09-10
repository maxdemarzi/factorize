//===----------------------------------------------------------------------===//
//                         factorize
//
// core/arena.hpp
//
// Chunked bump allocator with stable addresses.
//
// Stability is the load-bearing property, not speed. Bottom-inserts append to
// the child list of an *arbitrary already-materialized* record at any later
// point (plan section 2.3), so a pointer handed out early must still be valid
// after millions of subsequent insertions. That rules out any structure that
// reallocates -- which is precisely why Kalumin & Deshpande's contiguous
// offset-based layout cannot support bottom-inserts, and why they measure at
// 0.98x (FINDINGS.md F4).
//
// Chunks double in size and are never moved or freed until the whole arena is
// released, mirroring the paper's FastDeque.
//
// No DuckDB headers (plan section 4).
//
//===----------------------------------------------------------------------===//

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace factorize {

//! Raw storage. This is `unsigned char` rather than `Byte` for one
//! reason: DuckDB compiles at C++11, and a translation unit that includes
//! DuckDB headers at C++17 turns their `static constexpr` members into inline
//! definitions, which then collide at link time with the out-of-line ones
//! DuckDB's own C++11 build emits (`multiple definition of
//! duckdb::LogicalType::BIGINT`). `Byte` was the core's only C++17
//! dependency, so dropping it lets the whole extension build at one standard.
using Byte = unsigned char;

//! Thrown when an allocation would exceed the cap.
//!
//! Its own type because it is the one failure a caller can do something about:
//! the same count, computed over a partition of the join key, needs a fraction
//! of the memory at once. Every other error means the query cannot be answered
//! by this engine at all.
struct MemoryLimitExceeded : std::runtime_error {
	explicit MemoryLimitExceeded(const std::string &what) : std::runtime_error(what) {
	}
};

//! What one slice of the engine may hold, summed across everything it builds.
//!
//! The per-slice limit used to be applied to each structure separately -- each
//! f-representation, each hash table, the top-insert snapshot arena -- and
//! never to their sum. One join holds four or five of those at once: both
//! inputs, the output, the index, the snapshots. So the limit bounded every
//! piece and not the whole, and measured, the engine's peak followed DuckDB's
//! entire memory_limit at one thread and at eight alike, twice the half it
//! had budgeted. With the section 7.5 fallback running on top of that, it
//! took a 31GB machine down (D43). One account per slice is what makes
//! "per slice" mean what it says.
struct SliceBudget {
	//! 0 means unlimited.
	size_t limit = 0;
	//! Bytes currently reserved by every arena and index charged to it. Atomic
	//! because an arena releases to the budget it charged, and nothing forbids
	//! that happening on another thread.
	std::atomic<size_t> used {0};
	//! High-water mark of `used` since the limit was last set. A measurement,
	//! not a control: it is what says whether the memory a process holds is
	//! memory this account knows about.
	std::atomic<size_t> peak {0};

	//! Offers `now` as a new high-water mark.
	void Note(size_t now) {
		size_t seen = peak.load(std::memory_order_relaxed);
		while (now > seen && !peak.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
		}
	}
};

//! This thread's budget. A slice runs start to finish on one thread, so a
//! thread-local is a per-slice account without threading one through every
//! constructor -- the same trade the memory limit in join.cpp already makes.
inline SliceBudget &ThreadBudget() {
	static thread_local SliceBudget budget;
	return budget;
}

class Arena {
public:
	explicit Arena(size_t first_chunk_bytes = 64 * 1024)
	    : next_chunk_bytes(first_chunk_bytes < kMinChunk ? kMinChunk : first_chunk_bytes) {
	}

	Arena(const Arena &) = delete;
	Arena &operator=(const Arena &) = delete;
	//! Moves carry the charge with the memory: a moved-from arena must not
	//! release what it no longer holds, or the budget is credited twice.
	Arena(Arena &&other) noexcept
	    : chunks(std::move(other.chunks)), used(other.used), capacity(other.capacity), allocated(other.allocated),
	      reserved(other.reserved), next_chunk_bytes(other.next_chunk_bytes), memory_limit(other.memory_limit),
	      budget(other.budget) {
		other.Forget();
	}
	Arena &operator=(Arena &&other) noexcept {
		if (this != &other) {
			Release();
			chunks = std::move(other.chunks);
			used = other.used;
			capacity = other.capacity;
			allocated = other.allocated;
			reserved = other.reserved;
			next_chunk_bytes = other.next_chunk_bytes;
			memory_limit = other.memory_limit;
			budget = other.budget;
			other.Forget();
		}
		return *this;
	}
	~Arena() {
		Release();
	}

	//! Allocates `bytes`, zeroed, aligned for any scalar type. The returned
	//! address stays valid for the lifetime of the arena.
	Byte *Allocate(size_t bytes) {
		const size_t aligned = (bytes + kAlign - 1) & ~(kAlign - 1);
		// FRepresentation's own segment allocator has checked itself against a
		// caller-supplied limit from the start; this arena, used bare by
		// ChainingHashTable's entries and by the top-insert snapshot arena in
		// join.cpp, did not, and a large build side with a small output could
		// exhaust memory with no check at all regardless of how small the
		// counted/materialized result turned out to be. Checked here rather
		// than only in FRepresentation so every Arena is covered by the one
		// change, not by remembering to wrap each new caller individually.
		if (memory_limit != 0 && allocated + aligned > memory_limit) {
			throw MemoryLimitExceeded("arena exceeded its memory limit");
		}
		if (used + aligned > capacity) {
			Grow(aligned);
		}
		Byte *result = chunks.back().get() + used;
		used += aligned;
		allocated += aligned;
		return result;
	}

	//! Caps total bytes this arena will hand out; 0 (the default) means
	//! unlimited. Set once, before any allocation the caller wants covered --
	//! typically right after construction, from the same limit
	//! FRepresentation::SetMemoryLimit uses.
	void SetMemoryLimit(size_t bytes) {
		memory_limit = bytes;
	}

	//! Total bytes handed out. Used for memory accounting; Phase 2 reports this
	//! to DuckDB's BufferManager so f-representations count against
	//! memory_limit rather than being invisible.
	size_t BytesAllocated() const {
		return allocated;
	}
	//! Bytes actually reserved from the system, including unused chunk tail.
	size_t BytesReserved() const {
		return reserved;
	}

	void Reset() {
		Release();
		chunks.clear();
		used = capacity = allocated = reserved = 0;
		next_chunk_bytes = kMinChunk;
	}

private:
	static constexpr size_t kAlign = alignof(std::max_align_t);
	static constexpr size_t kMinChunk = 4096;

	void Grow(size_t at_least) {
		size_t bytes = next_chunk_bytes;
		while (bytes < at_least) {
			bytes *= 2;
		}
		// The slice as a whole, not this arena alone, and before the memory
		// exists. Charged only after it does, so an allocation that fails
		// charges nothing.
		SliceBudget &slice = budget != nullptr ? *budget : ThreadBudget();
		if (slice.limit != 0 && slice.used.load(std::memory_order_relaxed) + bytes > slice.limit) {
			throw MemoryLimitExceeded("the engine exceeded its per-slice memory budget");
		}
		auto chunk = std::unique_ptr<Byte[]>(new Byte[bytes]);
		std::memset(chunk.get(), 0, bytes);
		chunks.push_back(std::move(chunk));
		used = 0;
		capacity = bytes;
		reserved += bytes;
		slice.Note(slice.used.fetch_add(bytes, std::memory_order_relaxed) + bytes);
		budget = &slice;
		// Double until a ceiling, so a large f-representation does not end up
		// making thousands of small allocations.
		if (next_chunk_bytes < (32u << 20)) {
			next_chunk_bytes = bytes * 2;
		}
	}

	//! Returns this arena's charge to the budget it was taken from.
	void Release() {
		if (budget != nullptr && reserved != 0) {
			budget->used.fetch_sub(reserved, std::memory_order_relaxed);
		}
		budget = nullptr;
	}
	//! Drops ownership without releasing, for a moved-from arena.
	void Forget() {
		chunks.clear();
		used = capacity = allocated = reserved = 0;
		budget = nullptr;
	}

	std::vector<std::unique_ptr<Byte[]>> chunks;
	size_t used = 0;
	size_t capacity = 0;
	size_t allocated = 0;
	size_t reserved = 0;
	size_t next_chunk_bytes;
	//! 0 = unlimited.
	size_t memory_limit = 0;
	//! The slice budget this arena charged; null until its first chunk.
	SliceBudget *budget = nullptr;
};

} // namespace factorize
