// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_DOWNSTREAM_DATA_STREAM_H
#define V90_DOWNSTREAM_DATA_STREAM_H

#include "pcm_frame_source.h"
#include "v90_downstream_data_handoff.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v90_downstream_data_stream_result {
	V90_DOWNSTREAM_DATA_STREAM_OK,
	V90_DOWNSTREAM_DATA_STREAM_COMPLETE,
	V90_DOWNSTREAM_DATA_STREAM_SOURCE_FAILURE,
	V90_DOWNSTREAM_DATA_STREAM_INVALID_ARGUMENT,
	V90_DOWNSTREAM_DATA_STREAM_INVALID_PARAMETERS,
	V90_DOWNSTREAM_DATA_STREAM_INVALID_STATE,
	V90_DOWNSTREAM_DATA_STREAM_OUT_OF_RANGE,
	V90_DOWNSTREAM_DATA_STREAM_NUMERIC_FAILURE,
	V90_DOWNSTREAM_DATA_STREAM_INTERNAL_ERROR,
};

/* Adapt the six-symbol PCM frame encoder to the authoritative 8 ksymbol/s
 * network clock. The fixed queue owns one frame in its unconsumed suffix;
 * spectral lookahead remains bounded inside the encoder. */
struct v90_downstream_data_stream {
	struct v90_downstream_data_tx transmitter;
	struct v90_pcm_symbol queue[V90_CP_FRAME_INTERVALS];
	uint64_t accepted_frames;
	uint64_t transmitted_symbols;
	uint8_t queue_count;
};

/* Start the fresh B1d epoch at the exact boundary after a completed Ed. */
enum v90_downstream_data_stream_result
v90_downstream_data_stream_start_after_ed(
	struct v90_downstream_data_stream *stream,
	const struct pcm_downstream_tx *completed_training,
	const struct v90_pcm_mapping *data_mapping);

bool v90_downstream_data_stream_is_valid(
	const struct v90_downstream_data_stream *stream);

/* True after every B1d network character has left the stream. */
bool v90_downstream_data_stream_is_online(
	const struct v90_downstream_data_stream *stream);

/* End source acquisition and emit every symbol already accepted by the
 * frame encoder. COMPLETE consumes no network character. A complete stream
 * always ends on the six-symbol data-frame boundary required before Rd. */
enum v90_downstream_data_stream_result v90_downstream_data_stream_finish(
	struct v90_downstream_data_stream *stream,
	struct v90_pcm_symbol *symbol);

/* Produce exactly one authoritative network character. Frame acquisition,
 * encoding, queueing, and emission commit together; rejected calls leave the
 * stream and output unchanged. The source owns any external read side effects
 * and is called at most once. */
enum v90_downstream_data_stream_result v90_downstream_data_stream_next(
	struct v90_downstream_data_stream *stream,
	pcm_frame_source source, void *source_context,
	struct v90_pcm_symbol *symbol);

/* Call-owner path: a valid stream was established at the epoch boundary.
 * Keep frame-source and coding failures transactional without rescanning the
 * complete mapping and queue for each network character. */
enum v90_downstream_data_stream_result v90_downstream_data_stream_next_owned(
	struct v90_downstream_data_stream *stream,
	pcm_frame_source source, void *source_context,
	struct v90_pcm_symbol *symbol);

#endif /* V90_DOWNSTREAM_DATA_STREAM_H */
