// SPDX-License-Identifier: BSD-3-Clause

#include "framing.h"

#include "self_sync_scrambler.h"

#include <stddef.h>

enum {
	V32_CALLING_TAP_DELAY = 18,
	V32_ANSWERING_TAP_DELAY = 5,
	V32_COMMON_TAP_DELAY = 23,
};

static const struct sippymodem_self_sync_scrambler_config
	v32_scrambler_configs[] = {
		[SIPPYMODEM_V32_CALLING_TRANSMITTER] = {
			.history_bits = SIPPYMODEM_V32_SCRAMBLER_BITS,
			.tap_a_delay = V32_CALLING_TAP_DELAY,
			.tap_b_delay = V32_COMMON_TAP_DELAY,
		},
		[SIPPYMODEM_V32_ANSWERING_TRANSMITTER] = {
			.history_bits = SIPPYMODEM_V32_SCRAMBLER_BITS,
			.tap_a_delay = V32_ANSWERING_TAP_DELAY,
			.tap_b_delay = V32_COMMON_TAP_DELAY,
		},
	};

static bool transmitter_role_is_valid(
	enum sippymodem_v32_transmitter_role role)
{
	return role == SIPPYMODEM_V32_CALLING_TRANSMITTER ||
	       role == SIPPYMODEM_V32_ANSWERING_TRANSMITTER;
}

static const struct sippymodem_self_sync_scrambler_config *
scrambler_config(enum sippymodem_v32_transmitter_role role)
{
	if (!transmitter_role_is_valid(role))
		return NULL;
	return &v32_scrambler_configs[(unsigned int)role];
}

