// SPDX-License-Identifier: BSD-3-Clause

#include "v22_framing.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum {
	V22_SCRAMBLER_TAP_14 = 13,
	V22_SCRAMBLER_TAP_17 = 16,
	V22_SCRAMBLER_HISTORY_MASK =
		(UINT32_C(1) << V22_SCRAMBLER_BITS) - 1,
};

bool v22_scrambler_is_valid(
	const struct v22_scrambler *scrambler)
{
	return scrambler != NULL &&
	       (scrambler->history &
		~(uint32_t)V22_SCRAMBLER_HISTORY_MASK) == 0 &&
	       scrambler->consecutive_ones <=
		       V22_SCRAMBLER_ONES_LIMIT;
}

static uint8_t scrambler_feedback(
	const struct v22_scrambler *scrambler)
{
	return (uint8_t)(((scrambler->history >> V22_SCRAMBLER_TAP_14) ^
			  (scrambler->history >> V22_SCRAMBLER_TAP_17)) &
			  1u);
}

static void remember_scrambled_bit(
	struct v22_scrambler *scrambler, uint8_t bit)
{
	scrambler->history =
		((scrambler->history << 1) | bit) &
		V22_SCRAMBLER_HISTORY_MASK;
	if (bit == 0)
		scrambler->consecutive_ones = 0;
	else
		scrambler->consecutive_ones++;
}

void v22_scrambler_reset(
	struct v22_scrambler *scrambler)
{
	if (scrambler != NULL)
		memset(scrambler, 0, sizeof(*scrambler));
}

enum v22_framing_result v22_scramble_bit(
	struct v22_scrambler *scrambler, uint8_t input,
	uint8_t *output)
{
	uint8_t encoded;

	if (!v22_scrambler_is_valid(scrambler) || input > 1 ||
	    output == NULL)
		return V22_FRAMING_INVALID_ARGUMENT;
	if (scrambler->consecutive_ones ==
	    V22_SCRAMBLER_ONES_LIMIT) {
		/* Escape a locked all-mark sequence without changing DTE data. */
		input ^= 1u;
		scrambler->consecutive_ones = 0;
	}
	encoded = input ^ scrambler_feedback(scrambler);
	remember_scrambled_bit(scrambler, encoded);
	*output = encoded;
	return V22_FRAMING_OK;
}

enum v22_framing_result v22_descramble_bit(
	struct v22_scrambler *descrambler, uint8_t input,
	uint8_t *output)
{
	bool repair;
	uint8_t decoded;

	if (!v22_scrambler_is_valid(descrambler) || input > 1 ||
	    output == NULL)
		return V22_FRAMING_INVALID_ARGUMENT;
	repair = descrambler->consecutive_ones ==
		 V22_SCRAMBLER_ONES_LIMIT;
	if (repair)
		descrambler->consecutive_ones = 0;
	decoded = input ^ scrambler_feedback(descrambler);
	remember_scrambled_bit(descrambler, input);
	if (repair)
		decoded ^= 1u;
	*output = decoded;
	return V22_FRAMING_OK;
}

enum v22_framing_result v22_symbol_encoder_init(
	struct v22_symbol_encoder *encoder,
	uint8_t initial_quadrant)
{
	if (encoder == NULL ||
	    initial_quadrant >= QPSK_QUADRANT_COUNT)
		return V22_FRAMING_INVALID_ARGUMENT;
	memset(encoder, 0, sizeof(*encoder));
	encoder->phase_quadrant = initial_quadrant;
	return V22_FRAMING_OK;
}

