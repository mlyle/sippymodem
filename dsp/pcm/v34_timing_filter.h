// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_TIMING_FILTER_H
#define V34_TIMING_FILTER_H

#include "v34_analytic_filter.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_TIMING_FILTER_COMPLEX_TAPS = 3,
	V34_TIMING_FILTER_FIR_TAPS = 40,
};

enum v34_timing_filter_result {
	V34_TIMING_FILTER_OK,
	V34_TIMING_FILTER_INVALID_ARGUMENT,
	V34_TIMING_FILTER_INVALID_STATE,
};

/* Timing-error and two-samples-per-symbol prefilter ownership. */
struct v34_timing_filter {
	int16_t positive_b_real[V34_TIMING_FILTER_COMPLEX_TAPS];
	int16_t positive_b_imag[V34_TIMING_FILTER_COMPLEX_TAPS];
	int16_t positive_a_real[V34_TIMING_FILTER_COMPLEX_TAPS];
	int16_t positive_a_imag[V34_TIMING_FILTER_COMPLEX_TAPS];
	int16_t negative_a_real[V34_TIMING_FILTER_COMPLEX_TAPS];
	int16_t negative_a_imag[V34_TIMING_FILTER_COMPLEX_TAPS];
	int16_t error_history[V34_TIMING_FILTER_FIR_TAPS];
	uint32_t prefilter_history[V34_TIMING_FILTER_FIR_TAPS];
	struct v34_analytic_sample current;
	struct v34_analytic_sample previous;
	bool initialized;
};

enum v34_timing_filter_result v34_timing_filter_init(
	struct v34_timing_filter *filter);
bool v34_timing_filter_is_valid(
	const struct v34_timing_filter *filter);

/* Observe one timing-resampler output and publish its signed timing error. */
enum v34_timing_filter_result v34_timing_filter_observe(
	struct v34_timing_filter *filter,
	struct v34_analytic_sample input, int16_t *timing_error);

/* Filter the latest adjacent pair for the two-samples-per-symbol equalizer.
 * The timing owner calls this after outputs 1 and 3 of each four-output symbol
 * interval. Rejected calls leave the owner and output unchanged. */
enum v34_timing_filter_result v34_timing_filter_prefilter(
	struct v34_timing_filter *filter,
	struct v34_analytic_sample *output);

#endif /* V34_TIMING_FILTER_H */
