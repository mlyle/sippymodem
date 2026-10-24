// SPDX-License-Identifier: BSD-3-Clause

#include "v34_gain_control.h"

#include "agc.h"
#include "integer_math.h"
#include "memory_region.h"
#include "sample_math.h"

#include <stdint.h>
#include <string.h>

enum {
	V34_GAIN_ROUNDING = 0x0200,
	V34_GAIN_CORRECTION_Q15 = 0x3333,
	V34_GAIN_INFO_CORRECTION_Q15 = 0x199a,
	V34_GAIN_TARGET_ENERGY = 0x0fa0,
	V34_GAIN_ENERGY_DEADBAND = 0x04b0,
	V34_GAIN_CORRECTION_LIMIT = 0x01f4,
	V34_GAIN_MINIMUM_LEVEL = 0x001f,
	V34_GAIN_ENERGY_HISTORY_Q15 = 0x6ccd,
};

static void correct_gain(
	struct v34_gain_control *control, int16_t correction_gain)
{
	int16_t delta = wrap_s16_from_s32(
		(int32_t)control->average_energy - V34_GAIN_TARGET_ENERGY);
	int16_t delta_magnitude = absolute_wrap_s16(delta);
	int32_t correction_step;
	int16_t next_correction;
	int16_t correction_magnitude;

	if (wrap_s16_from_s32((int32_t)delta_magnitude -
		     V34_GAIN_ENERGY_DEADBAND) <= 0)
		return;
	correction_step = arithmetic_shift_right_u32(
		(uint32_t)((int32_t)correction_gain * delta), 16);
	next_correction = wrap_s16_from_s32(
		(int32_t)control->correction + correction_step);
	correction_magnitude = absolute_wrap_s16(next_correction);
	control->correction = next_correction;
	if (wrap_s16_from_s32((int32_t)correction_magnitude -
		     V34_GAIN_CORRECTION_LIMIT) <= 0)
		return;
	control->correction = 0;
	control->gain = agc_step_gain_q10(control->gain, next_correction > 0);
}

