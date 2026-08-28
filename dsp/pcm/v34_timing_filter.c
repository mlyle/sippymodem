// SPDX-License-Identifier: BSD-3-Clause

#include "v34_timing_filter.h"

#include "integer_math.h"
#include "memory_region.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const int16_t PREFILTER[V34_TIMING_FILTER_FIR_TAPS] = {
	1,    -5,   -5,     8,    32,    40,     1,   -84,
	-150, -102,  94,   337,   406,   115,  -481, -1014,
	-958,   23, 1779,  3649,  4789,  4653,  3322,  1450,
	-137, -899, -819,  -293,   195,   371,   252,    32,
	-111, -121,  -53,    12,    34,    21,     4,    -4,
};

static const int16_t ERROR_HIGH_PASS[V34_TIMING_FILTER_FIR_TAPS] = {
	3,      9,   -27,   -34,    87,    75,  -195,  -128,
	368,   187,  -634,  -247,  1048,   301, -1751,  -344,
	3267,  373, -10358, 16001, -10358, 373,  3267,  -344,
	-1751, 301,  1048,  -247,  -634,   187,  368,  -128,
	-195,   75,    87,   -34,   -27,     9,    3,     0,
};

static const int16_t A_REAL[] = { 16384, -22805, -33 };
static const int16_t POSITIVE_A_IMAG[] = { 0, -22853, 15908 };
static const int16_t NEGATIVE_A_IMAG[] = { 0, 22853, -15908 };
static const int16_t B_REAL[] = { 64, 0, -64 };

_Static_assert(
	sizeof(PREFILTER) / sizeof(PREFILTER[0]) ==
		V34_TIMING_FILTER_FIR_TAPS,
	"V.34 timing prefilter size changed");
_Static_assert(
	sizeof(A_REAL) / sizeof(A_REAL[0]) ==
		V34_TIMING_FILTER_COMPLEX_TAPS,
	"V.34 timing complex filter size changed");

static int32_t rounded_shift(
	uint32_t value, uint32_t rounding, unsigned int shift)
{
	return arithmetic_shift_right_u32(value + rounding, shift);
}

static int16_t scale_input(int16_t input)
{
	return wrap_s16_from_s32(rounded_shift(
		multiply_low_s32(input, 0x599a), UINT32_C(0x8000), 15));
}

static void accumulate_complex(
	int16_t input_real, int16_t input_imag,
	int16_t coefficient_real, int16_t coefficient_imag,
	uint32_t *real, uint32_t *imag)
{
	*real += multiply_low_s32(input_real, coefficient_real) -
		 multiply_low_s32(input_imag, coefficient_imag);
	*imag += multiply_low_s32(input_real, coefficient_imag) +
		 multiply_low_s32(input_imag, coefficient_real);
}

static void shift_recursive_filter(
	int16_t *real_history, int16_t *imag_history,
	const int16_t *real_coefficients,
	const int16_t *imag_coefficients,
	uint32_t *real, uint32_t *imag)
{
	int16_t input_real = real_history[0];
	int16_t input_imag = imag_history[0];

	for (size_t tap = 1; tap < V34_TIMING_FILTER_COMPLEX_TAPS; ++tap) {
		int16_t old_real = real_history[tap];
		int16_t old_imag = imag_history[tap];

		real_history[tap] = input_real;
		imag_history[tap] = input_imag;
		accumulate_complex(
			input_real, input_imag, real_coefficients[tap],
			imag_coefficients[tap], real, imag);
		input_real = old_real;
		input_imag = old_imag;
	}
}

static int16_t high_pass_error(
	int16_t history[V34_TIMING_FILTER_FIR_TAPS], int16_t input)
{
	uint32_t accumulator = 0;

	for (size_t tap = 0; tap < V34_TIMING_FILTER_FIR_TAPS; ++tap) {
		int16_t old = history[tap];

		history[tap] = input;
		accumulator += multiply_low_s32(input, ERROR_HIGH_PASS[tap]);
		input = old;
	}
	return round_shift_s32_word_to_s16(accumulator, 15);
}

static uint32_t pack_sample(struct v34_analytic_sample sample)
{
	return ((uint32_t)(uint16_t)sample.quadrature << 16) |
	       (uint16_t)sample.in_phase;
}

static int16_t low_s16(uint32_t value)
{
	return wrap_s16_from_s32((int32_t)(uint16_t)value);
}

static int16_t high_s16(uint32_t value)
{
	return wrap_s16_from_s32((int32_t)(uint16_t)(value >> 16));
}

bool v34_timing_filter_is_valid(const struct v34_timing_filter *filter)
{
	return filter != NULL && filter->initialized;
}

enum v34_timing_filter_result v34_timing_filter_init(
	struct v34_timing_filter *filter)
{
	struct v34_timing_filter initialized;

	if (filter == NULL)
		return V34_TIMING_FILTER_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	initialized.initialized = true;
	*filter = initialized;
	return V34_TIMING_FILTER_OK;
}