bool sippymodem_v32_scrambler_is_valid(
	const struct sippymodem_v32_scrambler *scrambler)
{
	struct sippymodem_self_sync_scrambler shared;

	if (scrambler == NULL)
		return false;
	shared.history = scrambler->history;
	return sippymodem_self_sync_scrambler_is_valid(
		&shared, scrambler_config(scrambler->transmitter_role));
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

static enum sippymodem_v32_framing_result transform_group(
	struct sippymodem_v32_scrambler *state, uint8_t input,
	unsigned int bit_count, uint8_t *output, bool descramble)
{
	struct sippymodem_v32_scrambler next;
	struct sippymodem_self_sync_scrambler shared;
	const struct sippymodem_self_sync_scrambler_config *config;
	enum sippymodem_self_sync_scrambler_result result;
	uint8_t transformed;

	if (!sippymodem_v32_scrambler_is_valid(state) || output == NULL)
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	next = *state;
	shared.history = next.history;
	config = scrambler_config(next.transmitter_role);
	if (descramble)
		result = sippymodem_self_sync_descramble_group(
			&shared, config, input, bit_count, &transformed);
	else
		result = sippymodem_self_sync_scramble_group(
			&shared, config, input, bit_count, &transformed);
	if (result != SIPPYMODEM_SELF_SYNC_SCRAMBLER_OK)
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	next.history = shared.history;
	*state = next;
	*output = transformed;
	return SIPPYMODEM_V32_FRAMING_OK;
}

enum sippymodem_v32_framing_result sippymodem_v32_scramble_dibit(
	struct sippymodem_v32_scrambler *scrambler, uint8_t input,
	uint8_t *output)
{
	return sippymodem_v32_scramble_group(
		scrambler, input, SIPPYMODEM_V32_4800_BITS_PER_SYMBOL,
		output);
}

enum sippymodem_v32_framing_result sippymodem_v32_descramble_dibit(
	struct sippymodem_v32_scrambler *descrambler, uint8_t input,
	uint8_t *output)
{
	return sippymodem_v32_descramble_group(
		descrambler, input, SIPPYMODEM_V32_4800_BITS_PER_SYMBOL,
		output);
}

enum sippymodem_v32_framing_result sippymodem_v32_scramble_group(
	struct sippymodem_v32_scrambler *scrambler, uint8_t input,
	unsigned int bit_count, uint8_t *output)
{
	return transform_group(scrambler, input, bit_count, output, false);
}

enum sippymodem_v32_framing_result sippymodem_v32_descramble_group(
	struct sippymodem_v32_scrambler *descrambler, uint8_t input,
	unsigned int bit_count, uint8_t *output)
{
	return transform_group(descrambler, input, bit_count, output, true);
}

static bool symbol_encoder_is_valid(
	const struct sippymodem_v32_symbol_encoder *encoder)
{
	return encoder != NULL &&
	       encoder->phase_quadrant < SIPPYMODEM_QPSK_QUADRANT_COUNT &&
	       sippymodem_v32_scrambler_is_valid(&encoder->scrambler);
}

static bool symbol_decoder_is_valid(
	const struct sippymodem_v32_symbol_decoder *decoder)
{
	return decoder != NULL &&
	       decoder->phase_quadrant < SIPPYMODEM_QPSK_QUADRANT_COUNT &&
	       sippymodem_v32_scrambler_is_valid(&decoder->descrambler);
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

static uint8_t gray_dibit(uint8_t dibit)
{
	/* The two-bit Gray transform exchanges 2 and 3 and is self-inverse. */
	return (uint8_t)(dibit ^ (dibit >> 1));
}

enum sippymodem_v32_framing_result
sippymodem_v32_phase_to_differential_output(
	uint8_t phase_quadrant, uint8_t *differential_output)
{
	if (phase_quadrant >= SIPPYMODEM_QPSK_QUADRANT_COUNT ||
	    differential_output == NULL)
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	*differential_output = gray_dibit(phase_quadrant);
	return SIPPYMODEM_V32_FRAMING_OK;
}

enum sippymodem_v32_framing_result sippymodem_v32_trn_symbol_encode(
	struct sippymodem_v32_symbol_encoder *encoder, uint8_t input,
	uint8_t *quadrant)
{
	struct sippymodem_v32_symbol_encoder next;
	uint8_t scrambled;
	uint8_t next_quadrant;

	if (!symbol_encoder_is_valid(encoder) || quadrant == NULL)
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	next = *encoder;
	if (sippymodem_v32_scramble_dibit(
		    &next.scrambler, input, &scrambled) !=
	    SIPPYMODEM_V32_FRAMING_OK)
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	next_quadrant = gray_dibit(scrambled);
	next.phase_quadrant = next_quadrant;
	*encoder = next;
	*quadrant = next_quadrant;
	return SIPPYMODEM_V32_FRAMING_OK;
}

enum sippymodem_v32_framing_result sippymodem_v32_4800_symbol_encode(
	struct sippymodem_v32_symbol_encoder *encoder, uint8_t input,
	uint8_t *quadrant)
{
	struct sippymodem_v32_symbol_encoder next;
	uint8_t scrambled;
	uint8_t next_quadrant;

	if (!symbol_encoder_is_valid(encoder) || quadrant == NULL)
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

enum sippymodem_v32_framing_result sippymodem_v32_9600_symbol_encode(
	struct sippymodem_v32_symbol_encoder *encoder, uint8_t input,
	uint8_t *symbol)
{
	struct sippymodem_v32_symbol_encoder next;
	uint8_t next_quadrant;
	uint8_t scrambled;

	if (!symbol_encoder_is_valid(encoder) || symbol == NULL)
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	next = *encoder;
	if (sippymodem_v32_scramble_group(
		    &next.scrambler, input,
		    SIPPYMODEM_V32_9600_BITS_PER_SYMBOL, &scrambled) !=
		    SIPPYMODEM_V32_FRAMING_OK ||
	    sippymodem_differential_qpsk_encode(
		    next.phase_quadrant, (uint8_t)(scrambled >> 2),
		    &next_quadrant) != SIPPYMODEM_DIFFERENTIAL_QPSK_OK)
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	next.phase_quadrant = next_quadrant;
	*encoder = next;
	*symbol = (uint8_t)((next_quadrant << 2) | (scrambled & 3u));
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

enum sippymodem_v32_framing_result sippymodem_v32_trn_symbol_decode(
	struct sippymodem_v32_symbol_decoder *decoder, uint8_t quadrant,
	uint8_t *output)
{
	struct sippymodem_v32_symbol_decoder next;
	uint8_t decoded;
	uint8_t scrambled;

	if (!symbol_decoder_is_valid(decoder) || output == NULL ||
	    quadrant >= SIPPYMODEM_QPSK_QUADRANT_COUNT)
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	next = *decoder;
	scrambled = gray_dibit(quadrant);
	if (sippymodem_v32_descramble_dibit(
		    &next.descrambler, scrambled, &decoded) !=
	    SIPPYMODEM_V32_FRAMING_OK)
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	next.phase_quadrant = quadrant;
	*decoder = next;
	*output = decoded;
	return SIPPYMODEM_V32_FRAMING_OK;
}

enum sippymodem_v32_framing_result sippymodem_v32_4800_symbol_decode(
	struct sippymodem_v32_symbol_decoder *decoder, uint8_t quadrant,
	uint8_t *output)
{
	struct sippymodem_v32_symbol_decoder next;
	uint8_t decoded;
	uint8_t scrambled;

	if (!symbol_decoder_is_valid(decoder) || output == NULL ||
	    quadrant >= SIPPYMODEM_QPSK_QUADRANT_COUNT)
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

enum sippymodem_v32_framing_result sippymodem_v32_9600_symbol_decode(
	struct sippymodem_v32_symbol_decoder *decoder, uint8_t symbol,
	uint8_t *output)
{
	struct sippymodem_v32_symbol_decoder next;
	uint8_t decoded;
	uint8_t high_dibit;
	uint8_t quadrant;
	uint8_t scrambled;

	if (!symbol_decoder_is_valid(decoder) || output == NULL ||
	    symbol >= SIPPYMODEM_V32_9600_SYMBOL_COUNT)
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	next = *decoder;
	quadrant = (uint8_t)(symbol >> 2);
	if (sippymodem_differential_qpsk_decode(
		    next.phase_quadrant, quadrant, &high_dibit) !=
		    SIPPYMODEM_DIFFERENTIAL_QPSK_OK)
		return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
	scrambled = (uint8_t)((high_dibit << 2) | (symbol & 3u));
	if (sippymodem_v32_descramble_group(
		    &next.descrambler, scrambled,
		    SIPPYMODEM_V32_9600_BITS_PER_SYMBOL, &decoded) !=
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
