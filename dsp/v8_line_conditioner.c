// SPDX-License-Identifier: BSD-3-Clause

#include "v8_line_conditioner.h"

#include "integer_math.h"
#include "memory_region.h"

#include <string.h>

enum {
	V8_LINE_POWER_WEIGHT_Q15 =
		(0x8000 + V8_LINE_POWER_HISTORY_SAMPLES / 2) /
		V8_LINE_POWER_HISTORY_SAMPLES,
};

static const int16_t V8_CALLER_ENTRANCE_FILTER[] = {
	-16, 11, 39, 21, -59, -111, 2, 190, 282, -281, -312, -636,
	1241, 272, 1444, -5382, 1199, 7549, -7124, 895, -42, 1626,
	-430, -276, -469, 210, 221, 75, -94, -87, 0, 41, 21, 0,
};

static const int16_t V8_ANSWERER_ENTRANCE_FILTER[] = {
	30, -2, -8, -16, 40, -84, -115, -98, -55, 699, 1103, 617,
	-824, -2022, -1731, 148, 2127, 2483, 894, -1233, -2129, -1325,
	205, 1063, 912, 159, -157, -65, -146, 37, -9, -7, -10, 9,
};

_Static_assert(
	sizeof(V8_CALLER_ENTRANCE_FILTER) /
			sizeof(V8_CALLER_ENTRANCE_FILTER[0]) ==
		V8_LINE_ENTRANCE_FILTER_TAPS &&
		sizeof(V8_ANSWERER_ENTRANCE_FILTER) /
			sizeof(V8_ANSWERER_ENTRANCE_FILTER[0]) ==
		V8_LINE_ENTRANCE_FILTER_TAPS,
	"V.8 entrance filter profile changed");

static bool role_is_valid(enum v8_station_role role)
{
	return role == V8_CALLING_STATION ||
	       role == V8_ANSWERING_STATION;
}

static bool gain_mode_is_valid(enum v8_line_gain_mode mode)
{
	return mode == V8_LINE_GAIN_FROZEN ||
	       mode == V8_LINE_GAIN_FINE ||
	       mode == V8_LINE_GAIN_COARSE;
}

bool v8_line_conditioner_is_valid(
	const struct v8_line_conditioner *conditioner)
{
	return conditioner != NULL && role_is_valid(conditioner->role) &&
	       gain_mode_is_valid(conditioner->gain_mode) &&
	       conditioner->gain != 0 &&
	       (int16_t)conditioner->gain > 0 &&
	       conditioner->power_index < V8_LINE_POWER_HISTORY_SAMPLES &&
	       conditioner->input_count < V8_LINE_INPUT_QUEUE_SAMPLES &&
	       conditioner->overflow_count < 10;
}

enum v8_line_conditioner_result v8_line_conditioner_init(
	struct v8_line_conditioner *conditioner,
	enum v8_station_role role)
{
	struct v8_line_conditioner initialized = {
		.gain = 0x0200,
		.role = role,
		.gain_mode = role == V8_ANSWERING_STATION ?
			V8_LINE_GAIN_COARSE : V8_LINE_GAIN_FROZEN,
	};

	if (conditioner == NULL || !role_is_valid(role) ||
	    !v8_line_conditioner_is_valid(&initialized))
		return V8_LINE_CONDITIONER_INVALID_ARGUMENT;
	*conditioner = initialized;
	return V8_LINE_CONDITIONER_OK;
}

enum v8_line_conditioner_result v8_line_conditioner_set_gain_mode(
	struct v8_line_conditioner *conditioner,
	enum v8_line_gain_mode mode)
{
	struct v8_line_conditioner next;

	if (!v8_line_conditioner_is_valid(conditioner) ||
	    !gain_mode_is_valid(mode))
		return V8_LINE_CONDITIONER_INVALID_ARGUMENT;
	next = *conditioner;
	next.gain_mode = mode;
	if (!v8_line_conditioner_is_valid(&next))
		return V8_LINE_CONDITIONER_INVALID_ARGUMENT;
	*conditioner = next;
	return V8_LINE_CONDITIONER_OK;
}

