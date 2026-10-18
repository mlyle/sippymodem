// SPDX-License-Identifier: BSD-3-Clause

#ifndef SAMPLE_MATH_H
#define SAMPLE_MATH_H

#include "integer_math.h"

#include <stddef.h>
#include <stdint.h>

/* Fixed-point level used by gain and carrier gates. Floor each weighted
 * sample before multiplying by the original sample, sum modulo 2^32, then
 * apply the legacy square root. The caller supplies count readable samples
 * and the Q15 weight; count zero accesses no samples and returns zero. */
static inline uint16_t sample_level_s16(
	const int16_t *samples, size_t count, uint16_t weight_q15)
{
	uint32_t energy = 0;

	for (size_t index = 0; index < count; ++index) {
		int32_t scaled = arithmetic_shift_right_u32(
			(uint32_t)((int32_t)samples[index] * weight_q15), 15);

		energy += (uint32_t)(scaled * samples[index]);
	}
	return fixed_square_root_u32(energy);
}

/* Exact squared magnitude of two signed samples, in their original units.
 * Each square fits int32_t, but their sum can reach 2^31. Add unsigned
 * squares so the full-scale negative pair stays representable. */
static inline uint32_t sample_pair_power_s16(int16_t first, int16_t second)
{
	return (uint32_t)((int32_t)first * first) +
	       (uint32_t)((int32_t)second * second);
}

/* Round a linear sample to nearest, ties away from zero, then saturate.
 * The caller must exclude NaN; infinities saturate. Subtracting the
 * truncated integer preserves values immediately beside a half-sample,
 * unlike adding 0.5 before truncation. No rounding-mode change is needed. */
static inline int16_t sample_round_s16(double value)
{
	if (value >= INT16_MAX)
		return INT16_MAX;
	if (value <= INT16_MIN)
		return INT16_MIN;
	int32_t integral = (int32_t)value;
	double fraction = value - integral;
	return (int16_t)(integral + (fraction >= 0.5) - (fraction <= -0.5));
}

#endif
