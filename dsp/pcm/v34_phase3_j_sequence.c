// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_j_sequence.h"

#include "memory_region.h"
#include "v34_phase3_trn.h"

#include <stddef.h>

_Static_assert(
	V34_PHASE3_J_PATTERN_SYMBOLS * 2 ==
		V34_PHASE3_J_PATTERN_BITS,
	"V.34 J must carry two bits in every 2D symbol");

bool v34_phase3_j_sequence_is_valid(
	const struct v34_phase3_j_sequence *sequence)
{
	return sequence != NULL &&
	       v34_constellation_size_is_valid(
		       sequence->remote_constellation) &&
	       sequence->previous_clockwise_quarter_turns < 4u &&
	       sequence->symbol_in_pattern <
		       V34_PHASE3_J_PATTERN_SYMBOLS &&
	       v34_transmit_scrambler_is_valid(
		       &sequence->scrambler, sequence->role);
}

bool v34_phase3_j_sequence_is_equal(
	const struct v34_phase3_j_sequence *left,
	const struct v34_phase3_j_sequence *right)
{
	return left != NULL && right != NULL &&
	       left->scrambler.history == right->scrambler.history &&
	       left->scrambler.polynomial == right->scrambler.polynomial &&
	       left->role == right->role &&
	       left->remote_constellation == right->remote_constellation &&
	       left->previous_clockwise_quarter_turns ==
		       right->previous_clockwise_quarter_turns &&
	       left->symbol_in_pattern == right->symbol_in_pattern;
}

enum v34_phase3_j_sequence_result
v34_phase3_j_sequence_init_from_trn(
	struct v34_phase3_j_sequence *sequence,
	const struct v34_phase3_trn *trn,
	enum v34_constellation_size remote_constellation)
{
	struct v34_phase3_j_sequence initialized;

	if (sequence == NULL || !v34_phase3_trn_is_valid(trn) ||
	    !trn->has_last_symbol ||
	    !v34_constellation_size_is_valid(
		    remote_constellation))
		return V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT;
	initialized = (struct v34_phase3_j_sequence){
		.scrambler = trn->scrambler,
		.role = trn->role,
		.remote_constellation = remote_constellation,
		.previous_clockwise_quarter_turns =
			trn->last_clockwise_quarter_turns,
	};
	if (!v34_phase3_j_sequence_is_valid(&initialized))
		return V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT;
	*sequence = initialized;
	return V34_PHASE3_J_SEQUENCE_OK;
}

bool v34_phase3_j_sequence_at_pattern_boundary(
	const struct v34_phase3_j_sequence *sequence)
{
	return v34_phase3_j_sequence_is_valid(sequence) &&
	       sequence->symbol_in_pattern == 0;
}

static uint8_t input_dibit(
	const struct v34_phase3_j_sequence *sequence)
{
	uint16_t pattern =
		sequence->remote_constellation ==
				V34_CONSTELLATION_4_POINT ?
			V34_PHASE3_J_4_POINT_PATTERN :
			V34_PHASE3_J_16_POINT_PATTERN;
	unsigned int shift =
		2u * (V34_PHASE3_J_PATTERN_SYMBOLS - 1u -
		      sequence->symbol_in_pattern);

	/* Table 18 writes the first transmitted bit on the left. */
	return (uint8_t)((pattern >> shift) & 3u);
}

static bool advance_j(
	struct v34_phase3_j_sequence *sequence, uint8_t *rotation)
{
	if (v34_scramble_differential_dibit(
		    &sequence->scrambler, input_dibit(sequence),
		    sequence->previous_clockwise_quarter_turns,
		    rotation) != V34_FRAMING_OK)
		return false;
	sequence->previous_clockwise_quarter_turns = *rotation;
	sequence->symbol_in_pattern++;
	if (sequence->symbol_in_pattern == V34_PHASE3_J_PATTERN_SYMBOLS)
		sequence->symbol_in_pattern = 0;
	return v34_phase3_j_sequence_is_valid(sequence);
}

static bool outputs_overlap(
	const struct v34_phase3_j_sequence *sequence, const void *point,
	size_t point_size, const uint8_t *rotation)
{
	return memory_regions_overlap(
		       sequence, sizeof(*sequence), point, point_size) ||
	       memory_regions_overlap(
		       sequence, sizeof(*sequence), rotation,
		       sizeof(*rotation)) ||
	       memory_regions_overlap(
		       point, point_size, rotation, sizeof(*rotation));
}

enum v34_phase3_j_sequence_result
v34_phase3_j_sequence_next(
	struct v34_phase3_j_sequence *sequence,
	struct qam_point *point,
	uint8_t *clockwise_quarter_turns)
{
	struct v34_phase3_j_sequence next;
	struct qam_point next_point;
	uint8_t rotation;

	if (!v34_phase3_j_sequence_is_valid(sequence) ||
	    point == NULL || clockwise_quarter_turns == NULL ||
	    outputs_overlap(
		    sequence, point, sizeof(*point),
		    clockwise_quarter_turns))
		return V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT;
	next = *sequence;
	if (!advance_j(&next, &rotation) ||
	    v34_training_constellation_point(
		    V34_CONSTELLATION_4_POINT, 0, rotation,
		    &next_point) != V34_CONSTELLATION_OK)
		return V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT;
	*sequence = next;
	*point = next_point;
	*clockwise_quarter_turns = rotation;
	return V34_PHASE3_J_SEQUENCE_OK;
}

enum v34_phase3_j_sequence_result
v34_phase3_j_sequence_next_fixed(
	struct v34_phase3_j_sequence *sequence,
	struct v34_analytic_sample *point,
	uint8_t *clockwise_quarter_turns)
{
	struct v34_phase3_j_sequence next;
	struct v34_analytic_sample next_point;
	uint8_t rotation;

	if (!v34_phase3_j_sequence_is_valid(sequence) ||
	    point == NULL || clockwise_quarter_turns == NULL ||
	    outputs_overlap(
		    sequence, point, sizeof(*point),
		    clockwise_quarter_turns))
		return V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT;
	next = *sequence;
	if (!advance_j(&next, &rotation) ||
	    v34_training_constellation_fixed_point(
		    V34_CONSTELLATION_4_POINT, 0, rotation,
		    &next_point) != V34_CONSTELLATION_OK)
		return V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT;
	*sequence = next;
	*point = next_point;
	*clockwise_quarter_turns = rotation;
	return V34_PHASE3_J_SEQUENCE_OK;
}
