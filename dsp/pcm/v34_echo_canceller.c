// SPDX-License-Identifier: BSD-3-Clause

#include "v34_echo_canceller.h"

#include "integer_math.h"
#include "memory_region.h"

#include <stddef.h>
#include <string.h>

enum {
	V34_ECHO_NEAR_ADAPT_SAMPLES = 18000,
	V34_ECHO_FAR_ADAPT_START_SAMPLE = 700,
	V34_ECHO_ALPHA_MEASURE_SAMPLE = V34_ECHO_TAP_COUNT,
	V34_ECHO_ALPHA_SMOOTH_START_SAMPLE = 2000,
	V34_ECHO_ALPHA_SMOOTH_PERIOD = 45,
	V34_ECHO_VALIDATION_START_SAMPLE = 1000,
	V34_ECHO_VALIDATION_MIN_SAMPLES = 256,
	V34_ECHO_NEAR_ENERGY_SCALE = 0x6666,
	V34_ECHO_FAR_ENERGY_SCALE = 0x2b84,
	V34_ECHO_ALPHA_SMOOTH_SCALE = 0x7f5c,
};

struct echo_step {
	int16_t cleaned;
	int16_t residual;
	int16_t near_alpha;
	int16_t far_alpha;
	int16_t near_error;
	int16_t far_error;
	uint32_t sample_count;
	bool adapt_near;
	bool adapt_far;
};

bool v34_echo_canceller_is_valid(
	const struct v34_echo_canceller *canceller)
{
	return canceller != NULL &&
	       canceller->reference_write < V34_ECHO_REFERENCE_SAMPLES &&
	       canceller->reference_fill <= V34_ECHO_REFERENCE_SAMPLES &&
	       (canceller->reference_fill == V34_ECHO_REFERENCE_SAMPLES ||
		canceller->reference_write == canceller->reference_fill) &&
	       (canceller->training_started || canceller->sample_count == 0) &&
	       (canceller->far_delay_samples == 0 ||
		(canceller->far_delay_samples >= V34_ECHO_TAP_COUNT &&
		 canceller->far_delay_samples <=
			 V34_ECHO_MAX_FAR_DELAY_SAMPLES));
}

bool v34_echo_canceller_is_adapting(
	const struct v34_echo_canceller *canceller)
{
	return v34_echo_canceller_is_valid(canceller) &&
	       canceller->adapting;
}

enum v34_echo_canceller_result v34_echo_canceller_init(
	struct v34_echo_canceller *canceller,
	uint16_t round_trip_delay_samples)
{
	struct v34_echo_canceller initialized;

	if (canceller == NULL)
		return V34_ECHO_CANCELLER_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	if (round_trip_delay_samples >= V34_ECHO_TAP_COUNT &&
	    round_trip_delay_samples <= V34_ECHO_MAX_FAR_DELAY_SAMPLES)
		initialized.far_delay_samples = round_trip_delay_samples;
	initialized.adapting = true;
	if (!v34_echo_canceller_is_valid(&initialized))
		return V34_ECHO_CANCELLER_INVALID_STATE;
	*canceller = initialized;
	return V34_ECHO_CANCELLER_OK;
}

static bool cancellation_is_ready(
	const struct v34_echo_canceller *canceller)
{
	uint64_t input = canceller->validation_input_energy;

	return canceller->validation_samples >=
		       V34_ECHO_VALIDATION_MIN_SAMPLES &&
	       canceller->validation_residual_energy < input - input / 20u;
}

enum v34_echo_canceller_result v34_echo_canceller_freeze(
	struct v34_echo_canceller *canceller)
{
	if (!v34_echo_canceller_is_valid(canceller))
		return V34_ECHO_CANCELLER_INVALID_STATE;
	if (canceller->adapting && !canceller->filtering)
		canceller->filtering = cancellation_is_ready(canceller);
	canceller->adapting = false;
	return V34_ECHO_CANCELLER_OK;
}

static int16_t reference_at_age(
	const struct v34_echo_canceller *canceller, size_t age)
{
	size_t index;

	if (age == 0 || age > canceller->reference_fill)
		return 0;
	index = canceller->reference_write + V34_ECHO_REFERENCE_SAMPLES - age;
	if (index >= V34_ECHO_REFERENCE_SAMPLES)
		index -= V34_ECHO_REFERENCE_SAMPLES;
	return canceller->references[index];
}

