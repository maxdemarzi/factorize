//===----------------------------------------------------------------------===//
//                         factorize
//
// test/unit/test_int128.cpp
//
// The portable 128-bit accumulator, checked against the compiler's native
// __int128 wherever the compiler has one.
//
// That oracle is the point of this file. src/core/int128.hpp cannot *use*
// __int128, because CI builds a plain-MSVC target that lacks it -- but the
// test runs under GCC and Clang, which have it, so every operation can be
// compared against a known-correct implementation over a large random sample
// rather than against hand-computed expectations. Hand-picked cases catch the
// boundaries a sampler is unlikely to hit; the sampler catches the ones nobody
// thought of.
//
//===----------------------------------------------------------------------===//

#include "../../src/core/int128.hpp"

#include <cstdio>
#include <limits>
#include <random>
#include <string>

using namespace factorize;

static int g_failures = 0;
static int g_checks = 0;

static void ExpectTrue(bool condition, const std::string &what) {
	g_checks++;
	if (condition) {
		std::printf("  ok   %s\n", what.c_str());
		return;
	}
	g_failures++;
	std::printf("  FAIL %s\n", what.c_str());
}

#if defined(__SIZEOF_INT128__)
#define HAVE_NATIVE_INT128 1
using Native = __int128;

static Native ToNative(const Int128 &value) {
	return (static_cast<Native>(value.high) << 64) | static_cast<unsigned __int128>(value.low);
}

static std::string NativeToString(Native value) {
	if (value == 0) {
		return "0";
	}
	const bool negative = value < 0;
	unsigned __int128 magnitude = negative ? -static_cast<unsigned __int128>(value) : value;
	std::string digits;
	while (magnitude != 0) {
		digits.push_back(static_cast<char>('0' + static_cast<int>(magnitude % 10)));
		magnitude /= 10;
	}
	if (negative) {
		digits.push_back('-');
	}
	return std::string(digits.rbegin(), digits.rend());
}
#endif

//===--------------------------------------------------------------------===//
// Boundaries, by hand
//===--------------------------------------------------------------------===//
static void TestBoundaries() {
	std::printf("Boundaries\n");
	constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
	constexpr int64_t kMin = std::numeric_limits<int64_t>::min();

	// The case the whole file exists for: a product that does not fit in 64
	// bits and must fit in 128. 1e10 * 1e10 = 1e20, past INT64_MAX (9.2e18).
	const Int128 big = MultiplyWide(10000000000LL, 10000000000LL);
	ExpectTrue(ToString(big) == "100000000000000000000", "1e10 * 1e10 is exact at 128 bits");

	// INT64_MIN has no positive counterpart in int64, so negating it as a
	// signed value is undefined; MultiplyWide takes magnitudes as unsigned
	// for exactly this.
	ExpectTrue(ToString(MultiplyWide(kMin, 1)) == "-9223372036854775808", "INT64_MIN * 1");
	ExpectTrue(ToString(MultiplyWide(kMin, -1)) == "9223372036854775808", "INT64_MIN * -1 is representable at 128 bits");
	ExpectTrue(ToString(MultiplyWide(kMin, kMin)) == "85070591730234615865843651857942052864", "INT64_MIN squared");
	ExpectTrue(ToString(MultiplyWide(kMax, kMax)) == "85070591730234615847396907784232501249", "INT64_MAX squared");

	ExpectTrue(ToString(Int128(0)) == "0", "zero renders as 0");
	ExpectTrue(ToString(Int128(-1)) == "-1", "negative one");
	ExpectTrue(ToString(MultiplyWide(-3, 5)) == "-15", "signs multiply");
	ExpectTrue(ToString(MultiplyWide(3, -5)) == "-15", "either way round");
	ExpectTrue(ToString(MultiplyWide(-3, -5)) == "15", "two negatives");
	ExpectTrue(ToString(MultiplyWide(0, kMin)) == "0", "zero times anything");

	// The case the first CheckedMul128By64 wrongly rejected: a subtotal far
	// wider than 64 bits, scaled by a small count.
	ExpectTrue(ToString(CheckedMul128By64(MultiplyWide(10000000000LL, 10000000000LL), 10)) ==
	               "1000000000000000000000",
	           "a 1e20 subtotal times 10 is representable");
	ExpectTrue(ToString(CheckedMul128By64(MultiplyWide(10000000000LL, 10000000000LL), -3)) ==
	               "-300000000000000000000",
	           "and signed");
	bool threw = false;
	try {
		CheckedMul128By64(MultiplyWide(kMax, kMax), kMax);
	} catch (const std::exception &) {
		threw = true;
	}
	ExpectTrue(threw, "but a 126-bit value times 2^63 does not fit and throws");

	threw = false;
	// Addition carries across the word boundary.
	const Int128 just_under(0, 0xFFFFFFFFFFFFFFFFULL);
	ExpectTrue(ToString(CheckedAdd128(just_under, Int128(1))) == "18446744073709551616", "carry into the high word");
	ExpectTrue(CheckedAdd128(Int128(5), Int128(-5)).IsZero(), "a value and its negation sum to zero");

	// And refuses to wrap at the top.
	const Int128 near_max(std::numeric_limits<int64_t>::max(), 0xFFFFFFFFFFFFFFFFULL);
	try {
		CheckedAdd128(near_max, Int128(1));
	} catch (const std::exception &) {
		threw = true;
	}
	ExpectTrue(threw, "adding past the 128-bit maximum throws");

	threw = false;
	const Int128 near_min(std::numeric_limits<int64_t>::min(), 0);
	try {
		CheckedAdd128(near_min, Int128(-1));
	} catch (const std::exception &) {
		threw = true;
	}
	ExpectTrue(threw, "adding past the 128-bit minimum throws");
}