static const int16_t *entrance_filter(enum v8_station_role role)
{
	return role == V8_CALLING_STATION ?
		       V8_CALLER_ENTRANCE_FILTER :
		       V8_ANSWERER_ENTRANCE_FILTER;
}

static int16_t entrance_filter_sample(
	struct v8_line_conditioner *conditioner, int16_t sample)
{
	const int16_t *coefficients = entrance_filter(conditioner->role);
	uint32_t accumulator = 0;

	for (size_t index = 0;
	     index < V8_LINE_ENTRANCE_FILTER_TAPS; ++index) {
		int16_t previous = conditioner->entrance_history[index];

		conditioner->entrance_history[index] = sample;
		accumulator += (uint32_t)((int32_t)sample * coefficients[index]);
		sample = previous;
	}
	return wrap_s16_from_s32(
		arithmetic_shift_right_u32(accumulator, 14));
}

static int16_t gain_step(enum v8_line_gain_mode mode)
{
	return mode == V8_LINE_GAIN_COARSE ? 0x3333 : 0x0800;
}

static void adapt_gain(
	struct v8_line_conditioner *conditioner, uint32_t block_energy)
{
	int32_t filtered = (int16_t)conditioner->filtered_energy;
	int32_t block_high = (uint16_t)(block_energy >> 16);
	int32_t next_filtered =
		arithmetic_shift_right_u32(
			(uint32_t)(filtered * 0x6952), 15) + block_high;
	int32_t deviation;
	int32_t delta;

	if (((uint32_t)next_filtered >> 15) == 0u ||
	    ((uint32_t)next_filtered >> 15) == 0x1ffffu)
		conditioner->filtered_energy = (uint16_t)next_filtered;
	else
		conditioner->filtered_energy = 0x7f00;
	if (conditioner->gain_mode == V8_LINE_GAIN_FROZEN)
		return;

	deviation = (int16_t)conditioner->filtered_energy - 4000;
	if (magnitude_s16(wrap_s16_from_s32(deviation)) <= 2000)
		return;
	delta = arithmetic_shift_right_u32(
		(uint32_t)(gain_step(conditioner->gain_mode) * deviation), 16) +
		(int16_t)conditioner->gain_delta;
	if (magnitude_s16(wrap_s16_from_s32(delta)) <= 1000) {
		conditioner->gain_delta = (uint16_t)delta;
		return;
	}

	conditioner->gain_delta = 0;
	if (delta > 0) {
		conditioner->gain = (uint16_t)arithmetic_shift_right_u32(
			(uint32_t)((int16_t)conditioner->gain * 0x390a), 14);
	} else if ((int16_t)conditioner->gain <= 0x6a00) {
		conditioner->gain = (uint16_t)arithmetic_shift_right_u32(
			(uint32_t)((int16_t)conditioner->gain * 0x47cf), 14);
	}
}

static int16_t scale_sample(
	int16_t gain, int16_t sample, bool *overflow)
{
	int32_t product = (int32_t)gain * sample;
	uint32_t top = (uint32_t)product >> 25;

	if (top == 0u || top == 0x7fu)
		return wrap_s16_from_s32(
			arithmetic_shift_right_u32((uint32_t)product, 10));
	*overflow = true;
	return wrap_s16_from_s32(
		       arithmetic_shift_right_u32((uint32_t)product, 16)) < 1 ?
		       (int16_t)0x8100 : (int16_t)0x7f00;
}

static void condition_block(
	struct v8_line_conditioner *conditioner,
	struct v8_line_conditioned_block *block)
{
	uint32_t history_energy = 0;
	uint16_t energy_level;

