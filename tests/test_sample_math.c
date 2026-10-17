// SPDX-License-Identifier: BSD-3-Clause

#include "sample_math.h"
#include "test.h"

#include <fenv.h>
#include <float.h>
#include <math.h>

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
	bool passed = saved != -1 && pair_power_boundaries();
	for (size_t i = 0; passed && i < ARRAY_LENGTH(modes); ++i)
		passed = fesetround(modes[i]) == 0 && rounding_boundaries();
	if (saved != -1 && fesetround(saved) != 0)
		passed = false;
	printf("[ %s ] sample pair power and saturated rounding in all rounding modes\n",
	       passed ? "OK" : "FAILED");
	return passed ? 0 : 1;
}
