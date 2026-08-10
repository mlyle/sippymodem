// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v22_framing.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum {
	V22_SCRAMBLER_TAP_14 = 13,
	V22_SCRAMBLER_TAP_17 = 16,
	V22_SCRAMBLER_HISTORY_MASK =
		(UINT32_C(1) << SIPPYMODEM_V22_SCRAMBLER_BITS) - 1,
};

bool sippymodem_v22_scrambler_is_valid(
	const struct sippymodem_v22_scrambler *scrambler)
{
	return scrambler != NULL &&
	       (scrambler->history &
		~(uint32_t)V22_SCRAMBLER_HISTORY_MASK) == 0 &&
	       scrambler->consecutive_ones <=
		       SIPPYMODEM_V22_SCRAMBLER_ONES_LIMIT;
}

static uint8_t scrambler_feedback(
	const struct sippymodem_v22_scrambler *scrambler)
{
	return (uint8_t)(((scrambler->history >> V22_SCRAMBLER_TAP_14) ^
			  (scrambler->history >> V22_SCRAMBLER_TAP_17)) &
			  1u);
}

static void remember_scrambled_bit(
	struct sippymodem_v22_scrambler *scrambler, uint8_t bit)
{
	scrambler->history =
		((scrambler->history << 1) | bit) &
		V22_SCRAMBLER_HISTORY_MASK;
	if (bit == 0)
		scrambler->consecutive_ones = 0;
	else
		scrambler->consecutive_ones++;
}

void sippymodem_v22_scrambler_reset(
	struct sippymodem_v22_scrambler *scrambler)
{
	if (scrambler != NULL)
		memset(scrambler, 0, sizeof(*scrambler));
}

enum sippymodem_v22_framing_result sippymodem_v22_scramble_bit(
	struct sippymodem_v22_scrambler *scrambler, uint8_t input,
	uint8_t *output)
{
	uint8_t encoded;

	if (!sippymodem_v22_scrambler_is_valid(scrambler) || input > 1 ||
	    output == NULL)
		return SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT;
	if (scrambler->consecutive_ones ==
	    SIPPYMODEM_V22_SCRAMBLER_ONES_LIMIT) {
		/* Escape a locked all-mark sequence without changing DTE data. */
		input ^= 1u;
		scrambler->consecutive_ones = 0;
	}
	encoded = input ^ scrambler_feedback(scrambler);
	remember_scrambled_bit(scrambler, encoded);
	*output = encoded;
	return SIPPYMODEM_V22_FRAMING_OK;
}

enum sippymodem_v22_framing_result sippymodem_v22_descramble_bit(
	struct sippymodem_v22_scrambler *descrambler, uint8_t input,
	uint8_t *output)
{
	bool repair;
	uint8_t decoded;

	if (!sippymodem_v22_scrambler_is_valid(descrambler) || input > 1 ||
	    output == NULL)
		return SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT;
	repair = descrambler->consecutive_ones ==
		 SIPPYMODEM_V22_SCRAMBLER_ONES_LIMIT;
	if (repair)
		descrambler->consecutive_ones = 0;
	decoded = input ^ scrambler_feedback(descrambler);
	remember_scrambled_bit(descrambler, input);
	if (repair)
		decoded ^= 1u;
	*output = decoded;
	return SIPPYMODEM_V22_FRAMING_OK;
}

enum sippymodem_v22_framing_result sippymodem_v22_differential_encode(
	uint8_t previous_quadrant, uint8_t dibit, uint8_t *quadrant)
{
	/* 00, 01, 10, 11 advance by 90, 0, 180, 270 degrees. */
	static const uint8_t quarter_turns[SIPPYMODEM_V22_PHASE_QUADRANTS] = {
		1, 0, 2, 3,
	};

	if (previous_quadrant >= SIPPYMODEM_V22_PHASE_QUADRANTS ||
	    dibit >= SIPPYMODEM_V22_PHASE_QUADRANTS || quadrant == NULL)
		return SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT;
	*quadrant = (uint8_t)((previous_quadrant + quarter_turns[dibit]) & 3u);
	return SIPPYMODEM_V22_FRAMING_OK;
}

enum sippymodem_v22_framing_result sippymodem_v22_differential_decode(
	uint8_t previous_quadrant, uint8_t quadrant, uint8_t *dibit)
{
	/* The mandated dibit-to-quarter-turn permutation is self-inverse. */
	static const uint8_t dibits[SIPPYMODEM_V22_PHASE_QUADRANTS] = {
		1, 0, 2, 3,
	};
	uint8_t quarter_turns;

	if (previous_quadrant >= SIPPYMODEM_V22_PHASE_QUADRANTS ||
	    quadrant >= SIPPYMODEM_V22_PHASE_QUADRANTS || dibit == NULL)
		return SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT;
	quarter_turns = (uint8_t)((quadrant - previous_quadrant) & 3u);
	*dibit = dibits[quarter_turns];
	return SIPPYMODEM_V22_FRAMING_OK;
}

