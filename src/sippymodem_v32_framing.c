// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v32_framing.h"

#include <stddef.h>

enum {
	V32_CALLING_TAP_DELAY = 18,
	V32_ANSWERING_TAP_DELAY = 5,
	V32_COMMON_TAP_DELAY = 23,
	V32_SCRAMBLER_HISTORY_MASK =
		(UINT32_C(1) << SIPPYMODEM_V32_SCRAMBLER_BITS) - 1,
};

static bool transmitter_role_is_valid(
	enum sippymodem_v32_transmitter_role role)
{
	return role == SIPPYMODEM_V32_CALLING_TRANSMITTER ||
	       role == SIPPYMODEM_V32_ANSWERING_TRANSMITTER;
}

bool sippymodem_v32_scrambler_is_valid(
	const struct sippymodem_v32_scrambler *scrambler)
{
	return scrambler != NULL &&
	       transmitter_role_is_valid(scrambler->transmitter_role) &&
	       (scrambler->history &
		~(uint32_t)V32_SCRAMBLER_HISTORY_MASK) == 0;
}

enum sippymodem_v32_framing_result sippymodem_v32_scrambler_init(
	struct sippymodem_v32_scrambler *scrambler,
	enum sippymodem_v32_transmitter_role transmitter_role)
{
	if (scrambler == NULL || !transmitter_role_is_valid(transmitter_role))
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	scrambler->history = 0;
	scrambler->transmitter_role = transmitter_role;
	return SIPPYMODEM_V32_FRAMING_OK;
}

static unsigned int role_tap_delay(
	enum sippymodem_v32_transmitter_role role)
{
	return role == SIPPYMODEM_V32_CALLING_TRANSMITTER ?
		       V32_CALLING_TAP_DELAY :
		       V32_ANSWERING_TAP_DELAY;
}

static uint8_t feedback_bit(const struct sippymodem_v32_scrambler *state)
{
	unsigned int role_delay = role_tap_delay(state->transmitter_role);

	return (uint8_t)(((state->history >> (role_delay - 1u)) ^
			  (state->history >> (V32_COMMON_TAP_DELAY - 1u))) &
			  1u);
}

static void remember_scrambled_bit(
	struct sippymodem_v32_scrambler *state, uint8_t bit)
{
	state->history = ((state->history << 1) | bit) &
			 V32_SCRAMBLER_HISTORY_MASK;
}

static uint8_t scramble_bit(
	struct sippymodem_v32_scrambler *state, uint8_t input)
{
	uint8_t output = input ^ feedback_bit(state);

	remember_scrambled_bit(state, output);
	return output;
}

static uint8_t descramble_bit(
	struct sippymodem_v32_scrambler *state, uint8_t input)
{
	uint8_t output = input ^ feedback_bit(state);

	remember_scrambled_bit(state, input);
	return output;
}

static enum sippymodem_v32_framing_result transform_dibit(
	struct sippymodem_v32_scrambler *state, uint8_t input,
	uint8_t *output, bool descramble)
{
	struct sippymodem_v32_scrambler next;
	uint8_t first;
	uint8_t second;

	if (!sippymodem_v32_scrambler_is_valid(state) || input >= 4 ||
	    output == NULL)
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	next = *state;
	if (descramble) {
		first = descramble_bit(&next, (uint8_t)(input >> 1));
		second = descramble_bit(&next, (uint8_t)(input & 1u));
	} else {
		first = scramble_bit(&next, (uint8_t)(input >> 1));
		second = scramble_bit(&next, (uint8_t)(input & 1u));
	}
	*state = next;
	*output = (uint8_t)((first << 1) | second);
	return SIPPYMODEM_V32_FRAMING_OK;
}

enum sippymodem_v32_framing_result sippymodem_v32_scramble_dibit(
	struct sippymodem_v32_scrambler *scrambler, uint8_t input,
	uint8_t *output)
{
	return transform_dibit(scrambler, input, output, false);
}

enum sippymodem_v32_framing_result sippymodem_v32_descramble_dibit(
	struct sippymodem_v32_scrambler *descrambler, uint8_t input,
	uint8_t *output)
{
	return transform_dibit(descrambler, input, output, true);
}

