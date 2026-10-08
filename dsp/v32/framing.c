// SPDX-License-Identifier: BSD-3-Clause

#include "framing.h"

#include "self_sync_scrambler.h"

#include <stddef.h>

static const struct self_sync_scrambler_config *
scrambler_config(enum v32_transmitter_role role)
{
	switch (role) {
	case V32_CALLING_TRANSMITTER: return &self_sync_scrambler_gpc;
	case V32_ANSWERING_TRANSMITTER: return &self_sync_scrambler_gpa;
	}
	return NULL;
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
	if (scrambler == NULL || scrambler_config(transmitter_role) == NULL)
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

/* The typed entry points fix the width and phase coding: absolute TRN
 * phases (V.32 Table 5), or differential 4800/uncoded 9600 (Table 1).
 * Scrambler rejection leaves the owner unchanged; phase coding cannot fail
 * after the owner and scrambled group have been validated. */
static inline enum v32_framing_result encode_symbol(
	struct v32_symbol_encoder *encoder, uint8_t input, uint8_t *output,
	unsigned int bit_count, bool differential)
{
	unsigned int low_bits = bit_count - V32_4800_BITS_PER_SYMBOL;
	uint8_t low_mask = (uint8_t)((1u << low_bits) - 1u);
	uint8_t scrambled;
	uint8_t quadrant;

	if (!symbol_encoder_is_valid(encoder) || output == NULL)
		return V32_FRAMING_INVALID_ARGUMENT;
	if (v32_scramble_group(
		    &encoder->scrambler, input, bit_count, &scrambled) !=
		    V32_FRAMING_OK)
		return V32_FRAMING_INVALID_ARGUMENT;
	quadrant = (uint8_t)(scrambled >> low_bits);
	if (differential) {
		if (differential_qpsk_encode(
			    encoder->phase_quadrant, quadrant, &quadrant) !=
		    DIFFERENTIAL_QPSK_OK)
			return V32_FRAMING_INVALID_ARGUMENT;
	} else {
		quadrant = gray_dibit(quadrant);
	}
	encoder->phase_quadrant = quadrant;
	*output = (uint8_t)((quadrant << low_bits) | (scrambled & low_mask));
	return V32_FRAMING_OK;
}

enum v32_framing_result v32_trn_symbol_encode(
	struct v32_symbol_encoder *encoder, uint8_t input, uint8_t *quadrant)
{
	return encode_symbol(
		encoder, input, quadrant, V32_4800_BITS_PER_SYMBOL, false);
}

enum v32_framing_result v32_4800_symbol_encode(
	struct v32_symbol_encoder *encoder, uint8_t input, uint8_t *quadrant)
{
	return encode_symbol(
		encoder, input, quadrant, V32_4800_BITS_PER_SYMBOL, true);
}

enum v32_framing_result v32_9600_symbol_encode(
	struct v32_symbol_encoder *encoder, uint8_t input, uint8_t *symbol)
{
	return encode_symbol(
		encoder, input, symbol, V32_9600_BITS_PER_SYMBOL, true);
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

static inline enum v32_framing_result decode_symbol(
	struct v32_symbol_decoder *decoder, uint8_t symbol, uint8_t *output,
	unsigned int bit_count, bool differential)
{
	unsigned int low_bits = bit_count - V32_4800_BITS_PER_SYMBOL;
	uint8_t low_mask = (uint8_t)((1u << low_bits) - 1u);
	uint8_t quadrant = (uint8_t)(symbol >> low_bits);
	uint8_t high_dibit;
	uint8_t decoded;
	uint8_t scrambled;

	if (!symbol_decoder_is_valid(decoder) || output == NULL ||
	    symbol >= (1u << bit_count))
		return V32_FRAMING_INVALID_ARGUMENT;
	if (differential) {
		if (differential_qpsk_decode(
			    decoder->phase_quadrant, quadrant, &high_dibit) !=
		    DIFFERENTIAL_QPSK_OK)
			return V32_FRAMING_INVALID_ARGUMENT;
	} else {
		high_dibit = gray_dibit(quadrant);
	}
	scrambled = (uint8_t)((high_dibit << low_bits) | (symbol & low_mask));
	if (v32_descramble_group(
		    &decoder->descrambler, scrambled, bit_count, &decoded) !=
		    V32_FRAMING_OK)
		return V32_FRAMING_INVALID_ARGUMENT;
	decoder->phase_quadrant = quadrant;
	*output = decoded;
	return V32_FRAMING_OK;
}

enum v32_framing_result v32_trn_symbol_decode(
	struct v32_symbol_decoder *decoder, uint8_t quadrant, uint8_t *output)
{
	return decode_symbol(
		decoder, quadrant, output, V32_4800_BITS_PER_SYMBOL, false);
}

enum v32_framing_result v32_4800_symbol_decode(
	struct v32_symbol_decoder *decoder, uint8_t quadrant, uint8_t *output)
{
	return decode_symbol(
		decoder, quadrant, output, V32_4800_BITS_PER_SYMBOL, true);
}

enum v32_framing_result v32_9600_symbol_decode(
	struct v32_symbol_decoder *decoder, uint8_t symbol, uint8_t *output)
{
	return decode_symbol(
		decoder, symbol, output, V32_9600_BITS_PER_SYMBOL, true);
}
