// SPDX-License-Identifier: BSD-3-Clause

#include "decision.h"

#include "constellation.h"
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
