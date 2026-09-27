// SPDX-License-Identifier: BSD-3-Clause

#include "uncoded_receiver.h"

#include "memory_region.h"

#include <stddef.h>

static bool decoder_is_valid(const struct v32_symbol_decoder *decoder)
{
	return decoder != NULL &&
	       decoder->phase_quadrant < QPSK_QUADRANT_COUNT &&
	       v32_scrambler_is_valid(&decoder->descrambler);
}

bool v32_uncoded_receiver_is_valid(
	const struct v32_uncoded_receiver *receiver)
{
	return receiver != NULL && decoder_is_valid(&receiver->decoder);
}

enum v32_uncoded_receiver_result v32_uncoded_receiver_init(
	struct v32_uncoded_receiver *receiver,
	const struct v32_symbol_decoder *seed)
{
	if (receiver == NULL || !decoder_is_valid(seed) ||
	    memory_regions_overlap(
		    receiver, sizeof(*receiver), seed, sizeof(*seed)))
		return V32_UNCODED_RECEIVER_INVALID_ARGUMENT;
	receiver->decoder = *seed;
	return V32_UNCODED_RECEIVER_OK;
}

static bool observe_arguments_are_separate(
	const struct v32_uncoded_receiver *receiver,
	const struct v32_equalizer_observation *observation,
	const struct v32_uncoded_symbol *symbol)
{
	return observation != NULL && symbol != NULL &&
	       !memory_regions_overlap(
		       receiver, sizeof(*receiver),
		       observation, sizeof(*observation)) &&
	       !memory_regions_overlap(
		       receiver, sizeof(*receiver), symbol, sizeof(*symbol)) &&
	       !memory_regions_overlap(
		       observation, sizeof(*observation), symbol, sizeof(*symbol));
}

enum v32_uncoded_receiver_result v32_uncoded_receiver_observe(
	struct v32_uncoded_receiver *receiver, uint8_t bits_per_symbol,
	const struct v32_equalizer_observation *observation,
	bool adapt_on_error, struct v32_uncoded_symbol *symbol)
{
	struct v32_uncoded_symbol next_symbol = { 0 };
	enum v32_framing_result result;

	if (!v32_uncoded_receiver_is_valid(receiver))
		return V32_UNCODED_RECEIVER_INVALID_STATE;
	if (!observe_arguments_are_separate(receiver, observation, symbol))
		return V32_UNCODED_RECEIVER_INVALID_ARGUMENT;
	/* Symbol framing already leaves its owner unchanged on rejection. */
	switch (bits_per_symbol) {
	case V32_4800_BITS_PER_SYMBOL: {
		struct v32_quadrant_decision decision;

		if (v32_decide_4_point(
			    observation, adapt_on_error, &decision) !=
		    V32_DECISION_OK)
			return V32_UNCODED_RECEIVER_INVALID_ARGUMENT;
		result = v32_4800_symbol_decode(
			&receiver->decoder, decision.quadrant, &next_symbol.group);
		next_symbol.equalizer = decision.equalizer;
		break;
	}
	case V32_9600_BITS_PER_SYMBOL: {
		struct v32_uncoded_decision decision;

		if (v32_decide_uncoded_16(
			    observation, adapt_on_error, &decision) !=
		    V32_DECISION_OK)
			return V32_UNCODED_RECEIVER_INVALID_ARGUMENT;
		result = v32_9600_symbol_decode(
			&receiver->decoder, decision.symbol, &next_symbol.group);
		next_symbol.equalizer = decision.equalizer;
		break;
	}
	default:
		return V32_UNCODED_RECEIVER_INVALID_ARGUMENT;
	}
	if (result != V32_FRAMING_OK)
		return V32_UNCODED_RECEIVER_INVALID_STATE;
	*symbol = next_symbol;
	return V32_UNCODED_RECEIVER_OK;
}