enum sippymodem_v22_framing_result sippymodem_v22_symbol_encoder_init(
	struct sippymodem_v22_symbol_encoder *encoder,
	uint8_t initial_quadrant)
{
	if (encoder == NULL ||
	    initial_quadrant >= SIPPYMODEM_V22_PHASE_QUADRANTS)
		return SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT;
	memset(encoder, 0, sizeof(*encoder));
	encoder->phase_quadrant = initial_quadrant;
	return SIPPYMODEM_V22_FRAMING_OK;
}

enum sippymodem_v22_framing_result sippymodem_v22_symbol_encode(
	struct sippymodem_v22_symbol_encoder *encoder, uint8_t first_bit,
	uint8_t second_bit, uint8_t *quadrant)
{
	uint8_t first_scrambled;
	uint8_t second_scrambled;
	uint8_t next_quadrant;

	if (encoder == NULL || first_bit > 1 || second_bit > 1 ||
	    quadrant == NULL ||
	    encoder->phase_quadrant >= SIPPYMODEM_V22_PHASE_QUADRANTS ||
	    !sippymodem_v22_scrambler_is_valid(&encoder->scrambler))
		return SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT;
	if (sippymodem_v22_scramble_bit(
		    &encoder->scrambler, first_bit, &first_scrambled) !=
		    SIPPYMODEM_V22_FRAMING_OK ||
	    sippymodem_v22_scramble_bit(
		    &encoder->scrambler, second_bit, &second_scrambled) !=
		    SIPPYMODEM_V22_FRAMING_OK ||
	    sippymodem_v22_differential_encode(
		    encoder->phase_quadrant,
		    (uint8_t)((first_scrambled << 1) | second_scrambled),
		    &next_quadrant) != SIPPYMODEM_V22_FRAMING_OK)
		return SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT;
	encoder->phase_quadrant = next_quadrant;
	*quadrant = next_quadrant;
	return SIPPYMODEM_V22_FRAMING_OK;
}

enum sippymodem_v22_framing_result sippymodem_v22_symbol_decoder_init(
	struct sippymodem_v22_symbol_decoder *decoder,
	uint8_t initial_quadrant)
{
	if (decoder == NULL ||
	    initial_quadrant >= SIPPYMODEM_V22_PHASE_QUADRANTS)
		return SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT;
	memset(decoder, 0, sizeof(*decoder));
	decoder->phase_quadrant = initial_quadrant;
	return SIPPYMODEM_V22_FRAMING_OK;
}

enum sippymodem_v22_framing_result sippymodem_v22_symbol_decode(
	struct sippymodem_v22_symbol_decoder *decoder, uint8_t quadrant,
	uint8_t *first_bit, uint8_t *second_bit)
{
	uint8_t dibit;
	uint8_t first_decoded;
	uint8_t second_decoded;

	if (decoder == NULL ||
	    quadrant >= SIPPYMODEM_V22_PHASE_QUADRANTS ||
	    first_bit == NULL || second_bit == NULL ||
	    decoder->phase_quadrant >= SIPPYMODEM_V22_PHASE_QUADRANTS ||
	    !sippymodem_v22_scrambler_is_valid(&decoder->descrambler))
		return SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT;
	if (sippymodem_v22_differential_decode(
		    decoder->phase_quadrant, quadrant, &dibit) !=
		    SIPPYMODEM_V22_FRAMING_OK ||
	    sippymodem_v22_descramble_bit(
		    &decoder->descrambler, (uint8_t)(dibit >> 1),
		    &first_decoded) != SIPPYMODEM_V22_FRAMING_OK ||
	    sippymodem_v22_descramble_bit(
		    &decoder->descrambler, (uint8_t)(dibit & 1u),
		    &second_decoded) != SIPPYMODEM_V22_FRAMING_OK)
		return SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT;
	decoder->phase_quadrant = quadrant;
	*first_bit = first_decoded;
	*second_bit = second_decoded;
	return SIPPYMODEM_V22_FRAMING_OK;
}

const char *sippymodem_v22_framing_result_name(
	enum sippymodem_v22_framing_result result)
{
	switch (result) {
	case SIPPYMODEM_V22_FRAMING_OK:
		return "ok";
	case SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
