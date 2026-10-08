// SPDX-License-Identifier: BSD-3-Clause

#include "fractional_delay.h"

#include "integer_math.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

enum {
	FRACTIONAL_DELAY_Q15_ONE = 32768,
	/* Unity DC gain constrains the sum, not each quantized tap. Rounding
	 * all taps can leave up to half their count in Q15 units for the
	 * central correction, including near an exact integer delay. */
	FRACTIONAL_DELAY_COEFFICIENT_LIMIT =
		FRACTIONAL_DELAY_Q15_ONE + FRACTIONAL_DELAY_TAPS / 2,
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
	 * Apply a Hamming window centered on sinc(n - 15 - phase), retaining
	 * the fixed sample history, then normalize to unity DC gain. Round
	 * to Q15 and remove the accumulated DC error at the central tap(s).
	 */
	static const double pi = 3.14159265358979323846264338327950288;
	double terms[FRACTIONAL_DELAY_TAPS];
	double sum = 0.0;
	int64_t quantized_sum;
	double phase;
	bool half_sample = phase_q16 == -FRACTIONAL_DELAY_HALF_SAMPLE_Q16 ||
		phase_q16 == FRACTIONAL_DELAY_HALF_SAMPLE_Q16;
	size_t count = FRACTIONAL_DELAY_TAPS - (unsigned)half_sample;
	size_t first = half_sample && phase_q16 > 0 ? 1u : 0u;

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
	/* Move the window with the sinc, rather than leaving it centered at
	 * 15. Exact half samples then have thirty symmetric nonzero taps,
	 * and nearby phases approach that kernel continuously. A fixed odd
	 * window leaves alternating energy that biases V.92 Su phase probes. */
	for (size_t tap = 0; tap < FRACTIONAL_DELAY_TAPS; ++tap) {
		double distance = (double)tap - FRACTIONAL_DELAY_CENTER - phase;
		if (fabs(distance) > FRACTIONAL_DELAY_CENTER) {
			terms[tap] = 0.0;
			continue;
		}
		double window = 0.54 + 0.46 * cos(pi * distance / FRACTIONAL_DELAY_CENTER);
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
	if (half_sample)
		for (size_t tap = 0; tap < count / 2u; ++tap)
			coefficients[first + count - 1u - tap] = coefficients[first + tap];
	quantized_sum = coefficient_sum(coefficients);
	if (half_sample) {
		/* Paired coefficients make the correction even. Preserve symmetry
		 * as well as unity DC gain when rounding to Q15. */
		int32_t correction = (int32_t)(FRACTIONAL_DELAY_Q15_ONE - quantized_sum) / 2;
		coefficients[first + count / 2u - 1u] += correction;
		coefficients[first + count / 2u] += correction;
	} else {
		coefficients[correction_tap(phase_q16)] +=
			(int32_t)(FRACTIONAL_DELAY_Q15_ONE - quantized_sum);
	}
	return coefficient_sum(coefficients) == FRACTIONAL_DELAY_Q15_ONE ?
		FRACTIONAL_DELAY_OK : FRACTIONAL_DELAY_NUMERIC_FAILURE;
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
			    -FRACTIONAL_DELAY_COEFFICIENT_LIMIT ||
		    delay->coefficients_q15[tap] >
			    FRACTIONAL_DELAY_COEFFICIENT_LIMIT)
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
	*output = round_shift_s64_to_s16_saturating(filtered, 15u);
	return FRACTIONAL_DELAY_OK;
}
