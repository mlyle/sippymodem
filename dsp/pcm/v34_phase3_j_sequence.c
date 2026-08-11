// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_j_sequence.h"

#include "v34_phase3_trn.h"

#include <stddef.h>

enum {
	V34_PHASE3_J_4_POINT_PATTERN = 0x0991,
	V34_PHASE3_J_16_POINT_PATTERN = 0x0d91,
};

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

enum v34_phase3_j_sequence_result
v34_phase3_j_sequence_next(
	struct v34_phase3_j_sequence *sequence,
	struct sippymodem_qam_point *point,
	uint8_t *clockwise_quarter_turns)
{
	struct v34_phase3_j_sequence next;
	struct sippymodem_qam_point next_point;
	uint8_t rotation;

	if (!v34_phase3_j_sequence_is_valid(sequence) ||
	    point == NULL || clockwise_quarter_turns == NULL)
		return V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT;
	next = *sequence;
	if (v34_scramble_differential_dibit(
		    &next.scrambler, input_dibit(&next),
		    next.previous_clockwise_quarter_turns, &rotation) !=
	    V34_FRAMING_OK)
		return V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT;
	next.previous_clockwise_quarter_turns = rotation;
	next.symbol_in_pattern++;
	if (next.symbol_in_pattern ==
	    V34_PHASE3_J_PATTERN_SYMBOLS)
		next.symbol_in_pattern = 0;
	next_point = v34_rotated_point_zero(-(int)rotation);
	if (!v34_phase3_j_sequence_is_valid(&next))
		return V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT;
	*sequence = next;
	*point = next_point;
	*clockwise_quarter_turns = rotation;
	return V34_PHASE3_J_SEQUENCE_OK;
}

const char *v34_phase3_j_sequence_result_name(
	enum v34_phase3_j_sequence_result result)
{
	switch (result) {
	case V34_PHASE3_J_SEQUENCE_OK:
		return "ok";
	case V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