enum v22_framing_result v22_symbol_encode(
	struct v22_symbol_encoder *encoder, uint8_t first_bit,
	uint8_t second_bit, uint8_t *quadrant)
{
	uint8_t first_scrambled;
	uint8_t second_scrambled;
	uint8_t next_quadrant;

	if (encoder == NULL || first_bit > 1 || second_bit > 1 ||
	    quadrant == NULL ||
	    encoder->phase_quadrant >= QPSK_QUADRANT_COUNT ||
	    !v22_scrambler_is_valid(&encoder->scrambler))
		return V22_FRAMING_INVALID_ARGUMENT;
	if (v22_scramble_bit(
		    &encoder->scrambler, first_bit, &first_scrambled) !=
		    V22_FRAMING_OK ||
	    v22_scramble_bit(
		    &encoder->scrambler, second_bit, &second_scrambled) !=
		    V22_FRAMING_OK ||
	    differential_qpsk_encode(
		    encoder->phase_quadrant,
		    (uint8_t)((first_scrambled << 1) | second_scrambled),
		    &next_quadrant) != DIFFERENTIAL_QPSK_OK)
		return V22_FRAMING_INVALID_ARGUMENT;
	encoder->phase_quadrant = next_quadrant;
	*quadrant = next_quadrant;
	return V22_FRAMING_OK;
}

enum v22_framing_result v22_600_symbol_encode(
	struct v22_symbol_encoder *encoder, uint8_t bit,
	uint8_t *quadrant)
{
	uint8_t scrambled;

	if (encoder == NULL || bit > 1 || quadrant == NULL ||
	    encoder->phase_quadrant >= QPSK_QUADRANT_COUNT ||
	    !v22_scrambler_is_valid(&encoder->scrambler))
		return V22_FRAMING_INVALID_ARGUMENT;
	if (v22_scramble_bit(&encoder->scrambler, bit, &scrambled) !=
	    V22_FRAMING_OK)
		return V22_FRAMING_INVALID_ARGUMENT;
	/* V.22 Table 1: zero advances 90 degrees and one advances 270. */
	encoder->phase_quadrant = (uint8_t)(
		(encoder->phase_quadrant + (scrambled == 0 ? 1u : 3u)) & 3u);
	*quadrant = encoder->phase_quadrant;
	return V22_FRAMING_OK;
}

enum v22_framing_result v22_symbol_decoder_init(
	struct v22_symbol_decoder *decoder,
	uint8_t initial_quadrant)
{
	if (decoder == NULL ||
	    initial_quadrant >= QPSK_QUADRANT_COUNT)
		return V22_FRAMING_INVALID_ARGUMENT;
	memset(decoder, 0, sizeof(*decoder));
	decoder->phase_quadrant = initial_quadrant;
	return V22_FRAMING_OK;
}

enum v22_framing_result v22_symbol_decode(
	struct v22_symbol_decoder *decoder, uint8_t quadrant,
	uint8_t *first_bit, uint8_t *second_bit)
{
	uint8_t dibit;
	uint8_t first_decoded;
	uint8_t second_decoded;

	if (decoder == NULL ||
	    quadrant >= QPSK_QUADRANT_COUNT ||
	    first_bit == NULL || second_bit == NULL ||
	    decoder->phase_quadrant >= QPSK_QUADRANT_COUNT ||
	    !v22_scrambler_is_valid(&decoder->descrambler))
		return V22_FRAMING_INVALID_ARGUMENT;
	if (differential_qpsk_decode(
		    decoder->phase_quadrant, quadrant, &dibit) !=
		    DIFFERENTIAL_QPSK_OK ||
	    v22_descramble_bit(
		    &decoder->descrambler, (uint8_t)(dibit >> 1),
		    &first_decoded) != V22_FRAMING_OK ||
	    v22_descramble_bit(
		    &decoder->descrambler, (uint8_t)(dibit & 1u),
		    &second_decoded) != V22_FRAMING_OK)
		return V22_FRAMING_INVALID_ARGUMENT;
	decoder->phase_quadrant = quadrant;
	*first_bit = first_decoded;
	*second_bit = second_decoded;
	return V22_FRAMING_OK;
}

