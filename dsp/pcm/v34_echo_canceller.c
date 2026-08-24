// SPDX-License-Identifier: BSD-3-Clause

#include "v34_echo_canceller.h"

#include "memory_region.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

static const double V34_ECHO_ADAPTATION = 0.35;
static const double V34_ECHO_ENERGY_FLOOR = 1048576.0;
static const double V34_ECHO_MIN_COEFFICIENT_SUM = 0.01;
static const uint32_t V34_ECHO_TRAIN_PEAK = 640;
static const uint32_t V34_ECHO_READY_MIN_PEAK = 64;

enum v34_echo_sample_action {
	V34_ECHO_SAMPLE_NONE,
	V34_ECHO_SAMPLE_ADAPT,
	V34_ECHO_SAMPLE_VALIDATE,
};

static bool mode_is_valid(enum v34_echo_mode mode)
{
	return mode >= V34_ECHO_DISABLED && mode <= V34_ECHO_ACTIVE;
}

static bool delay_is_valid(uint16_t delay)
{
	return delay >= V34_ECHO_MIN_DELAY_SAMPLES &&
	       delay <= V34_ECHO_MAX_DELAY_SAMPLES;
}

static uint8_t pre_taps_for_delay(uint16_t delay)
{
	return delay > V34_ECHO_PRE_TAPS ? V34_ECHO_PRE_TAPS :
						  (uint8_t)(delay - 1u);
}

static double coefficient_sum(
	const struct v34_echo_canceller *canceller)
{
	double sum = 0.0;

	for (size_t tap = 0; tap < V34_ECHO_TAPS; ++tap)
		sum += fabs(canceller->coefficients[tap]);
	return sum;
}

static bool model_is_ready(
	const struct v34_echo_canceller *canceller)
{
	double sum = coefficient_sum(canceller);

	return canceller->train_samples == V34_ECHO_TRAIN_SAMPLES &&
	       canceller->validation_samples == V34_ECHO_VALIDATE_SAMPLES &&
	       canceller->training_peak >= V34_ECHO_READY_MIN_PEAK &&
	       isfinite(sum) && sum >= V34_ECHO_MIN_COEFFICIENT_SUM &&
	       canceller->validation_input_energy > 0.0 &&
	       canceller->validation_error_energy * 2.0 <=
		       canceller->validation_input_energy;
}

static bool reference_position_is_valid(
	const struct v34_echo_canceller *canceller)
{
	uint64_t expected_fill =
		canceller->epoch < V34_ECHO_REFERENCE_SAMPLES ?
			canceller->epoch : V34_ECHO_REFERENCE_SAMPLES;

	return canceller->reference_write ==
		       canceller->epoch % V34_ECHO_REFERENCE_SAMPLES &&
	       canceller->reference_fill == expected_fill;
}

static bool state_header_is_valid(
	const struct v34_echo_canceller *canceller)
{
	if (canceller == NULL || !mode_is_valid(canceller->mode) ||
	    !reference_position_is_valid(canceller) ||
	    canceller->epoch == UINT64_MAX ||
	    canceller->quiet_samples > V34_ECHO_QUIET_SAMPLES ||
	    canceller->train_samples > V34_ECHO_TRAIN_SAMPLES ||
	    canceller->validation_samples > V34_ECHO_VALIDATE_SAMPLES ||
	    !isfinite(canceller->validation_input_energy) ||
	    !isfinite(canceller->validation_error_energy) ||
	    canceller->validation_input_energy < 0.0 ||
	    canceller->validation_error_energy < 0.0)
		return false;
	if (canceller->mode == V34_ECHO_DISABLED) {
		if (canceller->round_trip_delay_samples != 0 ||
		    canceller->pre_taps != 0 ||
		    canceller->quiet_samples != 0 ||
		    canceller->train_samples != 0 ||
		    canceller->validation_samples != 0 ||
		    canceller->training_peak != 0 ||
		    canceller->validation_input_energy != 0.0 ||
		    canceller->validation_error_energy != 0.0)
			return false;
		return true;
	}
	if (!delay_is_valid(canceller->round_trip_delay_samples) ||
	    canceller->pre_taps != pre_taps_for_delay(
		    canceller->round_trip_delay_samples) ||
	    canceller->reference_fill <
		    (uint32_t)canceller->round_trip_delay_samples -
			    canceller->pre_taps + V34_ECHO_TAPS)
		return false;
	if (canceller->train_samples < V34_ECHO_TRAIN_SAMPLES &&
	    (canceller->validation_samples != 0 ||
	     canceller->validation_input_energy != 0.0 ||
	     canceller->validation_error_energy != 0.0))
		return false;
	return true;
}

