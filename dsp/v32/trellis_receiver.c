// SPDX-License-Identifier: BSD-3-Clause

#include "trellis_receiver.h"

#include "memory_region.h"

#include <stddef.h>

static bool receiver_metadata_is_valid(
	const struct v32_trellis_receiver *receiver)
{
	uint8_t expected_history;

	if (receiver == NULL ||
	    !v32_scrambler_is_valid(&receiver->descrambler) ||
	    receiver->b1_observations > V32_TRELLIS_RECEIVER_B1_SYMBOLS ||
	    receiver->b1_flush_symbols >
		    V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS)
		return false;
	expected_history = receiver->b1_observations <
				   V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS ?
			   receiver->b1_observations :
			   V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS;
	if (receiver->b1_flush_symbols == 0)
		return receiver->decoder.history_fill == expected_history;
	return receiver->b1_observations ==
		       V32_TRELLIS_RECEIVER_B1_SYMBOLS &&
	       receiver->decoder.history_fill ==
		       V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS;
}

bool v32_trellis_receiver_is_valid(
	const struct v32_trellis_receiver *receiver)
{
	return receiver_metadata_is_valid(receiver) &&
	       v32_trellis_decoder_is_valid(&receiver->decoder);
}

enum v32_trellis_receiver_result v32_trellis_receiver_init(
	struct v32_trellis_receiver *receiver,
	const struct v32_scrambler *continuous_descrambler,
	uint8_t data_bits_per_symbol)
{
	struct v32_trellis_receiver initialized = {
		.b1_matches = true,
	};

	if (receiver == NULL ||
	    !v32_scrambler_is_valid(continuous_descrambler) ||
	    memory_regions_overlap(
		    receiver, sizeof(*receiver), continuous_descrambler,
		    sizeof(*continuous_descrambler)) ||
	    v32_trellis_decoder_init(
		    &initialized.decoder, data_bits_per_symbol, 0) !=
		    V32_TRELLIS_DECODER_OK)
		return V32_TRELLIS_RECEIVER_INVALID_ARGUMENT;
	initialized.descrambler = *continuous_descrambler;
	if (!v32_trellis_receiver_is_valid(&initialized))
		return V32_TRELLIS_RECEIVER_INVALID_STATE;
	*receiver = initialized;
	return V32_TRELLIS_RECEIVER_OK;
}

static bool observation_is_separate(
	const struct v32_trellis_receiver *receiver,
	const struct v32_equalizer_observation *observation)
{
	return observation != NULL &&
	       !memory_regions_overlap(
		       receiver, sizeof(*receiver),
		       observation, sizeof(*observation));
}

static enum v32_trellis_receiver_result decode_observation(
	struct v32_trellis_receiver *receiver,
	const struct v32_equalizer_observation *observation,
	struct v32_trellis_decoded_symbol *decoded)
{
	enum v32_trellis_decoder_result result =
		v32_trellis_decoder_decode(
			&receiver->decoder, observation, decoded);

	if (result == V32_TRELLIS_DECODER_INVALID_ARGUMENT)
		return V32_TRELLIS_RECEIVER_INVALID_ARGUMENT;
	if (result != V32_TRELLIS_DECODER_OK)
		return V32_TRELLIS_RECEIVER_INVALID_STATE;
	return V32_TRELLIS_RECEIVER_OK;
}

static enum v32_trellis_receiver_result descramble(
	struct v32_trellis_receiver *receiver, uint8_t scrambled,
	uint8_t *plain)
{
	return v32_descramble_group(
		       &receiver->descrambler, scrambled,
		       receiver->decoder.data_bits_per_symbol, plain) ==
		       V32_FRAMING_OK ?
		       V32_TRELLIS_RECEIVER_OK :
		       V32_TRELLIS_RECEIVER_INVALID_STATE;
}

