// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_H
#define SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_H

#include "v34_channel_conditioner.h"
#include "v34_channel_frame_decoder.h"

#include <stdbool.h>

enum sippymodem_v34_channel_frame_receiver_result {
	SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OK,
	SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT,
	SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_NUMERIC_FAILURE,
	SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OUT_OF_RANGE,
};

struct sippymodem_v34_channel_frame_receiver {
	struct sippymodem_v34_channel_conditioner conditioner;
	/* Present-time decision feedback, separate from delayed traceback. */
	struct sippymodem_v34_precoder receive_precoder;
	struct sippymodem_v34_channel_frame_decoder frame_decoder;
};

/* Start at the first 4-D interval of the peer's B1 data frame. The normalizer
 * and mapping selection must describe that peer's negotiated transmitter.
 * All configuration and mutable history are owned by the destination; calls
 * on separate instances may proceed concurrently. */
enum sippymodem_v34_channel_frame_receiver_result
sippymodem_v34_channel_frame_receiver_init(
	struct sippymodem_v34_channel_frame_receiver *receiver,
	const struct sippymodem_v34_power_normalizer *normalizer,
	const struct sippymodem_v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct sippymodem_v34_q14_coefficient
		coefficients[SIPPYMODEM_V34_PRECODER_TAP_COUNT],
	enum sippymodem_v34_trellis_selection selection);

bool sippymodem_v34_channel_frame_receiver_is_valid(
	const struct sippymodem_v34_channel_frame_receiver *receiver);

/* Accept one equalized, unit-average-power pair after carrier and timing
 * recovery. The receiver reverses power normalization and nonlinear
 * projection, reconstructs y(n), applies scheduled trellis decoding, and
 * corrects its present-time feedback history from the hard decision.
 * `current` always receives that decision. `decision_targets` receives the
 * corresponding exact nonlinear-projected, power-normalized line symbols for
 * decision-directed channel adaptation. `decided_frame` changes only when
 * `frame_ready` becomes true after traceback. State and every output are
 * unchanged on failure. Advance one instance from only one thread at a time. */
enum sippymodem_v34_channel_frame_receiver_result
sippymodem_v34_channel_frame_receiver_advance(
	struct sippymodem_v34_channel_frame_receiver *receiver,
	const struct sippymodem_qam_point
		normalized[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct sippymodem_v34_survivor_decision *current,
	struct sippymodem_qam_point
		decision_targets[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct sippymodem_v34_decided_4d_interval
		decided_frame[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME],
	bool *frame_ready);

const char *sippymodem_v34_channel_frame_receiver_result_name(
	enum sippymodem_v34_channel_frame_receiver_result result);

#endif /* SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_H */
