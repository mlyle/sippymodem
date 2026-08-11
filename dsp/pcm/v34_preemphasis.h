// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PREEMPHASIS_H
#define SIPPYMODEM_V34_PREEMPHASIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PREEMPHASIS_MAX_FILTER_INDEX = 10,
	SIPPYMODEM_V34_PREEMPHASIS_TAPS = 21,
	SIPPYMODEM_V34_PREEMPHASIS_DELAY_SAMPLES =
		(SIPPYMODEM_V34_PREEMPHASIS_TAPS - 1) / 2,
};

enum sippymodem_v34_preemphasis_result {
	SIPPYMODEM_V34_PREEMPHASIS_OK,
	SIPPYMODEM_V34_PREEMPHASIS_INVALID_ARGUMENT,
	SIPPYMODEM_V34_PREEMPHASIS_NUMERIC_FAILURE,
};

struct sippymodem_v34_preemphasis {
	double coefficients[SIPPYMODEM_V34_PREEMPHASIS_TAPS];
	double history[SIPPYMODEM_V34_PREEMPHASIS_TAPS];
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
enum sippymodem_v34_preemphasis_result
sippymodem_v34_preemphasis_template_db(
	uint8_t filter_index, double normalized_frequency,
	double *magnitude_db, bool *constrained);

enum sippymodem_v34_preemphasis_result
sippymodem_v34_preemphasis_init(
	struct sippymodem_v34_preemphasis *filter, uint32_t sample_rate,
	uint32_t symbol_rate_numerator, uint32_t symbol_rate_denominator,
	uint8_t filter_index);
bool sippymodem_v34_preemphasis_is_valid(
	const struct sippymodem_v34_preemphasis *filter);
enum sippymodem_v34_preemphasis_result
sippymodem_v34_preemphasis_reset(
	struct sippymodem_v34_preemphasis *filter);

/* Report the generated FIR response at a non-negative normalized frequency
 * no greater than the configured Nyquist frequency. */
enum sippymodem_v34_preemphasis_result
sippymodem_v34_preemphasis_response_db(
	const struct sippymodem_v34_preemphasis *filter,
	double normalized_frequency, double *magnitude_db);

/* One successful call consumes one finite line sample. Invalid or
 * non-representable calls leave both the filter and output unchanged. */
enum sippymodem_v34_preemphasis_result
sippymodem_v34_preemphasis_sample(
	struct sippymodem_v34_preemphasis *filter, double input,
	double *output);

double sippymodem_v34_preemphasis_peak_gain_bound(
	const struct sippymodem_v34_preemphasis *filter);
const char *sippymodem_v34_preemphasis_result_name(
	enum sippymodem_v34_preemphasis_result result);

#endif
