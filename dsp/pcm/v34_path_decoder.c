// SPDX-License-Identifier: BSD-3-Clause

#include "v34_path_decoder.h"

#include "v34_trellis.h"

#include <stddef.h>

static enum v34_path_decoder_result precoder_result(
	enum v34_precoder_result result)
{
	return result == V34_PRECODER_OUT_OF_RANGE ?
		V34_PATH_DECODER_OUT_OF_RANGE :
		V34_PATH_DECODER_INVALID_ARGUMENT;
}

enum v34_path_decoder_result
v34_decode_survivor_path(
	struct v34_precoder *inverse_precoder,
	const struct v34_survivor_decision *survivor_decision,
	struct v34_decided_4d_interval *decided)
{
	struct v34_precoder next;
	struct v34_decided_4d_interval generated = { 0 };
	struct v34_precoder_symbol symbol[2];
	uint8_t modulo_output;

	if (!v34_precoder_is_valid(inverse_precoder) ||
	    !v34_survivor_decision_is_valid(survivor_decision) ||
	    decided == NULL)
		return V34_PATH_DECODER_INVALID_ARGUMENT;
	next = *inverse_precoder;
	for (uint8_t index = 0;
	     index < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++index) {
		enum v34_precoder_result result =
			v34_precoder_inverse_step(
				&next, &survivor_decision->channel_output[index],
				&generated.mapper_output[index], &symbol[index]);

		if (result != V34_PRECODER_OK)
			return precoder_result(result);
	}
	if (v34_modulo_encoder_output(
		    &symbol[0].lattice_offset, &symbol[1].lattice_offset,
		    &modulo_output) != V34_TRELLIS_OK)
		return V34_PATH_DECODER_INVALID_ARGUMENT;
	generated.trellis_output = modulo_output ^
				   survivor_decision->convolutional_output ^
				   survivor_decision->inversion_bit;
	*inverse_precoder = next;
	*decided = generated;
	return V34_PATH_DECODER_OK;
}
