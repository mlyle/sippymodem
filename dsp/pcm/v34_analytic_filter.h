// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_ANALYTIC_FILTER_H
#define V34_ANALYTIC_FILTER_H

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_ANALYTIC_FILTER_TAPS = 64,
	V34_ANALYTIC_FILTER_GROUP_DELAY_SAMPLES = 30,
};

enum v34_analytic_filter_result {
	V34_ANALYTIC_FILTER_OK,
	V34_ANALYTIC_FILTER_INVALID_ARGUMENT,
	V34_ANALYTIC_FILTER_INVALID_STATE,
};

struct v34_analytic_sample {
	int16_t in_phase;
	int16_t quadrature;
};

/* Fixed 8-kHz analytic band-pass ownership for the V.34 receive lane. */
struct v34_analytic_filter {
	int16_t history[V34_ANALYTIC_FILTER_TAPS];
	bool initialized;
};

enum v34_analytic_filter_result v34_analytic_filter_init(
	struct v34_analytic_filter *filter);
bool v34_analytic_filter_is_valid(
	const struct v34_analytic_filter *filter);

/* Consume one signed line sample and emit the matching Q14 analytic sample.
 * Rejected calls leave the owner and output unchanged. */
enum v34_analytic_filter_result v34_analytic_filter_process(
	struct v34_analytic_filter *filter, int16_t sample,
	struct v34_analytic_sample *output);

#endif /* V34_ANALYTIC_FILTER_H */