bool v34_echo_canceller_is_valid(
	const struct v34_echo_canceller *canceller)
{
	if (!state_header_is_valid(canceller))
		return false;
	for (size_t tap = 0; tap < V34_ECHO_TAPS; ++tap) {
		if (!isfinite(canceller->coefficients[tap]) ||
		    (canceller->mode == V34_ECHO_DISABLED &&
		     canceller->coefficients[tap] != 0.0))
			return false;
	}
	return canceller->mode != V34_ECHO_ACTIVE ||
	       model_is_ready(canceller);
}

enum v34_echo_result v34_echo_canceller_init(
	struct v34_echo_canceller *canceller)
{
	struct v34_echo_canceller initialized = { 0 };

	if (canceller == NULL)
		return V34_ECHO_INVALID_ARGUMENT;
	if (!v34_echo_canceller_is_valid(&initialized))
		return V34_ECHO_INVALID_STATE;
	*canceller = initialized;
	return V34_ECHO_OK;
}

static void clear_model(struct v34_echo_canceller *canceller)
{
	memset(canceller->coefficients, 0, sizeof(canceller->coefficients));
	canceller->validation_input_energy = 0.0;
	canceller->validation_error_energy = 0.0;
	canceller->train_samples = 0;
	canceller->validation_samples = 0;
	canceller->training_peak = 0;
	canceller->round_trip_delay_samples = 0;
	canceller->pre_taps = 0;
	canceller->quiet_samples = 0;
	canceller->mode = V34_ECHO_DISABLED;
}

enum v34_echo_result v34_echo_canceller_start_training(
	struct v34_echo_canceller *canceller,
	uint16_t round_trip_delay_samples)
{
	uint32_t required;

	if (!v34_echo_canceller_is_valid(canceller))
		return V34_ECHO_INVALID_STATE;
	if (!delay_is_valid(round_trip_delay_samples))
		return V34_ECHO_INVALID_ARGUMENT;
	required = (uint32_t)round_trip_delay_samples -
		   pre_taps_for_delay(round_trip_delay_samples) +
		   V34_ECHO_TAPS;
	if (canceller->reference_fill < required)
		return V34_ECHO_INVALID_STATE;
	clear_model(canceller);
	canceller->round_trip_delay_samples = round_trip_delay_samples;
	canceller->pre_taps = pre_taps_for_delay(round_trip_delay_samples);
	canceller->mode = V34_ECHO_TRAINING;
	return v34_echo_canceller_is_valid(canceller) ? V34_ECHO_OK :
						      V34_ECHO_INVALID_STATE;
}

enum v34_echo_result v34_echo_canceller_finish_training(
	struct v34_echo_canceller *canceller)
{
	if (!v34_echo_canceller_is_valid(canceller))
		return V34_ECHO_INVALID_STATE;
	if (canceller->mode != V34_ECHO_TRAINING)
		return V34_ECHO_INVALID_STATE;
	if (model_is_ready(canceller))
		canceller->mode = V34_ECHO_ACTIVE;
	else
		clear_model(canceller);
	return state_header_is_valid(canceller) ? V34_ECHO_OK :
						      V34_ECHO_INVALID_STATE;
}

static int16_t reference_at(
	const struct v34_echo_canceller *canceller, size_t tap)
{
	uint32_t back = (uint32_t)canceller->round_trip_delay_samples -
			canceller->pre_taps + (uint32_t)tap;
	uint32_t index = canceller->reference_write +
			 V34_ECHO_REFERENCE_SAMPLES - back;

	if (index >= V34_ECHO_REFERENCE_SAMPLES)
		index -= V34_ECHO_REFERENCE_SAMPLES;
	return canceller->reference[index];
}

