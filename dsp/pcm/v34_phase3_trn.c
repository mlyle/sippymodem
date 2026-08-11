// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_trn.h"

#include "v34_constellation.h"

#include <stddef.h>

bool v34_phase3_trn_is_valid(
	const struct v34_phase3_trn *trn)
{
	if (trn == NULL || trn->last_clockwise_quarter_turns > 3u ||
	    (!trn->has_last_symbol &&
	     trn->last_clockwise_quarter_turns != 0u))
		return false;
	return v34_transmit_scrambler_is_valid(
		&trn->scrambler, trn->role);
}

enum v34_phase3_trn_result v34_phase3_trn_init(
	struct v34_phase3_trn *trn,
	enum v34_station_role role)
{
	struct v34_phase3_trn initialized = { .role = role };

	if (trn == NULL)
		return V34_PHASE3_TRN_INVALID_ARGUMENT;
	if (v34_transmit_scrambler_init(
		    &initialized.scrambler, role) !=
	    V34_FRAMING_OK)
		return V34_PHASE3_TRN_INVALID_ARGUMENT;
	*trn = initialized;
	return V34_PHASE3_TRN_OK;
}

enum v34_phase3_trn_result v34_phase3_trn_next(
	struct v34_phase3_trn *trn,
	struct qam_point *point,
	uint8_t *clockwise_quarter_turns)
{
	struct v34_phase3_trn next;
	struct qam_point next_point;
	uint8_t rotation;

	if (!v34_phase3_trn_is_valid(trn) || point == NULL ||
	    clockwise_quarter_turns == NULL)
		return V34_PHASE3_TRN_INVALID_ARGUMENT;
	next = *trn;
	if (v34_scramble_mapped_dibit(
		    &next.scrambler, 3u, &rotation) !=
	    V34_FRAMING_OK)
		return V34_PHASE3_TRN_INVALID_ARGUMENT;
	if (v34_training_constellation_point(
		    V34_CONSTELLATION_4_POINT, 0, rotation,
		    &next_point) != V34_CONSTELLATION_OK)
		return V34_PHASE3_TRN_INVALID_ARGUMENT;
	next.last_clockwise_quarter_turns = rotation;
	next.has_last_symbol = true;
	if (!v34_phase3_trn_is_valid(&next))
		return V34_PHASE3_TRN_INVALID_ARGUMENT;
	*trn = next;
	*point = next_point;
	*clockwise_quarter_turns = rotation;
	return V34_PHASE3_TRN_OK;
}

const char *v34_phase3_trn_result_name(
	enum v34_phase3_trn_result result)
{
	switch (result) {
	case V34_PHASE3_TRN_OK:
		return "ok";
	case V34_PHASE3_TRN_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
