// SPDX-License-Identifier: BSD-3-Clause

#include "v34_training_sequence.h"

#include "memory_region.h"

#include <stddef.h>

bool v34_training_sequence_is_valid(
	const struct v34_training_sequence *trn)
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

enum v34_training_sequence_result v34_training_sequence_init(
	struct v34_training_sequence *trn,
	enum v34_station_role role,
	enum v34_constellation_size constellation)
{
	struct v34_training_sequence initialized = {
		.role = role,
		.constellation = constellation,
	};

	if (trn == NULL ||
	    !v34_constellation_size_is_valid(constellation))
		return V34_TRAINING_SEQUENCE_INVALID_ARGUMENT;
	if (v34_transmit_scrambler_init(
		    &initialized.scrambler, role) !=
	    V34_FRAMING_OK)
		return V34_TRAINING_SEQUENCE_INVALID_ARGUMENT;
	*trn = initialized;
	return V34_TRAINING_SEQUENCE_OK;
}

static enum v34_training_sequence_result next_labels(
	struct v34_training_sequence *trn, struct v34_training_sequence *next,
	uint8_t *quarter_point, uint8_t *clockwise_quarter_turns)
{
	uint8_t next_quarter_point = 0;
	uint8_t rotation;

	*next = *trn;
	if (v34_scramble_mapped_dibit(
		    &next->scrambler, 3u, &rotation) !=
	    V34_FRAMING_OK)
		return V34_TRAINING_SEQUENCE_INVALID_ARGUMENT;
	if (next->constellation == V34_CONSTELLATION_16_POINT &&
	    v34_scramble_mapped_dibit(
		    &next->scrambler, 3u, &next_quarter_point) !=
		    V34_FRAMING_OK)
		return V34_TRAINING_SEQUENCE_INVALID_ARGUMENT;
	next->last_clockwise_quarter_turns = rotation;
	next->has_last_symbol = true;
	if (!v34_training_sequence_is_valid(next))
		return V34_TRAINING_SEQUENCE_INVALID_ARGUMENT;
	*quarter_point = next_quarter_point;
	*clockwise_quarter_turns = rotation;
	return V34_TRAINING_SEQUENCE_OK;
}

static bool outputs_overlap(
	const struct v34_training_sequence *trn,
	const void *point, size_t point_size,
	const uint8_t *quarter_point,
	const uint8_t *clockwise_quarter_turns)
{
	return memory_regions_overlap(
		       trn, sizeof(*trn), point, point_size) ||
	       memory_regions_overlap(
		       trn, sizeof(*trn), quarter_point,
		       sizeof(*quarter_point)) ||
	       memory_regions_overlap(
		       trn, sizeof(*trn), clockwise_quarter_turns,
		       sizeof(*clockwise_quarter_turns)) ||
	       memory_regions_overlap(
		       point, point_size, quarter_point,
		       sizeof(*quarter_point)) ||
	       memory_regions_overlap(
		       point, point_size, clockwise_quarter_turns,
		       sizeof(*clockwise_quarter_turns)) ||
	       memory_regions_overlap(
		       quarter_point, sizeof(*quarter_point),
		       clockwise_quarter_turns,
		       sizeof(*clockwise_quarter_turns));
}

enum v34_training_sequence_result v34_training_sequence_next(
	struct v34_training_sequence *trn,
	struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns)
{
	struct v34_training_sequence next;
	struct qam_point generated;
	uint8_t quarter;
	uint8_t rotation;

	if (!v34_training_sequence_is_valid(trn) || point == NULL ||
	    quarter_point == NULL ||
	    clockwise_quarter_turns == NULL ||
	    outputs_overlap(
		    trn, point, sizeof(*point), quarter_point,
		    clockwise_quarter_turns) ||
	    next_labels(trn, &next, &quarter, &rotation) !=
		    V34_TRAINING_SEQUENCE_OK ||
	    v34_training_constellation_point(
		    next.constellation, quarter, rotation, &generated) !=
		    V34_CONSTELLATION_OK)
		return V34_TRAINING_SEQUENCE_INVALID_ARGUMENT;
	*trn = next;
	*point = generated;
	*quarter_point = quarter;
	*clockwise_quarter_turns = rotation;
	return V34_TRAINING_SEQUENCE_OK;
}

enum v34_training_sequence_result v34_training_sequence_next_fixed(
	struct v34_training_sequence *trn,
	struct v34_analytic_sample *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns)
{
	struct v34_training_sequence next;
	struct v34_analytic_sample generated;
	uint8_t quarter;
	uint8_t rotation;

	if (!v34_training_sequence_is_valid(trn) || point == NULL ||
	    quarter_point == NULL ||
	    clockwise_quarter_turns == NULL ||
	    outputs_overlap(
		    trn, point, sizeof(*point), quarter_point,
		    clockwise_quarter_turns) ||
	    next_labels(trn, &next, &quarter, &rotation) !=
		    V34_TRAINING_SEQUENCE_OK ||
	    v34_training_constellation_fixed_point(
		    next.constellation, quarter, rotation, &generated) !=
		    V34_CONSTELLATION_OK)
		return V34_TRAINING_SEQUENCE_INVALID_ARGUMENT;
	*trn = next;
	*point = generated;
	*quarter_point = quarter;
	*clockwise_quarter_turns = rotation;
	return V34_TRAINING_SEQUENCE_OK;
}
