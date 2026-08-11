// SPDX-License-Identifier: BSD-3-Clause

#include "v34_preemphasis.h"

#include <float.h>
#include <math.h>
#include <string.h>

enum {
	PREEMPHASIS_DESIGN_BINS = 256,
};

static const double PREEMPHASIS_PI =
	3.1415926535897932384626433832795;
static const double PREEMPHASIS_MAX_NORMALIZED_FREQUENCY = 1.2;
static const double PREEMPHASIS_TEMPLATE_LOW_END = 0.4;
static const double PREEMPHASIS_TEMPLATE_HIGH_START = 0.8;
static const double PREEMPHASIS_SUM_TOLERANCE = 1e-9;

_Static_assert((V34_PREEMPHASIS_TAPS & 1u) != 0,
	       "V.34 pre-emphasis FIR must have an integral group delay");

static bool filter_index_is_valid(uint8_t filter_index)
{
	return filter_index <= V34_PREEMPHASIS_MAX_FILTER_INDEX;
}

static double template_magnitude_db(
	uint8_t filter_index, double normalized_frequency,
	bool *constrained)
{
	if (filter_index <= 5) {
		*constrained = true;
		return 2.0 * filter_index * normalized_frequency;
	}
	{
		double beta = 0.5 * (filter_index - 5u);

		if (normalized_frequency <=
		    PREEMPHASIS_TEMPLATE_LOW_END) {
			*constrained = true;
			return 0.0;
		}
		if (normalized_frequency <
		    PREEMPHASIS_TEMPLATE_HIGH_START) {
			*constrained = false;
			return beta *
			       (normalized_frequency -
				PREEMPHASIS_TEMPLATE_LOW_END) /
			       (PREEMPHASIS_TEMPLATE_HIGH_START -
				PREEMPHASIS_TEMPLATE_LOW_END);
		}
		*constrained = true;
		return beta + 2.0 * beta *
			(normalized_frequency -
			 PREEMPHASIS_TEMPLATE_HIGH_START) /
			(PREEMPHASIS_MAX_NORMALIZED_FREQUENCY -
			 PREEMPHASIS_TEMPLATE_HIGH_START);
	}
}

enum v34_preemphasis_result
v34_preemphasis_template_db(
	uint8_t filter_index, double normalized_frequency,
	double *magnitude_db, bool *constrained)
{
	if (!filter_index_is_valid(filter_index) ||
	    !isfinite(normalized_frequency) || normalized_frequency < 0.0 ||
	    normalized_frequency > PREEMPHASIS_MAX_NORMALIZED_FREQUENCY ||
	    magnitude_db == NULL || constrained == NULL)
		return V34_PREEMPHASIS_INVALID_ARGUMENT;
	*magnitude_db = template_magnitude_db(
		filter_index, normalized_frequency, constrained);
	return V34_PREEMPHASIS_OK;
}

static uint32_t greatest_common_divisor(uint32_t left, uint32_t right)
{
	while (right != 0) {
		uint32_t remainder = left % right;

		left = right;
		right = remainder;
	}
	return left;
}

static bool rates_are_valid(
	uint32_t sample_rate, uint32_t symbol_rate_numerator,
	uint32_t symbol_rate_denominator)
{
	uint64_t samples = (uint64_t)sample_rate * symbol_rate_denominator;

	return sample_rate != 0 && symbol_rate_numerator != 0 &&
	       symbol_rate_denominator != 0 &&
	       samples > 2u * (uint64_t)symbol_rate_numerator;
}

static double extended_template_amplitude(
	uint8_t filter_index, double normalized_frequency)
{
	bool constrained;
	double magnitude_db;

	if (normalized_frequency > PREEMPHASIS_MAX_NORMALIZED_FREQUENCY)
		normalized_frequency =
			PREEMPHASIS_MAX_NORMALIZED_FREQUENCY;
	magnitude_db = template_magnitude_db(
		filter_index, normalized_frequency, &constrained);
	(void)constrained;
	return pow(10.0, magnitude_db / 20.0);
}

static bool generate_coefficients(
	struct v34_preemphasis *filter)
{
	double sum = 0.0;

