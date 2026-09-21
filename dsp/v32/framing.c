// SPDX-License-Identifier: BSD-3-Clause

#include "framing.h"

#include "self_sync_scrambler.h"

#include <stddef.h>

enum {
	V32_CALLING_TAP_DELAY = 18,
	V32_ANSWERING_TAP_DELAY = 5,
	V32_COMMON_TAP_DELAY = 23,
};

static const struct self_sync_scrambler_config
	v32_scrambler_configs[] = {
		[V32_CALLING_TRANSMITTER] = {
			.history_bits = V32_SCRAMBLER_BITS,
			.tap_a_delay = V32_CALLING_TAP_DELAY,
			.tap_b_delay = V32_COMMON_TAP_DELAY,
		},
		[V32_ANSWERING_TRANSMITTER] = {
			.history_bits = V32_SCRAMBLER_BITS,
			.tap_a_delay = V32_ANSWERING_TAP_DELAY,
			.tap_b_delay = V32_COMMON_TAP_DELAY,
		},
	};

static bool transmitter_role_is_valid(
	enum v32_transmitter_role role)
{
	return role == V32_CALLING_TRANSMITTER ||
	       role == V32_ANSWERING_TRANSMITTER;
}

static const struct self_sync_scrambler_config *
scrambler_config(enum v32_transmitter_role role)
{
	if (!transmitter_role_is_valid(role))
		return NULL;
	return &v32_scrambler_configs[(unsigned int)role];
}

bool v32_scrambler_is_valid(
	const struct v32_scrambler *scrambler)
{
	struct self_sync_scrambler shared;

	if (scrambler == NULL)
		return false;
	shared.history = scrambler->history;
	return self_sync_scrambler_is_valid(
		&shared, scrambler_config(scrambler->transmitter_role));
}

enum v32_framing_result v32_scrambler_init(
	struct v32_scrambler *scrambler,
	enum v32_transmitter_role transmitter_role)
{
	if (scrambler == NULL || !transmitter_role_is_valid(transmitter_role))
		return V32_FRAMING_INVALID_ARGUMENT;
	scrambler->history = 0;
	scrambler->transmitter_role = transmitter_role;
	return V32_FRAMING_OK;
}

static enum v32_framing_result transform_group(
	struct v32_scrambler *state, uint8_t input,
	unsigned int bit_count, uint8_t *output, bool descramble)
{
	struct self_sync_scrambler shared;
	const struct self_sync_scrambler_config *config;
	enum self_sync_scrambler_result result;
	uint8_t transformed;

	if (state == NULL || output == NULL)
		return V32_FRAMING_INVALID_ARGUMENT;
	shared.history = state->history;
	config = scrambler_config(state->transmitter_role);
	if (descramble)
		result = self_sync_descramble_group(
			&shared, config, input, bit_count, &transformed);
	else
		result = self_sync_scramble_group(
			&shared, config, input, bit_count, &transformed);
	if (result != SELF_SYNC_SCRAMBLER_OK)
		return V32_FRAMING_INVALID_ARGUMENT;
	state->history = shared.history;
	*output = transformed;
	return V32_FRAMING_OK;
}

enum v32_framing_result v32_scramble_dibit(
	struct v32_scrambler *scrambler, uint8_t input,
	uint8_t *output)
{
	return v32_scramble_group(
		scrambler, input, V32_4800_BITS_PER_SYMBOL,
		output);
}

enum v32_framing_result v32_descramble_dibit(
	struct v32_scrambler *descrambler, uint8_t input,
	uint8_t *output)
{
	return v32_descramble_group(
		descrambler, input, V32_4800_BITS_PER_SYMBOL,
		output);
}

enum v32_framing_result v32_scramble_group(
	struct v32_scrambler *scrambler, uint8_t input,
	unsigned int bit_count, uint8_t *output)
{
	return transform_group(scrambler, input, bit_count, output, false);
}

enum v32_framing_result v32_descramble_group(
	struct v32_scrambler *descrambler, uint8_t input,
	unsigned int bit_count, uint8_t *output)
{
	return transform_group(descrambler, input, bit_count, output, true);
}

