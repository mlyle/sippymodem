// SPDX-License-Identifier: BSD-3-Clause

#include "v90_downstream_data_stream.h"

#include "memory_region.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	V90_B1D_SYMBOLS =
		V90_PHASE4_B1D_FRAMES * V90_CP_FRAME_INTERVALS,
};

static uint64_t delayed_symbols(
	const struct v90_downstream_data_stream *stream)
{
	const struct v90_pcm_mapping *mapping =
		&stream->transmitter.encoder.mapping;

	if (mapping->sign_redundancy == 0)
		return 0;
	return (uint64_t)stream->transmitter.encoder.shaper.pending_count *
	       (V90_CP_FRAME_INTERVALS / mapping->sign_redundancy);
}

static bool queue_is_valid(
	const struct v90_downstream_data_stream *stream)
{
	enum g711_law law = stream->transmitter.encoder.mapping.law;

	if (stream->queue_count > V90_CP_FRAME_INTERVALS)
		return false;
	for (size_t index = 0; index < V90_CP_FRAME_INTERVALS; ++index) {
		if (index >= (size_t)V90_CP_FRAME_INTERVALS - stream->queue_count) {
			if (!v90_pcm_symbol_is_valid(&stream->queue[index], law))
				return false;
		} else if (!memory_region_is_zero(
				   &stream->queue[index], sizeof(stream->queue[index]))) {
			return false;
		}
	}
	return true;
}

bool v90_downstream_data_stream_is_valid(
	const struct v90_downstream_data_stream *stream)
{
	const struct v90_pcm_mapping *mapping;
	uint64_t accepted_symbols;
	uint64_t delayed;

	if (stream == NULL ||
	    !v90_downstream_data_tx_is_valid(&stream->transmitter) ||
	    !queue_is_valid(stream) ||
	    stream->accepted_frames >
		    UINT64_MAX / V90_CP_FRAME_INTERVALS ||
	    stream->transmitter.b1d_frames !=
		    (stream->accepted_frames < V90_PHASE4_B1D_FRAMES ?
			     stream->accepted_frames : V90_PHASE4_B1D_FRAMES) ||
	    (stream->transmitter.stage == V90_DOWNSTREAM_DATA_TX_STAGE_B1D &&
	     stream->accepted_frames >= V90_PHASE4_B1D_FRAMES) ||
	    (stream->transmitter.stage != V90_DOWNSTREAM_DATA_TX_STAGE_B1D &&
	     stream->accepted_frames < V90_PHASE4_B1D_FRAMES))
		return false;
	mapping = &stream->transmitter.encoder.mapping;
	if (stream->transmitter.stage == V90_DOWNSTREAM_DATA_TX_STAGE_DATA &&
	    mapping->sign_redundancy != 0 &&
	    stream->transmitter.encoder.shaper.pending_count !=
		    mapping->spectral_lookahead)
		return false;
	accepted_symbols =
		stream->accepted_frames * V90_CP_FRAME_INTERVALS;
	delayed = delayed_symbols(stream);
	return stream->transmitted_symbols <= accepted_symbols &&
	       delayed <= accepted_symbols - stream->transmitted_symbols &&
	       stream->queue_count <=
		       accepted_symbols - stream->transmitted_symbols - delayed &&
	       stream->transmitted_symbols + stream->queue_count + delayed ==
		       accepted_symbols;
}

enum v90_downstream_data_stream_result
v90_downstream_data_stream_start_after_ed(
	struct v90_downstream_data_stream *stream,
	const struct pcm_downstream_tx *completed_training,
	const struct v90_pcm_mapping *data_mapping)
{
	struct v90_downstream_data_stream initialized = { 0 };
	enum v90_downstream_data_tx_result result;

	if (stream == NULL || completed_training == NULL ||
	    data_mapping == NULL)
		return V90_DOWNSTREAM_DATA_STREAM_INVALID_ARGUMENT;
	result = v90_downstream_data_tx_start_after_ed(
		&initialized.transmitter, completed_training, data_mapping);
	if (result == V90_DOWNSTREAM_DATA_TX_INVALID_STATE)
		return V90_DOWNSTREAM_DATA_STREAM_INVALID_STATE;
	if (result == V90_DOWNSTREAM_DATA_TX_INVALID_ARGUMENT)
		return V90_DOWNSTREAM_DATA_STREAM_INVALID_ARGUMENT;
	if (result == V90_DOWNSTREAM_DATA_TX_INVALID_PARAMETERS)
		return V90_DOWNSTREAM_DATA_STREAM_INVALID_PARAMETERS;
	if (result != V90_DOWNSTREAM_DATA_TX_OK ||
	    !v90_downstream_data_stream_is_valid(&initialized))
		return V90_DOWNSTREAM_DATA_STREAM_INTERNAL_ERROR;
	*stream = initialized;
	return V90_DOWNSTREAM_DATA_STREAM_OK;
}

