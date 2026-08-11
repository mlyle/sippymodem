// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE3_S_SEQUENCE_H
#define SIPPYMODEM_V34_PHASE3_S_SEQUENCE_H

#include "qam.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE3_S_SYMBOLS = 128,
	SIPPYMODEM_V34_PHASE3_S_BAR_SYMBOLS = 16,
	SIPPYMODEM_V34_PHASE3_S_SEQUENCE_SYMBOLS =
		SIPPYMODEM_V34_PHASE3_S_SYMBOLS +
		SIPPYMODEM_V34_PHASE3_S_BAR_SYMBOLS,
};

enum sippymodem_v34_phase3_s_section {
	SIPPYMODEM_V34_PHASE3_S_SECTION,
	SIPPYMODEM_V34_PHASE3_S_BAR_SECTION,
};

enum sippymodem_v34_phase3_s_sequence_result {
	SIPPYMODEM_V34_PHASE3_S_SEQUENCE_OK,
	SIPPYMODEM_V34_PHASE3_S_SEQUENCE_COMPLETE,
	SIPPYMODEM_V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase3_s_sequence {
	uint16_t emitted_symbols;
};

enum sippymodem_v34_phase3_s_sequence_result
sippymodem_v34_phase3_s_sequence_init(
	struct sippymodem_v34_phase3_s_sequence *sequence);

bool sippymodem_v34_phase3_s_sequence_is_valid(
	const struct sippymodem_v34_phase3_s_sequence *sequence);
bool sippymodem_v34_phase3_s_sequence_is_complete(
	const struct sippymodem_v34_phase3_s_sequence *sequence);

/* Point 0 is normalized to unit energy. One successful call consumes exactly
 * one 2D symbol; completion and invalid calls leave every output unchanged. */
enum sippymodem_v34_phase3_s_sequence_result
sippymodem_v34_phase3_s_sequence_next(
	struct sippymodem_v34_phase3_s_sequence *sequence,
	struct sippymodem_qam_point *point,
	enum sippymodem_v34_phase3_s_section *section);

const char *sippymodem_v34_phase3_s_sequence_result_name(
	enum sippymodem_v34_phase3_s_sequence_result result);

#endif
