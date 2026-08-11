// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_j_prime_sequence.h"

#include "v34_constellation.h"
#include "v34_phase3_j_sequence.h"

#include <stddef.h>

enum {
	V34_PHASE4_J_PRIME_PATTERN = 0xf991,
};

_Static_assert(
	SIPPYMODEM_V34_PHASE4_J_PRIME_SYMBOLS * 2 ==
		SIPPYMODEM_V34_PHASE4_J_PRIME_BITS,
	"V.34 J-prime must carry two bits in every 2D symbol");

bool sippymodem_v34_phase4_j_prime_sequence_is_valid(
	const struct sippymodem_v34_phase4_j_prime_sequence *sequence)
{
	return sequence != NULL &&
	       sequence->role == SIPPYMODEM_V34_CALLING_STATION &&
	       sequence->previous_clockwise_quarter_turns < 4u &&
	       sequence->emitted_symbols <=
		       SIPPYMODEM_V34_PHASE4_J_PRIME_SYMBOLS &&
	       sippymodem_v34_transmit_scrambler_is_valid(
		       &sequence->scrambler, sequence->role);
}

enum sippymodem_v34_phase4_j_prime_sequence_result
sippymodem_v34_phase4_j_prime_sequence_init_from_j(
	struct sippymodem_v34_phase4_j_prime_sequence *sequence,
	const struct sippymodem_v34_phase3_j_sequence *j_sequence)
{
	struct sippymodem_v34_phase4_j_prime_sequence initialized;

	if (sequence == NULL || j_sequence == NULL ||
	    !sippymodem_v34_phase3_j_sequence_at_pattern_boundary(j_sequence) ||
	    j_sequence->role != SIPPYMODEM_V34_CALLING_STATION)
		return SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT;
	initialized = (struct sippymodem_v34_phase4_j_prime_sequence){
		.scrambler = j_sequence->scrambler,
		.role = j_sequence->role,
		.previous_clockwise_quarter_turns =
			j_sequence->previous_clockwise_quarter_turns,
	};
	if (!sippymodem_v34_phase4_j_prime_sequence_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT;
	*sequence = initialized;
	return SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_OK;
}

bool sippymodem_v34_phase4_j_prime_sequence_is_complete(
	const struct sippymodem_v34_phase4_j_prime_sequence *sequence)
{
	return sippymodem_v34_phase4_j_prime_sequence_is_valid(sequence) &&
	       sequence->emitted_symbols ==
		       SIPPYMODEM_V34_PHASE4_J_PRIME_SYMBOLS;
}

static uint8_t input_dibit(uint8_t symbol)
{
	unsigned int shift =
		2u * (SIPPYMODEM_V34_PHASE4_J_PRIME_SYMBOLS - 1u - symbol);

	/* Table 19 writes the first transmitted bit on the left. */
	return (uint8_t)((V34_PHASE4_J_PRIME_PATTERN >> shift) & 3u);
}

enum sippymodem_v34_phase4_j_prime_sequence_result
sippymodem_v34_phase4_j_prime_sequence_next(
	struct sippymodem_v34_phase4_j_prime_sequence *sequence,
	struct sippymodem_qam_point *point,
	uint8_t *clockwise_quarter_turns)
{
	struct sippymodem_v34_phase4_j_prime_sequence next;
	struct sippymodem_qam_point next_point;
	uint8_t rotation;

	if (!sippymodem_v34_phase4_j_prime_sequence_is_valid(sequence) ||
	    point == NULL || clockwise_quarter_turns == NULL)
		return SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT;
	if (sippymodem_v34_phase4_j_prime_sequence_is_complete(sequence))
		return SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_COMPLETE;
	next = *sequence;
	if (sippymodem_v34_scramble_differential_dibit(
		    &next.scrambler, input_dibit(next.emitted_symbols),
		    next.previous_clockwise_quarter_turns, &rotation) !=
	    SIPPYMODEM_V34_FRAMING_OK)
		return SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT;
	next.previous_clockwise_quarter_turns = rotation;
	next.emitted_symbols++;
	next_point = sippymodem_v34_rotated_point_zero(-(int)rotation);
	if (!sippymodem_v34_phase4_j_prime_sequence_is_valid(&next))
		return SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT;
	*sequence = next;
	*point = next_point;
	*clockwise_quarter_turns = rotation;
	return SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_OK;
}

const char *sippymodem_v34_phase4_j_prime_sequence_result_name(
	enum sippymodem_v34_phase4_j_prime_sequence_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_COMPLETE:
		return "complete";
	case SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
