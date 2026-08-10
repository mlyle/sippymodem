// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_data_stream.h"

#include "sippymodem_v34_mapping_parameters.h"

#include <stddef.h>
#include <string.h>

static enum sippymodem_v34_data_stream_result transmitter_result(
	enum sippymodem_v34_data_transmitter_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_DATA_TRANSMITTER_OK:
		return SIPPYMODEM_V34_DATA_STREAM_OK;
	case SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT:
		return SIPPYMODEM_V34_DATA_STREAM_INVALID_ARGUMENT;
	case SIPPYMODEM_V34_DATA_TRANSMITTER_OUT_OF_RANGE:
		return SIPPYMODEM_V34_DATA_STREAM_OUT_OF_RANGE;
	case SIPPYMODEM_V34_DATA_TRANSMITTER_CLIPPING_RISK:
		return SIPPYMODEM_V34_DATA_STREAM_CLIPPING_RISK;
	case SIPPYMODEM_V34_DATA_TRANSMITTER_NUMERIC_FAILURE:
		return SIPPYMODEM_V34_DATA_STREAM_NUMERIC_FAILURE;
	case SIPPYMODEM_V34_DATA_TRANSMITTER_MAPPING_FRAME_REQUIRED:
	case SIPPYMODEM_V34_DATA_TRANSMITTER_MAPPING_FRAME_PENDING:
		break;
	}
	return SIPPYMODEM_V34_DATA_STREAM_NUMERIC_FAILURE;
}

static bool schedule_is_at_b1_start(
	const struct sippymodem_v34_frame_schedule *schedule)
{
	return sippymodem_v34_frame_schedule_is_valid(schedule) &&
	       schedule->data_frame_in_superframe + 1u ==
		       schedule->geometry.data_frames_per_superframe &&
	       schedule->next_mapping_frame == 0;
}

bool sippymodem_v34_data_stream_is_valid(
	const struct sippymodem_v34_data_stream *stream)
{
	return stream != NULL &&
	       sippymodem_v34_data_transmitter_is_valid(
		       &stream->transmitter) &&
	       (!stream->b1_active ||
		stream->transmitter.schedule.data_frame_in_superframe + 1u ==
			stream->transmitter.schedule.geometry
				.data_frames_per_superframe);
}

enum sippymodem_v34_data_stream_result sippymodem_v34_data_stream_init_b1(
	struct sippymodem_v34_data_stream *stream,
	const struct sippymodem_v34_data_transmitter *transmitter)
{
	struct sippymodem_v34_data_stream initialized;

	if (stream == NULL ||
	    !sippymodem_v34_data_transmitter_is_valid(transmitter) ||
	    !sippymodem_v34_data_transmitter_needs_mapping_frame(transmitter) ||
	    !schedule_is_at_b1_start(&transmitter->schedule))
		return SIPPYMODEM_V34_DATA_STREAM_INVALID_ARGUMENT;
	initialized = (struct sippymodem_v34_data_stream){
		.transmitter = *transmitter,
		.b1_active = true,
	};
	if (!sippymodem_v34_data_stream_is_valid(&initialized))
		return SIPPYMODEM_V34_DATA_STREAM_NUMERIC_FAILURE;
	*stream = initialized;
	return SIPPYMODEM_V34_DATA_STREAM_OK;
}

bool sippymodem_v34_data_stream_is_transmitting_b1(
	const struct sippymodem_v34_data_stream *stream)
{
	return sippymodem_v34_data_stream_is_valid(stream) &&
	       stream->b1_active;
}

static bool bits_are_binary(const uint8_t *bits, size_t count)
{
	if (bits == NULL)
		return false;
	for (size_t index = 0; index < count; ++index) {
		if (bits[index] > 1u)
			return false;
	}
	return true;
}

