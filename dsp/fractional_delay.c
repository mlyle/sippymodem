// SPDX-License-Identifier: BSD-3-Clause

#include "fractional_delay.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

enum {
	FRACTIONAL_DELAY_Q15_ONE = 32768,
	FRACTIONAL_DELAY_Q15_ROUND = 16384,
};

static bool phase_is_valid(int32_t phase_q16)
{
	return phase_q16 >= -FRACTIONAL_DELAY_ONE_SAMPLE_Q16 &&
	       phase_q16 <= FRACTIONAL_DELAY_ONE_SAMPLE_Q16;
}

static int64_t coefficient_sum(
	const int32_t coefficients[FRACTIONAL_DELAY_TAPS])
{
	int64_t sum = 0;

	for (size_t tap = 0; tap < FRACTIONAL_DELAY_TAPS; ++tap)
		sum += coefficients[tap];
	return sum;
}

static void set_integer_delay(
	int32_t coefficients[FRACTIONAL_DELAY_TAPS], size_t tap)
{
	memset(coefficients, 0,
	       FRACTIONAL_DELAY_TAPS * sizeof(*coefficients));
	coefficients[tap] = FRACTIONAL_DELAY_Q15_ONE;
}

static size_t correction_tap(int32_t phase_q16)
{
	if (phase_q16 >= FRACTIONAL_DELAY_HALF_SAMPLE_Q16)
		return FRACTIONAL_DELAY_CENTER + 1u;
	if (phase_q16 <= -FRACTIONAL_DELAY_HALF_SAMPLE_Q16)
		return FRACTIONAL_DELAY_CENTER - 1u;
	return FRACTIONAL_DELAY_CENTER;
}

static enum fractional_delay_result generate_coefficients(
	int32_t phase_q16,
	int32_t coefficients[FRACTIONAL_DELAY_TAPS])
{
	/*
	 * Generate h[n] sinc(n - 15 - phase), where h is a 31-point Hamming
	 * window, then normalize the result to unity DC gain.  Each tap is
	 * rounded to the nearest Q15 unit; correcting the nearest central tap
	 * removes the accumulated quantization error exactly.
	 */
	static const double pi = 3.14159265358979323846264338327950288;
	double terms[FRACTIONAL_DELAY_TAPS];
	double sum = 0.0;
	int64_t quantized_sum;
	double phase;

	if (!phase_is_valid(phase_q16))
		return FRACTIONAL_DELAY_OUT_OF_RANGE;
	if (phase_q16 % FRACTIONAL_DELAY_ONE_SAMPLE_Q16 == 0) {
		ptrdiff_t tap = FRACTIONAL_DELAY_CENTER +
			phase_q16 / FRACTIONAL_DELAY_ONE_SAMPLE_Q16;

		set_integer_delay(coefficients, (size_t)tap);
		return FRACTIONAL_DELAY_OK;
	}
	phase = (double)phase_q16 /
		(double)FRACTIONAL_DELAY_ONE_SAMPLE_Q16;
	for (size_t tap = 0; tap < FRACTIONAL_DELAY_TAPS; ++tap) {
		double distance = (double)tap - FRACTIONAL_DELAY_CENTER - phase;
		double window = 0.54 - 0.46 * cos(
			2.0 * pi * (double)tap /
			(double)(FRACTIONAL_DELAY_TAPS - 1));
		double sinc = sin(pi * distance) / (pi * distance);

		terms[tap] = window * sinc;
		sum += terms[tap];
	}
	if (!isfinite(sum) || fabs(sum) < 1.0e-12)
		return FRACTIONAL_DELAY_NUMERIC_FAILURE;
	for (size_t tap = 0; tap < FRACTIONAL_DELAY_TAPS; ++tap) {
		double scaled = terms[tap] * FRACTIONAL_DELAY_Q15_ONE / sum;
		long long quantized;

		if (!isfinite(scaled) || scaled < INT32_MIN ||
		    scaled > INT32_MAX)
			return FRACTIONAL_DELAY_NUMERIC_FAILURE;
		quantized = llround(scaled);
		if (quantized < INT32_MIN || quantized > INT32_MAX)
			return FRACTIONAL_DELAY_NUMERIC_FAILURE;
		coefficients[tap] = (int32_t)quantized;
	}
	quantized_sum = coefficient_sum(coefficients);
	coefficients[correction_tap(phase_q16)] +=
		(int32_t)(FRACTIONAL_DELAY_Q15_ONE - quantized_sum);
	return coefficient_sum(coefficients) == FRACTIONAL_DELAY_Q15_ONE ?
		FRACTIONAL_DELAY_OK : FRACTIONAL_DELAY_NUMERIC_FAILURE;
}

