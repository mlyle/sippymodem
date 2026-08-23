// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PREEMPHASIS_H
#define V34_PREEMPHASIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V34_PREEMPHASIS_MAX_FILTER_INDEX = 10,
	V34_PREEMPHASIS_TAPS = 16,
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

enum v34_preemphasis_result
v34_preemphasis_init(
	struct v34_preemphasis *filter, uint32_t sample_rate,
	uint32_t symbol_rate_numerator, uint32_t symbol_rate_denominator,
	uint8_t filter_index);
bool v34_preemphasis_is_valid(
	const struct v34_preemphasis *filter);

/* The immutable 8-kHz profiles retain the exact negotiated V.34 line shape.
 * The 2743-symbol/s selection shares the neighboring 2800-symbol/s profile;
 * all other sample rates and non-standard symbol rates are rejected. */

/* One successful call consumes one finite line sample. Invalid or
 * non-representable calls leave both the filter and output unchanged. */
enum v34_preemphasis_result
v34_preemphasis_sample(
	struct v34_preemphasis *filter, double input,
	double *output);
#endif
