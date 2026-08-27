// SPDX-License-Identifier: BSD-3-Clause

#include "decision.h"

#include "constellation.h"
#include "integer_math.h"
#include "memory_region.h"

#include <limits.h>
#include <stddef.h>

static const uint16_t PHASE_16[16] = {
	0x2fff, 0x3972, 0x268d, 0x2fff,
	0x468d, 0x5000, 0x5000, 0x5972,
	0x1972, 0x0fff, 0x0fff, 0x068d,
	0x7000, 0x668d, 0x7972, 0x6fff,
};

static bool observation_is_valid(
	const struct v32_equalizer_observation *observation)
{
	return observation != NULL && observation->epoch != 0 &&
	       observation->phase <= INT16_MAX &&
	       observation->magnitude <= INT16_MAX;
}

static bool output_is_separate(
	const struct v32_equalizer_observation *observation,
	const void *output, size_t output_size)
{
	return output != NULL &&
	       !memory_regions_overlap(
		       observation, sizeof(*observation), output, output_size);
}

static struct v32_equalizer_decision equalizer_decision(
	const struct v32_equalizer_observation *observation,
	uint16_t phase, uint16_t magnitude, bool adapt_on_error)
{
	return (struct v32_equalizer_decision){
		.observation = *observation,
		.ideal_phase = phase,
		.ideal_magnitude = magnitude,
		.adapt_on_error = adapt_on_error,
	};
}

static uint64_t distance_squared(
	const struct v32_equalizer_observation *observation,
	struct v32_fixed_point point)
{
	int32_t difference_i = (int32_t)observation->i - point.i;
	int32_t difference_q = (int32_t)observation->q - point.q;

	return (uint64_t)((int64_t)difference_i * difference_i) +
	       (uint64_t)((int64_t)difference_q * difference_q);
}

static struct v32_fixed_point point_16(uint8_t index)
{
	bool positive_i = (index & 8u) != 0;
	bool negative_q = (index & 4u) != 0;
	bool outer_i = ((index >> 1) & 1u) == (unsigned int)positive_i;
	bool outer_q = (index & 1u) == (unsigned int)negative_q;
	int16_t i = (int16_t)((outer_i ? 3 : 1) * 4096);
	int16_t q = (int16_t)((outer_q ? 3 : 1) * 4096);

	return (struct v32_fixed_point){
		.i = positive_i ? i : (int16_t)-i,
		.q = negative_q ? (int16_t)-q : q,
	};
}

static uint8_t nearest_generated(
	const struct v32_equalizer_observation *observation,
	uint8_t start, uint8_t count,
	struct v32_fixed_point (*point_for_index)(uint8_t))
{
	uint64_t best_distance = UINT64_MAX;
	uint8_t selected = start;

	for (uint8_t offset = 0; offset < count; ++offset) {
		uint8_t index = (uint8_t)(start + offset);
		uint64_t distance = distance_squared(
			observation, point_for_index(index));

		if (distance < best_distance) {
			best_distance = distance;
			selected = index;
		}
	}
	return selected;
}

static uint8_t nearest_16(
	const struct v32_equalizer_observation *observation)
{
	return nearest_generated(observation, 0, 16, point_16);
}

static uint16_t magnitude_16(uint8_t index, bool sparse)
{
	static const uint16_t NORMAL[3] = {
		0x16a0, 0x3299, 0x43e2,
	};
	static const uint16_t SPARSE[3] = {
		0, 0x2820, 0,
	};
	struct v32_fixed_point point = point_16(index);
	uint16_t absolute_i = (uint16_t)(point.i < 0 ? -point.i : point.i);
	uint16_t absolute_q = (uint16_t)(point.q < 0 ? -point.q : point.q);
	uint16_t magnitude_index = (uint16_t)(
		((absolute_i + absolute_q) >> 13) - 1u);

	return sparse ? SPARSE[magnitude_index] : NORMAL[magnitude_index];
}

static uint8_t symbol_16(uint8_t index)
{
	static const uint8_t RAW_TO_SYMBOL[16] = {
		15, 14, 13, 12, 1, 3, 0, 2,
		10, 8, 11, 9, 4, 5, 6, 7,
	};

	return RAW_TO_SYMBOL[index];
}

enum v32_decision_result v32_decide_known_quadrant(
	const struct v32_equalizer_observation *observation,
	uint8_t quadrant, bool adapt_on_error,
	struct v32_quadrant_decision *decision)
{
	struct v32_quadrant_decision next;
	uint8_t point_index;

