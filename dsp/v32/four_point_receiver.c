// SPDX-License-Identifier: BSD-3-Clause

#include "four_point_receiver.h"

#include "constellation.h"
#include "memory_region.h"

#include <stddef.h>

static bool decoder_is_valid(const struct v32_symbol_decoder *decoder)
{
	return decoder != NULL &&
	       decoder->phase_quadrant < V32_4800_SIGNAL_POINTS &&
	       v32_scrambler_is_valid(&decoder->descrambler);
}

bool v32_four_point_receiver_is_valid(
	const struct v32_four_point_receiver *receiver)
{
	return receiver != NULL && decoder_is_valid(&receiver->decoder);
}

enum v32_four_point_receiver_result v32_four_point_receiver_init(
	struct v32_four_point_receiver *receiver,
	const struct v32_symbol_decoder *seed)
{
	struct v32_four_point_receiver initialized;

	if (receiver == NULL || !decoder_is_valid(seed) ||
	    memory_regions_overlap(
		    receiver, sizeof(*receiver), seed, sizeof(*seed)))
		return V32_FOUR_POINT_RECEIVER_INVALID_ARGUMENT;
	initialized.decoder = *seed;
	if (!v32_four_point_receiver_is_valid(&initialized))
		return V32_FOUR_POINT_RECEIVER_INVALID_STATE;
	*receiver = initialized;
	return V32_FOUR_POINT_RECEIVER_OK;
}

static bool observe_arguments_are_separate(
	const struct v32_four_point_receiver *receiver,
	const struct v32_equalizer_observation *observation,
	const struct v32_four_point_symbol *symbol)
{
	return observation != NULL && symbol != NULL &&
	       !memory_regions_overlap(
		       receiver, sizeof(*receiver),
		       observation, sizeof(*observation)) &&
	       !memory_regions_overlap(
		       receiver, sizeof(*receiver), symbol, sizeof(*symbol)) &&
	       !memory_regions_overlap(
		       observation, sizeof(*observation),
		       symbol, sizeof(*symbol));
}

enum v32_four_point_receiver_result v32_four_point_receiver_observe(
	struct v32_four_point_receiver *receiver,
	const struct v32_equalizer_observation *observation,
	bool adapt_on_error, struct v32_four_point_symbol *symbol)
{
	struct v32_four_point_receiver next;
	struct v32_four_point_symbol next_symbol = { 0 };
	struct v32_quadrant_decision quadrant;

	if (!v32_four_point_receiver_is_valid(receiver))
		return V32_FOUR_POINT_RECEIVER_INVALID_STATE;
	if (!observe_arguments_are_separate(receiver, observation, symbol))
		return V32_FOUR_POINT_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	if (v32_decide_4_point(
		    observation, adapt_on_error, &quadrant) !=
		    V32_DECISION_OK)
		return V32_FOUR_POINT_RECEIVER_INVALID_ARGUMENT;
	if (v32_4800_symbol_decode(
		    &next.decoder, quadrant.quadrant,
		    &next_symbol.dibit) != V32_FRAMING_OK ||
	    !v32_four_point_receiver_is_valid(&next))
		return V32_FOUR_POINT_RECEIVER_INVALID_STATE;
	next_symbol.equalizer = quadrant.equalizer;
	*receiver = next;
	*symbol = next_symbol;
	return V32_FOUR_POINT_RECEIVER_OK;
}
