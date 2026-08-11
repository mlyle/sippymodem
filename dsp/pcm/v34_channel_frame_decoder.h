// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_CHANNEL_FRAME_DECODER_H
#define V34_CHANNEL_FRAME_DECODER_H

#include "v34_channel_decoder.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_channel_frame_decoder_result {
	V34_CHANNEL_FRAME_DECODER_OK,
	V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT,
	V34_CHANNEL_FRAME_DECODER_NUMERIC_FAILURE,
	V34_CHANNEL_FRAME_DECODER_OUT_OF_RANGE,
};

struct v34_channel_frame_decoder {
	struct v34_channel_decoder channel_decoder;
	/* Present-time schedule, independent of the delayed decided stream. */
	struct v34_frame_schedule channel_schedule;
	struct v34_mapping_frame channel_frame;
	struct v34_decided_4d_interval
		pending_frame[V34_4D_INTERVALS_PER_MAPPING_FRAME];
	uint8_t next_channel_interval;
	uint8_t pending_interval_count;
};

/* Start at the first 4-D interval of the peer's B1 data frame. Mapping
 * parameters, scheduled inversion, trellis history, and inverse-precoder
 * history are initialized together. The destination is unchanged on
 * rejection. */
enum v34_channel_frame_decoder_result
v34_channel_frame_decoder_init(
	struct v34_channel_frame_decoder *decoder,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection);

bool v34_channel_frame_decoder_is_valid(
	const struct v34_channel_frame_decoder *decoder);

/* Accept one present-time pair of conditioned channel estimates. The
 * scheduled V0 bit is supplied internally, before advancing the independent
 * channel schedule. `current` always receives the present-time hard decision.
 * Once traceback has produced four ordered intervals, `decided_frame` receives
 * one complete mapping frame and `frame_ready` is true; otherwise that array is
 * unchanged. State and every output are unchanged on failure. */
enum v34_channel_frame_decoder_result
v34_channel_frame_decoder_advance(
	struct v34_channel_frame_decoder *decoder,
	const struct sippymodem_qam_point
		estimates[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct v34_survivor_decision *current,
	struct v34_decided_4d_interval
		decided_frame[V34_4D_INTERVALS_PER_MAPPING_FRAME],
	bool *frame_ready);

const char *v34_channel_frame_decoder_result_name(
	enum v34_channel_frame_decoder_result result);

#endif /* V34_CHANNEL_FRAME_DECODER_H */