	if (!observation_is_valid(observation) ||
	    quadrant >= V32_4800_SIGNAL_POINTS ||
	    !output_is_separate(observation, decision, sizeof(*decision)))
		return V32_DECISION_INVALID_ARGUMENT;
	point_index = (uint8_t)((quadrant + 1u) & 3u);
	next.quadrant = quadrant;
	next.equalizer = equalizer_decision(
		observation, (const uint16_t[]){
			0x268d, 0x468d, 0x668d, 0x068e,
		}[point_index], 0x3299, adapt_on_error);
	*decision = next;
	return V32_DECISION_OK;
}

static const uint16_t PHASE_32_SECTOR[8] = {
	0x2bfa, 0x2972, 0x1fff, 0x24fd,
	0x1fff, 0x168d, 0x1b02, 0x1405,
};

static const uint16_t MAGNITUDE_32_SECTOR[8] = {
	0x39b0, 0x23c6, 0x1000, 0x41f8,
	0x3000, 0x23c6, 0x41f8, 0x39b0,
};

/* First-quadrant phase profile for the odd 4-by-4 decision grid, indexed by
 * increasing absolute I and then absolute Q.  Reflections generate the other
 * three quadrants without carrying a 64-word table. */
static const uint16_t PHASE_64_QUADRANT[16] = {
	0x0fff, 0x1972, 0x1bfa, 0x1d1b,
	0x068d, 0x0fff, 0x14fd, 0x17c0,
	0x0405, 0x0b02, 0x0fff, 0x135d,
	0x02e4, 0x083f, 0x0ca2, 0x0fff,
};

static const uint16_t MAGNITUDE_64_QUADRANT[16] = {
	0x0b50, 0x194c, 0x28ca, 0x3891,
	0x194c, 0x21f0, 0x2ea5, 0x3ced,
	0x28ca, 0x2ea5, 0x3891, 0x44d1,
	0x3891, 0x3ced, 0x44d1, 0x4f32,
};

/* The 128-point decision set is four exact rotations of this sector. */
static const uint16_t PHASE_128_SECTOR[32] = {
	0x2e27, 0x2dbe, 0x2a93, 0x2972,
	0x2d1b, 0x2bfa, 0x27c0, 0x24fd,
	0x2972, 0x1fff, 0x1fff, 0x168d,
	0x274f, 0x25ab, 0x2474, 0x2288,
	0x235d, 0x1fff, 0x1fff, 0x1ca2,
	0x1b02, 0x1405, 0x183f, 0x12e4,
	0x1d77, 0x1a54, 0x1b8b, 0x18b0,
	0x168d, 0x1241, 0x156c, 0x11d8,
};

static const uint16_t MAGNITUDE_128_SECTOR[32] = {
	0x3e7b, 0x3339, 0x407f, 0x35aa,
	0x2800, 0x1cd8, 0x2b14, 0x20fc,
	0x11e3, 0x0800, 0x1800, 0x11e3,
	0x445a, 0x3a3d, 0x49c1, 0x407f,
	0x30a9, 0x2800, 0x3800, 0x30a9,
	0x20fc, 0x1cd8, 0x2b14, 0x2800,
	0x407f, 0x3a3d, 0x49c1, 0x445a,
	0x35aa, 0x3339, 0x407f, 0x3e7b,
};

static const int16_t ROTATED_LEVEL_32[3] = {
	0x0b50, 0x21f1, 0x3891,
};

static const int16_t ROTATED_LEVEL_128[6] = {
	0x05a8, 0x10f8, 0x1c48, 0x2799, 0x32e9, 0x3e39,
};

static int16_t absolute_wrap_s16(int16_t value)
{
	return value < 0 ? wrap_s16_from_s32(-(int32_t)value) : value;
}

struct rotated_sample {
	struct v32_fixed_point point;
	uint8_t rotation;
};

