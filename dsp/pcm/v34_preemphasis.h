// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PREEMPHASIS_H
#define V34_PREEMPHASIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V34_PREEMPHASIS_MAX_FILTER_INDEX = 10,
	V34_PREEMPHASIS_TAPS = 21,
	V34_PREEMPHASIS_DELAY_SAMPLES =
		(V34_PREEMPHASIS_TAPS - 1) / 2,
};

enum v34_preemphasis_result {
	V34_PREEMPHASIS_OK,
	V34_PREEMPHASIS_INVALID_ARGUMENT,
	V34_PREEMPHASIS_NUMERIC_FAILURE,
};

struct v34_preemphasis {
	double coefficients[V34_PREEMPHASIS_TAPS];
	double history[V34_PREEMPHASIS_TAPS];
	uint32_t sample_rate;
	uint32_t symbol_rate_numerator;
	uint32_t symbol_rate_denominator;
	size_t next_history;
	uint8_t filter_index;
};

/* Return the V.34 relative-magnitude template in decibels. Frequencies are
 * normalized by the symbol rate and are accepted over the specified 0..1.2
 * range. Indices 6..10 leave 0.4..0.8 unconstrained; the returned straight
 * bridge is the filter designer's continuous extension, and constrained is
 * false strictly inside that interval. */
enum v34_preemphasis_result
v34_preemphasis_template_db(
	uint8_t filter_index, double normalized_frequency,
	double *magnitude_db, bool *constrained);

enum v34_preemphasis_result
v34_preemphasis_init(
	struct v34_preemphasis *filter, uint32_t sample_rate,
	uint32_t symbol_rate_numerator, uint32_t symbol_rate_denominator,
	uint8_t filter_index);
bool v34_preemphasis_is_valid(
	const struct v34_preemphasis *filter);
enum v34_preemphasis_result
v34_preemphasis_reset(
	struct v34_preemphasis *filter);

/* Report the generated FIR response at a non-negative normalized frequency
 * no greater than the configured Nyquist frequency. */
enum v34_preemphasis_result
v34_preemphasis_response_db(
	const struct v34_preemphasis *filter,
	double normalized_frequency, double *magnitude_db);

/* One successful call consumes one finite line sample. Invalid or
 * non-representable calls leave both the filter and output unchanged. */
enum v34_preemphasis_result
v34_preemphasis_sample(
	struct v34_preemphasis *filter, double input,
	double *output);

double v34_preemphasis_peak_gain_bound(
	const struct v34_preemphasis *filter);
const char *v34_preemphasis_result_name(
	enum v34_preemphasis_result result);

#endif
