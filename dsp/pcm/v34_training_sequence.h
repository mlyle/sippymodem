// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_TRAINING_SEQUENCE_H
#define V34_TRAINING_SEQUENCE_H

#include "qam.h"
#include "v34_constellation.h"
#include "v34_framing.h"
#include "v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_TRAINING_SEQUENCE_MIN_SYMBOLS = 512,
};

enum v34_training_sequence_result {
	V34_TRAINING_SEQUENCE_OK,
	V34_TRAINING_SEQUENCE_INVALID_ARGUMENT,
};

/* TRN has no intrinsic end marker. The startup controller owns its minimum
 * duration and any role-specific maximum that includes round-trip delay. */
struct v34_training_sequence {
	struct v34_scrambler scrambler;
	enum v34_station_role role;
	enum v34_constellation_size constellation;
	uint8_t last_clockwise_quarter_turns;
	bool has_last_symbol;
};

enum v34_training_sequence_result v34_training_sequence_init(
	struct v34_training_sequence *trn,
	enum v34_station_role role,
	enum v34_constellation_size constellation);
bool v34_training_sequence_is_valid(
	const struct v34_training_sequence *trn);

/* Consume binary ones in transmission order and emit one selected training
 * symbol. The final rotation is retained to initialize MP differential phase.
 * Rejected calls leave state and outputs unchanged. */
enum v34_training_sequence_result v34_training_sequence_next(
	struct v34_training_sequence *trn,
	struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns);

/* Emit the same selected symbol in the persistent fixed receive coordinates.
 * This keeps known-symbol adaptation in the receive equalizer's coordinate
 * system without converting through normalized doubles. */
enum v34_training_sequence_result v34_training_sequence_next_fixed(
	struct v34_training_sequence *trn,
	struct v34_analytic_sample *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns);

#endif
