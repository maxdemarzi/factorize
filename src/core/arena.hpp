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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
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

class Arena {
public:
	explicit Arena(size_t first_chunk_bytes = 64 * 1024)
	    : next_chunk_bytes(first_chunk_bytes < kMinChunk ? kMinChunk : first_chunk_bytes) {
	}

	Arena(const Arena &) = delete;
	Arena &operator=(const Arena &) = delete;
	Arena(Arena &&) = default;
	Arena &operator=(Arena &&) = default;

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
			throw std::runtime_error("arena exceeded its memory limit");
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
		auto chunk = std::unique_ptr<Byte[]>(new Byte[bytes]);
		std::memset(chunk.get(), 0, bytes);
		chunks.push_back(std::move(chunk));
		used = 0;
		capacity = bytes;
		reserved += bytes;
		// Double until a ceiling, so a large f-representation does not end up
		// making thousands of small allocations.
		if (next_chunk_bytes < (32u << 20)) {
			next_chunk_bytes = bytes * 2;
		}
	}

	std::vector<std::unique_ptr<Byte[]>> chunks;
	size_t used = 0;
	size_t capacity = 0;
	size_t allocated = 0;
	size_t reserved = 0;
	size_t next_chunk_bytes;
	//! 0 = unlimited.
	size_t memory_limit = 0;
};

} // namespace factorize
