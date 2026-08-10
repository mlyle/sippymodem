// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_phase3_j_sequence.h"

#include "sippymodem_v34_phase3_trn.h"

#include <stddef.h>

enum {
	V34_PHASE3_J_4_POINT_PATTERN = 0x0991,
	V34_PHASE3_J_16_POINT_PATTERN = 0x0d91,
};

_Static_assert(
	SIPPYMODEM_V34_PHASE3_J_PATTERN_SYMBOLS * 2 ==
		SIPPYMODEM_V34_PHASE3_J_PATTERN_BITS,
	"V.34 J must carry two bits in every 2D symbol");

static bool constellation_is_valid(
	enum sippymodem_v34_constellation_size constellation)
{
	return constellation == SIPPYMODEM_V34_CONSTELLATION_4_POINT ||
	       constellation == SIPPYMODEM_V34_CONSTELLATION_16_POINT;
}

bool sippymodem_v34_phase3_j_sequence_is_valid(
	const struct sippymodem_v34_phase3_j_sequence *sequence)
{
	return sequence != NULL &&
	       constellation_is_valid(sequence->remote_constellation) &&
	       sequence->previous_clockwise_quarter_turns < 4u &&
	       sequence->symbol_in_pattern <
		       SIPPYMODEM_V34_PHASE3_J_PATTERN_SYMBOLS &&
	       sippymodem_v34_transmit_scrambler_is_valid(
		       &sequence->scrambler, sequence->role);
}

enum sippymodem_v34_phase3_j_sequence_result
sippymodem_v34_phase3_j_sequence_init_from_trn(
	struct sippymodem_v34_phase3_j_sequence *sequence,
	const struct sippymodem_v34_phase3_trn *trn,
	enum sippymodem_v34_constellation_size remote_constellation)
{
	struct sippymodem_v34_phase3_j_sequence initialized;

	if (sequence == NULL || !sippymodem_v34_phase3_trn_is_valid(trn) ||
	    !trn->has_last_symbol ||
	    !constellation_is_valid(remote_constellation))
		return SIPPYMODEM_V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT;
	initialized = (struct sippymodem_v34_phase3_j_sequence){
		.scrambler = trn->scrambler,
		.role = trn->role,
		.remote_constellation = remote_constellation,
		.previous_clockwise_quarter_turns =
			trn->last_clockwise_quarter_turns,
	};
	if (!sippymodem_v34_phase3_j_sequence_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT;
	*sequence = initialized;
	return SIPPYMODEM_V34_PHASE3_J_SEQUENCE_OK;
}

bool sippymodem_v34_phase3_j_sequence_at_pattern_boundary(
	const struct sippymodem_v34_phase3_j_sequence *sequence)
{
	return sippymodem_v34_phase3_j_sequence_is_valid(sequence) &&
	       sequence->symbol_in_pattern == 0;
}

static uint8_t input_dibit(
	const struct sippymodem_v34_phase3_j_sequence *sequence)
{
	uint16_t pattern =
		sequence->remote_constellation ==
				SIPPYMODEM_V34_CONSTELLATION_4_POINT ?
			V34_PHASE3_J_4_POINT_PATTERN :
			V34_PHASE3_J_16_POINT_PATTERN;
	unsigned int shift =
		2u * (SIPPYMODEM_V34_PHASE3_J_PATTERN_SYMBOLS - 1u -
		      sequence->symbol_in_pattern);

	/* Table 18 writes the first transmitted bit on the left. */
	return (uint8_t)((pattern >> shift) & 3u);
}

static uint8_t differential_increment(uint8_t transmitted_dibit)
{
	uint8_t i1 = (uint8_t)((transmitted_dibit >> 1) & 1u);
	uint8_t i2 = (uint8_t)(transmitted_dibit & 1u);

	return (uint8_t)((i2 << 1) | i1);
}

enum sippymodem_v34_phase3_j_sequence_result
sippymodem_v34_phase3_j_sequence_next(
	struct sippymodem_v34_phase3_j_sequence *sequence,
	struct sippymodem_qam_point *point,
	uint8_t *clockwise_quarter_turns)
{
	struct sippymodem_v34_phase3_j_sequence next;
	struct sippymodem_qam_point next_point;
	uint8_t scrambled_dibit;
	uint8_t rotation;

	if (!sippymodem_v34_phase3_j_sequence_is_valid(sequence) ||
	    point == NULL || clockwise_quarter_turns == NULL)
		return SIPPYMODEM_V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT;
	next = *sequence;
	if (sippymodem_v34_scramble_group(
		    &next.scrambler, input_dibit(&next), 2u,
		    &scrambled_dibit) != SIPPYMODEM_V34_FRAMING_OK)
		return SIPPYMODEM_V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT;
	rotation = (uint8_t)(
		(next.previous_clockwise_quarter_turns +
		 differential_increment(scrambled_dibit)) &
		3u);
	next.previous_clockwise_quarter_turns = rotation;
	next.symbol_in_pattern++;
	if (next.symbol_in_pattern ==
	    SIPPYMODEM_V34_PHASE3_J_PATTERN_SYMBOLS)
		next.symbol_in_pattern = 0;
	next_point = sippymodem_v34_rotated_point_zero(-(int)rotation);
	if (!sippymodem_v34_phase3_j_sequence_is_valid(&next))
		return SIPPYMODEM_V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT;
	*sequence = next;
	*point = next_point;
	*clockwise_quarter_turns = rotation;
	return SIPPYMODEM_V34_PHASE3_J_SEQUENCE_OK;
}

const char *sippymodem_v34_phase3_j_sequence_result_name(
	enum sippymodem_v34_phase3_j_sequence_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE3_J_SEQUENCE_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