static bool symbol_encoder_is_valid(
	const struct v32_symbol_encoder *encoder)
{
	return encoder != NULL &&
	       encoder->phase_quadrant < QPSK_QUADRANT_COUNT &&
	       v32_scrambler_is_valid(&encoder->scrambler);
}

static bool symbol_decoder_is_valid(
	const struct v32_symbol_decoder *decoder)
{
	return decoder != NULL &&
	       decoder->phase_quadrant < QPSK_QUADRANT_COUNT &&
	       v32_scrambler_is_valid(&decoder->descrambler);
}

enum v32_framing_result v32_symbol_encoder_init(
	struct v32_symbol_encoder *encoder,
	enum v32_transmitter_role transmitter_role,
	uint8_t initial_quadrant)
{
	struct v32_symbol_encoder next = { 0 };

	if (encoder == NULL ||
	    initial_quadrant >= QPSK_QUADRANT_COUNT ||
	    v32_scrambler_init(&next.scrambler,
					 transmitter_role) !=
		    V32_FRAMING_OK)
		return V32_FRAMING_INVALID_ARGUMENT;
	next.phase_quadrant = initial_quadrant;
	*encoder = next;
	return V32_FRAMING_OK;
}

static uint8_t gray_dibit(uint8_t dibit)
{
	/* The two-bit Gray transform exchanges 2 and 3 and is self-inverse. */
	return (uint8_t)(dibit ^ (dibit >> 1));
}

enum v32_framing_result v32_trn_symbol_encode(
	struct v32_symbol_encoder *encoder, uint8_t input,
	uint8_t *quadrant)
{
	struct v32_symbol_encoder next;
	uint8_t scrambled;
	uint8_t next_quadrant;

	if (!symbol_encoder_is_valid(encoder) || quadrant == NULL)
		return V32_FRAMING_INVALID_ARGUMENT;
	next = *encoder;
	if (v32_scramble_dibit(
		    &next.scrambler, input, &scrambled) !=
	    V32_FRAMING_OK)
		return V32_FRAMING_INVALID_ARGUMENT;
	next_quadrant = gray_dibit(scrambled);
	next.phase_quadrant = next_quadrant;
	*encoder = next;
	*quadrant = next_quadrant;
	return V32_FRAMING_OK;
}

enum v32_framing_result v32_4800_symbol_encode(
	struct v32_symbol_encoder *encoder, uint8_t input,
	uint8_t *quadrant)
{
	struct v32_symbol_encoder next;
	uint8_t scrambled;
	uint8_t next_quadrant;

	if (!symbol_encoder_is_valid(encoder) || quadrant == NULL)
		return V32_FRAMING_INVALID_ARGUMENT;
	next = *encoder;
	if (v32_scramble_dibit(
		    &next.scrambler, input, &scrambled) !=
		    V32_FRAMING_OK ||
	    differential_qpsk_encode(
		    next.phase_quadrant, scrambled, &next_quadrant) !=
		    DIFFERENTIAL_QPSK_OK)
		return V32_FRAMING_INVALID_ARGUMENT;
	next.phase_quadrant = next_quadrant;
	*encoder = next;
	*quadrant = next_quadrant;
	return V32_FRAMING_OK;
}

enum v32_framing_result v32_9600_symbol_encode(
	struct v32_symbol_encoder *encoder, uint8_t input,
	uint8_t *symbol)
{
	struct v32_symbol_encoder next;
	uint8_t next_quadrant;
	uint8_t scrambled;

	if (!symbol_encoder_is_valid(encoder) || symbol == NULL)
		return V32_FRAMING_INVALID_ARGUMENT;
	next = *encoder;
	if (v32_scramble_group(
		    &next.scrambler, input,
		    V32_9600_BITS_PER_SYMBOL, &scrambled) !=
		    V32_FRAMING_OK ||
	    differential_qpsk_encode(
		    next.phase_quadrant, (uint8_t)(scrambled >> 2),
		    &next_quadrant) != DIFFERENTIAL_QPSK_OK)
		return V32_FRAMING_INVALID_ARGUMENT;
	next.phase_quadrant = next_quadrant;
	*encoder = next;
	*symbol = (uint8_t)((next_quadrant << 2) | (scrambled & 3u));
	return V32_FRAMING_OK;
}