enum v32_trellis_receiver_result v32_trellis_receiver_track_b1(
	struct v32_trellis_receiver *receiver,
	const struct v32_equalizer_observation *observation)
{
	struct v32_trellis_receiver next;
	struct v32_trellis_decoded_symbol decoded;
	enum v32_trellis_receiver_result result;
	uint8_t plain;
	uint8_t mark;

	if (receiver == NULL)
		return V32_TRELLIS_RECEIVER_INVALID_STATE;
	if (!observation_is_separate(receiver, observation))
		return V32_TRELLIS_RECEIVER_INVALID_ARGUMENT;
	if (!receiver_metadata_is_valid(receiver) ||
	    receiver->b1_observations ==
		    V32_TRELLIS_RECEIVER_B1_SYMBOLS)
		return V32_TRELLIS_RECEIVER_INVALID_STATE;
	next = *receiver;
	result = decode_observation(&next, observation, &decoded);
	if (result != V32_TRELLIS_RECEIVER_OK)
		return result;
	if (decoded.ready) {
		result = descramble(&next, decoded.scrambled_group, &plain);
		if (result != V32_TRELLIS_RECEIVER_OK)
			return result;
		mark = (uint8_t)((UINT8_C(1) <<
				 next.decoder.data_bits_per_symbol) - 1u);
		next.b1_matches = next.b1_matches && plain == mark;
	}
	next.b1_observations++;
	if (!receiver_metadata_is_valid(&next))
		return V32_TRELLIS_RECEIVER_INVALID_STATE;
	*receiver = next;
	return V32_TRELLIS_RECEIVER_OK;
}

static bool decode_arguments_are_separate(
	const struct v32_trellis_receiver *receiver,
	const struct v32_equalizer_observation *observation,
	const struct v32_trellis_receiver_symbol *symbol)
{
	return observation_is_separate(receiver, observation) &&
	       symbol != NULL &&
	       !memory_regions_overlap(
		       receiver, sizeof(*receiver), symbol, sizeof(*symbol)) &&
	       !memory_regions_overlap(
		       observation, sizeof(*observation),
		       symbol, sizeof(*symbol));
}

enum v32_trellis_receiver_result v32_trellis_receiver_decode(
	struct v32_trellis_receiver *receiver,
	const struct v32_equalizer_observation *observation,
	struct v32_trellis_receiver_symbol *symbol)
{
	struct v32_trellis_receiver next;
	struct v32_trellis_receiver_symbol next_symbol = { 0 };
	struct v32_trellis_decoded_symbol decoded;
	enum v32_trellis_receiver_result result;
	uint8_t plain;
	uint8_t mark;

	if (receiver == NULL)
		return V32_TRELLIS_RECEIVER_INVALID_STATE;
	if (!decode_arguments_are_separate(receiver, observation, symbol))
		return V32_TRELLIS_RECEIVER_INVALID_ARGUMENT;
	if (!receiver_metadata_is_valid(receiver) ||
	    receiver->b1_observations !=
		    V32_TRELLIS_RECEIVER_B1_SYMBOLS)
		return V32_TRELLIS_RECEIVER_INVALID_STATE;
	next = *receiver;
	result = decode_observation(&next, observation, &decoded);
	if (result != V32_TRELLIS_RECEIVER_OK)
		return result;
	if (!decoded.ready)
		return V32_TRELLIS_RECEIVER_INVALID_STATE;
	result = descramble(&next, decoded.scrambled_group, &plain);
	if (result != V32_TRELLIS_RECEIVER_OK)
		return result;
	if (next.b1_flush_symbols <
	    V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS) {
		mark = (uint8_t)((UINT8_C(1) <<
				 next.decoder.data_bits_per_symbol) - 1u);
		next.b1_matches = next.b1_matches && plain == mark;
		next.b1_flush_symbols++;
	} else {
		next_symbol.group = plain;
		next_symbol.ready = true;
	}
	if (!receiver_metadata_is_valid(&next))
		return V32_TRELLIS_RECEIVER_INVALID_STATE;
	*receiver = next;
	*symbol = next_symbol;
	return V32_TRELLIS_RECEIVER_OK;
}

bool v32_trellis_receiver_b1_is_confirmed(
	const struct v32_trellis_receiver *receiver)
{
	return v32_trellis_receiver_is_valid(receiver) &&
	       receiver->b1_matches &&
	       receiver->b1_flush_symbols ==
		       V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS;
}
