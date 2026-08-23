// SPDX-License-Identifier: BSD-3-Clause

#include "v34_preemphasis.h"

#include "integer_math.h"

#include <math.h>

enum {
	V34_PREEMPHASIS_SAMPLE_RATE = 8000,
	V34_PREEMPHASIS_Q14_SCALE = 16384,
};

#include "v34_preemphasis_profiles.inc"

static const int16_t V34_PREEMPHASIS_IDENTITY[V34_PREEMPHASIS_TAPS] = {
	[7] = V34_PREEMPHASIS_Q14_SCALE,
};

static bool filter_index_is_valid(uint8_t filter_index)
{
	return filter_index <= V34_PREEMPHASIS_MAX_FILTER_INDEX;
}

static const int16_t (*profile_bank(
	uint32_t sample_rate, uint32_t symbol_rate_numerator,
	uint32_t symbol_rate_denominator))[V34_PREEMPHASIS_TAPS]
{
	uint32_t divisor;

	if (sample_rate != V34_PREEMPHASIS_SAMPLE_RATE ||
	    symbol_rate_numerator == 0 || symbol_rate_denominator == 0)
		return NULL;
	divisor = (uint32_t)gcd_u64(
		symbol_rate_numerator, symbol_rate_denominator);
	symbol_rate_numerator /= divisor;
	symbol_rate_denominator /= divisor;
	if (symbol_rate_denominator == 1) {
		switch (symbol_rate_numerator) {
		case 2400:
			return V34_PREEMPHASIS_2400;
		case 2800:
			return V34_PREEMPHASIS_2800;
		case 3000:
			return V34_PREEMPHASIS_3000;
		case 3200:
			return V34_PREEMPHASIS_3200;
		default:
			return NULL;
		}
	}
	if (symbol_rate_numerator == 19200 &&
	    symbol_rate_denominator == 7)
		return V34_PREEMPHASIS_2800;
	if (symbol_rate_numerator == 24000 &&
	    symbol_rate_denominator == 7)
		return V34_PREEMPHASIS_3429;
	return NULL;
}

static const int16_t *selected_profile(
	uint32_t sample_rate, uint32_t symbol_rate_numerator,
	uint32_t symbol_rate_denominator, uint8_t filter_index)
{
	const int16_t (*bank)[V34_PREEMPHASIS_TAPS] = profile_bank(
		sample_rate, symbol_rate_numerator, symbol_rate_denominator);

	if (bank == NULL || !filter_index_is_valid(filter_index))
		return NULL;
	return filter_index == 0 ? V34_PREEMPHASIS_IDENTITY :
				 bank[filter_index - 1u];
}

static double coefficient_value(int16_t coefficient)
{
	return (double)coefficient / V34_PREEMPHASIS_Q14_SCALE;
}

enum v34_preemphasis_result
v34_preemphasis_init(
	struct v34_preemphasis *filter, uint32_t sample_rate,
	uint32_t symbol_rate_numerator, uint32_t symbol_rate_denominator,
	uint8_t filter_index)
{
	struct v34_preemphasis initialized = { 0 };
	const int16_t *profile;
	uint32_t divisor;

	if (filter == NULL || symbol_rate_numerator == 0 ||
	    symbol_rate_denominator == 0)
		return V34_PREEMPHASIS_INVALID_ARGUMENT;
	profile = selected_profile(
		sample_rate, symbol_rate_numerator, symbol_rate_denominator,
		filter_index);
	if (profile == NULL)
		return V34_PREEMPHASIS_INVALID_ARGUMENT;
	divisor = (uint32_t)gcd_u64(
		symbol_rate_numerator, symbol_rate_denominator);
	initialized.sample_rate = sample_rate;
	initialized.symbol_rate_numerator =
		symbol_rate_numerator / divisor;
	initialized.symbol_rate_denominator =
		symbol_rate_denominator / divisor;
	initialized.filter_index = filter_index;
	for (size_t tap = 0; tap < V34_PREEMPHASIS_TAPS; ++tap)
		initialized.coefficients[tap] = coefficient_value(profile[tap]);
	if (!v34_preemphasis_is_valid(&initialized))
		return V34_PREEMPHASIS_NUMERIC_FAILURE;
	*filter = initialized;
	return V34_PREEMPHASIS_OK;
}

bool v34_preemphasis_is_valid(const struct v34_preemphasis *filter)
{
	const int16_t *profile;

	if (filter == NULL || filter->next_history >= V34_PREEMPHASIS_TAPS ||
	    gcd_u64(filter->symbol_rate_numerator,
		    filter->symbol_rate_denominator) != 1)
		return false;
	profile = selected_profile(
		filter->sample_rate, filter->symbol_rate_numerator,
		filter->symbol_rate_denominator, filter->filter_index);
	if (profile == NULL)
		return false;
	for (size_t tap = 0; tap < V34_PREEMPHASIS_TAPS; ++tap)
		if (filter->coefficients[tap] !=
			    coefficient_value(profile[tap]) ||
		    !isfinite(filter->history[tap]))
			return false;
	return true;
}

enum v34_preemphasis_result
v34_preemphasis_sample(
	struct v34_preemphasis *filter, double input, double *output)
{
	struct v34_preemphasis next;
	double filtered;
	size_t history;

	if (!v34_preemphasis_is_valid(filter) || !isfinite(input) ||
	    output == NULL)
		return V34_PREEMPHASIS_INVALID_ARGUMENT;
	next = *filter;
	history = next.next_history;
	filtered = next.coefficients[0] * input;
	for (size_t tap = 1; tap < V34_PREEMPHASIS_TAPS; ++tap) {
		size_t previous =
			(history + V34_PREEMPHASIS_TAPS - tap) %
			V34_PREEMPHASIS_TAPS;

		filtered += next.coefficients[tap] * next.history[previous];
	}
	if (!isfinite(filtered))
		return V34_PREEMPHASIS_NUMERIC_FAILURE;
	next.history[history] = input;
	next.next_history = (history + 1u) % V34_PREEMPHASIS_TAPS;
	if (!v34_preemphasis_is_valid(&next))
		return V34_PREEMPHASIS_NUMERIC_FAILURE;
	*filter = next;
	*output = filtered;
	return V34_PREEMPHASIS_OK;
}
