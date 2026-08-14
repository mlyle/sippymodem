// SPDX-License-Identifier: BSD-3-Clause

#ifndef FRACTIONAL_DELAY_H
#define FRACTIONAL_DELAY_H

#include <stdbool.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

enum {
	FRACTIONAL_DELAY_TAPS = 31,
	FRACTIONAL_DELAY_CENTER = FRACTIONAL_DELAY_TAPS / 2,
	FRACTIONAL_DELAY_HALF_SAMPLE_Q16 = 0x8000,
	FRACTIONAL_DELAY_ONE_SAMPLE_Q16 = 0x10000,
};

enum fractional_delay_result {
	FRACTIONAL_DELAY_OK,
	FRACTIONAL_DELAY_INVALID_ARGUMENT,
	FRACTIONAL_DELAY_INVALID_STATE,
	FRACTIONAL_DELAY_OUT_OF_RANGE,
	FRACTIONAL_DELAY_NUMERIC_FAILURE,
};

/*
 * A band-limited fractional delay with a fixed fifteen-sample group delay.
 * Positive phase delays the stream; negative phase advances it.  Changing
 * phase preserves the sample history so one owner can maintain a continuous
 * waveform across a protocol boundary.
 */
struct fractional_delay {
	int32_t coefficients_q15[FRACTIONAL_DELAY_TAPS];
	int16_t history[FRACTIONAL_DELAY_TAPS];
	int32_t phase_q16;
	uint8_t history_position;
};

void fractional_delay_init(struct fractional_delay *delay);
bool fractional_delay_is_valid(const struct fractional_delay *delay);
enum fractional_delay_result fractional_delay_set_phase(
	struct fractional_delay *delay,
	int32_t phase_q16);
enum fractional_delay_result fractional_delay_process(
	struct fractional_delay *delay,
	int16_t input,
	int16_t *output);

/* Interpolate between samples[1] and samples[2] with one finite neighbor on
 * either side. A fraction of zero or one returns the corresponding endpoint
 * exactly. Rejected calls leave output unchanged. Keeping this small primitive
 * inline lets DSP owners use it without acquiring the streaming delay filter. */
static inline enum fractional_delay_result
fractional_delay_interpolate_cubic(
	const double samples[4], double fraction, double *output)
{
	double plus_one;
	double minus_one;
	double minus_two;
	double interpolated;

	if (samples == NULL || output == NULL || !isfinite(fraction))
		return FRACTIONAL_DELAY_INVALID_ARGUMENT;
	if (fraction < 0.0 || fraction > 1.0)
		return FRACTIONAL_DELAY_OUT_OF_RANGE;
	for (size_t index = 0; index < 4; ++index)
		if (!isfinite(samples[index]))
			return FRACTIONAL_DELAY_INVALID_ARGUMENT;
	if (fraction == 0.0) {
		*output = samples[1];
		return FRACTIONAL_DELAY_OK;
	}
	if (fraction == 1.0) {
		*output = samples[2];
		return FRACTIONAL_DELAY_OK;
	}
	plus_one = fraction + 1.0;
	minus_one = fraction - 1.0;
	minus_two = fraction - 2.0;
	/* Four-point Lagrange interpolation at nodes -1, 0, 1, and 2. */
	interpolated =
		samples[0] * (-fraction * minus_one * minus_two / 6.0) +
		samples[1] * (plus_one * minus_one * minus_two / 2.0) +
		samples[2] * (-plus_one * fraction * minus_two / 2.0) +
		samples[3] * (plus_one * fraction * minus_one / 6.0);
	if (!isfinite(interpolated))
		return FRACTIONAL_DELAY_NUMERIC_FAILURE;
	*output = interpolated;
	return FRACTIONAL_DELAY_OK;
}

const char *fractional_delay_result_name(
	enum fractional_delay_result result);

#endif
