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

uint16_t fixed_phase_atan2_u15(int32_t q, int32_t i)
{
	uint32_t absolute_q;
	uint32_t absolute_i;
	uint32_t smaller;
	uint32_t larger;
	uint16_t ratio_q15;
	int32_t angle_input;
	int32_t angle;

	if (i == 0)
		return (uint16_t)(q < 0 ? 0x6000u : 0x2000u);
	if (q == 0)
		return (uint16_t)(i < 0 ? 0x4000u : 0);
	absolute_q = q < 0 ? 0u - (uint32_t)q : (uint32_t)q;
	absolute_i = i < 0 ? 0u - (uint32_t)i : (uint32_t)i;
	smaller = absolute_q < absolute_i ? absolute_q : absolute_i;
	larger = absolute_q < absolute_i ? absolute_i : absolute_q;
	/* The numerator is at most 2^46; the ratio is 0..32768.
	 * Divide before reducing precision so scaling a vector keeps its angle. */
	ratio_q15 = (uint16_t)(((uint64_t)smaller << 15) / larger);
	angle_input = ratio_q15;
	if (ratio_q15 >= 128u)
		angle_input = atan_profile_u15(ratio_q15 >> 7);
	angle = (angle_input * 0x145f + 0x4000) >> 15;
	if (absolute_q >= absolute_i)
		angle = 0x2000 - angle;
	if (i < 0)
		angle = 0x4000 - angle;
	if (q < 0)
		angle = -angle;
	return (uint16_t)((uint32_t)angle & 0x7fffu);
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
