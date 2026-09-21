// SPDX-License-Identifier: BSD-3-Clause

#include "v34_echo_canceller.h"

#include "integer_math.h"
#include "memory_region.h"

#include <stddef.h>
#include <string.h>

enum {
	V34_ECHO_NEAR_ADAPT_SAMPLES = 18000,
	V34_ECHO_ALPHA_MEASURE_SAMPLE = V34_ECHO_TAP_COUNT,
	V34_ECHO_ALPHA_SMOOTH_START_SAMPLE = 2000,
	V34_ECHO_ALPHA_SMOOTH_PERIOD = 45,
	V34_ECHO_VALIDATION_START_SAMPLE = 1000,
	V34_ECHO_VALIDATION_MIN_SAMPLES = 256,
	V34_ECHO_ENERGY_SCALE = 0x6666,
	V34_ECHO_ALPHA_SMOOTH_SCALE = 0x7f5c,
};

struct echo_step {
	int16_t cleaned;
	int16_t residual;
	int16_t alpha;
	int16_t error;
	uint32_t sample_count;
	bool adapt;
};

bool v34_echo_canceller_is_valid(
	const struct v34_echo_canceller *canceller)
{
	return canceller != NULL &&
	       canceller->reference_write < V34_ECHO_TAP_COUNT &&
	       canceller->reference_fill <= V34_ECHO_TAP_COUNT &&
	       (canceller->reference_fill == V34_ECHO_TAP_COUNT ||
		canceller->reference_write == canceller->reference_fill) &&
	       (canceller->training_started || canceller->sample_count == 0);
}

bool v34_echo_canceller_is_adapting(
	const struct v34_echo_canceller *canceller)
{
	return v34_echo_canceller_is_valid(canceller) &&
	       canceller->adapting;
}

enum v34_echo_canceller_result v34_echo_canceller_init(
	struct v34_echo_canceller *canceller)
{
	if (canceller == NULL)
		return V34_ECHO_CANCELLER_INVALID_ARGUMENT;
	memset(canceller, 0, sizeof(*canceller));
	canceller->adapting = true;
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
	index = canceller->reference_write + V34_ECHO_TAP_COUNT - age;
	if (index >= V34_ECHO_TAP_COUNT)
		index -= V34_ECHO_TAP_COUNT;
	return canceller->references[index];
}

static int32_t filter_prediction(
	const struct v34_echo_canceller *canceller)
{
	uint32_t accumulated = 0;

	for (size_t tap = 0; tap < V34_ECHO_TAP_COUNT; ++tap) {
		int16_t reference = reference_at_age(
			canceller, tap + 1u);

		accumulated += multiply_low_s32(
			canceller->coefficients[tap], reference);
	}
	return wrap_s32_from_u32(accumulated);
}

static uint32_t reference_energy(
	const struct v34_echo_canceller *canceller)
{
	uint32_t accumulated = 0;

	for (size_t tap = 0; tap < V34_ECHO_TAP_COUNT; ++tap) {
		int16_t reference = reference_at_age(
			canceller, tap + 1u);

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
		alpha = round_shift_s32_word_to_s16(
			multiply_low_s32(alpha, V34_ECHO_ALPHA_SMOOTH_SCALE),
			15u);
	return alpha;
}

static void calculate_step(
	const struct v34_echo_canceller *canceller,
	int16_t received_sample, struct echo_step *step)
{
	uint32_t prediction = (uint32_t)filter_prediction(canceller);
	uint32_t feedback;
	bool smooth;
	uint32_t energy = 0;

	feedback = prediction * 4u + 0x8000u;
	step->residual = wrap_s16_from_s32(
		(int32_t)received_sample +
		arithmetic_shift_right_u32(feedback, 16));
	step->cleaned = canceller->filtering ?
			step->residual : received_sample;
	step->alpha = canceller->alpha;
	step->error = 0;
	step->sample_count = canceller->sample_count;
	step->adapt = false;
	if (!canceller->adapting || !canceller->training_started)
		return;
	step->sample_count++;
	smooth = step->sample_count > V34_ECHO_ALPHA_SMOOTH_START_SAMPLE &&
		 step->sample_count % V34_ECHO_ALPHA_SMOOTH_PERIOD == 0;
	if (step->sample_count == V34_ECHO_ALPHA_MEASURE_SAMPLE)
		energy = reference_energy(canceller);
	step->alpha = update_alpha(
		step->alpha, energy, smooth, V34_ECHO_ENERGY_SCALE);
	step->error = round_shift_s32_word_to_s16(
		multiply_low_s32(step->alpha, step->residual) * 2u, 14u);
	step->adapt = step->sample_count < V34_ECHO_NEAR_ADAPT_SAMPLES;
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
	struct v34_echo_canceller *canceller, int16_t error)
{
	for (size_t tap = 0; tap < V34_ECHO_TAP_COUNT; ++tap) {
		int16_t reference = reference_at_age(
			canceller, tap + 1u);
		uint32_t word =
			((uint32_t)(uint16_t)canceller->coefficients[tap] << 16) |
			(uint16_t)canceller->residues[tap];

		word += multiply_low_s32(reference, error);
		canceller->coefficients[tap] = wrap_s16_from_s32(
			arithmetic_shift_right_u32(word, 16));
		canceller->residues[tap] = (int16_t)(uint16_t)word;
	}
}

static void push_reference(
	struct v34_echo_canceller *canceller, int16_t transmit_sample)
{
	int16_t reference = wrap_s16_from_s32(arithmetic_shift_right_u32(
		(uint32_t)(int32_t)transmit_sample, 1));

	canceller->references[canceller->reference_write] = reference;
	canceller->reference_write++;
	if (canceller->reference_write == V34_ECHO_TAP_COUNT)
		canceller->reference_write = 0;
	if (canceller->reference_fill < V34_ECHO_TAP_COUNT)
		canceller->reference_fill++;
	if (reference != 0)
		canceller->training_started = true;
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
		canceller->validation_input_energy = add_u64_saturating(
			canceller->validation_input_energy,
			square_s16(received_sample));
		canceller->validation_residual_energy = add_u64_saturating(
			canceller->validation_residual_energy,
			square_s16(step.residual));
		if (canceller->validation_samples != UINT32_MAX)
			canceller->validation_samples++;
	}
	if (step.adapt)
		adapt_filter(canceller, step.error);
	canceller->alpha = step.alpha;
	canceller->sample_count = step.sample_count;
	push_reference(canceller, transmit_sample);
	return V34_ECHO_CANCELLER_OK;
}
