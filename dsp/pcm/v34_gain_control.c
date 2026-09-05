// SPDX-License-Identifier: BSD-3-Clause

#include "v34_gain_control.h"

#include "agc.h"
#include "integer_math.h"
#include "memory_region.h"

#include <stdint.h>
#include <string.h>

enum {
	V34_GAIN_ROUNDING = 0x0200,
	V34_GAIN_CORRECTION_Q15 = 0x3333,
	V34_GAIN_TARGET_ENERGY = 0x0fa0,
	V34_GAIN_ENERGY_DEADBAND = 0x04b0,
	V34_GAIN_CORRECTION_LIMIT = 0x01f4,
	V34_GAIN_DECREASE_Q14 = 0x390a,
	V34_GAIN_INCREASE_Q14 = 0x47cf,
	V34_GAIN_INCREASE_CEILING = 0x6a00,
	V34_GAIN_MINIMUM_LEVEL = 0x001f,
	V34_GAIN_ENERGY_HISTORY_Q15 = 0x6ccd,
};

static int16_t scale_sample(int16_t gain, int16_t sample)
{
	uint32_t product = (uint32_t)((int32_t)gain * sample);
	uint32_t top = product >> 25;
	int16_t overflow = wrap_s16_from_s32(
		arithmetic_shift_right_u32(product, 16));

	if (top == 0 || top == 0x7f)
		return wrap_s16_from_s32(arithmetic_shift_right_u32(
			product + V34_GAIN_ROUNDING, V34_GAIN_FRACTION_BITS));
	return overflow <= 0 ? (int16_t)0x8100 : (int16_t)0x7f00;
}

static uint16_t smooth_energy(uint16_t average, uint32_t energy)
{
	int32_t history = arithmetic_shift_right_u32(
		(uint32_t)((int32_t)(int16_t)average *
			   V34_GAIN_ENERGY_HISTORY_Q15),
		15);
	uint32_t combined = (uint32_t)history + (energy >> 16);
	uint32_t top = combined >> 15;

	return top == 0 || top == 0x1ffff ?
		       (uint16_t)combined : UINT16_C(0x7f00);
}

static void correct_gain(struct v34_gain_control *control)
{
	int16_t delta = wrap_s16_from_s32(
		(int32_t)control->average_energy - V34_GAIN_TARGET_ENERGY);
	int16_t delta_magnitude = absolute_wrap_s16(delta);
	int32_t correction_step;
	int16_t next_correction;
	int16_t correction_magnitude;
	int16_t scale;

	if (wrap_s16_from_s32((int32_t)delta_magnitude -
		     V34_GAIN_ENERGY_DEADBAND) <= 0)
		return;
	correction_step = arithmetic_shift_right_u32(
		(uint32_t)((int32_t)V34_GAIN_CORRECTION_Q15 * delta), 16);
	next_correction = wrap_s16_from_s32(
		(int32_t)control->correction + correction_step);
	correction_magnitude = absolute_wrap_s16(next_correction);
	control->correction = next_correction;
	if (wrap_s16_from_s32((int32_t)correction_magnitude -
		     V34_GAIN_CORRECTION_LIMIT) <= 0)
		return;
	control->correction = 0;
	if (next_correction > 0) {
		scale = V34_GAIN_DECREASE_Q14;
	} else {
		if (control->gain > V34_GAIN_INCREASE_CEILING)
			return;
		scale = V34_GAIN_INCREASE_Q14;
	}
	control->gain = multiply_shift_s16(control->gain, scale, 14);
}

bool v34_gain_control_is_valid(const struct v34_gain_control *control)
{
	return control != NULL && control->initialized &&
	       control->history_index < V34_GAIN_HISTORY_SAMPLES &&
	       control->energy_count < V34_GAIN_UPDATE_SAMPLES &&
	       control->gain >= 0 &&
	       (control->mode == V34_GAIN_TRACKING ||
		control->mode == V34_GAIN_FROZEN);
}

enum v34_gain_control_result v34_gain_control_init(
	struct v34_gain_control *control, int16_t initial_gain)
{
	struct v34_gain_control initialized;

	if (control == NULL || initial_gain <= 0)
		return V34_GAIN_CONTROL_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	initialized.gain = initial_gain;
	initialized.mode = V34_GAIN_TRACKING;
	initialized.initialized = true;
	*control = initialized;
	return V34_GAIN_CONTROL_OK;
}

enum v34_gain_control_result v34_gain_control_set_mode(
	struct v34_gain_control *control, enum v34_gain_mode mode)
{
	if (!v34_gain_control_is_valid(control))
		return V34_GAIN_CONTROL_INVALID_STATE;
	if (mode != V34_GAIN_TRACKING && mode != V34_GAIN_FROZEN)
		return V34_GAIN_CONTROL_INVALID_ARGUMENT;
	control->mode = mode;
	return V34_GAIN_CONTROL_OK;
}

enum v34_gain_control_result v34_gain_control_process(
	struct v34_gain_control *control,
	struct v34_analytic_sample input,
	struct v34_analytic_sample *output)
{
	struct v34_gain_control next;
	struct v34_analytic_sample generated;
	uint16_t next_count;
	uint32_t next_energy;

	if (!v34_gain_control_is_valid(control))
		return V34_GAIN_CONTROL_INVALID_STATE;
	if (output == NULL || memory_regions_overlap(
			control, sizeof(*control), output, sizeof(*output)))
		return V34_GAIN_CONTROL_INVALID_ARGUMENT;
	next = *control;
	next.history[next.history_index] = input.in_phase;
	next.history_index = (uint16_t)(next.history_index + 1u);
	if (next.history_index == V34_GAIN_HISTORY_SAMPLES)
		next.history_index = 0;
	generated = (struct v34_analytic_sample){
		.in_phase = scale_sample(next.gain, input.in_phase),
		.quadrature = scale_sample(next.gain, input.quadrature),
	};
	next_count = (uint16_t)(next.energy_count + 1u);
	next_energy = (uint32_t)((int32_t)generated.in_phase *
				 (int32_t)generated.in_phase) +
		      next.energy_accumulator;
	if (next_count < V34_GAIN_UPDATE_SAMPLES) {
		next.energy_count = next_count;
		next.energy_accumulator = next_energy;
	} else {
		if (next.mode == V34_GAIN_TRACKING) {
			int16_t level;

			if (agc_measure_level_s16(
				    next.history, V34_GAIN_HISTORY_SAMPLES,
				    &level) != AGC_OK)
				return V34_GAIN_CONTROL_INVALID_STATE;
			if (level > V34_GAIN_MINIMUM_LEVEL) {
				next.average_energy = smooth_energy(
					next.average_energy, next_energy);
				correct_gain(&next);
			}
		}
		next.energy_count = 0;
		next.energy_accumulator = 0;
	}
	if (!v34_gain_control_is_valid(&next))
		return V34_GAIN_CONTROL_INVALID_STATE;
	*control = next;
	*output = generated;
	return V34_GAIN_CONTROL_OK;
}
