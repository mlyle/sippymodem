// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_DOWNSTREAM_DATA_STREAM_H
#define V90_DOWNSTREAM_DATA_STREAM_H

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

enum {
	V90_RD_SYMBOLS = 384,
	V90_RD_BAR_SYMBOLS = 24,
	V90_RD_SEQUENCE_SYMBOLS =
		V90_RD_SYMBOLS + V90_RD_BAR_SYMBOLS,
};

enum v90_rd_tx_result {
	V90_RD_TX_OK,
	V90_RD_TX_COMPLETE,
	V90_RD_TX_INVALID_ARGUMENT,
	V90_RD_TX_INVALID_PARAMETERS,
	V90_RD_TX_INTERNAL_ERROR,
};

/* V.90 8.6.4 Rd/Rd-bar uses the highest-power member of each current
 * data-mode constellation. It is deliberately independent of the data
 * encoder: neither signal is scrambled, differentially encoded, or shaped. */
struct v90_rd_tx {
	struct v90_pcm_mapping mapping;
	uint16_t transmitted_symbols;
};

/* Supply one complete post-B1d PCM data frame. The source must initialize all
 * requested bits to zero or one. Returning false rejects the frame. A NULL
 * source deliberately transmits idle ones. */
typedef bool (*v90_downstream_frame_source)(
	void *context, uint8_t *bits, size_t bit_count);

/* Adapt the six-symbol PCM frame encoder to the authoritative 8 ksymbol/s
 * network clock. The fixed queue owns one emitted frame; spectral lookahead
 * remains bounded inside the encoder. */
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
	v90_downstream_frame_source source, void *source_context,
	struct v90_pcm_symbol *symbol);

enum v90_rd_tx_result v90_rd_tx_init(
	struct v90_rd_tx *transmitter,
	const struct v90_pcm_mapping *current_data_mapping);
bool v90_rd_tx_is_valid(const struct v90_rd_tx *transmitter);
bool v90_rd_tx_is_complete(const struct v90_rd_tx *transmitter);

/* Emit exactly 384T of Rd followed by 24T of Rd-bar. COMPLETE consumes no
 * character and leaves symbol unchanged. */
enum v90_rd_tx_result v90_rd_tx_next(
	struct v90_rd_tx *transmitter, struct v90_pcm_symbol *symbol);

const char *v90_downstream_data_stream_result_name(
	enum v90_downstream_data_stream_result result);

#endif /* V90_DOWNSTREAM_DATA_STREAM_H */