	block->gain_reset = false;
	for (size_t index = 0; index < V8_LINE_BLOCK_SAMPLES; ++index)
		block->samples[index] = entrance_filter_sample(
			conditioner, block->samples[index]);
	for (size_t index = 0; index < V8_LINE_BLOCK_SAMPLES; ++index) {
		conditioner->power_history[conditioner->power_index] =
			(uint16_t)block->samples[index];
		conditioner->power_index++;
		if (conditioner->power_index == V8_LINE_POWER_HISTORY_SAMPLES)
			conditioner->power_index = 0;
	}
	for (size_t index = 0;
	     index < V8_LINE_POWER_HISTORY_SAMPLES; ++index) {
		int16_t sample = (int16_t)conditioner->power_history[index];
		int32_t weighted = arithmetic_shift_right_u32(
			(uint32_t)((int32_t)sample * V8_LINE_POWER_WEIGHT_Q15), 15);

		history_energy += (uint32_t)(weighted * sample);
	}
	energy_level = fixed_square_root_u32(history_energy);

	for (size_t index = 0; index < V8_LINE_BLOCK_SAMPLES; ++index) {
		bool overflow = false;

		block->samples[index] = scale_sample(
			(int16_t)conditioner->gain, block->samples[index],
			&overflow);
		if (!overflow)
			continue;
		conditioner->overflow_count++;
		if (conditioner->overflow_count != 10)
			continue;
		conditioner->gain = 0x0400;
		conditioner->overflow_count = 0;
		block->gain_reset = true;
	}
	if (energy_level > 0x001fu) {
		uint32_t block_energy = 0;

		for (size_t index = 0; index < V8_LINE_BLOCK_SAMPLES; ++index)
			block_energy += (uint32_t)(
				(int32_t)block->samples[index] * block->samples[index]);
		adapt_gain(conditioner, block_energy);
	}
}

enum v8_line_conditioner_result v8_line_conditioner_process_sample(
	struct v8_line_conditioner *conditioner, int16_t sample,
	struct v8_line_conditioned_block *block, bool *ready)
{
	struct v8_line_conditioner next;
	struct v8_line_conditioned_block produced;
	uint16_t filtered_word;
	uint32_t feedback;
	bool produced_block = false;

	if (!v8_line_conditioner_is_valid(conditioner) || block == NULL ||
	    ready == NULL ||
	    memory_regions_overlap(
		    conditioner, sizeof(*conditioner), block, sizeof(*block)) ||
	    memory_regions_overlap(
		    conditioner, sizeof(*conditioner), ready, sizeof(*ready)) ||
	    memory_regions_overlap(block, sizeof(*block), ready, sizeof(*ready)))
		return V8_LINE_CONDITIONER_INVALID_ARGUMENT;
	next = *conditioner;
	filtered_word = (uint16_t)sample + next.post_filter_word;
	next.input_queue[next.input_count++] =
		wrap_s16_from_s32(filtered_word);
	feedback =
		(uint32_t)((int32_t)wrap_s16_from_s32(filtered_word) * 0x0f6d) -
		((uint32_t)(int32_t)sample << 12);
	next.post_filter_word =
		(uint16_t)arithmetic_shift_right_u32(feedback, 12);
	if (next.input_count == V8_LINE_INPUT_QUEUE_SAMPLES) {
		memcpy(
			produced.samples, next.input_queue,
			sizeof(produced.samples));
		condition_block(&next, &produced);
		memmove(
			next.input_queue,
			&next.input_queue[V8_LINE_BLOCK_SAMPLES],
			(V8_LINE_INPUT_QUEUE_SAMPLES - V8_LINE_BLOCK_SAMPLES) *
				sizeof(next.input_queue[0]));
		next.input_count =
			V8_LINE_INPUT_QUEUE_SAMPLES - V8_LINE_BLOCK_SAMPLES;
		produced_block = true;
	}
	if (!v8_line_conditioner_is_valid(&next))
		return V8_LINE_CONDITIONER_INVALID_ARGUMENT;
	*conditioner = next;
	if (produced_block)
		*block = produced;
	*ready = produced_block;
	return V8_LINE_CONDITIONER_OK;
}