static int32_t filter_prediction(
	const struct v34_echo_canceller *canceller,
	const struct v34_echo_filter *filter, size_t delay)
{
	uint32_t accumulated = 0;

	for (size_t tap = 0; tap < V34_ECHO_TAP_COUNT; ++tap) {
		int16_t reference = reference_at_age(
			canceller, delay + tap + 1u);

		accumulated += multiply_low_s32(
			filter->coefficients[tap], reference);
	}
	return wrap_s32_from_u32(accumulated);
}

static uint32_t reference_energy(
	const struct v34_echo_canceller *canceller, size_t delay)
{
	uint32_t accumulated = 0;

	for (size_t tap = 0; tap < V34_ECHO_TAP_COUNT; ++tap) {
		int16_t reference = reference_at_age(
			canceller, delay + tap + 1u);

		accumulated += multiply_low_s32(reference, reference) >> 5;
	}
	return accumulated;
}

static int16_t alpha_from_energy(uint32_t energy, int32_t scale)
{
	uint8_t shift = 0;
	int32_t denominator;
	int32_t numerator;
	int16_t quotient;
	int32_t bounded;
	int32_t result;

	if (energy == 0)
		return 0;
	while ((energy & UINT32_C(0x40000000)) == 0) {
		++shift;
		energy <<= 1;
	}
	denominator = arithmetic_shift_right_u32(energy + 0x8000u, 16);
	numerator = wrap_s32_from_u32(
		UINT32_C(1) << ((shift + 0x15u) & 0x1fu));
	quotient = wrap_s16_from_s32(numerator / denominator);
	bounded = quotient < 0 ? 0x2000 : quotient;
	result = arithmetic_shift_right_u32(
		multiply_low_s32(bounded, scale) + 0x2000u, 15);
	return wrap_s16_from_s32(wrap_s32_from_u32(0u - (uint32_t)result));
}

static int16_t update_alpha(
	int16_t alpha, uint32_t energy, bool smooth, int32_t energy_scale)
{
	if (energy != 0)
		alpha = alpha_from_energy(energy, energy_scale);
	if (smooth)
		alpha = wrap_s16_from_s32(arithmetic_shift_right_u32(
			multiply_low_s32(alpha, V34_ECHO_ALPHA_SMOOTH_SCALE) +
				0x4000u,
			15));
	return alpha;
}

static int16_t adaptation_error(int16_t alpha, int16_t cleaned)
{
	uint32_t scaled = multiply_low_s32(alpha, cleaned);

	return wrap_s16_from_s32(arithmetic_shift_right_u32(
		scaled * 2u + 0x2000u, 14));
}

static void calculate_step(
	const struct v34_echo_canceller *canceller,
	int16_t received_sample, struct echo_step *step)
{
	uint32_t near_prediction = (uint32_t)filter_prediction(
		canceller, &canceller->near_filter, 0);
	uint32_t far_prediction = 0;
	uint32_t feedback;
	bool smooth_near;
	bool smooth_far;
	uint32_t near_energy = 0;
	uint32_t far_energy = 0;

	if (canceller->far_delay_samples != 0)
		far_prediction = (uint32_t)filter_prediction(
			canceller, &canceller->far_filter,
			canceller->far_delay_samples);
	feedback = near_prediction * 4u + far_prediction * 4u + 0x8000u;
	step->residual = wrap_s16_from_s32(
		(int32_t)received_sample +
		arithmetic_shift_right_u32(feedback, 16));
	step->cleaned = canceller->filtering ?
			step->residual : received_sample;
	step->near_alpha = canceller->near_filter.alpha;
	step->far_alpha = canceller->far_filter.alpha;
	step->near_error = 0;
	step->far_error = 0;
	step->sample_count = canceller->sample_count;
	step->adapt_near = false;
	step->adapt_far = false;
	if (!canceller->adapting || !canceller->training_started)
		return;
	step->sample_count++;
	smooth_near = step->sample_count >
			      V34_ECHO_ALPHA_SMOOTH_START_SAMPLE &&
		      step->sample_count % V34_ECHO_ALPHA_SMOOTH_PERIOD == 0;
	smooth_far = step->sample_count >
			     V34_ECHO_FAR_ADAPT_START_SAMPLE +
				     V34_ECHO_ALPHA_SMOOTH_START_SAMPLE &&
		     smooth_near;
	if (step->sample_count == V34_ECHO_ALPHA_MEASURE_SAMPLE)
		near_energy = reference_energy(canceller, 0);
	if (canceller->far_delay_samples != 0 &&
	    step->sample_count == V34_ECHO_FAR_ADAPT_START_SAMPLE +
				  V34_ECHO_ALPHA_MEASURE_SAMPLE)
		far_energy = reference_energy(
			canceller, canceller->far_delay_samples);
	step->near_alpha = update_alpha(
		step->near_alpha, near_energy, smooth_near,
		V34_ECHO_NEAR_ENERGY_SCALE);
	if (canceller->far_delay_samples != 0)
		step->far_alpha = update_alpha(
			step->far_alpha, far_energy, smooth_far,
			V34_ECHO_FAR_ENERGY_SCALE);
	step->near_error = adaptation_error(
		step->near_alpha, step->residual);
	step->far_error = adaptation_error(
		step->far_alpha, step->residual);
	step->adapt_near = step->sample_count < V34_ECHO_NEAR_ADAPT_SAMPLES;
	step->adapt_far = canceller->far_delay_samples != 0 &&
			  step->sample_count >
				  V34_ECHO_FAR_ADAPT_START_SAMPLE;
}