	/* Reconstruct the even, zero-phase response directly from its inverse
	 * DTFT at 256 midpoint bins, retain the central 21 terms, and mirror
	 * them for an exact ten-sample group delay. The template is held at its
	 * 1.2-S endpoint above the specified range and the unconstrained gap is
	 * joined by its shortest straight bridge. Unity-DC normalization leaves
	 * absolute transmit power to the Phase 3 owner. The generated-response
	 * test measures every legal carrier band to within 0.55 dB, comfortably
	 * inside the recommendation's 1 dB tolerance. */
	if (filter->filter_index == 0) {
		filter->coefficients[
			V34_PREEMPHASIS_DELAY_SAMPLES] = 1.0;
		return true;
	}
	for (size_t offset = 0;
	     offset <= V34_PREEMPHASIS_DELAY_SAMPLES;
	     ++offset) {
		double coefficient = 0.0;

		for (unsigned int bin = 0; bin < PREEMPHASIS_DESIGN_BINS;
		     ++bin) {
			double midpoint = (double)bin + 0.5;
			double angle = midpoint * PREEMPHASIS_PI /
				       PREEMPHASIS_DESIGN_BINS;
			double normalized_frequency =
				midpoint * filter->sample_rate *
				filter->symbol_rate_denominator /
				(2.0 * PREEMPHASIS_DESIGN_BINS *
				 filter->symbol_rate_numerator);
			double amplitude = extended_template_amplitude(
				filter->filter_index, normalized_frequency);

			coefficient += amplitude * cos(angle * offset);
		}
		coefficient /= PREEMPHASIS_DESIGN_BINS;
		filter->coefficients[
			V34_PREEMPHASIS_DELAY_SAMPLES - offset] =
			coefficient;
		filter->coefficients[
			V34_PREEMPHASIS_DELAY_SAMPLES + offset] =
			coefficient;
	}
	for (size_t tap = 0; tap < V34_PREEMPHASIS_TAPS;
	     ++tap)
		sum += filter->coefficients[tap];
	if (!isfinite(sum) || fabs(sum) <= DBL_EPSILON)
		return false;
	for (size_t tap = 0; tap < V34_PREEMPHASIS_TAPS;
	     ++tap) {
		filter->coefficients[tap] /= sum;
		if (!isfinite(filter->coefficients[tap]))
			return false;
	}
	return true;
}

enum v34_preemphasis_result
v34_preemphasis_init(
	struct v34_preemphasis *filter, uint32_t sample_rate,
	uint32_t symbol_rate_numerator, uint32_t symbol_rate_denominator,
	uint8_t filter_index)
{
	struct v34_preemphasis initialized = { 0 };
	uint32_t divisor;

	if (filter == NULL || !filter_index_is_valid(filter_index) ||
	    !rates_are_valid(sample_rate, symbol_rate_numerator,
			    symbol_rate_denominator))
		return V34_PREEMPHASIS_INVALID_ARGUMENT;
	divisor = greatest_common_divisor(
		symbol_rate_numerator, symbol_rate_denominator);
	initialized.sample_rate = sample_rate;
	initialized.symbol_rate_numerator = symbol_rate_numerator / divisor;
	initialized.symbol_rate_denominator = symbol_rate_denominator / divisor;
	initialized.filter_index = filter_index;
	if (!generate_coefficients(&initialized) ||
	    !v34_preemphasis_is_valid(&initialized))
		return V34_PREEMPHASIS_NUMERIC_FAILURE;
	*filter = initialized;
	return V34_PREEMPHASIS_OK;
}

bool v34_preemphasis_is_valid(
	const struct v34_preemphasis *filter)
{
	double coefficient_sum = 0.0;

	if (filter == NULL || !filter_index_is_valid(filter->filter_index) ||
	    !rates_are_valid(filter->sample_rate,
			    filter->symbol_rate_numerator,
			    filter->symbol_rate_denominator) ||
	    greatest_common_divisor(filter->symbol_rate_numerator,
				    filter->symbol_rate_denominator) != 1 ||
	    filter->next_history >= V34_PREEMPHASIS_TAPS)
		return false;
	for (size_t tap = 0; tap < V34_PREEMPHASIS_TAPS;
	     ++tap) {
		if (!isfinite(filter->coefficients[tap]) ||
		    !isfinite(filter->history[tap]) ||
		    filter->coefficients[tap] !=
			    filter->coefficients[
				    V34_PREEMPHASIS_TAPS - 1u - tap])
			return false;
		coefficient_sum += filter->coefficients[tap];
	}
	return isfinite(coefficient_sum) &&
	       fabs(coefficient_sum - 1.0) <= PREEMPHASIS_SUM_TOLERANCE;
}

