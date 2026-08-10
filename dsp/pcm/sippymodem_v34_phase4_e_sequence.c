// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_phase4_e_sequence.h"

#include <stddef.h>

_Static_assert(
	SIPPYMODEM_V34_PHASE4_E_4_POINT_SYMBOLS * 2 ==
		SIPPYMODEM_V34_PHASE4_E_BITS,
	"V.34 four-point E must contain whole symbols");
_Static_assert(
	SIPPYMODEM_V34_PHASE4_E_16_POINT_SYMBOLS * 4 ==
		SIPPYMODEM_V34_PHASE4_E_BITS,
	"V.34 sixteen-point E must contain whole symbols");

static uint8_t bits_per_symbol(
	enum sippymodem_v34_constellation_size constellation)
{
	return constellation == SIPPYMODEM_V34_CONSTELLATION_16_POINT ? 4u :
								    2u;
}

bool sippymodem_v34_phase4_e_sequence_is_valid(
	const struct sippymodem_v34_phase4_e_sequence *sequence)
{
	if (sequence == NULL ||
	    !sippymodem_v34_constellation_size_is_valid(
		    sequence->constellation) ||
	    sequence->previous_clockwise_quarter_turns > 3u ||
	    sequence->emitted_bits > SIPPYMODEM_V34_PHASE4_E_BITS ||
	    !sippymodem_v34_transmit_scrambler_is_valid(
		    &sequence->scrambler, sequence->role))
		return false;
	return sequence->emitted_bits % bits_per_symbol(sequence->constellation) ==
	       0;
}

bool sippymodem_v34_phase4_e_sequence_is_complete(
	const struct sippymodem_v34_phase4_e_sequence *sequence)
{
	return sippymodem_v34_phase4_e_sequence_is_valid(sequence) &&
	       sequence->emitted_bits == SIPPYMODEM_V34_PHASE4_E_BITS;
}

enum sippymodem_v34_phase4_e_sequence_result
sippymodem_v34_phase4_e_sequence_init_from_mp(
	struct sippymodem_v34_phase4_e_sequence *sequence,
	const struct sippymodem_v34_phase4_mp *mp)
{
	struct sippymodem_v34_phase4_e_sequence initialized;

	if (sequence == NULL ||
	    !sippymodem_v34_phase4_mp_record_is_complete(mp))
		return SIPPYMODEM_V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	initialized = (struct sippymodem_v34_phase4_e_sequence){
		.scrambler = mp->scrambler,
		.role = mp->role,
		.constellation = mp->constellation,
		.previous_clockwise_quarter_turns =
			mp->previous_clockwise_quarter_turns,
	};
	if (!sippymodem_v34_phase4_e_sequence_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	*sequence = initialized;
	return SIPPYMODEM_V34_PHASE4_E_SEQUENCE_OK;
}

enum sippymodem_v34_phase4_e_sequence_result
sippymodem_v34_phase4_e_sequence_next(
	struct sippymodem_v34_phase4_e_sequence *sequence,
	struct sippymodem_qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns)
{
	struct sippymodem_v34_phase4_e_sequence next;
	struct sippymodem_qam_point next_point;
	uint8_t next_quarter_point = 0;
	uint8_t next_rotation;

	if (!sippymodem_v34_phase4_e_sequence_is_valid(sequence) ||
	    point == NULL || quarter_point == NULL ||
	    clockwise_quarter_turns == NULL)
		return SIPPYMODEM_V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	if (sequence->emitted_bits == SIPPYMODEM_V34_PHASE4_E_BITS)
		return SIPPYMODEM_V34_PHASE4_E_SEQUENCE_COMPLETE;
	next = *sequence;
	if (sippymodem_v34_scramble_differential_dibit(
		    &next.scrambler, 3u,
		    next.previous_clockwise_quarter_turns,
		    &next_rotation) != SIPPYMODEM_V34_FRAMING_OK)
		return SIPPYMODEM_V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	if (next.constellation == SIPPYMODEM_V34_CONSTELLATION_16_POINT &&
	    sippymodem_v34_scramble_mapped_dibit(
		    &next.scrambler, 3u, &next_quarter_point) !=
		    SIPPYMODEM_V34_FRAMING_OK)
		return SIPPYMODEM_V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	if (sippymodem_v34_training_constellation_point(
		    next.constellation, next_quarter_point, next_rotation,
		    &next_point) != SIPPYMODEM_V34_CONSTELLATION_OK)
		return SIPPYMODEM_V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	next.previous_clockwise_quarter_turns = next_rotation;
	next.emitted_bits = (uint8_t)(
		next.emitted_bits + bits_per_symbol(next.constellation));
	if (!sippymodem_v34_phase4_e_sequence_is_valid(&next))
		return SIPPYMODEM_V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT;
	*sequence = next;
	*point = next_point;
	*quarter_point = next_quarter_point;
	*clockwise_quarter_turns = next_rotation;
	return SIPPYMODEM_V34_PHASE4_E_SEQUENCE_OK;
}

const char *sippymodem_v34_phase4_e_sequence_result_name(
	enum sippymodem_v34_phase4_e_sequence_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE4_E_SEQUENCE_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE4_E_SEQUENCE_COMPLETE:
		return "complete";
	case SIPPYMODEM_V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
