// SPDX-License-Identifier: BSD-3-Clause
#include "wide_uint.h"
#include "test.h"

static bool equal(struct wide_uint value, uint64_t high, uint64_t low)
{
	return value.high == high && value.low == low;
}

static bool test_boundaries(void)
{
	struct wide_uint maximum = { UINT64_MAX, UINT64_MAX }, value, quotient;
	struct wide_uint one = wide_from_u64(1);

	CHECK(!wide_add(maximum, one, &value) && equal(value, 0, 0),
	      "addition overflow differs");
	value = (struct wide_uint){ 7, UINT64_MAX };
	CHECK(wide_add(value, one, &value) && equal(value, 8, 0),
	      "aliased addition lost its carry");
	CHECK(equal(wide_subtract(value, one), 7, UINT64_MAX),
	      "subtraction lost its borrow");
	CHECK(equal(wide_subtract(wide_from_u64(0), one), UINT64_MAX, UINT64_MAX),
	      "subtraction does not wrap");
	CHECK(wide_compare(one, maximum) < 0 && wide_compare(maximum, one) > 0 &&
	      wide_compare(maximum, maximum) == 0, "comparison differs");
	CHECK(!wide_multiply_u32(maximum, 2, &value) &&
	      equal(value, UINT64_MAX, UINT64_MAX - 1), "multiplication overflow differs");
	CHECK(wide_multiply_u32(maximum, 0, &value) && equal(value, 0, 0),
	      "zero multiplication differs");
	value = wide_from_u64(UINT64_MAX);
	CHECK(wide_multiply_u32(value, UINT32_MAX, &value) &&
	      equal(value, UINT32_MAX - 1, UINT64_C(0xffffffff00000001)),
	      "aliased multiplication lost carry");
	CHECK(equal(wide_product_u64(UINT64_MAX, UINT64_MAX), UINT64_MAX - 1, 1),
	      "64-bit product lost its carry");
	CHECK(equal(wide_multiply(maximum, maximum), 0, 1),
	      "full-width product does not wrap");
	CHECK(equal(wide_multiply((struct wide_uint){ 1, 1 },
	                         (struct wide_uint){ 1, 1 }), 2, 1),
	      "full-width product lost its cross terms");
	CHECK(wide_divide_u32(maximum, UINT32_MAX, &quotient) == 0 &&
	      equal(quotient, UINT64_C(0x100000001), UINT64_C(0x100000001)),
	      "four-limb division differs");
	CHECK(wide_divide_u32(maximum, 3, &quotient) == 0 &&
	      equal(quotient, UINT64_C(0x5555555555555555), UINT64_C(0x5555555555555555)),
	      "division by three differs");
	value = maximum;
	CHECK(wide_divide_u32(value, 1, &value) == 0 &&
	      equal(value, UINT64_MAX, UINT64_MAX), "aliased division differs");
	CHECK(equal(wide_shift_left(one, 0), 0, 1) &&
	      equal(wide_shift_left(one, 64), 1, 0) &&
	      equal(wide_shift_left(one, 127), UINT64_C(1) << 63, 0) &&
	      equal(wide_shift_left(one, 128), 0, 0) &&
	      equal(wide_shift_right(maximum, 0), UINT64_MAX, UINT64_MAX) &&
	      equal(wide_shift_right(maximum, 64), 0, UINT64_MAX) &&
	      equal(wide_shift_right(maximum, 127), 0, 1) &&
	      equal(wide_shift_right(maximum, 128), 0, 0), "shift boundaries differ");
	return true;
}

#ifdef __SIZEOF_INT128__
static uint64_t random_word(uint64_t *state)
{
	*state ^= *state << 13;
	*state ^= *state >> 7;
	*state ^= *state << 17;
	return *state;
}

/* The ordinary portable build compares the limb implementation to compiler
 * arithmetic. A 32-bit build retains the literal carry/division fixtures. */
static bool test_native_oracle(void)
{
	uint64_t seed = UINT64_C(0x8d14eb7ac09523f6);
	for (unsigned index = 0; index < 10000; ++index) {
		struct wide_uint a = { random_word(&seed), random_word(&seed) };
		struct wide_uint b = { random_word(&seed), random_word(&seed) }, value;
		__uint128_t x = ((__uint128_t)a.high << 64) | a.low;
		__uint128_t y = ((__uint128_t)b.high << 64) | b.low;
		uint32_t factor = (uint32_t)random_word(&seed);
		uint32_t divisor = (uint32_t)random_word(&seed) | 1u;
		unsigned shift = index % 130;
		__uint128_t expected = x + y;
		CHECK(wide_add(a, b, &value) == (expected >= x) &&
		      equal(value, expected >> 64, expected), "sum oracle differs at %u", index);
		expected = x - y;
		CHECK(equal(wide_subtract(a, b), expected >> 64, expected) &&
		      wide_compare(a, b) == ((x > y) - (x < y)),
		      "difference/comparison oracle differs at %u", index);
		expected = (__uint128_t)a.low * b.low;
		CHECK(equal(wide_product_u64(a.low, b.low), expected >> 64, expected),
		      "64-bit product oracle differs at %u", index);
		expected = x * y;
		CHECK(equal(wide_multiply(a, b), expected >> 64, expected),
		      "full-width product oracle differs at %u", index);
		bool fits = factor == 0 || x <= ~(__uint128_t)0 / factor;
		expected = x * factor;
		CHECK(wide_multiply_u32(a, factor, &value) == fits &&
		      equal(value, expected >> 64, expected), "product oracle differs at %u", index);
		expected = x / divisor;
		CHECK(wide_divide_u32(a, divisor, &value) == x % divisor &&
		      equal(value, expected >> 64, expected), "division oracle differs at %u", index);
		expected = shift < 128 ? x << shift : 0;
		CHECK(equal(wide_shift_left(a, shift), expected >> 64, expected),
		      "left shift oracle differs at %u", index);
		expected = shift < 128 ? x >> shift : 0;
		CHECK(equal(wide_shift_right(a, shift), expected >> 64, expected),
		      "right shift oracle differs at %u", index);
	}
	return true;
}
#endif

int main(void)
{
	if (!test_boundaries())
		return 1;
#ifdef __SIZEOF_INT128__
	if (!test_native_oracle())
		return 1;
#endif
	return 0;
}