enum v34_timing_filter_result v34_timing_filter_observe(
	struct v34_timing_filter *filter,
	struct v34_analytic_sample input, int16_t *timing_error)
{
	struct v34_timing_filter next;
	uint32_t b_real = 0;
	uint32_t b_imag = 0;
	uint32_t positive_a_real = 0;
	uint32_t positive_a_imag = 0;
	uint32_t negative_a_real = 0;
	uint32_t negative_a_imag = 0;
	int16_t input_real;
	int16_t input_imag;
	int32_t filtered_positive_real;
	int32_t filtered_positive_imag;
	int32_t filtered_negative_real;
	int32_t filtered_negative_imag;
	int16_t generated;

	if (!v34_timing_filter_is_valid(filter))
		return V34_TIMING_FILTER_INVALID_STATE;
	if (timing_error == NULL || memory_regions_overlap(
			filter, sizeof(*filter), timing_error,
			sizeof(*timing_error)))
		return V34_TIMING_FILTER_INVALID_ARGUMENT;
	next = *filter;
	input_real = scale_input(input.in_phase);
	input_imag = scale_input(input.quadrature);
	next.previous = next.current;
	next.current = input;
	for (size_t tap = 0; tap < V34_TIMING_FILTER_COMPLEX_TAPS; ++tap) {
		int16_t old_real = next.positive_b_real[tap];
		int16_t old_imag = next.positive_b_imag[tap];

		next.positive_b_real[tap] = input_real;
		next.positive_b_imag[tap] = input_imag;
		accumulate_complex(
			input_real, input_imag, B_REAL[tap], 0,
			&b_real, &b_imag);
		input_real = old_real;
		input_imag = old_imag;
	}
	shift_recursive_filter(
		next.positive_a_real, next.positive_a_imag,
		A_REAL, POSITIVE_A_IMAG,
		&positive_a_real, &positive_a_imag);
	shift_recursive_filter(
		next.negative_a_real, next.negative_a_imag,
		A_REAL, NEGATIVE_A_IMAG,
		&negative_a_real, &negative_a_imag);
	filtered_positive_real = rounded_shift(
		b_real - positive_a_real, UINT32_C(0x2000), 14);
	filtered_positive_imag = rounded_shift(
		b_imag - positive_a_imag, UINT32_C(0x2000), 14);
	filtered_negative_real = rounded_shift(
		b_real - negative_a_real, UINT32_C(0x2000), 14);
	filtered_negative_imag = rounded_shift(
		b_imag - negative_a_imag, UINT32_C(0x2000), 14);
	next.positive_a_real[0] = wrap_s16_from_s32(filtered_positive_real);
	next.positive_a_imag[0] = wrap_s16_from_s32(filtered_positive_imag);
	next.negative_a_real[0] = wrap_s16_from_s32(filtered_negative_real);
	next.negative_a_imag[0] = wrap_s16_from_s32(filtered_negative_imag);
	generated = round_shift_s32_word_to_s16(
		multiply_low_s32(filtered_positive_imag, filtered_negative_real) -
			multiply_low_s32(filtered_positive_real,
				     filtered_negative_imag),
		14);
	generated = high_pass_error(next.error_history, generated);
	*filter = next;
	*timing_error = generated;
	return V34_TIMING_FILTER_OK;
}

enum v34_timing_filter_result v34_timing_filter_prefilter(
	struct v34_timing_filter *filter,
	struct v34_analytic_sample *output)
{
	struct v34_timing_filter next;
	uint32_t current;
	uint32_t previous;
	uint32_t real = 0;
	uint32_t imag = 0;
	struct v34_analytic_sample generated;

	if (!v34_timing_filter_is_valid(filter))
		return V34_TIMING_FILTER_INVALID_STATE;
	if (output == NULL || memory_regions_overlap(
			filter, sizeof(*filter), output, sizeof(*output)))
		return V34_TIMING_FILTER_INVALID_ARGUMENT;
	next = *filter;
	current = pack_sample(next.current);
	previous = pack_sample(next.previous);
	for (size_t tap = 0; tap < V34_TIMING_FILTER_FIR_TAPS;
	     tap += 2u) {
		uint32_t old_current = next.prefilter_history[tap];
		uint32_t old_previous = next.prefilter_history[tap + 1u];

		next.prefilter_history[tap] = current;
		next.prefilter_history[tap + 1u] = previous;
		real += multiply_low_s32(high_s16(current), PREFILTER[tap]);
		real += multiply_low_s32(high_s16(previous), PREFILTER[tap + 1u]);
		imag += multiply_low_s32(low_s16(current), PREFILTER[tap]);
		imag += multiply_low_s32(low_s16(previous), PREFILTER[tap + 1u]);
		current = old_current;
		previous = old_previous;
	}
	generated = (struct v34_analytic_sample){
		.in_phase = round_shift_s32_word_to_s16(imag, 14),
		.quadrature = round_shift_s32_word_to_s16(real, 14),
	};
	*filter = next;
	*output = generated;
	return V34_TIMING_FILTER_OK;
}
