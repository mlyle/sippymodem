// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE3_S_SEQUENCE_H
#define V34_PHASE3_S_SEQUENCE_H

#include "qam.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE3_S_SYMBOLS = 128,
	V34_PHASE3_S_BAR_SYMBOLS = 16,
	V34_PHASE3_S_SEQUENCE_SYMBOLS =
		V34_PHASE3_S_SYMBOLS +
		V34_PHASE3_S_BAR_SYMBOLS,
};

enum v34_phase3_s_section {
	V34_PHASE3_S_SECTION,
	V34_PHASE3_S_BAR_SECTION,
};

enum v34_phase3_s_sequence_result {
	V34_PHASE3_S_SEQUENCE_OK,
	V34_PHASE3_S_SEQUENCE_COMPLETE,
	V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT,
};

struct v34_phase3_s_sequence {
	uint16_t emitted_symbols;
};

enum v34_phase3_s_sequence_result
v34_phase3_s_sequence_init(
	struct v34_phase3_s_sequence *sequence);

bool v34_phase3_s_sequence_is_valid(
	const struct v34_phase3_s_sequence *sequence);
bool v34_phase3_s_sequence_is_complete(
	const struct v34_phase3_s_sequence *sequence);

/* Point 0 is normalized to unit energy. One successful call consumes exactly
 * one 2D symbol; completion and invalid calls leave every output unchanged. */
enum v34_phase3_s_sequence_result
v34_phase3_s_sequence_next(
	struct v34_phase3_s_sequence *sequence,
	struct qam_point *point,
	enum v34_phase3_s_section *section);

#endif
