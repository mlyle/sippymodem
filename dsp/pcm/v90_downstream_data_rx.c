// SPDX-License-Identifier: BSD-3-Clause

#include "v90_downstream_data_rx.h"

#include "memory_region.h"

#include <stddef.h>

static bool stage_is_valid(enum v90_downstream_data_rx_stage stage)
{
	return stage >= V90_DOWNSTREAM_DATA_RX_STAGE_B1D &&
	       stage <= V90_DOWNSTREAM_DATA_RX_STAGE_DATA;
}

static bool b1d_state_is_valid(
	const struct v90_downstream_data_rx *receiver)
{
	uint64_t classified_bits;

	if (receiver == NULL || !stage_is_valid(receiver->stage) ||
	    receiver->stream.decoder.mapping.kind != V90_CP_DATA ||
	    receiver->b1d_frames > V90_PHASE4_B1D_FRAMES ||
	    receiver->b1d_clean_frames > receiver->b1d_frames ||
	    receiver->stream.received_frames < receiver->b1d_frames)
		return false;
	classified_bits = (uint64_t)receiver->b1d_frames *
			  receiver->stream.decoder.mapping.frame_bits;
	if (receiver->b1d_bit_errors > classified_bits)
		return false;
	if (receiver->stage == V90_DOWNSTREAM_DATA_RX_STAGE_B1D)
		return receiver->b1d_frames < V90_PHASE4_B1D_FRAMES &&
		       receiver->stream.received_frames == receiver->b1d_frames;
	return receiver->b1d_frames == V90_PHASE4_B1D_FRAMES;
}

bool v90_downstream_data_rx_is_valid(
	const struct v90_downstream_data_rx *receiver)
{
	return b1d_state_is_valid(receiver) &&
	       v90_pcm_stream_rx_is_valid(&receiver->stream);
}

enum v90_downstream_data_rx_result v90_downstream_data_rx_init(
	struct v90_downstream_data_rx *receiver,
	const struct v90_pcm_mapping *data_mapping,
	enum v90_pcm_mapping_plane decision_plane)
{
	struct v90_downstream_data_rx initialized = { 0 };
	enum v90_pcm_stream_rx_result stream_result;

	if (receiver == NULL || data_mapping == NULL)
		return V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(data_mapping) ||
	    data_mapping->kind != V90_CP_DATA)
		return V90_DOWNSTREAM_DATA_RX_INVALID_PARAMETERS;
	stream_result = v90_pcm_stream_rx_init(
		&initialized.stream, data_mapping, decision_plane);
	if (stream_result == V90_PCM_STREAM_RX_INVALID_ARGUMENT)
		return V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT;
	if (stream_result == V90_PCM_STREAM_RX_INVALID_PARAMETERS)
		return V90_DOWNSTREAM_DATA_RX_INVALID_PARAMETERS;
	if (stream_result != V90_PCM_STREAM_RX_OK)
		return V90_DOWNSTREAM_DATA_RX_INTERNAL_ERROR;
	initialized.stage = V90_DOWNSTREAM_DATA_RX_STAGE_B1D;
	if (!v90_downstream_data_rx_is_valid(&initialized))
		return V90_DOWNSTREAM_DATA_RX_INTERNAL_ERROR;
	*receiver = initialized;
	return V90_DOWNSTREAM_DATA_RX_OK;
}

bool v90_downstream_data_rx_b1d_complete(
	const struct v90_downstream_data_rx *receiver)
{
	return v90_downstream_data_rx_is_valid(receiver) &&
	       receiver->stage == V90_DOWNSTREAM_DATA_RX_STAGE_DATA;
}

static enum v90_downstream_data_rx_result map_stream_failure(
	enum v90_pcm_stream_rx_result result)
{
	switch (result) {
	case V90_PCM_STREAM_RX_INVALID_ARGUMENT:
		return V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT;
	case V90_PCM_STREAM_RX_INVALID_PARAMETERS:
		return V90_DOWNSTREAM_DATA_RX_INVALID_PARAMETERS;
	case V90_PCM_STREAM_RX_INVALID_STATE:
		return V90_DOWNSTREAM_DATA_RX_INVALID_STATE;
	case V90_PCM_STREAM_RX_INVALID_SYMBOL:
		return V90_DOWNSTREAM_DATA_RX_INVALID_SYMBOL;
	case V90_PCM_STREAM_RX_OUT_OF_RANGE:
		return V90_DOWNSTREAM_DATA_RX_OUT_OF_RANGE;
	case V90_PCM_STREAM_RX_OK:
	case V90_PCM_STREAM_RX_BUFFERING:
	case V90_PCM_STREAM_RX_FRAME_READY:
	case V90_PCM_STREAM_RX_INTERNAL_ERROR:
		return V90_DOWNSTREAM_DATA_RX_INTERNAL_ERROR;
	}
	return V90_DOWNSTREAM_DATA_RX_INTERNAL_ERROR;
}

