// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_DATA_STREAM_H
#define SIPPYMODEM_V34_DATA_STREAM_H

#include "v34_data_transmitter.h"

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

/* The negotiated settings needed to enter the data phase. The Phase 3
 * profile owns the exact carrier and symbol rates; the remaining fields are
 * the selected Phase 4 transmit settings and local line interface. */
struct sippymodem_v34_data_stream_configuration {
	struct sippymodem_v34_phase3_profile profile;
	struct sippymodem_v34_q14_coefficient
		precoder_coefficients[SIPPYMODEM_V34_PRECODER_TAP_COUNT];
	uint32_t sample_rate;
	int16_t nominal_peak_amplitude;
	enum sippymodem_v34_trellis_selection trellis_selection;
	enum sippymodem_v34_nonlinear_parameter nonlinear_parameter;
	uint8_t primary_data_rate_2400_bps_units;
	bool auxiliary_channel_enabled;
	bool expanded_shaping;
};

/* Derive and calibrate a complete transmitter at the first B1 mapping frame.
 * The destination and configuration remain unchanged on every failure. */
enum sippymodem_v34_data_stream_result sippymodem_v34_data_stream_init(
	struct sippymodem_v34_data_stream *stream,
	const struct sippymodem_v34_data_stream_configuration *configuration);

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
