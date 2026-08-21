// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE3_PP_SEQUENCE_H
#define V34_PHASE3_PP_SEQUENCE_H

#include "qam.h"
#include "v34_analytic_filter.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE3_PP_PERIOD_SYMBOLS = 48,
	V34_PHASE3_PP_PERIODS = 6,
	V34_PHASE3_PP_SEQUENCE_SYMBOLS =
		V34_PHASE3_PP_PERIOD_SYMBOLS *
		V34_PHASE3_PP_PERIODS,
};

enum v34_phase3_pp_sequence_result {
	V34_PHASE3_PP_SEQUENCE_OK,
	V34_PHASE3_PP_SEQUENCE_COMPLETE,
	V34_PHASE3_PP_SEQUENCE_INVALID_ARGUMENT,
};

struct v34_phase3_pp_sequence {
	uint16_t emitted_symbols;
};

enum v34_phase3_pp_sequence_result
v34_phase3_pp_sequence_init(
	struct v34_phase3_pp_sequence *sequence);

bool v34_phase3_pp_sequence_is_valid(
	const struct v34_phase3_pp_sequence *sequence);
bool v34_phase3_pp_sequence_is_complete(
	const struct v34_phase3_pp_sequence *sequence);

/* Generate the six normative periods directly from equation 10-1. Each
 * successful call emits one finite, unit-energy 2D symbol. Completion and
 * invalid calls leave both state and output unchanged. */
enum v34_phase3_pp_sequence_result
v34_phase3_pp_sequence_next(
	struct v34_phase3_pp_sequence *sequence,
	struct qam_point *point);

/* Emit the same clause 10.1.3.6 sequence in the exact half-scale coordinates
 * consumed by the fixed receive equalizer. */
enum v34_phase3_pp_sequence_result
v34_phase3_pp_sequence_next_fixed(
	struct v34_phase3_pp_sequence *sequence,
	struct v34_analytic_sample *point);

#endif
