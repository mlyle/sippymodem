// SPDX-License-Identifier: BSD-3-Clause

#include "v90_downstream_data_tx.h"

#include "packed_bits.h"

#include <string.h>

static bool stage_is_valid(enum v90_downstream_data_tx_stage stage)
{
	return stage >= V90_DOWNSTREAM_DATA_TX_STAGE_B1D &&
	       stage <= V90_DOWNSTREAM_DATA_TX_STAGE_COMPLETE;
}

bool v90_downstream_data_tx_is_valid(
	const struct v90_downstream_data_tx *transmitter)
{
	if (transmitter == NULL ||
	    !stage_is_valid(transmitter->stage) ||
	    !v90_pcm_frame_encoder_is_valid(&transmitter->encoder) ||
	    transmitter->encoder.mapping.kind != V90_CP_DATA ||
	    !v34_scrambler_is_valid(&transmitter->scrambler) ||
	    transmitter->scrambler.polynomial != V34_POLYNOMIAL_GPC ||
	    transmitter->b1d_frames > V90_PHASE4_B1D_FRAMES)
		return false;

	switch (transmitter->stage) {
	case V90_DOWNSTREAM_DATA_TX_STAGE_B1D:
		return transmitter->b1d_frames < V90_PHASE4_B1D_FRAMES &&
		       !transmitter->encoder.finishing;
	case V90_DOWNSTREAM_DATA_TX_STAGE_DATA:
		return transmitter->b1d_frames == V90_PHASE4_B1D_FRAMES &&
		       !transmitter->encoder.finishing;
	case V90_DOWNSTREAM_DATA_TX_STAGE_FINISHING:
		return transmitter->b1d_frames == V90_PHASE4_B1D_FRAMES &&
		       transmitter->encoder.finishing;
	case V90_DOWNSTREAM_DATA_TX_STAGE_COMPLETE:
		return transmitter->b1d_frames == V90_PHASE4_B1D_FRAMES &&
		       transmitter->encoder.finishing &&
		       (transmitter->encoder.mapping.sign_redundancy == 0 ||
			transmitter->encoder.shaper.pending_count == 0);
	}
	return false;
}

enum v90_downstream_data_tx_result v90_downstream_data_tx_init(
	struct v90_downstream_data_tx *transmitter,
	const struct v90_pcm_mapping *data_mapping)
{
	struct v90_downstream_data_tx initialized = { 0 };

	if (transmitter == NULL || data_mapping == NULL)
		return V90_DOWNSTREAM_DATA_TX_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(data_mapping) ||
	    data_mapping->kind != V90_CP_DATA)
		return V90_DOWNSTREAM_DATA_TX_INVALID_PARAMETERS;
	if (v90_pcm_frame_encoder_init(
		    &initialized.encoder, data_mapping) !=
		    V90_PCM_FRAME_ENCODER_OK ||
	    v34_scrambler_init(
		    &initialized.scrambler, V34_POLYNOMIAL_GPC) !=
		    V34_FRAMING_OK)
		return V90_DOWNSTREAM_DATA_TX_INTERNAL_ERROR;
	initialized.stage = V90_DOWNSTREAM_DATA_TX_STAGE_B1D;
	if (!v90_downstream_data_tx_is_valid(&initialized))
		return V90_DOWNSTREAM_DATA_TX_INTERNAL_ERROR;
	*transmitter = initialized;
	return V90_DOWNSTREAM_DATA_TX_OK;
}

bool v90_downstream_data_tx_b1d_complete(
	const struct v90_downstream_data_tx *transmitter)
{
	return v90_downstream_data_tx_is_valid(transmitter) &&
	       transmitter->b1d_frames == V90_PHASE4_B1D_FRAMES;
}

bool v90_downstream_data_tx_is_complete(
	const struct v90_downstream_data_tx *transmitter)
{
	return v90_downstream_data_tx_is_valid(transmitter) &&
	       transmitter->stage == V90_DOWNSTREAM_DATA_TX_STAGE_COMPLETE;
}

static enum v90_downstream_data_tx_result encode_frame(
	struct v90_downstream_data_tx *transmitter,
	const uint8_t *bits, size_t bit_count,
	struct v90_pcm_frame_output *output)
{
	uint8_t scrambled[V90_PCM_FRAME_MAX_BITS];
	enum v90_pcm_frame_encoder_result result;

	if (v34_scramble_unpacked_bits(
		    &transmitter->scrambler, bits, bit_count, scrambled) !=
	    V34_FRAMING_OK)
		return V90_DOWNSTREAM_DATA_TX_INTERNAL_ERROR;
	result = v90_pcm_frame_encoder_process(
		&transmitter->encoder, scrambled, bit_count, output);
	switch (result) {
	case V90_PCM_FRAME_ENCODER_BUFFERING:
		return V90_DOWNSTREAM_DATA_TX_BUFFERING;
	case V90_PCM_FRAME_ENCODER_SYMBOLS_READY:
		return V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY;
	case V90_PCM_FRAME_ENCODER_NUMERIC_FAILURE:
		return V90_DOWNSTREAM_DATA_TX_NUMERIC_FAILURE;
	default:
		return V90_DOWNSTREAM_DATA_TX_INTERNAL_ERROR;
	}
}

enum v90_downstream_data_tx_result v90_downstream_data_tx_next_b1d(
	struct v90_downstream_data_tx *transmitter,
	struct v90_pcm_frame_output *output)
{
	struct v90_downstream_data_tx next;
	struct v90_pcm_frame_output generated;
	uint8_t ones[V90_PCM_FRAME_MAX_BITS];
	enum v90_downstream_data_tx_result result;

