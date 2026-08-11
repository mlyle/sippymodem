// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_CHANNEL_FRAME_RECEIVER_H
#define V34_CHANNEL_FRAME_RECEIVER_H

#include "v34_channel_conditioner.h"
#include "v34_channel_frame_decoder.h"

#include <stdbool.h>

enum v34_channel_frame_receiver_result {
	V34_CHANNEL_FRAME_RECEIVER_OK,
	V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT,
	V34_CHANNEL_FRAME_RECEIVER_NUMERIC_FAILURE,
	V34_CHANNEL_FRAME_RECEIVER_OUT_OF_RANGE,
};

struct v34_channel_frame_receiver {
	struct v34_channel_conditioner conditioner;
	/* Present-time decision feedback, separate from delayed traceback. */
	struct v34_precoder receive_precoder;
	struct v34_channel_frame_decoder frame_decoder;
};

/* Start at the first 4-D interval of the peer's B1 data frame. The normalizer
 * and mapping selection must describe that peer's negotiated transmitter.
 * All configuration and mutable history are owned by the destination; calls
 * on separate instances may proceed concurrently. */
enum v34_channel_frame_receiver_result
v34_channel_frame_receiver_init(
	struct v34_channel_frame_receiver *receiver,
	const struct v34_power_normalizer *normalizer,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection);

bool v34_channel_frame_receiver_is_valid(
	const struct v34_channel_frame_receiver *receiver);

/* Accept one equalized, unit-average-power pair after carrier and timing
 * recovery. The receiver reverses power normalization and nonlinear
 * projection, reconstructs y(n), applies scheduled trellis decoding, and
 * corrects its present-time feedback history from the hard decision.
 * `current` always receives that decision. `decision_targets` receives the
 * corresponding exact nonlinear-projected, power-normalized line symbols for
 * decision-directed channel adaptation. `decided_frame` changes only when
 * `frame_ready` becomes true after traceback. State and every output are
 * unchanged on failure. Advance one instance from only one thread at a time. */
enum v34_channel_frame_receiver_result
v34_channel_frame_receiver_advance(
	struct v34_channel_frame_receiver *receiver,
	const struct sippymodem_qam_point
		normalized[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct v34_survivor_decision *current,
	struct sippymodem_qam_point
		decision_targets[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct v34_decided_4d_interval
		decided_frame[V34_4D_INTERVALS_PER_MAPPING_FRAME],
	bool *frame_ready);

const char *v34_channel_frame_receiver_result_name(
	enum v34_channel_frame_receiver_result result);

#endif /* V34_CHANNEL_FRAME_RECEIVER_H */