static bool prediction_and_energy(
	const struct v34_echo_canceller *canceller,
	double *prediction, double *energy)
{
	double predicted = 0.0;
	double reference_energy = 0.0;

	for (size_t tap = 0; tap < V34_ECHO_TAPS; ++tap) {
		double reference = reference_at(canceller, tap);
		double coefficient = canceller->coefficients[tap];

		if (!isfinite(coefficient))
			return false;
		predicted += coefficient * reference;
		reference_energy += reference * reference;
	}
	if (!isfinite(predicted) || !isfinite(reference_energy))
		return false;
	*prediction = predicted;
	*energy = reference_energy;
	return true;
}

static uint32_t magnitude_i16(int16_t sample)
{
	return sample < 0 ? (uint32_t)(-(int32_t)sample) :
			    (uint32_t)sample;
}

static int16_t quantize_error(double error)
{
	if (error <= INT16_MIN)
		return INT16_MIN;
	if (error >= INT16_MAX)
		return INT16_MAX;
	return (int16_t)lround(error);
}

static bool update_is_finite(
	const struct v34_echo_canceller *canceller, double scale)
{
	for (size_t tap = 0; tap < V34_ECHO_TAPS; ++tap) {
		double updated = canceller->coefficients[tap] +
				 scale * reference_at(canceller, tap);

		if (!isfinite(updated))
			return false;
	}
	return true;
}

enum v34_echo_result v34_echo_canceller_prepare(
	const struct v34_echo_canceller *canceller, int16_t received_sample,
	int16_t *cleaned_sample,
	struct v34_echo_prepared_sample *prepared)
{
	struct v34_echo_prepared_sample next = { 0 };
	double prediction;
	double energy;
	double error;
	uint32_t magnitude;

	if (!state_header_is_valid(canceller))
		return V34_ECHO_INVALID_STATE;
	if (cleaned_sample == NULL || prepared == NULL ||
	    memory_regions_overlap(
		    canceller, sizeof(*canceller),
		    cleaned_sample, sizeof(*cleaned_sample)) ||
	    memory_regions_overlap(
		    canceller, sizeof(*canceller), prepared, sizeof(*prepared)) ||
	    memory_regions_overlap(
		    cleaned_sample, sizeof(*cleaned_sample),
		    prepared, sizeof(*prepared)))
		return V34_ECHO_INVALID_ARGUMENT;
	next.epoch = canceller->epoch;
	if (canceller->mode == V34_ECHO_DISABLED) {
		*cleaned_sample = received_sample;
		*prepared = next;
		return V34_ECHO_OK;
	}
	if (!prediction_and_energy(canceller, &prediction, &energy))
		return V34_ECHO_INVALID_STATE;
	error = (double)received_sample - prediction;
	if (!isfinite(error))
		return V34_ECHO_INVALID_STATE;
	if (canceller->mode == V34_ECHO_ACTIVE) {
		*cleaned_sample = quantize_error(error);
		*prepared = next;
		return V34_ECHO_OK;
	}
	*cleaned_sample = received_sample;
	magnitude = magnitude_i16(received_sample);
	next.input_magnitude = magnitude;
	next.quiet_samples = magnitude > V34_ECHO_TRAIN_PEAK ? 0 :
		canceller->quiet_samples < V34_ECHO_QUIET_SAMPLES ?
			canceller->quiet_samples + 1u :
			V34_ECHO_QUIET_SAMPLES;
	if (next.quiet_samples < V34_ECHO_QUIET_SAMPLES) {
		*prepared = next;
		return V34_ECHO_OK;
	}
	if (canceller->train_samples < V34_ECHO_TRAIN_SAMPLES) {
		next.scale = V34_ECHO_ADAPTATION * error /
			     (energy + V34_ECHO_ENERGY_FLOOR);
		if (!isfinite(next.scale) ||
		    !update_is_finite(canceller, next.scale))
			return V34_ECHO_INVALID_STATE;
		next.action = V34_ECHO_SAMPLE_ADAPT;
	} else if (canceller->validation_samples <
		   V34_ECHO_VALIDATE_SAMPLES) {
		next.input_energy =
			(double)received_sample * received_sample;
		next.error_energy = error * error;
		if (!isfinite(next.input_energy) ||
		    !isfinite(next.error_energy))
			return V34_ECHO_INVALID_STATE;
		next.action = V34_ECHO_SAMPLE_VALIDATE;
	}
	*prepared = next;
	return V34_ECHO_OK;
}

