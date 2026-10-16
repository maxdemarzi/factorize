//===----------------------------------------------------------------------===//
//                         factorize
//
// src/core/int128.hpp
//
// A signed 128-bit integer, just wide enough for the product-sum fold.
//
// Why this exists rather than `__int128` or DuckDB's `hugeint_t`. D87 measured
// sum(x * y) on the excluded corpus and found it rescues 7 queries where min
// and max rescue 45, because the *answer* does not fit: a product of two ~1e5
// values summed over ~1e10 tuples is on the order of 1e20 and int64 stops at
// 9.2e18. Seventeen of twenty sampled failures were that, not slowness. So the
// fold needs a wider accumulator -- and neither obvious wide type is available
// here. `src/core` is deliberately free of DuckDB (scripts/core-test.sh builds
// it standalone, and the plan requires it), which rules out `hugeint_t`; and
// this project's CI builds a plain-MSVC Windows target, which has no
// `__int128` -- the same constraint that already rules out
// __builtin_mul_overflow for the int64 checks in frep.hpp.
//
// Deliberately minimal. Only what the bilinear combine performs: widening a
// 64-bit value, adding, multiplying by a 64-bit value, comparing, and
// rendering a decimal string. No division, no bit shifts beyond what those
// need. A general 128-bit type would be more code to get wrong in a file whose
// whole purpose is arithmetic that must not be wrong.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace factorize {

//! Two's-complement 128-bit signed integer as {high, low}.
//!
//! `high` carries the sign, exactly as the top word of a native two's
//! complement value does, so negation, addition and comparison are the
//! ordinary word-at-a-time algorithms rather than sign-magnitude special
//! cases. Sign-magnitude was the first attempt and it needs a branch per
//! operation for "signs differ"; every one of those is a place to be wrong.
struct Int128 {
	uint64_t low = 0;
	int64_t high = 0;

	Int128() = default;
	//! Explicit: an int64 silently widening into a 128-bit accumulator is the
	//! kind of conversion this type exists to make deliberate. Every widening
	//! in the fold goes through MultiplyWide or is written out.
	explicit Int128(int64_t value) : low(static_cast<uint64_t>(value)), high(value < 0 ? -1 : 0) {
	}
	Int128(int64_t high_word, uint64_t low_word) : low(low_word), high(high_word) {
	}

	bool IsNegative() const {
		return high < 0;
	}
	bool IsZero() const {
		return low == 0 && high == 0;
	}
	bool operator==(const Int128 &other) const {
		return low == other.low && high == other.high;
	}
	bool operator!=(const Int128 &other) const {
		return !(*this == other);
	}
	bool operator<(const Int128 &other) const {
		return high != other.high ? high < other.high : low < other.low;
	}
};

//! Two's-complement negation: invert and add one, carrying into the high word.
inline Int128 Negate(const Int128 &value) {
	Int128 out;
	out.low = ~value.low + 1;
	out.high = ~value.high + (out.low == 0 ? 1 : 0);
	return out;
}

//! `a + b`, throwing rather than wrapping.
//!
//! Overflow in two's complement addition is exactly "both operands share a
//! sign and the result does not", which is one test on the high words and no
//! case analysis.
inline Int128 CheckedAdd128(const Int128 &a, const Int128 &b) {
	Int128 out;
	out.low = a.low + b.low;
	const uint64_t carry = out.low < a.low ? 1 : 0;
	out.high = static_cast<int64_t>(static_cast<uint64_t>(a.high) + static_cast<uint64_t>(b.high) + carry);
	if ((a.high < 0) == (b.high < 0) && (out.high < 0) != (a.high < 0)) {
		throw std::runtime_error("factorized sum exceeds the representable range (int128 overflow)");
	}
	return out;
}

//! The exact 128-bit product of two 64-bit values. Cannot overflow:
//! |a*b| <= 2^126 for 64-bit operands, which a 128-bit result holds.
//!
//! Computed on magnitudes in 32-bit limbs, because that is the widest partial
//! product that fits a uint64_t without the platform-specific wide-multiply
//! intrinsics this file exists to avoid.
inline Int128 MultiplyWide(int64_t a, int64_t b) {
	const bool negative = (a < 0) != (b < 0);
	// Magnitudes as unsigned, which also handles INT64_MIN: negating it
	// overflows as a signed value but is exact as an unsigned one.
	const uint64_t ua = a < 0 ? ~static_cast<uint64_t>(a) + 1 : static_cast<uint64_t>(a);
	const uint64_t ub = b < 0 ? ~static_cast<uint64_t>(b) + 1 : static_cast<uint64_t>(b);

	const uint64_t a_lo = ua & 0xFFFFFFFFULL, a_hi = ua >> 32;
	const uint64_t b_lo = ub & 0xFFFFFFFFULL, b_hi = ub >> 32;

	const uint64_t ll = a_lo * b_lo;
	const uint64_t lh = a_lo * b_hi;
	const uint64_t hl = a_hi * b_lo;
	const uint64_t hh = a_hi * b_hi;

	const uint64_t middle = (ll >> 32) + (lh & 0xFFFFFFFFULL) + (hl & 0xFFFFFFFFULL);
	Int128 magnitude;
	magnitude.low = (ll & 0xFFFFFFFFULL) | (middle << 32);
	magnitude.high = static_cast<int64_t>(hh + (lh >> 32) + (hl >> 32) + (middle >> 32));
	return negative ? Negate(magnitude) : magnitude;
}

