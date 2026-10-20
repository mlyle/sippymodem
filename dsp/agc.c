// SPDX-License-Identifier: BSD-3-Clause

#include "agc.h"
#include "integer_math.h"
#include "memory_region.h"
#include "sample_math.h"

#include <stdint.h>

enum {
	AGC_ENERGY_SCALE_Q15 = 0x038e,
};

enum agc_result agc_measure_level_s16(
	const int16_t *samples, size_t count, int16_t *level)
{
	int16_t measured;

	if (samples == NULL || count == 0 || count > UINT16_MAX ||
	    level == NULL || count > SIZE_MAX / sizeof(*samples) ||
	    memory_regions_overlap(
		    samples, count * sizeof(*samples), level, sizeof(*level)))
		return AGC_INVALID_ARGUMENT;

	measured = wrap_s16_from_s32(
		(int32_t)sample_level_s16(samples, count, AGC_ENERGY_SCALE_Q15));
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
	       config->history_weight_q15 > 0;
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
		block_gain = wrap_s16_from_s32(smooth_q15_s16(
			next.smoothed_level, level,
			(uint16_t)next.config.history_weight_q15));
		next.smoothed_level = block_gain;
		if (next.gain_mode == AGC_GAIN_TRACKING && block_gain != 0) {
			uint16_t exponent;
			uint16_t reciprocal = normalized_reciprocal_q15(
				(uint16_t)block_gain, &exponent);

			next.gain_shift = (int16_t)exponent - 1;
			next.gain_mantissa_q15 = wrap_s16_from_s32(
				arithmetic_shift_right_u32(
					(uint32_t)((int32_t)reciprocal *
						   next.config.target_level_q15), 15u));
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
			int32_t normalized = arithmetic_shift_right_u32(
				(uint32_t)product, 15u);
			uint16_t shift = (uint16_t)next.gain_shift & 0x1fu;

			output[index] = clamp_s16(
				(int64_t)normalized * (INT64_C(1) << shift));
		}
	}
	*control = next;
	return AGC_OK;
}
