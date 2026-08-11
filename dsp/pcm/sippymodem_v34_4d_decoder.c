// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_4d_decoder.h"

#include <stddef.h>

bool sippymodem_v34_4d_decoder_is_valid(
	const struct sippymodem_v34_4d_decoder *decoder)
{
	return decoder != NULL &&
	       sippymodem_v34_mapping_parameters_is_valid(
		       &decoder->parameters) &&
	       decoder->previous_differential_output < 4u;
}

enum sippymodem_v34_4d_decoder_result sippymodem_v34_4d_decoder_init(
	struct sippymodem_v34_4d_decoder *decoder,
	const struct sippymodem_v34_mapping_parameters *parameters)
{
	struct sippymodem_v34_4d_decoder initialized = { 0 };

	if (decoder == NULL ||
	    !sippymodem_v34_mapping_parameters_is_valid(parameters))
		return SIPPYMODEM_V34_4D_DECODER_INVALID_ARGUMENT;
	initialized.parameters = *parameters;
	*decoder = initialized;
	return SIPPYMODEM_V34_4D_DECODER_OK;
}

enum sippymodem_v34_4d_decoder_result sippymodem_v34_decode_4d_interval(
	struct sippymodem_v34_4d_decoder *decoder,
	const struct sippymodem_v34_decided_4d_interval *decided,
	struct sippymodem_v34_decoded_4d_interval *decoded)
{
	struct sippymodem_v34_4d_decoder next;
	struct sippymodem_v34_decoded_4d_interval generated = { 0 };
	struct sippymodem_v34_2d_mapping_input mapped[2];
	uint8_t relative_rotation;

	if (!sippymodem_v34_4d_decoder_is_valid(decoder) || decided == NULL ||
	    decided->trellis_output > 1u || decoded == NULL)
		return SIPPYMODEM_V34_4D_DECODER_INVALID_ARGUMENT;
	next = *decoder;
	for (uint8_t symbol = 0;
	     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		if (sippymodem_v34_unmap_2d_symbol(
			    &next.parameters, &decided->mapper_output[symbol],
			    &mapped[symbol]) != SIPPYMODEM_V34_MAPPER_OK)
			return SIPPYMODEM_V34_4D_DECODER_INVALID_ARGUMENT;
		generated.ring_index[symbol] = mapped[symbol].ring_index;
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
		return SIPPYMODEM_V34_4D_DECODER_INVALID_ARGUMENT;
	generated.mapping.rotation_bit = relative_rotation >> 1;
	next.previous_differential_output = generated.differential_output;
	*decoder = next;
	*decoded = generated;
	return SIPPYMODEM_V34_4D_DECODER_OK;
}

const char *sippymodem_v34_4d_decoder_result_name(
	enum sippymodem_v34_4d_decoder_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_4D_DECODER_OK:
		return "ok";
	case SIPPYMODEM_V34_4D_DECODER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