static struct rotated_sample mirrored_rotation(
	const struct v32_equalizer_observation *observation)
{
	static const int16_t COSINE[4] = {
		0x5a82, (int16_t)0xa57e, (int16_t)0xa57e, 0x5a82,
	};
	static const int16_t SINE[4] = {
		(int16_t)0xa57e, (int16_t)0xa57e, 0x5a82, 0x5a82,
	};
	int16_t absolute_i = absolute_wrap_s16(observation->i);
	int16_t absolute_q = absolute_wrap_s16(observation->q);
	uint8_t rotation = observation->q < observation->i ?
		(uint8_t)((absolute_q < absolute_i) + 2u) :
		(uint8_t)(absolute_q < absolute_i);
	int16_t i_cos = wrap_s16_from_s32(arithmetic_shift_right_u32(
		(uint32_t)((int32_t)observation->i * COSINE[rotation]), 15));
	int16_t q_sin = wrap_s16_from_s32(arithmetic_shift_right_u32(
		(uint32_t)((int32_t)observation->q * SINE[rotation]), 15));
	int16_t q_cos = wrap_s16_from_s32(arithmetic_shift_right_u32(
		(uint32_t)((int32_t)COSINE[rotation] * observation->q), 15));
	int16_t i_sin = wrap_s16_from_s32(arithmetic_shift_right_u32(
		(uint32_t)((int32_t)SINE[rotation] * observation->i), 15));

	return (struct rotated_sample){
		.point = {
			.i = wrap_s16_from_s32((int32_t)i_cos - q_sin),
			.q = wrap_s16_from_s32((int32_t)q_cos + i_sin),
		},
		.rotation = rotation,
	};
}

static struct v32_fixed_point point_32_sector(uint8_t index)
{
	uint8_t i_level = index < 3 ? 0 : (index < 6 ? 1 : 2);
	uint8_t q_level = index < 6 ?
		(uint8_t)(2u - index % 3u) : (uint8_t)(7u - index);

	return (struct v32_fixed_point){
		.i = ROTATED_LEVEL_32[i_level],
		.q = ROTATED_LEVEL_32[q_level],
	};
}

static uint8_t quantize_32(
	const struct v32_equalizer_observation *observation)
{
	struct rotated_sample rotated = mirrored_rotation(observation);
	struct v32_equalizer_observation rotated_observation = {
		.i = rotated.point.i,
		.q = rotated.point.q,
	};
	uint8_t start = 0;
	uint8_t count = 3;
	uint8_t selected;

	if (rotated.point.i > 0x16a0) {
		start = 3;
		if (rotated.point.i > 0x2d41 &&
		    rotated.point.q <= 0x2d40) {
			start = 6;
			count = 2;
		}
	}
	selected = nearest_generated(
		&rotated_observation, start, count, point_32_sector);
	if (selected == 3 || selected == 6)
		selected = distance_squared(
			&rotated_observation, point_32_sector(6)) <
			distance_squared(
				&rotated_observation, point_32_sector(3)) ? 6 : 3;
	return (uint8_t)(selected + rotated.rotation * 8u);
}

static uint16_t rotate_sector_phase(uint16_t phase, uint8_t rotation)
{
	uint16_t rotated = (uint16_t)(phase + ((uint16_t)rotation << 13));

	/* The open first-sector axis is one count below the exact axes reached
	 * after a quarter turn. */
	if (phase == 0x1fff && rotation != 0)
		rotated++;
	return rotated & 0x7fffu;
}

static struct v32_fixed_point point_64(uint8_t index)
{
	int i_level = -7 + 2 * ((index >> 1) & 1u) +
		4 * ((index >> 3) & 1u) + 8 * ((index >> 5) & 1u);
	int q_level = 7 - 2 * (index & 1u) -
		4 * ((index >> 2) & 1u) - 8 * ((index >> 4) & 1u);

	return (struct v32_fixed_point){
		.i = (int16_t)(i_level * 2048),
		.q = (int16_t)(q_level * 2048),
	};
}

static uint8_t start_64(const struct v32_equalizer_observation *sample)
{
	if (sample->i <= 0) {
		if (sample->q <= 0) {
			if (sample->i <= (int16_t)0xe000)
				return sample->q < (int16_t)0xe000 ? 0x14 : 0x10;
			return sample->q < (int16_t)0xe000 ? 0x1c : 0x18;
		}
		if (sample->i <= (int16_t)0xe000)
			return sample->q < 0x2000 ? 0x04 : 0x00;
		return sample->q < 0x2000 ? 0x0c : 0x08;
	}
	if (sample->q <= 0) {
		if (sample->i <= 0x2000)
			return sample->q < (int16_t)0xe000 ? 0x34 : 0x30;
		return sample->q < (int16_t)0xe000 ? 0x3c : 0x38;
	}
	if (sample->i <= 0x2000)
		return sample->q < 0x2000 ? 0x24 : 0x20;
	return sample->q < 0x2000 ? 0x2c : 0x28;
}

