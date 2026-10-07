// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_e_sequence.h"

#include "v34_training_symbol.h"
#include "memory_region.h"

#include <stddef.h>

_Static_assert(
	V34_PHASE4_E_4_POINT_SYMBOLS * 2 ==
		V34_PHASE4_E_BITS,
	"V.34 four-point E must contain whole symbols");
_Static_assert(
	V34_PHASE4_E_16_POINT_SYMBOLS * 4 ==
		V34_PHASE4_E_BITS,
	"V.34 sixteen-point E must contain whole symbols");

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
	return sequence->emitted_bits %
		       v34_constellation_bits_per_symbol(sequence->constellation) ==
	       0;
}

bool v34_phase4_e_sequence_is_complete(
	const struct v34_phase4_e_sequence *sequence)
{
	return v34_phase4_e_sequence_is_valid(sequence) &&
	       sequence->emitted_bits == V34_PHASE4_E_BITS;
}

enum v34_phase4_e_sequence_result
v34_phase4_e_sequence_init(
	struct v34_phase4_e_sequence *sequence,
	const struct v34_scrambler *scrambler,
	enum v34_station_role role,
	enum v34_constellation_size constellation,
	uint8_t previous_clockwise_quarter_turns)
{
	struct v34_phase4_e_sequence initialized;

	if (sequence == NULL || scrambler == NULL ||
	    !v34_constellation_size_is_valid(constellation) ||
	    previous_clockwise_quarter_turns > 3u ||
	    !v34_transmit_scrambler_is_valid(scrambler, role))
		return V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	initialized = (struct v34_phase4_e_sequence){
		.scrambler = *scrambler,
		.role = role,
		.constellation = constellation,
		.previous_clockwise_quarter_turns =
			previous_clockwise_quarter_turns,
	};
	if (!v34_phase4_e_sequence_is_valid(&initialized))
		return V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	*sequence = initialized;
	return V34_PHASE4_E_SEQUENCE_OK;
}

enum v34_phase4_e_sequence_result
v34_phase4_e_sequence_init_from_mp(
	struct v34_phase4_e_sequence *sequence,
	const struct v34_phase4_mp *mp)
{
	if (!v34_phase4_mp_record_is_complete(mp))
		return V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	return v34_phase4_e_sequence_init(
		sequence, &mp->scrambler, mp->role,
		mp->constellation,
		mp->previous_clockwise_quarter_turns);
}

enum v34_phase4_e_sequence_result
v34_phase4_e_sequence_next(
	struct v34_phase4_e_sequence *sequence,
	struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns)
{
	struct v34_phase4_e_sequence next;
	struct qam_point next_point;
	uint8_t next_quarter_point;

	const struct memory_region regions[] = {
		{ sequence, sizeof(*sequence) }, { point, sizeof(*point) },
		{ quarter_point, sizeof(*quarter_point) },
		{ clockwise_quarter_turns, sizeof(*clockwise_quarter_turns) },
	};

	if (!v34_phase4_e_sequence_is_valid(sequence) ||
	    point == NULL || quarter_point == NULL ||
	    clockwise_quarter_turns == NULL ||
	    memory_region_set_overlaps(regions, sizeof(regions) / sizeof(regions[0])))
		return V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	if (sequence->emitted_bits == V34_PHASE4_E_BITS)
		return V34_PHASE4_E_SEQUENCE_COMPLETE;
	next = *sequence;
	if (!v34_training_encode_symbol(
		    &next.scrambler, next.constellation, 3u, 3u,
		    &next.previous_clockwise_quarter_turns,
		    &next_point, &next_quarter_point))
		return V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	next.emitted_bits = (uint8_t)(
		next.emitted_bits +
		v34_constellation_bits_per_symbol(next.constellation));
	if (!v34_phase4_e_sequence_is_valid(&next))
		return V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	*sequence = next;
	*point = next_point;
	*quarter_point = next_quarter_point;
	*clockwise_quarter_turns = next.previous_clockwise_quarter_turns;
	return V34_PHASE4_E_SEQUENCE_OK;
}
