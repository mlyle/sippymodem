// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_H
#define SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_H

#include "sippymodem_qam.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE3_PP_PERIOD_SYMBOLS = 48,
	SIPPYMODEM_V34_PHASE3_PP_PERIODS = 6,
	SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_SYMBOLS =
		SIPPYMODEM_V34_PHASE3_PP_PERIOD_SYMBOLS *
		SIPPYMODEM_V34_PHASE3_PP_PERIODS,
};

enum sippymodem_v34_phase3_pp_sequence_result {
	SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_OK,
	SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_COMPLETE,
	SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase3_pp_sequence {
	uint16_t emitted_symbols;
};

enum sippymodem_v34_phase3_pp_sequence_result
sippymodem_v34_phase3_pp_sequence_init(
	struct sippymodem_v34_phase3_pp_sequence *sequence);

bool sippymodem_v34_phase3_pp_sequence_is_valid(
	const struct sippymodem_v34_phase3_pp_sequence *sequence);
bool sippymodem_v34_phase3_pp_sequence_is_complete(
	const struct sippymodem_v34_phase3_pp_sequence *sequence);

/* Generate the six normative periods directly from equation 10-1. Each
 * successful call emits one finite, unit-energy 2D symbol. Completion and
 * invalid calls leave both state and output unchanged. */
enum sippymodem_v34_phase3_pp_sequence_result
sippymodem_v34_phase3_pp_sequence_next(
	struct sippymodem_v34_phase3_pp_sequence *sequence,
	struct sippymodem_qam_point *point);

const char *sippymodem_v34_phase3_pp_sequence_result_name(
	enum sippymodem_v34_phase3_pp_sequence_result result);

#endif
