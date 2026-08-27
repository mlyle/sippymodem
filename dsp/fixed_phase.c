// SPDX-License-Identifier: BSD-3-Clause

#include "fixed_phase.h"

#include "integer_math.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>

enum {
	FIXED_PHASE_QUARTER_STEPS = 256,
	FIXED_PHASE_INTERPOLATION_STEPS = 32,
};

static const double FIXED_PHASE_PI =
	3.1415926535897932384626433832795;

static uint16_t reciprocal_q15(
	uint16_t value, uint16_t *exponent)
{
	uint16_t normalized = value;
	uint16_t shift = 0;
	uint16_t high;

	while ((normalized & UINT16_C(0x8000)) == 0) {
		normalized = (uint16_t)(normalized << 1);
		shift++;
	}
	high = (uint16_t)(((uint32_t)normalized + 0x80u) >> 8);
	*exponent = shift;
	return (uint16_t)((UINT32_C(1) << 22) / high);
}

static uint16_t atan_profile_u15(uint16_t ratio_q8)
{
	return (uint16_t)lround(
		atan((double)ratio_q8 / 256.0) * 32768.0);
}

uint16_t fixed_phase_atan2_u15(int16_t x, int16_t y)
{
	uint16_t absolute_x;
	uint16_t absolute_y;
	uint16_t mantissa;
	uint16_t exponent;
	uint32_t scale;
	uint16_t index;
	int32_t angle_input;
	int32_t angle;

	if (y == 0)
		return (uint16_t)(x < 0 ? 0x6000u : 0x2000u);
	if (x == 0)
		return (uint16_t)(y < 0 ? 0x4000u : 0);
	absolute_x = (uint16_t)magnitude_s16(x);
	absolute_y = (uint16_t)magnitude_s16(y);
	if (absolute_x < absolute_y) {
		mantissa = reciprocal_q15(absolute_y, &exponent);
		scale = (uint32_t)mantissa * absolute_x;
	} else {
		mantissa = reciprocal_q15(absolute_x, &exponent);
		scale = (uint32_t)mantissa * absolute_y;
	}
	index = (uint16_t)(((scale >> 15) << (exponent & 0x1fu)) &
			     UINT16_MAX);
	angle_input = wrap_s16_from_s32(index);
	if (index > 0x7eu)
		angle_input = (int16_t)atan_profile_u15(index >> 7);
	angle = arithmetic_shift_right_u32(
		(uint32_t)(angle_input * 0x145f + 0x4000), 15);
	if (absolute_x < absolute_y) {
		if (y < 0)
			return (uint16_t)(x >= 0 ? 0x4000 - angle :
						 angle + 0x4000);
		return (uint16_t)(x < 0 ? 0x7fff - angle : angle);
	}
	if (x < 0)
		return (uint16_t)(y < 0 ? 0x6000 - angle :
					 angle + 0x6000);
	return (uint16_t)(y >= 0 ? 0x2000 - angle : angle + 0x2000);
}

/* These are exact integer endpoints of the original Q15 quarter-wave
 * profile.  Truncation is intentional; rounding differs by one at 114 of
 * the 257 endpoints. */
static uint16_t quadrant_sine_q15(unsigned int index)
{
	double radians = FIXED_PHASE_PI * (double)index /
		FIXED_PHASE_QUARTER_STEPS / 2.0;

	return (uint16_t)(uint32_t)(sin(radians) * 32768.0);
}

static int16_t sine_u16_q14(uint16_t phase)
{
	uint16_t quadrant = (uint16_t)(phase >> 14);
	uint16_t offset = (uint16_t)(phase & UINT16_C(0x3fff));
	uint16_t index = (uint16_t)(offset >> 6);
	uint16_t fraction = (uint16_t)(offset & UINT16_C(0x003f));
	int32_t sample0;
	int32_t sample1;
	int32_t value;

	if ((quadrant & 1u) != 0) {
		index = (uint16_t)(255u - index);
		fraction = (uint16_t)(64u - fraction);
	}
	sample0 = quadrant_sine_q15(index);
	sample1 = quadrant_sine_q15((unsigned int)index + 1u);
	value = sample0 +
		((sample1 - sample0) * fraction + 32) / 64;
	value = (value + 1) >> 1;
	if (quadrant >= 2u)
		value = -value;
	return (int16_t)value;
}

struct fixed_phase_q14 fixed_phase_sin_cos_u16_q14(uint16_t phase)
{
	return (struct fixed_phase_q14){
		.sine = sine_u16_q14(phase),
		.cosine = sine_u16_q14((uint16_t)(phase + 0x4000u)),
	};
}

int16_t fixed_phase_cosine_u8_q14(uint8_t phase)
{
	if (phase == 128u)
		return -16383;
	return (int16_t)(cos(
		2.0 * FIXED_PHASE_PI * phase / 256.0) * 16384.0);
}

static int32_t floor_divide_power_of_two(
	int32_t value, unsigned int shift)
{
	if (value >= 0)
		return value / (INT32_C(1) << shift);
	return -(int32_t)(((uint32_t)(-value) +
			  (UINT32_C(1) << shift) - 1u) >> shift);
}

struct fixed_phase_q14 fixed_phase_sin_cos_q14(int16_t phase)
{
	static const int16_t cosine_sign[] = {
		16384, 16384, -16384, -16384,
		16384, -16384, -16384, 16384,
	};
	static const int16_t sine_sign[] = {
		28620, -25834, 12917, 0,
		16384, 16384, -16384, -16384,
	};
	int32_t index = floor_divide_power_of_two(phase, 5);
	int32_t quadrant = floor_divide_power_of_two(phase, 13);
	int32_t fraction = (int32_t)phase -
		index * FIXED_PHASE_INTERPOLATION_STEPS;
	uint16_t sine0;
	uint16_t sine1;
	uint16_t cosine0;
	uint16_t cosine1;
	uint32_t sine;
	uint32_t cosine;
	struct fixed_phase_q14 result;

	if (quadrant % 2 != 0) {
		index = -index - 1;
		fraction = FIXED_PHASE_INTERPOLATION_STEPS - fraction;
	}
	index = (int32_t)((uint32_t)index & 0xffu);
	sine0 = quadrant_sine_q15((unsigned int)index);
	sine1 = quadrant_sine_q15((unsigned int)index + 1u);
	cosine0 = quadrant_sine_q15(
		FIXED_PHASE_QUARTER_STEPS - (unsigned int)index);
	cosine1 = quadrant_sine_q15(
		FIXED_PHASE_QUARTER_STEPS - (unsigned int)index - 1u);
	sine = ((uint32_t)sine0 +
		 (uint32_t)arithmetic_shift_right_u32(
			 (uint32_t)(((int32_t)sine1 - (int32_t)sine0) *
				    fraction), 5)) &
		UINT16_MAX;
	cosine = ((uint32_t)cosine0 +
		   (uint32_t)arithmetic_shift_right_u32(
			   (uint32_t)(((int32_t)cosine1 -
				       (int32_t)cosine0) * fraction),
			   5)) &
		  UINT16_MAX;
	result.sine = wrap_s16_from_s32(arithmetic_shift_right_u32(
		(uint32_t)((int32_t)sine * sine_sign[quadrant + 4]), 15));
	result.cosine = wrap_s16_from_s32(arithmetic_shift_right_u32(
		(uint32_t)((int32_t)cosine * cosine_sign[quadrant + 4]), 15));
	return result;
}