static uint16_t phase_64(uint8_t index)
{
	struct v32_fixed_point point = point_64(index);
	unsigned absolute_i = (unsigned)(point.i < 0 ? -point.i : point.i);
	unsigned absolute_q = (unsigned)(point.q < 0 ? -point.q : point.q);
	unsigned i_index = (absolute_i / 2048u - 1u) / 2u;
	unsigned q_index = (absolute_q / 2048u - 1u) / 2u;
	uint16_t base = PHASE_64_QUADRANT[i_index * 4u + q_index];
	uint16_t diagonal = absolute_i == absolute_q ? 1u : 0u;

	if (point.i > 0 && point.q > 0)
		return base;
	if (point.i < 0 && point.q > 0)
		return (uint16_t)(0x3fffu - base - diagonal);
	if (point.i < 0 && point.q < 0)
		return (uint16_t)(0x4000u + base + diagonal);
	return (uint16_t)(0x7fffu - base - diagonal);
}

static unsigned profile_index_64(struct v32_fixed_point point)
{
	unsigned absolute_i = (unsigned)(point.i < 0 ? -point.i : point.i);
	unsigned absolute_q = (unsigned)(point.q < 0 ? -point.q : point.q);

	return ((absolute_i / 2048u - 1u) / 2u) * 4u +
	       (absolute_q / 2048u - 1u) / 2u;
}

static uint16_t magnitude_64(uint8_t index)
{
	return MAGNITUDE_64_QUADRANT[
		profile_index_64(point_64(index))];
}

static uint8_t quantize_64(
	const struct v32_equalizer_observation *observation)
{
	return nearest_generated(
		observation, start_64(observation), 4, point_64);
}

static struct v32_fixed_point point_128_sector(uint8_t index)
{
	uint8_t group = (uint8_t)(index >> 3);
	uint8_t i_level = (uint8_t)((index < 12 ? 0 :
		(index < 24 ? 2 : 4)) + ((index >> 1) & 1u));
	uint8_t q_level;

	if (group == 0)
		q_level = (uint8_t)(5u - (index & 1u) -
			2u * ((index >> 2) & 1u));
	else if (group == 1)
		q_level = (uint8_t)(1u - (index & 1u) +
			4u * ((index >> 2) & 1u));
	else
		q_level = (uint8_t)(3u - (index & 1u) -
			2u * ((index >> 2) & 1u));
	return (struct v32_fixed_point){
		.i = ROTATED_LEVEL_128[i_level],
		.q = ROTATED_LEVEL_128[q_level],
	};
}

static uint8_t nearest_pair(
	struct v32_fixed_point sample,
	struct v32_fixed_point first, uint8_t first_index,
	struct v32_fixed_point second, uint8_t second_index)
{
	struct v32_equalizer_observation observation = {
		.i = sample.i,
		.q = sample.q,
	};

	return distance_squared(&observation, second) <
		       distance_squared(&observation, first) ?
		       second_index : first_index;
}

static uint8_t quantize_128(
	const struct v32_equalizer_observation *observation)
{
	struct rotated_sample rotated = mirrored_rotation(observation);
	uint8_t start;
	uint8_t selected;

	if (rotated.point.i <= 0x16a0) {
		start = rotated.point.q > 0x2d40 ? 0x00 :
			(rotated.point.q <= 0x169f ? 0x08 : 0x04);
	} else if (rotated.point.i <= 0x2d41) {
		start = rotated.point.q > 0x2d40 ? 0x0c :
			(rotated.point.q <= 0x169f ? 0x14 : 0x10);
	} else if (rotated.point.q <= 0x2d41) {
		start = rotated.point.q <= 0x16a0 ? 0x1c : 0x18;
	} else if (rotated.point.i <= 0x3891) {
		if (rotated.point.q <= 0x3891) {
			selected = nearest_pair(
				rotated.point,
				(struct v32_fixed_point){ 0x32e9, 0x2799 }, 0x18,
				(struct v32_fixed_point){ 0x2799, 0x32e9 }, 0x0f);
			return (uint8_t)(selected + rotated.rotation * 32u);
		}
		start = 0x0e;
	} else if (rotated.point.q > 0x3890) {
		selected = nearest_pair(
			rotated.point,
			(struct v32_fixed_point){ 0x2799, 0x3e39 }, 0x1a,
			(struct v32_fixed_point){ 0x3e3a, 0x2799 }, 0x0e);
		return (uint8_t)(selected + rotated.rotation * 32u);
	} else {
		start = 0x1a;
	}
	selected = nearest_generated(
		&(struct v32_equalizer_observation){
			.i = rotated.point.i,
			.q = rotated.point.q,
		}, start, 4, point_128_sector);
	return (uint8_t)(selected + rotated.rotation * 32u);
}

