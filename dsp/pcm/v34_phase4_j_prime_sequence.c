// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_j_prime_sequence.h"

#include "v34_constellation.h"
#include "v34_phase3_j_sequence.h"

#include <stddef.h>

enum {
	V34_PHASE4_J_PRIME_PATTERN = 0xf991,
};

_Static_assert(
	V34_PHASE4_J_PRIME_SYMBOLS * 2 ==
		V34_PHASE4_J_PRIME_BITS,
	"V.34 J-prime must carry two bits in every 2D symbol");

bool v34_phase4_j_prime_sequence_is_valid(
	const struct v34_phase4_j_prime_sequence *sequence)
{
	return sequence != NULL &&
	       sequence->role == V34_CALLING_STATION &&
	       sequence->previous_clockwise_quarter_turns < 4u &&
	       sequence->emitted_symbols <=
		       V34_PHASE4_J_PRIME_SYMBOLS &&
	       v34_transmit_scrambler_is_valid(
		       &sequence->scrambler, sequence->role);
}

enum v34_phase4_j_prime_sequence_result
v34_phase4_j_prime_sequence_init_from_j(
	struct v34_phase4_j_prime_sequence *sequence,
	const struct v34_phase3_j_sequence *j_sequence)
{
	struct v34_phase4_j_prime_sequence initialized;

	if (sequence == NULL || j_sequence == NULL ||
	    !v34_phase3_j_sequence_at_pattern_boundary(j_sequence) ||
	    j_sequence->role != V34_CALLING_STATION)
		return V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT;
	initialized = (struct v34_phase4_j_prime_sequence){
		.scrambler = j_sequence->scrambler,
		.role = j_sequence->role,
		.previous_clockwise_quarter_turns =
			j_sequence->previous_clockwise_quarter_turns,
	};
	if (!v34_phase4_j_prime_sequence_is_valid(&initialized))
		return V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT;
	*sequence = initialized;
	return V34_PHASE4_J_PRIME_SEQUENCE_OK;
}

bool v34_phase4_j_prime_sequence_is_complete(
	const struct v34_phase4_j_prime_sequence *sequence)
{
	return v34_phase4_j_prime_sequence_is_valid(sequence) &&
	       sequence->emitted_symbols ==
		       V34_PHASE4_J_PRIME_SYMBOLS;
}

static uint8_t input_dibit(uint8_t symbol)
{
	unsigned int shift =
		2u * (V34_PHASE4_J_PRIME_SYMBOLS - 1u - symbol);

	/* Table 19 writes the first transmitted bit on the left. */
	return (uint8_t)((V34_PHASE4_J_PRIME_PATTERN >> shift) & 3u);
}

enum v34_phase4_j_prime_sequence_result
v34_phase4_j_prime_sequence_next(
	struct v34_phase4_j_prime_sequence *sequence,
	struct qam_point *point,
	uint8_t *clockwise_quarter_turns)
{
	struct v34_phase4_j_prime_sequence next;
	struct qam_point next_point;
	uint8_t rotation;

	if (!v34_phase4_j_prime_sequence_is_valid(sequence) ||
	    point == NULL || clockwise_quarter_turns == NULL)
		return V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT;
	if (v34_phase4_j_prime_sequence_is_complete(sequence))
		return V34_PHASE4_J_PRIME_SEQUENCE_COMPLETE;
	next = *sequence;
	if (v34_scramble_differential_dibit(
		    &next.scrambler, input_dibit(next.emitted_symbols),
		    next.previous_clockwise_quarter_turns, &rotation) !=
	    V34_FRAMING_OK)
		return V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT;
	next.previous_clockwise_quarter_turns = rotation;
	next.emitted_symbols++;
	next_point = v34_rotated_point_zero(-(int)rotation);
	if (!v34_phase4_j_prime_sequence_is_valid(&next))
		return V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT;
	*sequence = next;
	*point = next_point;
	*clockwise_quarter_turns = rotation;
	return V34_PHASE4_J_PRIME_SEQUENCE_OK;
}

const char *v34_phase4_j_prime_sequence_result_name(
	enum v34_phase4_j_prime_sequence_result result)
{
	switch (result) {
	case V34_PHASE4_J_PRIME_SEQUENCE_OK:
		return "ok";
	case V34_PHASE4_J_PRIME_SEQUENCE_COMPLETE:
		return "complete";
	case V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
