// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_STREAM_H
#define V34_DATA_STREAM_H

#include "v34_data_transmitter.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_data_stream_result {
	V34_DATA_STREAM_OK,
	V34_DATA_STREAM_SOURCE_FAILURE,
	V34_DATA_STREAM_INVALID_ARGUMENT,
	V34_DATA_STREAM_OUT_OF_RANGE,
	V34_DATA_STREAM_CLIPPING_RISK,
	V34_DATA_STREAM_NUMERIC_FAILURE,
};

/* Supply one complete post-B1 mapping frame. The source must initialize every
 * requested primary bit and, when non-NULL, the auxiliary bit to zero or one.
 * Returning false rejects the frame. A NULL source deliberately transmits idle
 * ones, matching an empty modem bit queue. */
typedef bool (*v34_mapping_frame_source)(
	void *context,
	const struct v34_mapping_frame *request,
	uint8_t *primary_bits, uint8_t *auxiliary_bit);

struct v34_data_stream {
	struct v34_data_transmitter transmitter;
	bool b1_active;
};

/* The negotiated settings needed to enter the data phase. The Phase 3
 * profile owns the exact carrier and symbol rates; the remaining fields are
 * the selected Phase 4 transmit settings and local line interface. */
struct v34_data_stream_configuration {
	struct v34_phase3_profile profile;
	struct v34_q14_coefficient
		precoder_coefficients[V34_PRECODER_TAP_COUNT];
	uint32_t sample_rate;
	int16_t nominal_peak_amplitude;
	enum v34_trellis_selection trellis_selection;
	enum v34_nonlinear_parameter nonlinear_parameter;
	uint8_t primary_data_rate_2400_bps_units;
	bool auxiliary_channel_enabled;
	bool expanded_shaping;
};

/* Derive and calibrate a complete transmitter at the first B1 mapping frame.
 * The destination and configuration remain unchanged on every failure. */
enum v34_data_stream_result v34_data_stream_init(
	struct v34_data_stream *stream,
	const struct v34_data_stream_configuration *configuration);

/* Take ownership of a transmitter positioned at the first mapping frame of
 * B1. The one-frame B1 payload is generated internally as binary ones; the
 * external source is not consulted until the following data frame. */
enum v34_data_stream_result v34_data_stream_init_b1(
	struct v34_data_stream *stream,
	const struct v34_data_transmitter *transmitter);

bool v34_data_stream_is_valid(
	const struct v34_data_stream *stream);
bool v34_data_stream_is_transmitting_b1(
	const struct v34_data_stream *stream);

/* Emit one line sample. Mapping-frame acquisition, queueing, B1 handoff, and
 * line emission commit together; rejected calls leave the stream and both
 * outputs unchanged. The source itself owns any external read side effects. */
enum v34_data_stream_result v34_data_stream_sample(
	struct v34_data_stream *stream,
	v34_mapping_frame_source source, void *source_context,
	int16_t *sample, bool *symbol_accepted);

const char *v34_data_stream_result_name(
	enum v34_data_stream_result result);

#endif