static bool acquire_mapping_frame(
	const struct sippymodem_v34_data_stream *stream,
	sippymodem_v34_mapping_frame_source source, void *source_context,
	const struct sippymodem_v34_mapping_frame *request,
	uint8_t primary_bits[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS],
	uint8_t *auxiliary_bit)
{
	uint8_t *auxiliary = request->carries_auxiliary_bit ?
		auxiliary_bit : NULL;

	if (stream->b1_active || source == NULL) {
		memset(primary_bits, 1, request->primary_bits);
		if (auxiliary != NULL)
			*auxiliary = 1;
		return true;
	}
	memset(primary_bits, 2, request->primary_bits);
	if (auxiliary != NULL)
		*auxiliary = 2;
	return source(
		       source_context, request, primary_bits, auxiliary) &&
	       bits_are_binary(primary_bits, request->primary_bits) &&
	       (auxiliary == NULL || *auxiliary <= 1u);
}

enum sippymodem_v34_data_stream_result sippymodem_v34_data_stream_sample(
	struct sippymodem_v34_data_stream *stream,
	sippymodem_v34_mapping_frame_source source, void *source_context,
	int16_t *sample, bool *symbol_accepted)
{
	struct sippymodem_v34_data_stream next;
	struct sippymodem_v34_mapping_frame request;
	uint8_t primary_bits[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary_bit;
	const uint8_t *auxiliary;
	int16_t generated_sample;
	bool generated_accepted;
	enum sippymodem_v34_data_stream_result result;

	if (!sippymodem_v34_data_stream_is_valid(stream) || sample == NULL ||
	    symbol_accepted == NULL)
		return SIPPYMODEM_V34_DATA_STREAM_INVALID_ARGUMENT;
	if (!sippymodem_v34_data_transmitter_needs_mapping_frame(
		    &stream->transmitter))
		return transmitter_result(sippymodem_v34_data_transmitter_sample(
			&stream->transmitter, sample, symbol_accepted));

	next = *stream;
	if (next.b1_active && sippymodem_v34_frame_schedule_is_complete(
				      &next.transmitter.schedule))
		next.b1_active = false;
	result = transmitter_result(
		sippymodem_v34_data_transmitter_preview_mapping_frame(
			&next.transmitter, &request));
	if (result != SIPPYMODEM_V34_DATA_STREAM_OK)
		return result;
	if (!acquire_mapping_frame(
		    &next, source, source_context, &request, primary_bits,
		    &auxiliary_bit))
		return SIPPYMODEM_V34_DATA_STREAM_SOURCE_FAILURE;
	auxiliary = request.carries_auxiliary_bit ? &auxiliary_bit : NULL;
	result = transmitter_result(
		sippymodem_v34_data_transmitter_queue_mapping_frame(
			&next.transmitter, primary_bits, request.primary_bits,
			auxiliary));
	if (result != SIPPYMODEM_V34_DATA_STREAM_OK)
		return result;
	result = transmitter_result(sippymodem_v34_data_transmitter_sample(
		&next.transmitter, &generated_sample, &generated_accepted));
	if (result != SIPPYMODEM_V34_DATA_STREAM_OK)
		return result;
	if (!sippymodem_v34_data_stream_is_valid(&next))
		return SIPPYMODEM_V34_DATA_STREAM_NUMERIC_FAILURE;
	*stream = next;
	*sample = generated_sample;
	*symbol_accepted = generated_accepted;
	return SIPPYMODEM_V34_DATA_STREAM_OK;
}

const char *sippymodem_v34_data_stream_result_name(
	enum sippymodem_v34_data_stream_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_DATA_STREAM_OK:
		return "ok";
	case SIPPYMODEM_V34_DATA_STREAM_SOURCE_FAILURE:
		return "mapping-frame source failure";
	case SIPPYMODEM_V34_DATA_STREAM_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_DATA_STREAM_OUT_OF_RANGE:
		return "out of range";
	case SIPPYMODEM_V34_DATA_STREAM_CLIPPING_RISK:
		return "clipping risk";
	case SIPPYMODEM_V34_DATA_STREAM_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
