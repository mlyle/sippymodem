// SPDX-License-Identifier: BSD-3-Clause

#include "v34_path_decoder.h"

#include "v34_trellis.h"

#include <stddef.h>

static enum sippymodem_v34_path_decoder_result precoder_result(
	enum sippymodem_v34_precoder_result result)
{
	return result == SIPPYMODEM_V34_PRECODER_OUT_OF_RANGE ?
		SIPPYMODEM_V34_PATH_DECODER_OUT_OF_RANGE :
		SIPPYMODEM_V34_PATH_DECODER_INVALID_ARGUMENT;
}

enum sippymodem_v34_path_decoder_result
sippymodem_v34_decode_survivor_path(
	struct sippymodem_v34_precoder *inverse_precoder,
	const struct sippymodem_v34_survivor_decision *survivor_decision,
	struct sippymodem_v34_decided_4d_interval *decided)
{
	struct sippymodem_v34_precoder next;
	struct sippymodem_v34_decided_4d_interval generated = { 0 };
	struct sippymodem_v34_precoder_symbol symbol[2];
	uint8_t modulo_output;

	if (!sippymodem_v34_precoder_is_valid(inverse_precoder) ||
	    !sippymodem_v34_survivor_decision_is_valid(survivor_decision) ||
	    decided == NULL)
		return SIPPYMODEM_V34_PATH_DECODER_INVALID_ARGUMENT;
	next = *inverse_precoder;
	for (uint8_t index = 0;
	     index < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++index) {
		enum sippymodem_v34_precoder_result result =
			sippymodem_v34_precoder_inverse_step(
				&next, &survivor_decision->channel_output[index],
				&generated.mapper_output[index], &symbol[index]);

		if (result != SIPPYMODEM_V34_PRECODER_OK)
			return precoder_result(result);
	}
	if (sippymodem_v34_modulo_encoder_output(
		    &symbol[0].lattice_offset, &symbol[1].lattice_offset,
		    &modulo_output) != SIPPYMODEM_V34_TRELLIS_OK)
		return SIPPYMODEM_V34_PATH_DECODER_INVALID_ARGUMENT;
	generated.trellis_output = modulo_output ^
				   survivor_decision->convolutional_output ^
				   survivor_decision->inversion_bit;
	*inverse_precoder = next;
	*decided = generated;
	return SIPPYMODEM_V34_PATH_DECODER_OK;
}

const char *sippymodem_v34_path_decoder_result_name(
	enum sippymodem_v34_path_decoder_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PATH_DECODER_OK:
		return "ok";
	case SIPPYMODEM_V34_PATH_DECODER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_PATH_DECODER_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
