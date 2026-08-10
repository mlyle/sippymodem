// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_DATA_STREAM_H
#define SIPPYMODEM_V34_DATA_STREAM_H

#include "sippymodem_v34_data_transmitter.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_data_stream_result {
	SIPPYMODEM_V34_DATA_STREAM_OK,
	SIPPYMODEM_V34_DATA_STREAM_SOURCE_FAILURE,
	SIPPYMODEM_V34_DATA_STREAM_INVALID_ARGUMENT,
	SIPPYMODEM_V34_DATA_STREAM_OUT_OF_RANGE,
	SIPPYMODEM_V34_DATA_STREAM_CLIPPING_RISK,
	SIPPYMODEM_V34_DATA_STREAM_NUMERIC_FAILURE,
};

/* Supply one complete post-B1 mapping frame. The source must initialize every
 * requested primary bit and, when non-NULL, the auxiliary bit to zero or one.
 * Returning false rejects the frame. A NULL source deliberately transmits idle
 * ones, matching an empty modem bit queue. */
typedef bool (*sippymodem_v34_mapping_frame_source)(
	void *context,
	const struct sippymodem_v34_mapping_frame *request,
	uint8_t *primary_bits, uint8_t *auxiliary_bit);

struct sippymodem_v34_data_stream {
	struct sippymodem_v34_data_transmitter transmitter;
	bool b1_active;
};

/* Take ownership of a transmitter positioned at the first mapping frame of
 * B1. The one-frame B1 payload is generated internally as binary ones; the
 * external source is not consulted until the following data frame. */
enum sippymodem_v34_data_stream_result sippymodem_v34_data_stream_init_b1(
	struct sippymodem_v34_data_stream *stream,
	const struct sippymodem_v34_data_transmitter *transmitter);

bool sippymodem_v34_data_stream_is_valid(
	const struct sippymodem_v34_data_stream *stream);
bool sippymodem_v34_data_stream_is_transmitting_b1(
	const struct sippymodem_v34_data_stream *stream);

/* Emit one line sample. Mapping-frame acquisition, queueing, B1 handoff, and
 * line emission commit together; rejected calls leave the stream and both
 * outputs unchanged. The source itself owns any external read side effects. */
enum sippymodem_v34_data_stream_result sippymodem_v34_data_stream_sample(
	struct sippymodem_v34_data_stream *stream,
	sippymodem_v34_mapping_frame_source source, void *source_context,
	int16_t *sample, bool *symbol_accepted);

const char *sippymodem_v34_data_stream_result_name(
	enum sippymodem_v34_data_stream_result result);

#endif