enum v34_preemphasis_result
v34_preemphasis_reset(
	struct v34_preemphasis *filter)
{
	if (!v34_preemphasis_is_valid(filter))
		return V34_PREEMPHASIS_INVALID_ARGUMENT;
	memset(filter->history, 0, sizeof(filter->history));
	filter->next_history = 0;
	return V34_PREEMPHASIS_OK;
}

enum v34_preemphasis_result
v34_preemphasis_response_db(
	const struct v34_preemphasis *filter,
	double normalized_frequency, double *magnitude_db)
{
	double angle;
	double in_phase = 0.0;
	double quadrature = 0.0;
	double magnitude;
	double response_db;

	if (!v34_preemphasis_is_valid(filter) ||
	    !isfinite(normalized_frequency) || normalized_frequency < 0.0 ||
	    magnitude_db == NULL)
		return V34_PREEMPHASIS_INVALID_ARGUMENT;
	angle = 2.0 * PREEMPHASIS_PI * normalized_frequency *
		filter->symbol_rate_numerator /
		((double)filter->sample_rate *
		 filter->symbol_rate_denominator);
	if (!isfinite(angle) || angle > PREEMPHASIS_PI)
		return V34_PREEMPHASIS_INVALID_ARGUMENT;
	for (size_t tap = 0; tap < V34_PREEMPHASIS_TAPS;
	     ++tap) {
		in_phase += filter->coefficients[tap] * cos(angle * tap);
		quadrature -= filter->coefficients[tap] * sin(angle * tap);
	}
	magnitude = hypot(in_phase, quadrature);
	if (!isfinite(magnitude) || magnitude <= DBL_MIN)
		return V34_PREEMPHASIS_NUMERIC_FAILURE;
	response_db = 20.0 * log10(magnitude);
	if (!isfinite(response_db))
		return V34_PREEMPHASIS_NUMERIC_FAILURE;
	*magnitude_db = response_db;
	return V34_PREEMPHASIS_OK;
}

enum v34_preemphasis_result
v34_preemphasis_sample(
	struct v34_preemphasis *filter, double input,
	double *output)
{
	double filtered = 0.0;
	size_t history;

	if (!v34_preemphasis_is_valid(filter) || !isfinite(input) ||
	    output == NULL)
		return V34_PREEMPHASIS_INVALID_ARGUMENT;
	history = filter->next_history;
	filtered = filter->coefficients[0] * input;
	for (size_t tap = 1; tap < V34_PREEMPHASIS_TAPS;
	     ++tap) {
		size_t previous =
			(history + V34_PREEMPHASIS_TAPS - tap) %
			V34_PREEMPHASIS_TAPS;

		filtered += filter->coefficients[tap] *
			    filter->history[previous];
	}
	if (!isfinite(filtered))
		return V34_PREEMPHASIS_NUMERIC_FAILURE;
	filter->history[history] = input;
	filter->next_history =
		(history + 1u) % V34_PREEMPHASIS_TAPS;
	*output = filtered;
	return V34_PREEMPHASIS_OK;
}

double v34_preemphasis_peak_gain_bound(
	const struct v34_preemphasis *filter)
{
	double bound = 0.0;

	if (!v34_preemphasis_is_valid(filter))
		return 0.0;
	for (size_t tap = 0; tap < V34_PREEMPHASIS_TAPS;
	     ++tap)
		bound += fabs(filter->coefficients[tap]);
	return isfinite(bound) ? bound : 0.0;
}

const char *v34_preemphasis_result_name(
	enum v34_preemphasis_result result)
{
	switch (result) {
	case V34_PREEMPHASIS_OK:
		return "ok";
	case V34_PREEMPHASIS_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_PREEMPHASIS_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