bool v90_downstream_data_stream_is_online(
	const struct v90_downstream_data_stream *stream)
{
	return v90_downstream_data_stream_is_valid(stream) &&
	       stream->transmitted_symbols >= V90_B1D_SYMBOLS;
}

static bool fill_queue(
	struct v90_downstream_data_stream *stream,
	const struct v90_pcm_frame_output *output)
{
	enum g711_law law = stream->transmitter.encoder.mapping.law;

	if (stream->queue_count != 0 || output->symbol_count == 0 ||
	    output->symbol_count > V90_CP_FRAME_INTERVALS)
		return false;
	memset(stream->queue, 0, sizeof(stream->queue));
	for (size_t index = 0; index < output->symbol_count; ++index) {
		if (!v90_pcm_symbol_is_valid(&output->symbols[index], law))
			return false;
		/* The queue has a deterministic representation for transactional
		 * snapshots; a frame's temporary symbol padding is not owned data. */
		size_t queued = V90_CP_FRAME_INTERVALS - output->symbol_count + index;

		stream->queue[queued].ucode = output->symbols[index].ucode;
		stream->queue[queued].positive = output->symbols[index].positive;
		stream->queue[queued].octet = output->symbols[index].octet;
		stream->queue[queued].sample = output->symbols[index].sample;
	}
	stream->queue_count = output->symbol_count;
	return true;
}

static enum v90_downstream_data_stream_result generate_frame(
	struct v90_downstream_data_stream *stream,
	pcm_frame_source source, void *source_context)
{
	struct v90_pcm_frame_output output;
	enum v90_downstream_data_tx_result result;

	if (stream->accepted_frames ==
	    UINT64_MAX / V90_CP_FRAME_INTERVALS)
		return V90_DOWNSTREAM_DATA_STREAM_OUT_OF_RANGE;
	if (stream->transmitter.stage ==
	    V90_DOWNSTREAM_DATA_TX_STAGE_B1D) {
		result = v90_downstream_data_tx_next_b1d(
			&stream->transmitter, &output);
	} else {
		uint8_t bits[V90_PCM_FRAME_MAX_BITS];
		size_t bit_count = stream->transmitter.encoder.mapping.frame_bits;

		if (!pcm_frame_source_read(source, source_context, bits, bit_count))
			return V90_DOWNSTREAM_DATA_STREAM_SOURCE_FAILURE;
		result = v90_downstream_data_tx_process(
			&stream->transmitter, bits, bit_count, &output);
	}
	if (result == V90_DOWNSTREAM_DATA_TX_NUMERIC_FAILURE)
		return V90_DOWNSTREAM_DATA_STREAM_NUMERIC_FAILURE;
	if (result != V90_DOWNSTREAM_DATA_TX_BUFFERING &&
	    result != V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY)
		return result == V90_DOWNSTREAM_DATA_TX_INVALID_BITS ?
			V90_DOWNSTREAM_DATA_STREAM_SOURCE_FAILURE :
			V90_DOWNSTREAM_DATA_STREAM_INTERNAL_ERROR;
	stream->accepted_frames++;
	if (result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY &&
	    !fill_queue(stream, &output))
		return V90_DOWNSTREAM_DATA_STREAM_INTERNAL_ERROR;
	return V90_DOWNSTREAM_DATA_STREAM_OK;
}

static struct v90_pcm_symbol pop_symbol(
	struct v90_downstream_data_stream *stream)
{
	size_t index = V90_CP_FRAME_INTERVALS - stream->queue_count;
	struct v90_pcm_symbol symbol = stream->queue[index];

	memset(&stream->queue[index], 0, sizeof(stream->queue[index]));
	stream->queue_count--;
	stream->transmitted_symbols++;
	return symbol;
}

static enum v90_downstream_data_stream_result next_symbol(
	struct v90_downstream_data_stream *stream,
	pcm_frame_source source, void *source_context,
	struct v90_pcm_symbol *symbol, bool owned)
{
	struct v90_downstream_data_stream next;
	struct v90_pcm_symbol generated;
	enum v90_downstream_data_stream_result result =
		V90_DOWNSTREAM_DATA_STREAM_OK;

