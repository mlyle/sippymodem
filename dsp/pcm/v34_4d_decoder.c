// SPDX-License-Identifier: BSD-3-Clause

#include "v34_4d_decoder.h"

#include <stddef.h>

bool v34_4d_decoder_is_valid(
	const struct v34_4d_decoder *decoder)
{
	return decoder != NULL &&
	       v34_mapping_parameters_is_valid(
		       &decoder->parameters) &&
	       decoder->previous_differential_output < 4u;
}

enum v34_4d_decoder_result v34_4d_decoder_init(
	struct v34_4d_decoder *decoder,
	const struct v34_mapping_parameters *parameters)
{
	struct v34_4d_decoder initialized = { 0 };

	if (decoder == NULL ||
	    !v34_mapping_parameters_is_valid(parameters))
		return V34_4D_DECODER_INVALID_ARGUMENT;
	initialized.parameters = *parameters;
	*decoder = initialized;
	return V34_4D_DECODER_OK;
}

enum v34_4d_decoder_result v34_decode_4d_interval(
	struct v34_4d_decoder *decoder,
	const struct v34_decided_4d_interval *decided,
	struct v34_decoded_4d_interval *decoded)
{
	struct v34_4d_decoder next;
	struct v34_decoded_4d_interval generated = { 0 };
	struct v34_2d_mapping_decision mapped[2];
	uint8_t relative_rotation;

	if (!v34_4d_decoder_is_valid(decoder) || decided == NULL ||
	    decided->trellis_output > 1u || decoded == NULL)
		return V34_4D_DECODER_INVALID_ARGUMENT;
	next = *decoder;
	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		if (v34_unmap_2d_decision(
			    &next.parameters, &decided->mapper_output[symbol],
			    &mapped[symbol]) != V34_MAPPER_OK)
			return V34_4D_DECODER_INVALID_ARGUMENT;
		generated.ring_prefix[symbol] = mapped[symbol].ring_prefix;
		generated.mapping.point_index[symbol] =
			mapped[symbol].point_index;
	}
	generated.differential_output = mapped[0].clockwise_quarter_turns;
	generated.mapping.differential_input = (uint8_t)(
		(generated.differential_output + 4u -
		 next.previous_differential_output) &
		3u);
	relative_rotation = (uint8_t)(
		(mapped[1].clockwise_quarter_turns + 8u -
		 generated.differential_output - decided->trellis_output) &
		3u);
	if ((relative_rotation & 1u) != 0u)
		return V34_4D_DECODER_INVALID_ARGUMENT;
	generated.mapping.rotation_bit = relative_rotation >> 1;
	next.previous_differential_output = generated.differential_output;
	*decoder = next;
	*decoded = generated;
	return V34_4D_DECODER_OK;
}