	if (transmitter == NULL || output == NULL)
		return V90_DOWNSTREAM_DATA_TX_INVALID_ARGUMENT;
	if (!v90_downstream_data_tx_is_valid(transmitter) ||
	    transmitter->stage != V90_DOWNSTREAM_DATA_TX_STAGE_B1D)
		return V90_DOWNSTREAM_DATA_TX_INVALID_STATE;
	next = *transmitter;
	memset(ones, 1, next.encoder.mapping.frame_bits);
	result = encode_frame(
		&next, ones, next.encoder.mapping.frame_bits, &generated);
	if (result != V90_DOWNSTREAM_DATA_TX_BUFFERING &&
	    result != V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY)
		return result;
	next.b1d_frames++;
	if (next.b1d_frames == V90_PHASE4_B1D_FRAMES)
		next.stage = V90_DOWNSTREAM_DATA_TX_STAGE_DATA;
	if (!v90_downstream_data_tx_is_valid(&next))
		return V90_DOWNSTREAM_DATA_TX_INTERNAL_ERROR;
	*transmitter = next;
	if (result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY)
		*output = generated;
	return result;
}

enum v90_downstream_data_tx_result v90_downstream_data_tx_process(
	struct v90_downstream_data_tx *transmitter,
	const uint8_t *bits, size_t bit_count,
	struct v90_pcm_frame_output *output)
{
	struct v90_downstream_data_tx next;
	struct v90_pcm_frame_output generated;
	enum v90_downstream_data_tx_result result;

	if (transmitter == NULL || bits == NULL || output == NULL)
		return V90_DOWNSTREAM_DATA_TX_INVALID_ARGUMENT;
	if (!v90_downstream_data_tx_is_valid(transmitter) ||
	    transmitter->stage != V90_DOWNSTREAM_DATA_TX_STAGE_DATA)
		return V90_DOWNSTREAM_DATA_TX_INVALID_STATE;
	if (bit_count != transmitter->encoder.mapping.frame_bits ||
	    !unpacked_bits_are_binary(bits, bit_count))
		return V90_DOWNSTREAM_DATA_TX_INVALID_BITS;
	next = *transmitter;
	result = encode_frame(&next, bits, bit_count, &generated);
	if (result != V90_DOWNSTREAM_DATA_TX_BUFFERING &&
	    result != V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY)
		return result;
	if (!v90_downstream_data_tx_is_valid(&next))
		return V90_DOWNSTREAM_DATA_TX_INTERNAL_ERROR;
	*transmitter = next;
	if (result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY)
		*output = generated;
	return result;
}

enum v90_downstream_data_tx_result v90_downstream_data_tx_finish(
	struct v90_downstream_data_tx *transmitter,
	struct v90_pcm_frame_output *output)
{
	struct v90_downstream_data_tx next;
	struct v90_pcm_frame_output generated;
	enum v90_pcm_frame_encoder_result result;

	if (transmitter == NULL || output == NULL)
		return V90_DOWNSTREAM_DATA_TX_INVALID_ARGUMENT;
	if (!v90_downstream_data_tx_is_valid(transmitter))
		return V90_DOWNSTREAM_DATA_TX_INVALID_STATE;
	if (transmitter->stage == V90_DOWNSTREAM_DATA_TX_STAGE_COMPLETE)
		return V90_DOWNSTREAM_DATA_TX_COMPLETE;
	if (transmitter->stage != V90_DOWNSTREAM_DATA_TX_STAGE_DATA &&
	    transmitter->stage != V90_DOWNSTREAM_DATA_TX_STAGE_FINISHING)
		return V90_DOWNSTREAM_DATA_TX_INVALID_STATE;
	next = *transmitter;
	result = v90_pcm_frame_encoder_finish(&next.encoder, &generated);
	if (result == V90_PCM_FRAME_ENCODER_SYMBOLS_READY) {
		next.stage = V90_DOWNSTREAM_DATA_TX_STAGE_FINISHING;
		if (!v90_downstream_data_tx_is_valid(&next))
			return V90_DOWNSTREAM_DATA_TX_INTERNAL_ERROR;
		*transmitter = next;
		*output = generated;
		return V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY;
	}
	if (result != V90_PCM_FRAME_ENCODER_COMPLETE)
		return result == V90_PCM_FRAME_ENCODER_NUMERIC_FAILURE ?
			       V90_DOWNSTREAM_DATA_TX_NUMERIC_FAILURE :
			       V90_DOWNSTREAM_DATA_TX_INTERNAL_ERROR;
	next.stage = V90_DOWNSTREAM_DATA_TX_STAGE_COMPLETE;
	if (!v90_downstream_data_tx_is_valid(&next))
		return V90_DOWNSTREAM_DATA_TX_INTERNAL_ERROR;
	*transmitter = next;
	return V90_DOWNSTREAM_DATA_TX_COMPLETE;
}

const char *v90_downstream_data_tx_result_name(
	enum v90_downstream_data_tx_result result)
{
	switch (result) {
	case V90_DOWNSTREAM_DATA_TX_OK:
		return "ok";
	case V90_DOWNSTREAM_DATA_TX_BUFFERING:
		return "buffering";
	case V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY:
		return "symbols ready";
	case V90_DOWNSTREAM_DATA_TX_COMPLETE:
		return "complete";
	case V90_DOWNSTREAM_DATA_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_DOWNSTREAM_DATA_TX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_DOWNSTREAM_DATA_TX_INVALID_STATE:
		return "invalid state";
	case V90_DOWNSTREAM_DATA_TX_INVALID_BITS:
		return "invalid bits";
	case V90_DOWNSTREAM_DATA_TX_NUMERIC_FAILURE:
		return "numeric failure";
	case V90_DOWNSTREAM_DATA_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
