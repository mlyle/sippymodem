// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE3_J_SEQUENCE_H
#define SIPPYMODEM_V34_PHASE3_J_SEQUENCE_H

#include "qam.h"
#include "v34_constellation.h"
#include "v34_framing.h"
#include "v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE3_J_PATTERN_BITS = 16,
	SIPPYMODEM_V34_PHASE3_J_PATTERN_SYMBOLS = 8,
};

enum sippymodem_v34_phase3_j_sequence_result {
	SIPPYMODEM_V34_PHASE3_J_SEQUENCE_OK,
	SIPPYMODEM_V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase3_trn;

struct sippymodem_v34_phase3_j_sequence {
	struct sippymodem_v34_scrambler scrambler;
	enum sippymodem_v34_station_role role;
	enum sippymodem_v34_constellation_size remote_constellation;
	uint8_t previous_clockwise_quarter_turns;
	uint8_t symbol_in_pattern;
};

/* Continue the live scrambler and differential phase from the final symbol of
 * a Phase 3 TRN stream. The owning startup controller enforces TRN duration. */
enum sippymodem_v34_phase3_j_sequence_result
sippymodem_v34_phase3_j_sequence_init_from_trn(
	struct sippymodem_v34_phase3_j_sequence *sequence,
	const struct sippymodem_v34_phase3_trn *trn,
	enum sippymodem_v34_constellation_size remote_constellation);

bool sippymodem_v34_phase3_j_sequence_is_valid(
	const struct sippymodem_v34_phase3_j_sequence *sequence);

/* J may stop only after a whole 16-bit pattern. A new sequence is initially
 * at a boundary; each boundary recurs after eight emitted symbols. */
bool sippymodem_v34_phase3_j_sequence_at_pattern_boundary(
	const struct sippymodem_v34_phase3_j_sequence *sequence);

/* Emit one unit-energy J symbol. Rejected calls leave state and outputs
 * unchanged. */
enum sippymodem_v34_phase3_j_sequence_result
sippymodem_v34_phase3_j_sequence_next(
	struct sippymodem_v34_phase3_j_sequence *sequence,
	struct sippymodem_qam_point *point,
	uint8_t *clockwise_quarter_turns);

const char *sippymodem_v34_phase3_j_sequence_result_name(
	enum sippymodem_v34_phase3_j_sequence_result result);

#endif
