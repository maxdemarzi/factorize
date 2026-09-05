//===----------------------------------------------------------------------===//
//                         factorize
//
// core/enumerate.hpp
//
// Flat tuples out of an f-representation, one at a time and on demand.
//
// Section 4.6: fix an entry at a node, iterate the required entries of its
// children, and take the cross product wherever a node has several child slots.
// Siblings are independent, which is the whole point of the representation, so
// "all combinations" is a nested loop and not a join.
//
// Two things make this worth having beyond completeness (plan sections 10.2 and
// 10.3). The honest one: emitting N flat tuples costs Omega(N) and no
// representation changes that -- what is saved is the *intermediate*
// materialisation, paid once at the end instead of at each of the n-1 joins.
// The interesting one: enumeration is incremental, so a caller that wants the
// first hundred tuples of a join with a trillion of them pays for a hundred.
// Stock DuckDB materialises the hash-join intermediates whatever the LIMIT
// says. That is a capability difference rather than a speed difference.
//
// The hazard section 4.6 warns about is the reason this is not a five-line
// recursion: bottom-inserts routinely leave a record with an *empty* child
// slot, and such a record contributes no tuples at all. Enumerating it as
// though it did would emit tuples that do not exist. Here the empty slot simply
// iterates nothing, so it drops out by construction -- the same arithmetic
// SubtreeSize does when it multiplies a slot total of zero.
//
// No DuckDB headers (plan section 4).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frep.hpp"

#include <cstdint>
#include <vector>

namespace factorize {

//! Where one attribute's value lands in the emitted tuple.
struct TupleColumn {
	AttributeId attribute = 0;
	//! Position in the emitted vector.
	size_t position = 0;
};

//! A borrowed "what to do once this subtree is fixed", without allocating.
//!
//! It has to be type-erased rather than a template parameter. The continuation
//! at depth d is "finish the parent's remaining slots, then *its* parent's",
//! so its type would grow with the depth of the tree -- and the depth is a
//! property of the data, not of the source. Templating it asks the compiler to
//! instantiate a family of functions whose size is known only at runtime, which
//! it cannot do. The indirect call per level is the price of the recursion
//! terminating at compile time.
class Continuation {
public:
	template <typename F>
	explicit Continuation(F &f) : state(&f), call(&Invoke<F>) {
	}
	void operator()() const {
		call(state);
	}

private:
	//! A named function template rather than a lambda: converting a lambda to a
	//! function pointer goes through a synthesised thunk that GCC then analyses
	//! as though its captured `this` could be null, and warns. This does the
	//! same work and says so plainly.
	template <typename F>
	static void Invoke(void *state) {
		(*static_cast<F *>(state))();
	}

	void *state;
	void (*call)(void *);
};

namespace internal {

template <typename Fn>
struct TupleWalker {
	const FRepresentation &rep;
	const std::vector<TupleColumn> &columns;
	std::vector<int64_t> &values;
	size_t limit;
	size_t &emitted;
	bool &stopped;
	Fn &emit;

	//! Writes `record`'s attributes into the tuple under construction, then
	//! enumerates the combinations of its children.
	void EnumerateRecord(Record record, const Continuation &next) {
		if (stopped) {
			return;
		}
		const auto &level = rep.GetLayout().Level(record.Level());
		for (size_t i = 0; i < level.payload.size(); i++) {
			for (const auto &column : columns) {
				if (column.attribute == level.payload[i].attribute) {
					values[column.position] = level.payload[i].type == ValueType::INT32
					                              ? static_cast<int64_t>(rep.GetInt32(record, i))
					                              : rep.GetInt64(record, i);
				}
			}
		}
		EnumerateSlots(record, 0, next);
	}

	//! The cross product over one record's child slots. A slot with no children
	//! iterates nothing, which is exactly right: such a record contributes no
	//! tuples, and inventing one is the section 4.6 hazard.
	void EnumerateSlots(Record record, size_t slot_index, const Continuation &next) {
		if (stopped) {
			return;
		}
		const auto &level = rep.GetLayout().Level(record.Level());
		if (slot_index == level.slots.size()) {
			next();
			return;
		}
		rep.ForEachChild(record, slot_index, [&](Record child) {
			if (stopped) {
				return;
			}
			auto rest = [&]() { EnumerateSlots(record, slot_index + 1, next); };
			EnumerateRecord(child, Continuation(rest));
		});
	}
};

} // namespace internal

//! Calls `emit(values)` for each flat tuple, stopping early if it returns false
//! or once `limit` tuples have been emitted (0 means no limit).
//!
//! `values` is reused between calls: a caller that wants to keep a tuple has to
//! copy it. Enumeration exists to avoid materialising the whole result, so
//! handing out a fresh vector per tuple would undo the point.
//!
//! Returns how many were emitted.
template <typename Fn>
size_t Enumerate(const FRepresentation &rep, const std::vector<TupleColumn> &columns, size_t limit, Fn &&emit) {
	std::vector<int64_t> values(columns.size(), 0);
	size_t emitted = 0;
	bool stopped = false;
	internal::TupleWalker<Fn> walker {rep, columns, values, limit, emitted, stopped, emit};

	rep.ForEachRoot([&](Record root) {
		if (stopped) {
			return;
		}
		auto complete = [&]() {
			if (!emit(values)) {
				stopped = true;
				return;
			}
			emitted++;
			if (limit != 0 && emitted >= limit) {
				stopped = true;
			}
		};
		walker.EnumerateRecord(root, Continuation(complete));
	});
	return emitted;
}

} // namespace factorize
