// SPDX-License-Identifier: BSD-3-Clause

#ifndef SAMPLE_MATH_H
#define SAMPLE_MATH_H

#include "integer_math.h"

#include <math.h>
#include <stdbool.h>
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

/* Smooth a signed energy word with a Q15 history pole, adding the high
 * word of the block's modulo-2^32 energy. Floor the history product and
 * retain any signed 16-bit result; overflow in either direction uses the
 * legacy positive limit 0x7f00 rather than ordinary signed saturation. */
static inline uint16_t sample_smooth_energy_q15(
	uint16_t previous, uint32_t block_energy, uint16_t history_q15)
{
	int32_t history = arithmetic_shift_right_u32(
		(uint32_t)((int32_t)wrap_s16_from_s32(previous) * history_q15), 15);
	uint32_t combined = (uint32_t)history + (block_energy >> 16);
	uint32_t top = combined >> 15;

	return top == 0 || top == 0x1ffffu ?
		(uint16_t)combined : UINT16_C(0x7f00);
}

/* Exact squared magnitude of two signed samples, in their original units.
 * Each square fits int32_t, but their sum can reach 2^31. Add unsigned
 * squares so the full-scale negative pair stays representable. */
static inline uint32_t sample_pair_power_s16(int16_t first, int16_t second)
{
	return (uint32_t)((int32_t)first * first) +
	       (uint32_t)((int32_t)second * second);
}

/* Round an already-scaled sample, rejecting nonfinite or clipping inputs.
 * Check before lround so conversion is defined even with a 32-bit long.
 * Ties go away from zero, independently of the active rounding mode.
 * Rejection leaves the destination unchanged. */
static inline bool sample_round_checked_s16(double value, int16_t *sample)
{
	if (sample == NULL ||
	    !(value > INT16_MIN - 0.5 && value < INT16_MAX + 0.5))
		return false;
	*sample = (int16_t)lround(value);
	return true;
}

/* Use the same quantizer with saturation instead of rejection. The caller
 * must exclude NaN; infinities saturate without an integer conversion. */
static inline int16_t sample_round_s16(double value)
{
	int16_t sample;

	if (sample_round_checked_s16(value, &sample))
		return sample;
	return value < 0.0 ? INT16_MIN : INT16_MAX;
}

#endif