static enum v90_downstream_data_rx_result commit_stream_result(
	struct v90_downstream_data_rx *receiver,
	enum v90_pcm_stream_rx_result stream_result,
	const struct v90_pcm_stream_frame *decoded,
	struct v90_pcm_stream_frame *frame)
{
	enum v90_downstream_data_rx_result result;

	if (stream_result == V90_PCM_STREAM_RX_BUFFERING)
		return V90_DOWNSTREAM_DATA_RX_BUFFERING;
	if (stream_result != V90_PCM_STREAM_RX_FRAME_READY)
		return map_stream_failure(stream_result);
	/* The stream commits a valid complete frame; its bounded bit count
	 * also bounds B1d error accounting. */
	if (receiver->stage == V90_DOWNSTREAM_DATA_RX_STAGE_B1D) {
		uint8_t errors = v90_pcm_stream_frame_bit_errors(decoded, 1u);

		receiver->b1d_bit_errors += errors;
		receiver->b1d_clean_frames = errors ? 0u : receiver->b1d_clean_frames + 1u;
		receiver->b1d_frames++;
		if (receiver->b1d_frames == V90_PHASE4_B1D_FRAMES) {
			receiver->stage = V90_DOWNSTREAM_DATA_RX_STAGE_DATA;
			result = V90_DOWNSTREAM_DATA_RX_B1D_COMPLETE;
		} else {
			result = V90_DOWNSTREAM_DATA_RX_B1D_FRAME;
		}
	} else {
		result = V90_DOWNSTREAM_DATA_RX_DATA_FRAME;
	}
	*frame = *decoded;
	return result;
}

enum v90_downstream_data_rx_result
v90_downstream_data_rx_receive_decision(
	struct v90_downstream_data_rx *receiver, uint8_t ucode,
	bool positive, struct v90_pcm_stream_frame *frame)
{
	struct v90_pcm_stream_frame decoded;
	enum v90_pcm_stream_rx_result stream_result;

	if (receiver == NULL || frame == NULL ||
	    memory_regions_overlap(receiver, sizeof(*receiver),
				   frame, sizeof(*frame)))
		return V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT;
	/* The stream receiver validates its own state before accepting input. */
	if (!b1d_state_is_valid(receiver))
		return V90_DOWNSTREAM_DATA_RX_INVALID_STATE;
	stream_result = v90_pcm_stream_rx_receive_decision(
		&receiver->stream, ucode, positive, &decoded);
	return commit_stream_result(
		receiver, stream_result, &decoded, frame);
}

enum v90_downstream_data_rx_result v90_downstream_data_rx_receive_octet(
	struct v90_downstream_data_rx *receiver, uint8_t network_octet,
	struct v90_pcm_stream_frame *frame)
{
	struct v90_pcm_stream_frame decoded;
	enum v90_pcm_stream_rx_result stream_result;

	if (receiver == NULL || frame == NULL ||
	    memory_regions_overlap(receiver, sizeof(*receiver),
				   frame, sizeof(*frame)))
		return V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT;
	if (!b1d_state_is_valid(receiver))
		return V90_DOWNSTREAM_DATA_RX_INVALID_STATE;
	stream_result = v90_pcm_stream_rx_receive_octet(
		&receiver->stream, network_octet, &decoded);
	return commit_stream_result(
		receiver, stream_result, &decoded, frame);
}

const char *v90_downstream_data_rx_result_name(
	enum v90_downstream_data_rx_result result)
{
	switch (result) {
	case V90_DOWNSTREAM_DATA_RX_OK:
		return "ok";
	case V90_DOWNSTREAM_DATA_RX_BUFFERING:
		return "buffering";
	case V90_DOWNSTREAM_DATA_RX_B1D_FRAME:
		return "B1d frame";
	case V90_DOWNSTREAM_DATA_RX_B1D_COMPLETE:
		return "B1d complete";
	case V90_DOWNSTREAM_DATA_RX_DATA_FRAME:
		return "data frame";
	case V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_DOWNSTREAM_DATA_RX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_DOWNSTREAM_DATA_RX_INVALID_STATE:
		return "invalid state";
	case V90_DOWNSTREAM_DATA_RX_INVALID_SYMBOL:
		return "invalid symbol";
	case V90_DOWNSTREAM_DATA_RX_OUT_OF_RANGE:
		return "out of range";
	case V90_DOWNSTREAM_DATA_RX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