static bool prepared_is_valid(
	const struct v34_echo_canceller *canceller,
	const struct v34_echo_prepared_sample *prepared)
{
	if (prepared == NULL || prepared->epoch != canceller->epoch ||
	    prepared->quiet_samples > V34_ECHO_QUIET_SAMPLES ||
	    prepared->input_magnitude > (uint32_t)INT16_MAX + 1u ||
	    !isfinite(prepared->scale) ||
	    !isfinite(prepared->input_energy) ||
	    !isfinite(prepared->error_energy) ||
	    prepared->input_energy < 0.0 || prepared->error_energy < 0.0)
		return false;
	switch (prepared->action) {
	case V34_ECHO_SAMPLE_NONE:
		return prepared->scale == 0.0 &&
		       prepared->input_energy == 0.0 &&
		       prepared->error_energy == 0.0;
	case V34_ECHO_SAMPLE_ADAPT:
		return canceller->mode == V34_ECHO_TRAINING &&
		       prepared->quiet_samples == V34_ECHO_QUIET_SAMPLES &&
		       canceller->train_samples < V34_ECHO_TRAIN_SAMPLES &&
		       prepared->input_energy == 0.0 &&
		       prepared->error_energy == 0.0 &&
		       update_is_finite(canceller, prepared->scale);
	case V34_ECHO_SAMPLE_VALIDATE:
		return canceller->mode == V34_ECHO_TRAINING &&
		       prepared->quiet_samples == V34_ECHO_QUIET_SAMPLES &&
		       canceller->train_samples == V34_ECHO_TRAIN_SAMPLES &&
		       canceller->validation_samples <
			       V34_ECHO_VALIDATE_SAMPLES &&
		       isfinite(canceller->validation_input_energy +
				prepared->input_energy) &&
		       isfinite(canceller->validation_error_energy +
				prepared->error_energy);
	}
	return false;
}

static void push_reference(
	struct v34_echo_canceller *canceller, int16_t sample)
{
	canceller->reference[canceller->reference_write] = sample;
	if (++canceller->reference_write == V34_ECHO_REFERENCE_SAMPLES)
		canceller->reference_write = 0;
	if (canceller->reference_fill < V34_ECHO_REFERENCE_SAMPLES)
		canceller->reference_fill++;
	canceller->epoch++;
}

enum v34_echo_result v34_echo_canceller_commit(
	struct v34_echo_canceller *canceller, int16_t transmit_sample,
	const struct v34_echo_prepared_sample *prepared)
{
	if (!state_header_is_valid(canceller))
		return V34_ECHO_INVALID_STATE;
	if (prepared == NULL ||
	    memory_regions_overlap(
		    canceller, sizeof(*canceller), prepared, sizeof(*prepared)))
		return V34_ECHO_INVALID_ARGUMENT;
	if (!prepared_is_valid(canceller, prepared))
		return V34_ECHO_INVALID_STATE;
	if (canceller->mode == V34_ECHO_TRAINING)
		canceller->quiet_samples = prepared->quiet_samples;
	if (prepared->action == V34_ECHO_SAMPLE_ADAPT) {
		for (size_t tap = 0; tap < V34_ECHO_TAPS; ++tap)
			canceller->coefficients[tap] +=
				prepared->scale * reference_at(canceller, tap);
		canceller->train_samples++;
		if (canceller->training_peak < prepared->input_magnitude)
			canceller->training_peak = prepared->input_magnitude;
	} else if (prepared->action == V34_ECHO_SAMPLE_VALIDATE) {
		canceller->validation_input_energy += prepared->input_energy;
		canceller->validation_error_energy += prepared->error_energy;
		canceller->validation_samples++;
		if (canceller->training_peak < prepared->input_magnitude)
			canceller->training_peak = prepared->input_magnitude;
	}
	push_reference(canceller, transmit_sample);
	return state_header_is_valid(canceller) ? V34_ECHO_OK :
						      V34_ECHO_INVALID_STATE;
}