//! `a * b` where `a` is already 128 bits, throwing rather than wrapping.
//!
//! Schoolbook multiplication of the magnitudes in 32-bit limbs -- four for
//! `a`, two for `b`, six for the product -- because a partial product of two
//! 32-bit limbs is the widest thing that fits a uint64_t without the
//! wide-multiply intrinsics this file exists to avoid.
//!
//! The first version of this threw whenever `a` exceeded 63 bits, on the
//! reasoning that the fold never builds one wider. That is exactly backwards:
//! `a` here is the running sum_xy, which is wide precisely in the queries this
//! type was added for, and a 1e20 subtotal times a count of 10 fits a 128-bit
//! result comfortably. Declining it would have left the measured failure
//! unfixed while looking like a fix.
inline Int128 CheckedMul128By64(const Int128 &a, int64_t b) {
	if (a.IsZero() || b == 0) {
		return Int128(0);
	}
	const bool negative = a.IsNegative() != (b < 0);
	const Int128 magnitude_a = a.IsNegative() ? Negate(a) : a;
	const uint64_t ub = b < 0 ? ~static_cast<uint64_t>(b) + 1 : static_cast<uint64_t>(b);

	const uint64_t a_limbs[4] = {magnitude_a.low & 0xFFFFFFFFULL, magnitude_a.low >> 32,
	                             static_cast<uint64_t>(magnitude_a.high) & 0xFFFFFFFFULL,
	                             static_cast<uint64_t>(magnitude_a.high) >> 32};
	const uint64_t b_limbs[2] = {ub & 0xFFFFFFFFULL, ub >> 32};

	uint64_t product[6] = {0, 0, 0, 0, 0, 0};
	for (size_t i = 0; i < 4; i++) {
		uint64_t carry = 0;
		for (size_t j = 0; j < 2; j++) {
			const uint64_t current = product[i + j] + a_limbs[i] * b_limbs[j] + carry;
			product[i + j] = current & 0xFFFFFFFFULL;
			carry = current >> 32;
		}
		product[i + 2] += carry;
	}

	// Anything above the fourth limb does not fit, and a set top bit of the
	// fourth would read back as negative. INT128_MIN is rejected with them:
	// the fold has no use for the one value whose magnitude is unrepresentable.
	if (product[4] != 0 || product[5] != 0 || (product[3] >> 31) != 0) {
		throw std::runtime_error("factorized sum exceeds the representable range (int128 overflow)");
	}
	Int128 magnitude;
	magnitude.low = product[0] | (product[1] << 32);
	magnitude.high = static_cast<int64_t>(product[2] | (product[3] << 32));
	return negative ? Negate(magnitude) : magnitude;
}

//! Decimal rendering, for handing the value back as a string.
//!
//! Repeated division by 10^9 on magnitudes, in 32-bit limbs for the same
//! reason MultiplyWide uses them.
inline std::string ToString(const Int128 &value) {
	if (value.IsZero()) {
		return "0";
	}
	const bool negative = value.IsNegative();
	Int128 magnitude = negative ? Negate(value) : value;

	std::string digits;
	while (!magnitude.IsZero()) {
		// Long division of the four 32-bit limbs by 10^9.
		uint64_t limbs[4] = {static_cast<uint64_t>(magnitude.high) >> 32,
		                     static_cast<uint64_t>(magnitude.high) & 0xFFFFFFFFULL, magnitude.low >> 32,
		                     magnitude.low & 0xFFFFFFFFULL};
		uint64_t remainder = 0;
		for (auto &limb : limbs) {
			const uint64_t current = (remainder << 32) | limb;
			limb = current / 1000000000ULL;
			remainder = current % 1000000000ULL;
		}
		magnitude.high = static_cast<int64_t>((limbs[0] << 32) | limbs[1]);
		magnitude.low = (limbs[2] << 32) | limbs[3];

		for (int i = 0; i < 9; i++) {
			digits.push_back(static_cast<char>('0' + remainder % 10));
			remainder /= 10;
		}
	}
	while (digits.size() > 1 && digits.back() == '0') {
		digits.pop_back();
	}
	if (negative) {
		digits.push_back('-');
	}
	return std::string(digits.rbegin(), digits.rend());
}

} // namespace factorize
