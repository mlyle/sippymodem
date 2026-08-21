// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_trn.h"

#include <stddef.h>

bool v34_phase4_trn_is_valid(
	const struct v34_phase4_trn *trn)
{
	if (trn == NULL ||
	    !v34_constellation_size_is_valid(trn->constellation) ||
	    trn->last_clockwise_quarter_turns > 3u ||
	    (!trn->has_last_symbol &&
	     trn->last_clockwise_quarter_turns != 0u))
		return false;
	return v34_transmit_scrambler_is_valid(
		&trn->scrambler, trn->role);
}

enum v34_phase4_trn_result v34_phase4_trn_init(
	struct v34_phase4_trn *trn,
	enum v34_station_role role,
	enum v34_constellation_size constellation)
{
	struct v34_phase4_trn initialized = {
		.role = role,
		.constellation = constellation,
	};

	if (trn == NULL ||
	    !v34_constellation_size_is_valid(constellation))
		return V34_PHASE4_TRN_INVALID_ARGUMENT;
	if (v34_transmit_scrambler_init(
		    &initialized.scrambler, role) !=
	    V34_FRAMING_OK)
		return V34_PHASE4_TRN_INVALID_ARGUMENT;
	*trn = initialized;
	return V34_PHASE4_TRN_OK;
}

static enum v34_phase4_trn_result next_labels(
	struct v34_phase4_trn *trn, struct v34_phase4_trn *next,
	uint8_t *quarter_point, uint8_t *clockwise_quarter_turns)
{
	uint8_t next_quarter_point = 0;
	uint8_t rotation;

	if (!v34_phase4_trn_is_valid(trn) || next == NULL ||
	    quarter_point == NULL || clockwise_quarter_turns == NULL)
		return V34_PHASE4_TRN_INVALID_ARGUMENT;
	*next = *trn;
	if (v34_scramble_mapped_dibit(
		    &next->scrambler, 3u, &rotation) !=
	    V34_FRAMING_OK)
		return V34_PHASE4_TRN_INVALID_ARGUMENT;
	if (next->constellation == V34_CONSTELLATION_16_POINT &&
	    v34_scramble_mapped_dibit(
		    &next->scrambler, 3u, &next_quarter_point) !=
		    V34_FRAMING_OK)
		return V34_PHASE4_TRN_INVALID_ARGUMENT;
	next->last_clockwise_quarter_turns = rotation;
	next->has_last_symbol = true;
	if (!v34_phase4_trn_is_valid(next))
		return V34_PHASE4_TRN_INVALID_ARGUMENT;
	*quarter_point = next_quarter_point;
	*clockwise_quarter_turns = rotation;
	return V34_PHASE4_TRN_OK;
}

enum v34_phase4_trn_result v34_phase4_trn_next(
	struct v34_phase4_trn *trn,
	struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns)
{
	struct v34_phase4_trn next;
	struct qam_point generated;
	uint8_t quarter;
	uint8_t rotation;

	if (point == NULL || quarter_point == NULL ||
	    clockwise_quarter_turns == NULL ||
	    next_labels(trn, &next, &quarter, &rotation) != V34_PHASE4_TRN_OK ||
	    v34_training_constellation_point(
		    next.constellation, quarter, rotation, &generated) !=
		    V34_CONSTELLATION_OK)
		return V34_PHASE4_TRN_INVALID_ARGUMENT;
	*trn = next;
	*point = generated;
	*quarter_point = quarter;
	*clockwise_quarter_turns = rotation;
	return V34_PHASE4_TRN_OK;
}

enum v34_phase4_trn_result v34_phase4_trn_next_fixed(
	struct v34_phase4_trn *trn,
	struct v34_analytic_sample *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns)
{
	struct v34_phase4_trn next;
	struct v34_analytic_sample generated;
	uint8_t quarter;
	uint8_t rotation;

	if (point == NULL || quarter_point == NULL ||
	    clockwise_quarter_turns == NULL ||
	    next_labels(trn, &next, &quarter, &rotation) != V34_PHASE4_TRN_OK ||
	    v34_training_constellation_fixed_point(
		    next.constellation, quarter, rotation, &generated) !=
		    V34_CONSTELLATION_OK)
		return V34_PHASE4_TRN_INVALID_ARGUMENT;
	*trn = next;
	*point = generated;
	*quarter_point = quarter;
	*clockwise_quarter_turns = rotation;
	return V34_PHASE4_TRN_OK;
}

const char *v34_phase4_trn_result_name(
	enum v34_phase4_trn_result result)
{
	switch (result) {
	case V34_PHASE4_TRN_OK:
		return "ok";
	case V34_PHASE4_TRN_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
