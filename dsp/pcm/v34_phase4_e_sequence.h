// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_E_SEQUENCE_H
#define V34_PHASE4_E_SEQUENCE_H

#include "qam.h"
#include "v34_constellation.h"
#include "v34_framing.h"
#include "v34_phase4_mp.h"
#include "v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE4_E_BITS = 20,
	V34_PHASE4_E_4_POINT_SYMBOLS =
		V34_PHASE4_E_BITS / 2,
	V34_PHASE4_E_16_POINT_SYMBOLS =
		V34_PHASE4_E_BITS / 4,
};

enum v34_phase4_e_sequence_result {
	V34_PHASE4_E_SEQUENCE_OK,
	V34_PHASE4_E_SEQUENCE_COMPLETE,
	V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT,
};

struct v34_phase4_e_sequence {
	struct v34_scrambler scrambler;
	enum v34_station_role role;
	enum v34_constellation_size constellation;
	uint8_t previous_clockwise_quarter_turns;
	uint8_t emitted_bits;
};

/* E follows a completed MP or MP-prime record without resetting the Phase 4
 * scrambler or differential encoder. */
enum v34_phase4_e_sequence_result
v34_phase4_e_sequence_init_from_mp(
	struct v34_phase4_e_sequence *sequence,
	const struct v34_phase4_mp *mp);

bool v34_phase4_e_sequence_is_valid(
	const struct v34_phase4_e_sequence *sequence);
bool v34_phase4_e_sequence_is_complete(
	const struct v34_phase4_e_sequence *sequence);

/* Emit one symbol from the fixed 20-bit sequence. Rejected calls leave state
 * and every output unchanged. */
enum v34_phase4_e_sequence_result
v34_phase4_e_sequence_next(
	struct v34_phase4_e_sequence *sequence,
	struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns);

const char *v34_phase4_e_sequence_result_name(
	enum v34_phase4_e_sequence_result result);

#endif