enum v22_framing_result v22_600_symbol_decode(
	struct v22_symbol_decoder *decoder, uint8_t quadrant,
	uint8_t *bit)
{
	uint8_t decoded;
	uint8_t quarter_turns;
	uint8_t scrambled;

	if (decoder == NULL || quadrant >= QPSK_QUADRANT_COUNT ||
	    bit == NULL ||
	    decoder->phase_quadrant >= QPSK_QUADRANT_COUNT ||
	    !v22_scrambler_is_valid(&decoder->descrambler))
		return V22_FRAMING_INVALID_ARGUMENT;
	quarter_turns = (uint8_t)(
		(quadrant - decoder->phase_quadrant) & 3u);
	if (quarter_turns != 1u && quarter_turns != 3u)
		return V22_FRAMING_INVALID_ARGUMENT;
	scrambled = quarter_turns == 3u ? 1u : 0u;
	if (v22_descramble_bit(
		    &decoder->descrambler, scrambled, &decoded) !=
	    V22_FRAMING_OK)
		return V22_FRAMING_INVALID_ARGUMENT;
	decoder->phase_quadrant = quadrant;
	*bit = decoded;
	return V22_FRAMING_OK;
}

void v22bis_rate_signal_detector_reset(
	struct v22bis_rate_signal_detector *detector)
{
	if (detector != NULL)
		memset(detector, 0, sizeof(*detector));
}

bool v22bis_rate_signal_detector_is_valid(
	const struct v22bis_rate_signal_detector *detector)
{
	return detector != NULL &&
	       detector->alternating_symbols <=
		       V22BIS_ALTERNATING_RUN_MAX_SYMBOLS &&
	       detector->previous_turn < QPSK_QUADRANT_COUNT &&
	       (detector->present ==
		(detector->alternating_symbols >=
		 V22BIS_RATE_SIGNAL_MIN_SYMBOLS));
}

enum v22_framing_result v22bis_rate_signal_turn(
	uint32_t symbol_index, uint8_t *quarter_turns)
{
	if (symbol_index >= V22BIS_RATE_SIGNAL_SYMBOLS ||
	    quarter_turns == NULL)
		return V22_FRAMING_INVALID_ARGUMENT;
	/* Unscrambled dibits 00 and 11 rotate by 90 and 270 degrees. */
	*quarter_turns = (symbol_index & 1u) == 0 ? 1u : 3u;
	return V22_FRAMING_OK;
}

static bool is_rate_signal_turn(uint8_t quarter_turns)
{
	return quarter_turns == 1u || quarter_turns == 3u;
}

enum v22_framing_result v22bis_rate_signal_observe(
	struct v22bis_rate_signal_detector *detector,
	uint8_t quarter_turns,
	enum v22bis_rate_signal_event *event)
{
	enum v22bis_rate_signal_event next_event =
		V22BIS_RATE_SIGNAL_NONE;
	bool alternating;

	if (!v22bis_rate_signal_detector_is_valid(detector) ||
	    quarter_turns >= QPSK_QUADRANT_COUNT || event == NULL)
		return V22_FRAMING_INVALID_ARGUMENT;
	alternating = is_rate_signal_turn(quarter_turns) &&
		(detector->alternating_symbols == 0 ||
		 quarter_turns != detector->previous_turn);
	if (!alternating) {
		if (detector->present)
			next_event = detector->alternating_symbols >=
				V22BIS_RATE_SIGNAL_COMPLETE_SYMBOLS ?
					V22BIS_RATE_SIGNAL_FULLY_ENDED :
					V22BIS_RATE_SIGNAL_ENDED;
		detector->present = false;
		detector->alternating_symbols =
			is_rate_signal_turn(quarter_turns) ? 1u : 0u;
	} else if (detector->alternating_symbols <
		   V22BIS_ALTERNATING_RUN_MAX_SYMBOLS) {
		detector->alternating_symbols++;
		if (detector->alternating_symbols ==
		    V22BIS_RATE_SIGNAL_MIN_SYMBOLS) {
			detector->present = true;
			next_event = V22BIS_RATE_SIGNAL_STARTED;
		}
	}
	detector->previous_turn = quarter_turns;
	*event = next_event;
	return V22_FRAMING_OK;
}

const char *v22_framing_result_name(
	enum v22_framing_result result)
{
	switch (result) {
	case V22_FRAMING_OK:
		return "ok";
	case V22_FRAMING_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
