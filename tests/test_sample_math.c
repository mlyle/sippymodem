// SPDX-License-Identifier: BSD-3-Clause

#include "sample_math.h"
#include "test.h"

#include <fenv.h>
#include <float.h>
#include <math.h>

static uint16_t level_oracle(
	const int16_t *samples, size_t count, uint16_t weight)
{
	uint64_t energy = 0;

	for (size_t i = 0; i < count; ++i) {
		int64_t product = (int64_t)samples[i] * weight;
		/* Independent floor division; reduce only after summing. */
		int64_t scaled = product >= 0 ? product / 32768 :
			-((-product + 32767) / 32768);

		energy += (uint64_t)(scaled * samples[i]);
	}
	return fixed_square_root_u32((uint32_t)energy);
}

static bool weighted_level_boundaries(void)
{
	const uint16_t weights[] = { 0, 1, 910, 1092, 32768, UINT16_MAX };
	const size_t counts[] = { 1, 9, 30, 36, 73 };
	int16_t samples[73];
	uint32_t random = 1;

	CHECK(sample_level_s16(NULL, 0, UINT16_MAX) == 0,
	      "empty level measurement accessed input");
	/* Both actual normalization weights must retain the negative-sample
	 * floor across the complete input range, including full scale. */
	for (int32_t value = INT16_MIN; value <= INT16_MAX; ++value) {
		int16_t sample = (int16_t)value;

		for (size_t i = 2; i < 4; ++i)
			CHECK(sample_level_s16(&sample, 1, weights[i]) ==
			      level_oracle(&sample, 1, weights[i]),
			      "single-sample level changed at %d/%u", (int)value, weights[i]);
	}
	for (unsigned fixture = 0; fixture < 512; ++fixture) {
		for (size_t i = 0; i < ARRAY_LENGTH(samples); ++i) {
			random = random * UINT32_C(1664525) + UINT32_C(1013904223);
			uint16_t word = (uint16_t)(random >> 16);

			samples[i] = fixture == 0 ? INT16_MIN : fixture == 1 ? INT16_MAX :
				(int16_t)(word <= INT16_MAX ? (int32_t)word : (int32_t)word - 65536);
		}
		for (size_t i = 0; i < ARRAY_LENGTH(weights); ++i)
			for (size_t j = 0; j < ARRAY_LENGTH(counts); ++j)
				CHECK(sample_level_s16(samples, counts[j], weights[i]) ==
				      level_oracle(samples, counts[j], weights[i]),
				      "weighted level fixture %u/%u/%zu changed",
				      fixture, weights[i], counts[j]);
	}
	return true;
}

static bool pair_power_boundaries(void)
{
	const int16_t edges[] = { INT16_MIN, -32767, -1, 0, 1, 32766, INT16_MAX };

	CHECK(sample_pair_power_s16(INT16_MIN, INT16_MIN) == UINT32_C(2147483648),
	      "full-scale negative pair overflowed");
	/* Sweep every sample value against both extremes, their neighbors and
	 * zero. Widen the reference sum before addition independently of the
	 * production unsigned-square arithmetic. */
	for (int32_t first = INT16_MIN; first <= INT16_MAX; ++first) {
		for (size_t i = 0; i < ARRAY_LENGTH(edges); ++i) {
			int64_t second = edges[i];
			uint32_t expected = (uint32_t)(
				(int64_t)first * first + second * second);

			CHECK(sample_pair_power_s16((int16_t)first, edges[i]) == expected &&
			      sample_pair_power_s16(edges[i], (int16_t)first) == expected,
			      "sample pair %d/%d power changed", (int)first, (int)second);
		}
	}
	return true;
}

static bool rounding_boundaries(void)
{
	for (int32_t integer = INT16_MIN; integer <= INT16_MAX; ++integer) {
		CHECK(sample_round_s16(integer) == integer,
		      "integer %d changed", (int)integer);
		if (integer == INT16_MAX)
			break;
		double half = integer + 0.5;
		CHECK(sample_round_s16(half) == (half < 0.0 ? integer : integer + 1),
		      "half-sample %.1f did not round away from zero", half);
		CHECK(sample_round_s16(nextafter(half, -INFINITY)) == integer &&
		      sample_round_s16(nextafter(half, INFINITY)) == integer + 1,
		      "neighbors of %.1f rounded across the boundary", half);
	}
	const struct { double value; int16_t expected; } edges[] = {
		{ 0.0, 0 }, { -0.0, 0 }, { DBL_TRUE_MIN, 0 }, { -DBL_TRUE_MIN, 0 },
		{ DBL_MAX, INT16_MAX }, { -DBL_MAX, INT16_MIN },
		{ INFINITY, INT16_MAX }, { -INFINITY, INT16_MIN },
		{ INT16_MAX + 0.5, INT16_MAX }, { INT16_MIN - 0.5, INT16_MIN },
	};
	for (size_t i = 0; i < ARRAY_LENGTH(edges); ++i)
		CHECK(sample_round_s16(edges[i].value) == edges[i].expected,
		      "saturation/zero case %zu changed", i);
	return true;
}

int main(void)
{
	const int modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
	int saved = fegetround();
	bool passed = saved != -1 && pair_power_boundaries() && weighted_level_boundaries();
	for (size_t i = 0; passed && i < ARRAY_LENGTH(modes); ++i)
		passed = fesetround(modes[i]) == 0 && rounding_boundaries();
	if (saved != -1 && fesetround(saved) != 0)
		passed = false;
	printf("[ %s ] sample pair power and saturated rounding in all rounding modes\n",
	       passed ? "OK" : "FAILED");
	return passed ? 0 : 1;
}
