// SPDX-License-Identifier: BSD-3-Clause

#include "sample_math.h"
#include "test.h"

#include <fenv.h>
#include <float.h>
#include <math.h>

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
	bool passed = saved != -1;
	for (size_t i = 0; passed && i < ARRAY_LENGTH(modes); ++i)
		passed = fesetround(modes[i]) == 0 && rounding_boundaries();
	if (saved != -1 && fesetround(saved) != 0)
		passed = false;
	printf("[ %s ] saturated sample rounding in all rounding modes\n",
	       passed ? "OK" : "FAILED");
	return passed ? 0 : 1;
}
