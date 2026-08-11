// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_H
#define SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_H

#include "v34_channel_decoder.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_channel_frame_decoder_result {
	SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_OK,
	SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT,
	SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_NUMERIC_FAILURE,
	SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_OUT_OF_RANGE,
};

struct sippymodem_v34_channel_frame_decoder {
	struct sippymodem_v34_channel_decoder channel_decoder;
	/* Present-time schedule, independent of the delayed decided stream. */
	struct sippymodem_v34_frame_schedule channel_schedule;
	struct sippymodem_v34_mapping_frame channel_frame;
	struct sippymodem_v34_decided_4d_interval
		pending_frame[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME];
	uint8_t next_channel_interval;
	uint8_t pending_interval_count;
};

/* Start at the first 4-D interval of the peer's B1 data frame. Mapping
 * parameters, scheduled inversion, trellis history, and inverse-precoder
 * history are initialized together. The destination is unchanged on
 * rejection. */
enum sippymodem_v34_channel_frame_decoder_result
sippymodem_v34_channel_frame_decoder_init(
	struct sippymodem_v34_channel_frame_decoder *decoder,
	const struct sippymodem_v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct sippymodem_v34_q14_coefficient
		coefficients[SIPPYMODEM_V34_PRECODER_TAP_COUNT],
	enum sippymodem_v34_trellis_selection selection);

bool sippymodem_v34_channel_frame_decoder_is_valid(
	const struct sippymodem_v34_channel_frame_decoder *decoder);

/* Accept one present-time pair of conditioned channel estimates. The
 * scheduled V0 bit is supplied internally, before advancing the independent
 * channel schedule. `current` always receives the present-time hard decision.
 * Once traceback has produced four ordered intervals, `decided_frame` receives
 * one complete mapping frame and `frame_ready` is true; otherwise that array is
 * unchanged. State and every output are unchanged on failure. */
enum sippymodem_v34_channel_frame_decoder_result
sippymodem_v34_channel_frame_decoder_advance(
	struct sippymodem_v34_channel_frame_decoder *decoder,
	const struct sippymodem_qam_point
		estimates[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct sippymodem_v34_survivor_decision *current,
	struct sippymodem_v34_decided_4d_interval
		decided_frame[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME],
	bool *frame_ready);

const char *sippymodem_v34_channel_frame_decoder_result_name(
	enum sippymodem_v34_channel_frame_decoder_result result);

#endif /* SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_H */