//===--------------------------------------------------------------------===//
// Against the native type
//===--------------------------------------------------------------------===//
static void TestAgainstNative() {
#if !defined(HAVE_NATIVE_INT128)
	std::printf("Native oracle\n  (skipped: this compiler has no __int128)\n");
#else
	std::printf("Native oracle\n");
	std::mt19937_64 rng(20261016);
	// Drawn across magnitudes rather than uniformly over the range: a uniform
	// 64-bit sample is almost always enormous, and the small and medium values
	// are where carries and sign handling actually differ.
	auto draw = [&rng]() -> int64_t {
		const int bits = static_cast<int>(rng() % 64) + 1;
		const uint64_t mask = bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
		const int64_t magnitude = static_cast<int64_t>(rng() & mask);
		return (rng() & 1) ? -magnitude : magnitude;
	};

	size_t products = 0, sums = 0, renders = 0;
	for (int i = 0; i < 200000; i++) {
		const int64_t a = draw(), b = draw();

		const Int128 ours = MultiplyWide(a, b);
		const Native theirs = static_cast<Native>(a) * static_cast<Native>(b);
		if (ToNative(ours) == theirs) {
			products++;
		}
		if (ToString(ours) == NativeToString(theirs)) {
			renders++;
		}

		// Sums of two products, which is the shape CombineProductSums builds.
		const Int128 other = MultiplyWide(draw(), draw());
		const Native other_native = ToNative(other);
		bool overflowed = false;
		Int128 sum;
		try {
			sum = CheckedAdd128(ours, other);
		} catch (const std::exception &) {
			overflowed = true;
		}
		// Two 126-bit products can genuinely exceed the type; when they do,
		// the only correct behaviour is to throw, and the oracle agrees by
		// construction since it cannot represent the answer either.
		if (overflowed || ToNative(sum) == theirs + other_native) {
			sums++;
		}
	}
	// Int128 * int64, which the first implementation got wrong in the
	// direction that mattered: it refused the wide-left-operand case the fold
	// actually produces.
	size_t scaled = 0;
	for (int i = 0; i < 200000; i++) {
		const Int128 wide = MultiplyWide(draw(), draw());
		const int64_t scale = draw();

		// Representability is decided on unsigned magnitudes, before any
		// multiplication. Computing the product as a signed __int128 first and
		// dividing back out is undefined behaviour the moment it overflows,
		// and the first version did exactly that: it passed at -O1 under the
		// sanitizers and failed at -O2, where the compiler is entitled to
		// assume the overflow cannot happen and fold the check away. The
		// implementation was right both times; the oracle was not.
		using Magnitude = unsigned __int128;
		const Native native_wide = ToNative(wide);
		const Magnitude mag_wide =
		    native_wide < 0 ? ~static_cast<Magnitude>(native_wide) + 1 : static_cast<Magnitude>(native_wide);
		const Magnitude mag_scale = scale < 0 ? ~static_cast<Magnitude>(static_cast<Native>(scale)) + 1
		                                      : static_cast<Magnitude>(scale);
		const Magnitude kMagnitudeMax = (static_cast<Magnitude>(1) << 127) - 1;
		const bool representable = mag_scale == 0 || mag_wide <= kMagnitudeMax / mag_scale;
		const bool negative = (native_wide < 0) != (scale < 0);
		const Magnitude product = representable ? mag_wide * mag_scale : 0;
		const Native expected =
		    negative ? -static_cast<Native>(product) : static_cast<Native>(product);
		try {
			const Int128 got = CheckedMul128By64(wide, scale);
			if (representable && ToNative(got) == expected) {
				scaled++;
			}
		} catch (const std::exception &) {
			if (!representable) {
				scaled++;
			}
		}
	}
	ExpectTrue(scaled == 200000, "200,000 random Int128 * int64 match __int128 or refuse to wrap");

	ExpectTrue(products == 200000, "200,000 random products match __int128");
	ExpectTrue(sums == 200000, "200,000 random sums match __int128 or refuse to wrap");
	ExpectTrue(renders == 200000, "200,000 random renderings match __int128");
#endif
}

int main() {
	std::printf("factorize core: 128-bit accumulator\n\n");
	TestBoundaries();
	std::printf("\n");
	TestAgainstNative();
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