static uint16_t phase_128(uint8_t index)
{
	return rotate_sector_phase(
		PHASE_128_SECTOR[index & 31u], (uint8_t)(index >> 5));
}

static uint16_t magnitude_128(uint8_t index)
{
	/* This one-word asymmetry is required for exact decision parity. */
	return index == 77 ? 0x3a3e : MAGNITUDE_128_SECTOR[index & 31u];
}

enum v32_decision_result v32_decide_tcm(
	const struct v32_equalizer_observation *observation,
	uint8_t data_bits_per_symbol, bool adapt_on_error,
	struct v32_tcm_decision *decision)
{
	struct v32_tcm_decision next;
	uint8_t selected;
	uint16_t phase;
	uint16_t magnitude;

	if (!observation_is_valid(observation) ||
	    data_bits_per_symbol < 3 || data_bits_per_symbol > 6 ||
	    !output_is_separate(observation, decision, sizeof(*decision)))
		return V32_DECISION_INVALID_ARGUMENT;
	switch (data_bits_per_symbol) {
	case 3:
		selected = nearest_16(observation);
		phase = PHASE_16[selected];
		magnitude = magnitude_16(selected, false);
		break;
	case 4:
		selected = quantize_32(observation);
		phase = rotate_sector_phase(
			PHASE_32_SECTOR[selected & 7u],
			(uint8_t)(selected >> 3));
		magnitude = MAGNITUDE_32_SECTOR[selected & 7u];
		break;
	case 5:
		selected = quantize_64(observation);
		phase = phase_64(selected);
		magnitude = magnitude_64(selected);
		break;
	default:
		selected = quantize_128(observation);
		phase = phase_128(selected);
		magnitude = magnitude_128(selected);
		break;
	}
	next.feedback_point = selected;
	next.equalizer = equalizer_decision(
		observation, phase, magnitude, adapt_on_error);
	*decision = next;
	return V32_DECISION_OK;
}

enum v32_decision_result v32_decide_4_point(
	const struct v32_equalizer_observation *observation,
	bool adapt_on_error, struct v32_quadrant_decision *decision)
{
	struct v32_quadrant_decision next;
	uint64_t best_distance = UINT64_MAX;
	uint8_t selected = 0;

	if (!observation_is_valid(observation) ||
	    !output_is_separate(observation, decision, sizeof(*decision)))
		return V32_DECISION_INVALID_ARGUMENT;
	for (uint8_t point_index = 0; point_index < 4; ++point_index) {
		struct v32_fixed_point point;
		uint8_t quadrant = (uint8_t)((point_index + 3u) & 3u);
		uint64_t distance;

		(void)v32_4800_fixed_point(quadrant, &point);
		distance = distance_squared(observation, point);
		if (distance < best_distance) {
			best_distance = distance;
			selected = point_index;
		}
	}
	next.quadrant = (uint8_t)((selected + 3u) & 3u);
	next.equalizer = equalizer_decision(
		observation, (const uint16_t[]){
			0x268d, 0x468d, 0x668d, 0x068e,
		}[selected], 0x3299, adapt_on_error);
	*decision = next;
	return V32_DECISION_OK;
}

enum v32_decision_result v32_decide_uncoded_16(
	const struct v32_equalizer_observation *observation,
	bool adapt_on_error, struct v32_uncoded_decision *decision)
{
	struct v32_uncoded_decision next;
	uint8_t selected;

	if (!observation_is_valid(observation) ||
	    !output_is_separate(observation, decision, sizeof(*decision)))
		return V32_DECISION_INVALID_ARGUMENT;
	selected = nearest_16(observation);
	next.symbol = symbol_16(selected);
	next.equalizer = equalizer_decision(
		observation, PHASE_16[selected], magnitude_16(selected, true),
		adapt_on_error);
	*decision = next;
	return V32_DECISION_OK;
}
