// SPDX-License-Identifier: BSD-3-Clause

#include "agc.h"
#include "integer_math.h"
#include "memory_region.h"

#include <limits.h>
#include <stdint.h>

enum {
	AGC_ENERGY_SCALE_Q15 = 0x038e,
	AGC_WEIGHT_TOTAL_Q15 = 0x8000,
};

static int32_t shift_q15(uint32_t bits)
{
	int64_t value = bits <= INT32_MAX ?
			(int64_t)bits :
			(int64_t)bits - (INT64_C(1) << 32);

	return (int32_t)(value >= 0 ? value / INT32_C(0x8000) :
			 -((-value + INT32_C(0x7fff)) / INT32_C(0x8000)));
}

/* Exact formula for the normalized 128-entry reciprocal profile. */
static uint16_t reciprocal_profile(
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

enum agc_result agc_measure_level_s16(
	const int16_t *samples, size_t count, int16_t *level)
{
	uint32_t energy = 0;
	int16_t measured;

	if (samples == NULL || count == 0 || count > UINT16_MAX ||
	    level == NULL || count > SIZE_MAX / sizeof(*samples) ||
	    memory_regions_overlap(
		    samples, count * sizeof(*samples), level, sizeof(*level)))
		return AGC_INVALID_ARGUMENT;

	for (size_t index = 0; index < count; ++index) {
		int32_t product =
			(int32_t)samples[index] * AGC_ENERGY_SCALE_Q15;
		int32_t scaled = shift_q15((uint32_t)product);

		energy += (uint32_t)(scaled * (int32_t)samples[index]);
	}
	measured = wrap_s16_from_s32(
		(int32_t)fixed_square_root_u32(energy));
	*level = measured;
	return AGC_OK;
}

static bool config_is_valid(const struct agc_config *config)
{
	return config != NULL && config->target_level_q15 > 0 &&
	       config->acquisition_threshold >= 0 &&
	       config->tracking_threshold >=
		       config->acquisition_threshold &&
	       config->block_samples != 0 &&
	       config->history_weight_q15 >= 0 &&
	       config->input_weight_q15 >= 0 &&
	       (uint32_t)config->history_weight_q15 +
		       (uint32_t)config->input_weight_q15 ==
		       AGC_WEIGHT_TOTAL_Q15;
}

bool agc_is_valid(const struct agc *control)
{
	return control != NULL && config_is_valid(&control->config) &&
	       control->smoothed_level >= 0 &&
	       control->gain_mantissa_q15 >= 0 &&
	       control->gain_shift >= 0 && control->gain_shift <= 14 &&
	       (control->gain_mode == AGC_GAIN_TRACKING ||
		control->gain_mode == AGC_GAIN_FROZEN);
}

enum agc_result agc_init(
	struct agc *control, const struct agc_config *config)
{
	struct agc initialized;

	if (control == NULL || !config_is_valid(config))
		return AGC_INVALID_ARGUMENT;
	initialized = (struct agc){
		.config = *config,
		.gain_mode = AGC_GAIN_TRACKING,
	};
	if (!agc_is_valid(&initialized))
		return AGC_INVALID_STATE;
	*control = initialized;
	return AGC_OK;
}

enum agc_result agc_freeze(struct agc *control)
{
	if (!agc_is_valid(control))
		return AGC_INVALID_STATE;
	control->gain_mode = AGC_GAIN_FROZEN;
	return AGC_OK;
}

enum agc_result agc_reacquire(struct agc *control)
{
	if (!agc_is_valid(control))
		return AGC_INVALID_STATE;
	control->gain_mode = AGC_GAIN_TRACKING;
	control->smoothed_level = 0;
	control->signal_present = false;
	return AGC_OK;
}

enum agc_result agc_process(
	struct agc *control, const int16_t *input, size_t count,
	int16_t *output)
{
	struct agc next;
	int16_t level;
	int16_t block_gain;
	bool active;
	size_t bytes;

	if (!agc_is_valid(control))
		return AGC_INVALID_STATE;
	if (input == NULL || output == NULL ||
	    count != control->config.block_samples)
		return AGC_INVALID_ARGUMENT;
	bytes = count * sizeof(*input);
	if (memory_regions_overlap(input, bytes, control, sizeof(*control)) ||
	    memory_regions_overlap(output, bytes, control, sizeof(*control)) ||
	    (input != output &&
	     memory_regions_overlap(input, bytes, output, bytes)))
		return AGC_INVALID_ARGUMENT;

	next = *control;
	if (agc_measure_level_s16(input, count, &level) != AGC_OK)
		return AGC_INVALID_STATE;
	block_gain = next.gain_mantissa_q15;
	active = !((block_gain == 0 &&
		    level < next.config.acquisition_threshold) ||
		   (block_gain != 0 &&
		    level < next.config.tracking_threshold));
	if (active) {
		int32_t history = shift_q15(
			(uint32_t)((int32_t)next.config.history_weight_q15 *
				   next.smoothed_level));
		int32_t observed = shift_q15(
			(uint32_t)((int32_t)level *
				   next.config.input_weight_q15));

		block_gain = wrap_s16_from_s32(history + observed);
		next.smoothed_level = block_gain;
		if (next.gain_mode == AGC_GAIN_TRACKING && block_gain != 0) {
			uint16_t exponent;
			uint16_t reciprocal = reciprocal_profile(
				(uint16_t)block_gain, &exponent);

			next.gain_shift = (int16_t)exponent - 1;
			next.gain_mantissa_q15 = wrap_s16_from_s32(
				shift_q15(
					(uint32_t)((int32_t)reciprocal *
						   next.config.target_level_q15)));
		}
	}
	next.signal_present = active;
	if (!agc_is_valid(&next))
		return AGC_INVALID_STATE;

	for (size_t index = 0; index < count; ++index) {
		if (!active) {
			output[index] = 0;
		} else {
			int32_t product =
				(int32_t)input[index] * next.gain_mantissa_q15;
			int32_t normalized = shift_q15((uint32_t)product);
			uint16_t shift = (uint16_t)next.gain_shift & 0x1fu;

			output[index] = clamp_s16(
				(int64_t)normalized * (INT64_C(1) << shift));
		}
	}
	*control = next;
	return AGC_OK;
}

const char *agc_result_name(enum agc_result result)
{
	switch (result) {
	case AGC_OK:
		return "ok";
	case AGC_INVALID_ARGUMENT:
		return "invalid argument";
	case AGC_INVALID_STATE:
		return "invalid state";
	}
	return "unknown AGC result";
}