enum sippymodem_v32_framing_result sippymodem_v32_symbol_encoder_init(
	struct sippymodem_v32_symbol_encoder *encoder,
	enum sippymodem_v32_transmitter_role transmitter_role,
	uint8_t initial_quadrant)
{
	struct sippymodem_v32_symbol_encoder next = { 0 };

	if (encoder == NULL ||
	    initial_quadrant >= SIPPYMODEM_QPSK_QUADRANT_COUNT ||
	    sippymodem_v32_scrambler_init(&next.scrambler,
					 transmitter_role) !=
		    SIPPYMODEM_V32_FRAMING_OK)
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	next.phase_quadrant = initial_quadrant;
	*encoder = next;
	return SIPPYMODEM_V32_FRAMING_OK;
}

enum sippymodem_v32_framing_result sippymodem_v32_symbol_encode(
	struct sippymodem_v32_symbol_encoder *encoder, uint8_t input,
	uint8_t *quadrant)
{
	struct sippymodem_v32_symbol_encoder next;
	uint8_t scrambled;
	uint8_t next_quadrant;

	if (encoder == NULL || quadrant == NULL ||
	    encoder->phase_quadrant >= SIPPYMODEM_QPSK_QUADRANT_COUNT ||
	    !sippymodem_v32_scrambler_is_valid(&encoder->scrambler))
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	next = *encoder;
	if (sippymodem_v32_scramble_dibit(
		    &next.scrambler, input, &scrambled) !=
		    SIPPYMODEM_V32_FRAMING_OK ||
	    sippymodem_differential_qpsk_encode(
		    next.phase_quadrant, scrambled, &next_quadrant) !=
		    SIPPYMODEM_DIFFERENTIAL_QPSK_OK)
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	next.phase_quadrant = next_quadrant;
	*encoder = next;
	*quadrant = next_quadrant;
	return SIPPYMODEM_V32_FRAMING_OK;
}

enum sippymodem_v32_framing_result sippymodem_v32_symbol_decoder_init(
	struct sippymodem_v32_symbol_decoder *decoder,
	enum sippymodem_v32_transmitter_role source_role,
	uint8_t initial_quadrant)
{
	struct sippymodem_v32_symbol_decoder next = { 0 };

	if (decoder == NULL ||
	    initial_quadrant >= SIPPYMODEM_QPSK_QUADRANT_COUNT ||
	    sippymodem_v32_scrambler_init(&next.descrambler, source_role) !=
		    SIPPYMODEM_V32_FRAMING_OK)
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	next.phase_quadrant = initial_quadrant;
	*decoder = next;
	return SIPPYMODEM_V32_FRAMING_OK;
}

enum sippymodem_v32_framing_result sippymodem_v32_symbol_decode(
	struct sippymodem_v32_symbol_decoder *decoder, uint8_t quadrant,
	uint8_t *output)
{
	struct sippymodem_v32_symbol_decoder next;
	uint8_t decoded;
	uint8_t scrambled;

	if (decoder == NULL || output == NULL ||
	    quadrant >= SIPPYMODEM_QPSK_QUADRANT_COUNT ||
	    decoder->phase_quadrant >= SIPPYMODEM_QPSK_QUADRANT_COUNT ||
	    !sippymodem_v32_scrambler_is_valid(&decoder->descrambler))
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	next = *decoder;
	if (sippymodem_differential_qpsk_decode(
		    next.phase_quadrant, quadrant, &scrambled) !=
		    SIPPYMODEM_DIFFERENTIAL_QPSK_OK ||
	    sippymodem_v32_descramble_dibit(
		    &next.descrambler, scrambled, &decoded) !=
		    SIPPYMODEM_V32_FRAMING_OK)
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	next.phase_quadrant = quadrant;
	*decoder = next;
	*output = decoded;
	return SIPPYMODEM_V32_FRAMING_OK;
}

const char *sippymodem_v32_framing_result_name(
	enum sippymodem_v32_framing_result result)
{
	switch (result) {
	case SIPPYMODEM_V32_FRAMING_OK:
		return "ok";
	case SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