static int16_t round_and_saturate(int64_t value_q15)
{
	int64_t rounded;

	if (value_q15 < 0)
		rounded = -((-value_q15 + FRACTIONAL_DELAY_Q15_ROUND) >> 15);
	else
		rounded = (value_q15 + FRACTIONAL_DELAY_Q15_ROUND) >> 15;
	if (rounded > INT16_MAX)
		return INT16_MAX;
	if (rounded < INT16_MIN)
		return INT16_MIN;
	return (int16_t)rounded;
}

void fractional_delay_init(struct fractional_delay *delay)
{
	if (delay == NULL)
		return;
	memset(delay, 0, sizeof(*delay));
	delay->coefficients_q15[FRACTIONAL_DELAY_CENTER] =
		FRACTIONAL_DELAY_Q15_ONE;
}

bool fractional_delay_is_valid(const struct fractional_delay *delay)
{
	if (delay == NULL ||
	    delay->history_position >= FRACTIONAL_DELAY_TAPS ||
	    !phase_is_valid(delay->phase_q16) ||
	    coefficient_sum(delay->coefficients_q15) !=
		    FRACTIONAL_DELAY_Q15_ONE)
		return false;
	for (size_t tap = 0; tap < FRACTIONAL_DELAY_TAPS; ++tap)
		if (delay->coefficients_q15[tap] <
			    -FRACTIONAL_DELAY_Q15_ONE ||
		    delay->coefficients_q15[tap] >
			    FRACTIONAL_DELAY_Q15_ONE)
			return false;
	return true;
}

enum fractional_delay_result fractional_delay_set_phase(
	struct fractional_delay *delay, int32_t phase_q16)
{
	int32_t coefficients[FRACTIONAL_DELAY_TAPS];
	enum fractional_delay_result result;

	if (delay == NULL)
		return FRACTIONAL_DELAY_INVALID_ARGUMENT;
	if (!fractional_delay_is_valid(delay))
		return FRACTIONAL_DELAY_INVALID_STATE;
	if (!phase_is_valid(phase_q16))
		return FRACTIONAL_DELAY_OUT_OF_RANGE;
	if (delay->phase_q16 == phase_q16)
		return FRACTIONAL_DELAY_OK;
	result = generate_coefficients(phase_q16, coefficients);
	if (result != FRACTIONAL_DELAY_OK)
		return result;
	memcpy(delay->coefficients_q15, coefficients,
	       sizeof(delay->coefficients_q15));
	delay->phase_q16 = phase_q16;
	return FRACTIONAL_DELAY_OK;
}

enum fractional_delay_result fractional_delay_process(
	struct fractional_delay *delay, int16_t input, int16_t *output)
{
	int64_t filtered = 0;
	size_t history;

	if (delay == NULL || output == NULL)
		return FRACTIONAL_DELAY_INVALID_ARGUMENT;
	if (!fractional_delay_is_valid(delay))
		return FRACTIONAL_DELAY_INVALID_STATE;
	history = delay->history_position;
	delay->history[history] = input;
	for (size_t tap = 0; tap < FRACTIONAL_DELAY_TAPS; ++tap) {
		filtered += (int64_t)delay->coefficients_q15[tap] *
			delay->history[history];
		history = history == 0 ? FRACTIONAL_DELAY_TAPS - 1u :
			history - 1u;
	}
	delay->history_position++;
	if (delay->history_position == FRACTIONAL_DELAY_TAPS)
		delay->history_position = 0;
	*output = round_and_saturate(filtered);
	return FRACTIONAL_DELAY_OK;
}

const char *fractional_delay_result_name(
	enum fractional_delay_result result)
{
	switch (result) {
	case FRACTIONAL_DELAY_OK:
		return "ok";
	case FRACTIONAL_DELAY_INVALID_ARGUMENT:
		return "invalid argument";
	case FRACTIONAL_DELAY_INVALID_STATE:
		return "invalid fractional delay state";
	case FRACTIONAL_DELAY_OUT_OF_RANGE:
		return "fractional delay is out of range";
	case FRACTIONAL_DELAY_NUMERIC_FAILURE:
		return "fractional delay coefficient generation failed";
	}
	return "unknown fractional delay result";
}
