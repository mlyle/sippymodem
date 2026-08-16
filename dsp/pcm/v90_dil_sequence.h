// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_DIL_SEQUENCE_H
#define V90_DIL_SEQUENCE_H

#include "v90_dil.h"
#include "v90_pcm_symbol.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V90_DIL_SEGMENT_INTERVALS = 6,
	V90_DIL_MAX_SEGMENT_SYMBOLS =
		(127 + 1) * V90_DIL_SEGMENT_INTERVALS,
};

enum v90_dil_sequence_result {
	V90_DIL_SEQUENCE_OK,
	V90_DIL_SEQUENCE_COMPLETE,
	V90_DIL_SEQUENCE_INVALID_ARGUMENT,
	V90_DIL_SEQUENCE_INVALID_STATE,
};

/* Owned, table-free realization of one repeating DIL. Patterns restart at
 * each segment and repeat independently. A finish request is honored only at
 * a segment boundary. */
struct v90_dil_sequence {
	struct v90_dil_parameters parameters;
	enum g711_law law;
	uint16_t segment_symbol;
	uint8_t segment;
	bool finish_requested;
	bool complete;
};

enum v90_dil_sequence_result v90_dil_sequence_init(
	struct v90_dil_sequence *sequence,
	const struct v90_dil_parameters *parameters,
	enum g711_law law);
bool v90_dil_sequence_is_valid(
	const struct v90_dil_sequence *sequence);

/* Generates one exact 8000-symbol/s network character. COMPLETE and rejected
 * calls leave both the sequence and output unchanged. */
enum v90_dil_sequence_result v90_dil_sequence_next(
	struct v90_dil_sequence *sequence,
	struct v90_pcm_symbol *symbol);

/* Latches termination. A request at a boundary completes immediately;
 * otherwise the current segment is completed first. */
enum v90_dil_sequence_result v90_dil_sequence_request_finish(
	struct v90_dil_sequence *sequence);
bool v90_dil_sequence_at_segment_boundary(
	const struct v90_dil_sequence *sequence);
bool v90_dil_sequence_is_complete(
	const struct v90_dil_sequence *sequence);

const char *v90_dil_sequence_result_name(
	enum v90_dil_sequence_result result);

#endif /* V90_DIL_SEQUENCE_H */