bool v34_gain_control_is_valid(const struct v34_gain_control *control)
{
	return control != NULL && control->initialized &&
	       control->history_index < V34_GAIN_HISTORY_SAMPLES &&
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

enum v34_gain_control_result v34_gain_control_refresh_level(
	struct v34_gain_control *control)
{
	int16_t level;
	uint64_t denominator;
	uint64_t target;
	uint32_t gain;
	uint32_t rounding_boundary;
	/* The level helper estimates RMS from 36 samples weighted by 910/32768
	 * and a quantized square root. Ignoring rounding, the ideal four-sample
	 * steady energy recurrence is
	 * A = A * pole / 2^15 + 4 * RMS^2 / 2^16. Thus the target gain squared
	 * is A * 2 * (2^15 - pole) * 2^20 / (4 * raw_RMS^2); 2^20 converts
	 * squared Q10 gain. Keep this ratio exact until final nearest rounding. */
	const uint64_t energy_scale = UINT64_C(2) *
		(0x8000 - V34_GAIN_ENERGY_HISTORY_Q15) *
		(UINT64_C(1) << (2 * V34_GAIN_FRACTION_BITS));

	if (!v34_gain_control_is_valid(control))
		return V34_GAIN_CONTROL_INVALID_STATE;
	if (agc_measure_level_s16(control->history,
				  V34_GAIN_HISTORY_SAMPLES, &level) != AGC_OK)
		return V34_GAIN_CONTROL_INVALID_STATE;
	if (level <= V34_GAIN_MINIMUM_LEVEL ||
	    control->gain >= AGC_Q10_INCREASE_CEILING)
		return V34_GAIN_CONTROL_OK;
	denominator = (uint64_t)level * level * V34_GAIN_UPDATE_SAMPLES;
	if ((uint64_t)control->gain * control->gain * denominator >=
	    (V34_GAIN_TARGET_ENERGY - V34_GAIN_ENERGY_DEADBAND) * energy_scale)
		return V34_GAIN_CONTROL_OK;
	target = V34_GAIN_TARGET_ENERGY * energy_scale;
	if (target >= denominator * AGC_Q10_INCREASE_CEILING *
			      AGC_Q10_INCREASE_CEILING) {
		gain = AGC_Q10_INCREASE_CEILING;
	} else {
		/* The ceiling bounds this quotient to a 32-bit square. */
		gain = integer_square_root_u32((uint32_t)(target / denominator));
		rounding_boundary = 2u * gain + 1u;
		if (4u * target >=
		    denominator * rounding_boundary * rounding_boundary)
			gain++;
	}
	control->gain = (int16_t)gain;
	return V34_GAIN_CONTROL_OK;
}

static enum v34_gain_control_result process(
	struct v34_gain_control *control,
	struct v34_analytic_sample input,
	struct v34_analytic_sample *output,
	uint32_t rounding, int16_t correction_gain)
{
	struct v34_analytic_sample generated;
	uint32_t next_energy;

	if (!v34_gain_control_is_valid(control))
		return V34_GAIN_CONTROL_INVALID_STATE;
	if (output == NULL || memory_regions_overlap(
			control, sizeof(*control), output, sizeof(*output)))
		return V34_GAIN_CONTROL_INVALID_ARGUMENT;
	control->history[control->history_index] = input.in_phase;
	control->history_index = (uint16_t)(control->history_index + 1u);
	if (control->history_index == V34_GAIN_HISTORY_SAMPLES)
		control->history_index = 0;
	generated = (struct v34_analytic_sample){
		.in_phase = agc_scale_q10(control->gain, input.in_phase, rounding, NULL),
		.quadrature = agc_scale_q10(control->gain, input.quadrature, rounding, NULL),
	};
	next_energy = (uint32_t)((int32_t)generated.in_phase *
				 (int32_t)generated.in_phase) +
		      control->energy_accumulator;
	if (control->history_index % V34_GAIN_UPDATE_SAMPLES != 0) {
		control->energy_accumulator = next_energy;
	} else {
		if (control->mode == V34_GAIN_TRACKING) {
			int16_t level;

			/* Fixed-size owned history and a distinct local output satisfy
			 * the level helper's entire validation contract. */
			(void)agc_measure_level_s16(control->history,
				V34_GAIN_HISTORY_SAMPLES, &level);
			if (level > V34_GAIN_MINIMUM_LEVEL) {
				control->average_energy = sample_smooth_energy_q15(
					control->average_energy, next_energy,
					V34_GAIN_ENERGY_HISTORY_Q15);
				correct_gain(control, correction_gain);
			}
		}
		control->energy_accumulator = 0;
	}
	*output = generated;
	return V34_GAIN_CONTROL_OK;
}

enum v34_gain_control_result v34_gain_control_process(
	struct v34_gain_control *control,
	struct v34_analytic_sample input,
	struct v34_analytic_sample *output)
{
	return process(control, input, output, V34_GAIN_ROUNDING,
		V34_GAIN_CORRECTION_Q15);
}

enum v34_gain_control_result v34_gain_control_process_info(
	struct v34_gain_control *control, int16_t input, int16_t *output)
{
	struct v34_analytic_sample generated;
	enum v34_gain_control_result result;

	if (!v34_gain_control_is_valid(control))
		return V34_GAIN_CONTROL_INVALID_STATE;
	if (output == NULL || memory_regions_overlap(
			control, sizeof(*control), output, sizeof(*output)))
		return V34_GAIN_CONTROL_INVALID_ARGUMENT;
	result = process(control,
		(struct v34_analytic_sample){ .in_phase = input }, &generated,
		0, V34_GAIN_INFO_CORRECTION_Q15);
	if (result != V34_GAIN_CONTROL_OK)
		return result;
	*output = generated.in_phase;
	return V34_GAIN_CONTROL_OK;
}
