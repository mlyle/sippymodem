// SPDX-License-Identifier: BSD-3-Clause

#include "fixed_phase.h"

#include "integer_math.h"
#include <phase_profile.h>

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

enum {
	FIXED_PHASE_QUARTER_STEPS = 256,
};

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
		mantissa = normalized_reciprocal_q15(absolute_y, &exponent);
		scale = (uint32_t)mantissa * absolute_x;
	} else {
		mantissa = normalized_reciprocal_q15(absolute_x, &exponent);
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

static int16_t sine_u16_q14(uint16_t phase, bool round_nearest)
{
	unsigned int quadrant = phase >> 14;
	unsigned int index = (phase & 0x3fffu) >> 6;
	unsigned int fraction = phase & 0x3fu;
	int32_t sample0;
	int32_t sample1;
	int32_t value;

	if ((quadrant & 1u) != 0) {
		index = 255u - index;
		fraction = 64u - fraction;
	}
	sample0 = FIXED_PHASE_QUARTER_Q15[index];
	sample1 = FIXED_PHASE_QUARTER_Q15[index + 1u];
	/* Adjacent quarter-wave endpoints increase, so this stays nonnegative. */
	value = sample0 +
		((sample1 - sample0) * fraction + (round_nearest ? 32 : 0)) / 64;
	/* The unsigned profile rounds before polarity; the signed profile
	 * floors after polarity so negative half units retain their rounding. */
	if (round_nearest)
		value = (value + 1) >> 1;
	if (quadrant >= 2u)
		value = -value;
	if (!round_nearest)
		value = arithmetic_shift_right_u32((uint32_t)value, 1);
	return (int16_t)value;
}

struct fixed_phase_q14 fixed_phase_sin_cos_u16_q14(uint16_t phase)
{
	return (struct fixed_phase_q14){
		.sine = sine_u16_q14(phase, true),
		.cosine = sine_u16_q14((uint16_t)(phase + 0x4000u), true),
	};
}

int16_t fixed_phase_cosine_u8_q14(uint8_t phase)
{
	unsigned int quadrant = phase >> 6;
	unsigned int index = (phase & 0x3fu) * 4u;
	int16_t value;

	if (phase == 128u)
		return -16383;
	if ((quadrant & 1u) == 0)
		index = FIXED_PHASE_QUARTER_STEPS - index;
	value = (int16_t)(FIXED_PHASE_QUARTER_Q15[index] >> 1);
	return quadrant == 1u || quadrant == 2u ? (int16_t)-value : value;
}

struct fixed_phase_q14 fixed_phase_sin_cos_q14(int16_t phase)
{
	/* Scale the 15-bit turn into the shared 16-bit carrier cycle. */
	uint16_t carrier_phase = (uint16_t)((uint16_t)phase * 2u);

	return (struct fixed_phase_q14){
		.sine = sine_u16_q14(carrier_phase, false),
		.cosine = sine_u16_q14((uint16_t)(carrier_phase + 0x4000u), false),
	};
}