enum v32_framing_result v32_symbol_decoder_init(
	struct v32_symbol_decoder *decoder,
	enum v32_transmitter_role source_role,
	uint8_t initial_quadrant)
{
	struct v32_symbol_decoder next = { 0 };

	if (decoder == NULL ||
	    initial_quadrant >= QPSK_QUADRANT_COUNT ||
	    v32_scrambler_init(&next.descrambler, source_role) !=
		    V32_FRAMING_OK)
		return V32_FRAMING_INVALID_ARGUMENT;
	next.phase_quadrant = initial_quadrant;
	*decoder = next;
	return V32_FRAMING_OK;
}

enum v32_framing_result v32_trn_symbol_decode(
	struct v32_symbol_decoder *decoder, uint8_t quadrant,
	uint8_t *output)
{
	struct v32_symbol_decoder next;
	uint8_t decoded;
	uint8_t scrambled;

	if (!symbol_decoder_is_valid(decoder) || output == NULL ||
	    quadrant >= QPSK_QUADRANT_COUNT)
		return V32_FRAMING_INVALID_ARGUMENT;
	next = *decoder;
	scrambled = gray_dibit(quadrant);
	if (v32_descramble_dibit(
		    &next.descrambler, scrambled, &decoded) !=
	    V32_FRAMING_OK)
		return V32_FRAMING_INVALID_ARGUMENT;
	next.phase_quadrant = quadrant;
	*decoder = next;
	*output = decoded;
	return V32_FRAMING_OK;
}

enum v32_framing_result v32_4800_symbol_decode(
	struct v32_symbol_decoder *decoder, uint8_t quadrant,
	uint8_t *output)
{
	struct v32_symbol_decoder next;
	uint8_t decoded;
	uint8_t scrambled;

	if (!symbol_decoder_is_valid(decoder) || output == NULL ||
	    quadrant >= QPSK_QUADRANT_COUNT)
		return V32_FRAMING_INVALID_ARGUMENT;
	next = *decoder;
	if (differential_qpsk_decode(
		    next.phase_quadrant, quadrant, &scrambled) !=
		    DIFFERENTIAL_QPSK_OK ||
	    v32_descramble_dibit(
		    &next.descrambler, scrambled, &decoded) !=
		    V32_FRAMING_OK)
		return V32_FRAMING_INVALID_ARGUMENT;
	next.phase_quadrant = quadrant;
	*decoder = next;
	*output = decoded;
	return V32_FRAMING_OK;
}

enum v32_framing_result v32_9600_symbol_decode(
	struct v32_symbol_decoder *decoder, uint8_t symbol,
	uint8_t *output)
{
	struct v32_symbol_decoder next;
	uint8_t decoded;
	uint8_t high_dibit;
	uint8_t quadrant;
	uint8_t scrambled;

	if (!symbol_decoder_is_valid(decoder) || output == NULL ||
	    symbol >= V32_9600_SYMBOL_COUNT)
		return V32_FRAMING_INVALID_ARGUMENT;
	next = *decoder;
	quadrant = (uint8_t)(symbol >> 2);
	if (differential_qpsk_decode(
		    next.phase_quadrant, quadrant, &high_dibit) !=
		    DIFFERENTIAL_QPSK_OK)
		return V32_FRAMING_INVALID_ARGUMENT;
	scrambled = (uint8_t)((high_dibit << 2) | (symbol & 3u));
	if (v32_descramble_group(
		    &next.descrambler, scrambled,
		    V32_9600_BITS_PER_SYMBOL, &decoded) !=
	    V32_FRAMING_OK)
		return V32_FRAMING_INVALID_ARGUMENT;
	next.phase_quadrant = quadrant;
	*decoder = next;
	*output = decoded;
	return V32_FRAMING_OK;
}
