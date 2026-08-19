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
	const struct v32_scrambler *continuous_descrambler)
{
	struct v32_uncoded_receiver initialized = {
		.decoder.phase_quadrant = 0,
	};

	if (receiver == NULL ||
	    !v32_scrambler_is_valid(continuous_descrambler) ||
	    memory_regions_overlap(
		    receiver, sizeof(*receiver), continuous_descrambler,
		    sizeof(*continuous_descrambler)))
		return V32_UNCODED_RECEIVER_INVALID_ARGUMENT;
	initialized.decoder.descrambler = *continuous_descrambler;
	if (!v32_uncoded_receiver_is_valid(&initialized))
		return V32_UNCODED_RECEIVER_INVALID_STATE;
	*receiver = initialized;
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
	struct v32_uncoded_receiver *receiver,
	const struct v32_equalizer_observation *observation,
	bool adapt_on_error, struct v32_uncoded_symbol *symbol)
{
	struct v32_uncoded_receiver next;
	struct v32_uncoded_symbol next_symbol = { 0 };
	struct v32_uncoded_decision decision;

	if (!v32_uncoded_receiver_is_valid(receiver))
		return V32_UNCODED_RECEIVER_INVALID_STATE;
	if (!observe_arguments_are_separate(receiver, observation, symbol))
		return V32_UNCODED_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	if (v32_decide_uncoded_16(
		    observation, adapt_on_error, &decision) !=
	    V32_DECISION_OK)
		return V32_UNCODED_RECEIVER_INVALID_ARGUMENT;
	if (v32_9600_symbol_decode(
		    &next.decoder, decision.symbol, &next_symbol.group) !=
		    V32_FRAMING_OK ||
	    !v32_uncoded_receiver_is_valid(&next))
		return V32_UNCODED_RECEIVER_INVALID_STATE;
	next_symbol.equalizer = decision.equalizer;
	*receiver = next;
	*symbol = next_symbol;
	return V32_UNCODED_RECEIVER_OK;
}
