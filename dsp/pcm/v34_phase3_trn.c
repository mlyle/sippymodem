// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_trn.h"

#include "memory_region.h"
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

static bool advance_trn(
	struct v34_phase3_trn *trn, uint8_t *rotation)
{
	if (v34_scramble_mapped_dibit(
		    &trn->scrambler, 3u, rotation) != V34_FRAMING_OK)
		return false;
	trn->last_clockwise_quarter_turns = *rotation;
	trn->has_last_symbol = true;
	return v34_phase3_trn_is_valid(trn);
}

static bool outputs_overlap(
	const struct v34_phase3_trn *trn, const void *point, size_t point_size,
	const uint8_t *clockwise_quarter_turns)
{
	return memory_regions_overlap(
		       trn, sizeof(*trn), point, point_size) ||
	       memory_regions_overlap(
		       trn, sizeof(*trn), clockwise_quarter_turns,
		       sizeof(*clockwise_quarter_turns)) ||
	       memory_regions_overlap(
		       point, point_size, clockwise_quarter_turns,
		       sizeof(*clockwise_quarter_turns));
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
	    clockwise_quarter_turns == NULL ||
	    outputs_overlap(
		    trn, point, sizeof(*point), clockwise_quarter_turns))
		return V34_PHASE3_TRN_INVALID_ARGUMENT;
	next = *trn;
	if (!advance_trn(&next, &rotation) ||
	    v34_training_constellation_point(
		    V34_CONSTELLATION_4_POINT, 0, rotation,
		    &next_point) != V34_CONSTELLATION_OK)
		return V34_PHASE3_TRN_INVALID_ARGUMENT;
	*trn = next;
	*point = next_point;
	*clockwise_quarter_turns = rotation;
	return V34_PHASE3_TRN_OK;
}

enum v34_phase3_trn_result v34_phase3_trn_next_fixed(
	struct v34_phase3_trn *trn,
	struct v34_analytic_sample *point,
	uint8_t *clockwise_quarter_turns)
{
	struct v34_phase3_trn next;
	struct v34_analytic_sample next_point;
	uint8_t rotation;

	if (!v34_phase3_trn_is_valid(trn) || point == NULL ||
	    clockwise_quarter_turns == NULL ||
	    outputs_overlap(
		    trn, point, sizeof(*point), clockwise_quarter_turns))
		return V34_PHASE3_TRN_INVALID_ARGUMENT;
	next = *trn;
	if (!advance_trn(&next, &rotation) ||
	    v34_training_constellation_fixed_point(
		    V34_CONSTELLATION_4_POINT, 0, rotation,
		    &next_point) != V34_CONSTELLATION_OK)
		return V34_PHASE3_TRN_INVALID_ARGUMENT;
	*trn = next;
	*point = next_point;
	*clockwise_quarter_turns = rotation;
	return V34_PHASE3_TRN_OK;
}