	if (stream == NULL || symbol == NULL ||
	    memory_regions_overlap(stream, sizeof(*stream),
				   symbol, sizeof(*symbol)) ||
	    (!owned && !v90_downstream_data_stream_is_valid(stream)))
		return V90_DOWNSTREAM_DATA_STREAM_INVALID_ARGUMENT;
	if (stream->transmitter.stage != V90_DOWNSTREAM_DATA_TX_STAGE_B1D &&
	    stream->transmitter.stage != V90_DOWNSTREAM_DATA_TX_STAGE_DATA)
		return V90_DOWNSTREAM_DATA_STREAM_INVALID_STATE;
	if (stream->queue_count != 0u) {
		*symbol = pop_symbol(stream);
		return V90_DOWNSTREAM_DATA_STREAM_OK;
	}
	next = *stream;
	for (size_t attempt = 0;
	     next.queue_count == 0 &&
	     next.transmitter.stage == V90_DOWNSTREAM_DATA_TX_STAGE_B1D &&
	     attempt <= V90_SPECTRAL_MAX_LOOKAHEAD;
	     ++attempt) {
		result = generate_frame(&next, NULL, NULL);
		if (result != V90_DOWNSTREAM_DATA_STREAM_OK)
			return result;
	}
	if (next.queue_count == 0 &&
	    next.transmitter.stage == V90_DOWNSTREAM_DATA_TX_STAGE_DATA) {
		result = generate_frame(&next, source, source_context);
		if (result != V90_DOWNSTREAM_DATA_STREAM_OK)
			return result;
	}
	if (next.queue_count == 0)
		return V90_DOWNSTREAM_DATA_STREAM_INTERNAL_ERROR;
	generated = pop_symbol(&next);
	if (!owned && !v90_downstream_data_stream_is_valid(&next))
		return V90_DOWNSTREAM_DATA_STREAM_INTERNAL_ERROR;
	*stream = next;
	*symbol = generated;
	return V90_DOWNSTREAM_DATA_STREAM_OK;
}

enum v90_downstream_data_stream_result v90_downstream_data_stream_next(
	struct v90_downstream_data_stream *stream,
	pcm_frame_source source, void *source_context,
	struct v90_pcm_symbol *symbol)
{
	return next_symbol(stream, source, source_context, symbol, false);
}

enum v90_downstream_data_stream_result v90_downstream_data_stream_next_owned(
	struct v90_downstream_data_stream *stream,
	pcm_frame_source source, void *source_context,
	struct v90_pcm_symbol *symbol)
{
	return next_symbol(stream, source, source_context, symbol, true);
}

enum v90_downstream_data_stream_result v90_downstream_data_stream_finish(
	struct v90_downstream_data_stream *stream,
	struct v90_pcm_symbol *symbol)
{
	struct v90_downstream_data_stream next;
	struct v90_pcm_symbol generated;

	if (stream == NULL || symbol == NULL ||
	    memory_regions_overlap(stream, sizeof(*stream),
				   symbol, sizeof(*symbol)) ||
	    !v90_downstream_data_stream_is_valid(stream))
		return V90_DOWNSTREAM_DATA_STREAM_INVALID_ARGUMENT;
	if (stream->transmitter.stage == V90_DOWNSTREAM_DATA_TX_STAGE_B1D)
		return V90_DOWNSTREAM_DATA_STREAM_INVALID_STATE;
	next = *stream;
	if (next.queue_count == 0) {
		struct v90_pcm_frame_output output;
		enum v90_downstream_data_tx_result result =
			v90_downstream_data_tx_finish(
				&next.transmitter, &output);

		if (result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY) {
			if (!fill_queue(&next, &output))
				return V90_DOWNSTREAM_DATA_STREAM_INTERNAL_ERROR;
		} else if (result == V90_DOWNSTREAM_DATA_TX_COMPLETE) {
			if (next.transmitted_symbols %
				    V90_CP_FRAME_INTERVALS != 0 ||
			    !v90_downstream_data_stream_is_valid(&next))
				return V90_DOWNSTREAM_DATA_STREAM_INTERNAL_ERROR;
			*stream = next;
			return V90_DOWNSTREAM_DATA_STREAM_COMPLETE;
		} else if (result == V90_DOWNSTREAM_DATA_TX_NUMERIC_FAILURE) {
			return V90_DOWNSTREAM_DATA_STREAM_NUMERIC_FAILURE;
		} else {
			return V90_DOWNSTREAM_DATA_STREAM_INTERNAL_ERROR;
		}
	}
	generated = pop_symbol(&next);
	if (!v90_downstream_data_stream_is_valid(&next))
		return V90_DOWNSTREAM_DATA_STREAM_INTERNAL_ERROR;
	*stream = next;
	*symbol = generated;
	return V90_DOWNSTREAM_DATA_STREAM_OK;
}
