// SPDX-License-Identifier: BSD-3-Clause

#include <integer_math.h>

#include "test.h"

static bool test_zero_and_symmetry(void)
{
	CHECK(gcd_u64(0, 0) == 0 &&
	      gcd_u64(0, 37) == 37 &&
	      gcd_u64(37, 0) == 37 &&
	      gcd_u64(18, 48) == 6 &&
	      gcd_u64(48, 18) == 6,
	      "GCD zero or symmetry contract changed");
	return true;
}

static bool test_full_width_values(void)
{
	CHECK(gcd_u64(UINT64_MAX, 3) == 3 &&
	      gcd_u64(UINT64_MAX, UINT64_MAX - 1) == 1 &&
	      gcd_u64(UINT64_C(1) << 63, UINT64_C(1) << 32) ==
		      (UINT64_C(1) << 32),
	      "GCD truncated or mishandled full-width values");
	return true;
}

static bool test_signed_rounding(void)
{
	static const struct {
		int64_t input;
		unsigned int shift;
		int64_t expected;
	} cases[] = {
		{ 7, 1, 4 },
		{ -7, 1, -4 },
		{ 6, 1, 3 },
		{ -6, 1, -3 },
		{ INT64_MAX, 63, 1 },
		{ INT64_MIN, 63, -1 },
		{ INT64_MAX, 64, 0 },
		{ INT64_MIN, 64, -1 },
		{ INT64_MIN, 65, 0 },
	};

	for (size_t index = 0u;
	     index < ARRAY_LENGTH(cases); ++index)
		CHECK(round_shift_s64(
			      cases[index].input,
			      cases[index].shift) == cases[index].expected,
		      "signed rounding case %zu changed", index);
	CHECK(round_shift_s64_to_s32_saturating(INT64_MAX, 0u) ==
		      INT32_MAX &&
	      round_shift_s64_to_s32_saturating(INT64_MIN, 0u) ==
		      INT32_MIN &&
	      round_shift_s64_to_s16_saturating(
		      INT64_C(32768) * 32768, 15u) == INT16_MAX &&
	      round_shift_s64_to_s16_saturating(
		      INT64_C(-32769) * 32768, 15u) == INT16_MIN,
	      "signed rounding saturation changed");
	return true;
}

static bool test_fixed_width_words(void)
{
	static const struct {
		uint32_t input;
		int32_t expected;
	} s32_cases[] = {
		{ 0u, 0 },
		{ UINT32_C(0x7fffffff), INT32_MAX },
		{ UINT32_C(0x80000000), INT32_MIN },
		{ UINT32_MAX, -1 },
	};
	static const struct {
		int32_t input;
		int16_t expected;
	} s16_cases[] = {
		{ 0, 0 },
		{ INT16_MAX, INT16_MAX },
		{ INT16_MAX + 1, INT16_MIN },
		{ 65535, -1 },
		{ INT32_MAX, -1 },
		{ INT32_MIN, 0 },
	};

	for (size_t index = 0u;
	     index < ARRAY_LENGTH(s32_cases); ++index)
		CHECK(wrap_s32_from_u32(s32_cases[index].input) ==
			      s32_cases[index].expected,
		      "32-bit wrap case %zu changed", index);
	for (size_t index = 0u;
	     index < ARRAY_LENGTH(s16_cases); ++index)
		CHECK(wrap_s16_from_s32(s16_cases[index].input) ==
			      s16_cases[index].expected,
		      "16-bit wrap case %zu changed", index);
	CHECK(wrap_s64_from_u64(UINT64_C(0x8000000000000000)) ==
		      INT64_MIN &&
	      wrap_s64_from_u64(UINT64_MAX) == -1 &&
	      magnitude_s16(INT16_MIN) == 32768 &&
	      absolute_wrap_s16(INT16_MIN) == INT16_MIN &&
	      absolute_wrap_s16(-1234) == 1234 &&
	      clamp_s16(INT64_MAX) == INT16_MAX &&
	      clamp_s16(INT64_MIN) == INT16_MIN,
	      "shared signed-word helpers changed");
	return true;
}

static bool test_arithmetic_shift(void)
{
	static const struct {
		uint32_t input;
		unsigned int shift;
		int32_t expected;
	} cases[] = {
		{ UINT32_C(0x7fffffff), 0u, INT32_MAX },
		{ UINT32_C(0x80000000), 0u, INT32_MIN },
		{ UINT32_C(0x40000000), 1u, 0x20000000 },
		{ UINT32_C(0x80000000), 1u, -1073741824 },
		{ UINT32_MAX, 15u, -1 },
		{ UINT32_C(0x80000000), 31u, -1 },
		{ UINT32_C(0x7fffffff), 31u, 0 },
		{ UINT32_C(0x80000000), 32u, -1 },
		{ UINT32_C(0x7fffffff), 33u, 0 },
	};

	for (size_t index = 0u; index < ARRAY_LENGTH(cases); ++index)
		CHECK(arithmetic_shift_right_u32(
			      cases[index].input, cases[index].shift) ==
			      cases[index].expected,
		      "arithmetic shift case %zu changed", index);
	CHECK(arithmetic_shift_right_u64(
		      UINT64_C(0x8000000000000000), 1u) ==
		      INT64_C(-4611686018427387904) &&
	      arithmetic_shift_right_u64(UINT64_MAX, 63u) == -1 &&
	      arithmetic_shift_right_u64(
		      UINT64_C(0x7fffffffffffffff), 64u) == 0 &&
	      arithmetic_shift_right_u64(
		      UINT64_C(0x8000000000000000), 65u) == -1 &&
	      multiply_shift_s16(0x4000, 0x4000, 14u) == 0x4000 &&
	      multiply_shift_s16(INT16_MIN, INT16_MIN, 15u) == INT16_MIN,
	      "wide shift or wrapped multiply changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "zero and symmetry", test_zero_and_symmetry },
		{ "full-width values", test_full_width_values },
		{ "signed rounding", test_signed_rounding },
		{ "fixed-width words", test_fixed_width_words },
		{ "arithmetic shift", test_arithmetic_shift },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		passed = tests[index].run();
		if (!passed) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
