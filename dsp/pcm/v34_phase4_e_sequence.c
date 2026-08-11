// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_e_sequence.h"

#include <stddef.h>

_Static_assert(
	V34_PHASE4_E_4_POINT_SYMBOLS * 2 ==
		V34_PHASE4_E_BITS,
	"V.34 four-point E must contain whole symbols");
_Static_assert(
	V34_PHASE4_E_16_POINT_SYMBOLS * 4 ==
		V34_PHASE4_E_BITS,
	"V.34 sixteen-point E must contain whole symbols");

static uint8_t bits_per_symbol(
	enum v34_constellation_size constellation)
{
	return constellation == V34_CONSTELLATION_16_POINT ? 4u :
								    2u;
}

bool v34_phase4_e_sequence_is_valid(
	const struct v34_phase4_e_sequence *sequence)
{
	if (sequence == NULL ||
	    !v34_constellation_size_is_valid(
		    sequence->constellation) ||
	    sequence->previous_clockwise_quarter_turns > 3u ||
	    sequence->emitted_bits > V34_PHASE4_E_BITS ||
	    !v34_transmit_scrambler_is_valid(
		    &sequence->scrambler, sequence->role))
		return false;
	return sequence->emitted_bits % bits_per_symbol(sequence->constellation) ==
	       0;
}

bool v34_phase4_e_sequence_is_complete(
	const struct v34_phase4_e_sequence *sequence)
{
	return v34_phase4_e_sequence_is_valid(sequence) &&
	       sequence->emitted_bits == V34_PHASE4_E_BITS;
}

enum v34_phase4_e_sequence_result
v34_phase4_e_sequence_init_from_mp(
	struct v34_phase4_e_sequence *sequence,
	const struct v34_phase4_mp *mp)
{
	struct v34_phase4_e_sequence initialized;

	if (sequence == NULL ||
	    !v34_phase4_mp_record_is_complete(mp))
		return V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	initialized = (struct v34_phase4_e_sequence){
		.scrambler = mp->scrambler,
		.role = mp->role,
		.constellation = mp->constellation,
		.previous_clockwise_quarter_turns =
			mp->previous_clockwise_quarter_turns,
	};
	if (!v34_phase4_e_sequence_is_valid(&initialized))
		return V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	*sequence = initialized;
	return V34_PHASE4_E_SEQUENCE_OK;
}

enum v34_phase4_e_sequence_result
v34_phase4_e_sequence_next(
	struct v34_phase4_e_sequence *sequence,
	struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns)
{
	struct v34_phase4_e_sequence next;
	struct qam_point next_point;
	uint8_t next_quarter_point = 0;
	uint8_t next_rotation;

	if (!v34_phase4_e_sequence_is_valid(sequence) ||
	    point == NULL || quarter_point == NULL ||
	    clockwise_quarter_turns == NULL)
		return V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	if (sequence->emitted_bits == V34_PHASE4_E_BITS)
		return V34_PHASE4_E_SEQUENCE_COMPLETE;
	next = *sequence;
	if (v34_scramble_differential_dibit(
		    &next.scrambler, 3u,
		    next.previous_clockwise_quarter_turns,
		    &next_rotation) != V34_FRAMING_OK)
		return V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	if (next.constellation == V34_CONSTELLATION_16_POINT &&
	    v34_scramble_mapped_dibit(
		    &next.scrambler, 3u, &next_quarter_point) !=
		    V34_FRAMING_OK)
		return V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	if (v34_training_constellation_point(
		    next.constellation, next_quarter_point, next_rotation,
		    &next_point) != V34_CONSTELLATION_OK)
		return V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	next.previous_clockwise_quarter_turns = next_rotation;
	next.emitted_bits = (uint8_t)(
		next.emitted_bits + bits_per_symbol(next.constellation));
	if (!v34_phase4_e_sequence_is_valid(&next))
		return V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	*sequence = next;
	*point = next_point;
	*quarter_point = next_quarter_point;
	*clockwise_quarter_turns = next_rotation;
	return V34_PHASE4_E_SEQUENCE_OK;
}

const char *v34_phase4_e_sequence_result_name(
	enum v34_phase4_e_sequence_result result)
{
	switch (result) {
	case V34_PHASE4_E_SEQUENCE_OK:
		return "ok";
	case V34_PHASE4_E_SEQUENCE_COMPLETE:
		return "complete";
	case V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