enum v34_echo_canceller_result v34_echo_canceller_predict(
	const struct v34_echo_canceller *canceller,
	int16_t received_sample, int16_t *cleaned_sample)
{
	struct echo_step step;

	if (!v34_echo_canceller_is_valid(canceller))
		return V34_ECHO_CANCELLER_INVALID_STATE;
	if (cleaned_sample == NULL || memory_regions_overlap(
		    canceller, sizeof(*canceller),
		    cleaned_sample, sizeof(*cleaned_sample)))
		return V34_ECHO_CANCELLER_INVALID_ARGUMENT;
	calculate_step(canceller, received_sample, &step);
	*cleaned_sample = step.cleaned;
	return V34_ECHO_CANCELLER_OK;
}

static void adapt_filter(
	struct v34_echo_canceller *canceller,
	struct v34_echo_filter *filter, size_t delay, int16_t error)
{
	for (size_t tap = 0; tap < V34_ECHO_TAP_COUNT; ++tap) {
		int16_t reference = reference_at_age(
			canceller, delay + tap + 1u);
		uint32_t word =
			((uint32_t)(uint16_t)filter->coefficients[tap] << 16) |
			(uint16_t)filter->residues[tap];

		word += multiply_low_s32(reference, error);
		filter->coefficients[tap] = wrap_s16_from_s32(
			arithmetic_shift_right_u32(word, 16));
		filter->residues[tap] = (int16_t)(uint16_t)word;
	}
}

static void push_reference(
	struct v34_echo_canceller *canceller, int16_t transmit_sample)
{
	int16_t reference = wrap_s16_from_s32(arithmetic_shift_right_u32(
		(uint32_t)(int32_t)transmit_sample, 1));

	canceller->references[canceller->reference_write] = reference;
	canceller->reference_write++;
	if (canceller->reference_write == V34_ECHO_REFERENCE_SAMPLES)
		canceller->reference_write = 0;
	if (canceller->reference_fill < V34_ECHO_REFERENCE_SAMPLES)
		canceller->reference_fill++;
	if (reference != 0)
		canceller->training_started = true;
}

static uint64_t add_energy(uint64_t accumulated, int16_t sample)
{
	uint64_t energy = (uint64_t)((int32_t)sample * sample);

	return UINT64_MAX - accumulated < energy ?
		       UINT64_MAX : accumulated + energy;
}

enum v34_echo_canceller_result v34_echo_canceller_advance(
	struct v34_echo_canceller *canceller,
	int16_t received_sample, int16_t cleaned_sample,
	int16_t transmit_sample)
{
	struct echo_step step;

	if (!v34_echo_canceller_is_valid(canceller))
		return V34_ECHO_CANCELLER_INVALID_STATE;
	calculate_step(canceller, received_sample, &step);
	if (step.cleaned != cleaned_sample)
		return V34_ECHO_CANCELLER_SEQUENCE_MISMATCH;
	if (canceller->adapting && canceller->training_started &&
	    step.sample_count >= V34_ECHO_VALIDATION_START_SAMPLE) {
		canceller->validation_input_energy = add_energy(
			canceller->validation_input_energy, received_sample);
		canceller->validation_residual_energy = add_energy(
			canceller->validation_residual_energy, step.residual);
		if (canceller->validation_samples != UINT32_MAX)
			canceller->validation_samples++;
	}
	if (step.adapt_near)
		adapt_filter(
			canceller, &canceller->near_filter, 0,
			step.near_error);
	if (step.adapt_far)
		adapt_filter(
			canceller, &canceller->far_filter,
			canceller->far_delay_samples, step.far_error);
	canceller->near_filter.alpha = step.near_alpha;
	canceller->far_filter.alpha = step.far_alpha;
	canceller->sample_count = step.sample_count;
	push_reference(canceller, transmit_sample);
	return V34_ECHO_CANCELLER_OK;
}
