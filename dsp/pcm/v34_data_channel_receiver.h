// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_CHANNEL_RECEIVER_H
#define V34_DATA_CHANNEL_RECEIVER_H

#include "v34_channel_frame_receiver.h"
#include "v34_data_receive.h"
#include "v34_data_receiver.h"

#include <stdbool.h>

/* Own the complete normalized-symbol to descrambled mapping-frame path. */
struct v34_data_channel_receiver {
	struct v34_channel_frame_receiver channel_receiver;
	struct v34_data_receiver data_receiver;
};

/* Start both present-time channel decoding and delayed data decoding at the
 * first 4-D interval of the peer's B1 frame. All negotiated configuration is
 * copied into the receiver. */
enum v34_data_receive_result
v34_data_channel_receiver_init(
	struct v34_data_channel_receiver *receiver,
	const struct v34_power_normalizer *normalizer,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection,
	enum v34_station_role transmitting_role);

bool v34_data_channel_receiver_is_valid(
	const struct v34_data_channel_receiver *receiver);
bool v34_data_channel_receiver_is_receiving_b1(
	const struct v34_data_channel_receiver *receiver);

/* Accept one equalized, unit-average-power 4-D interval. `current` and
 * `decision_targets` always receive the present-time hard channel decision
 * and its exact normalized line symbols after a successful call. `received`
 * changes only for MAPPING_FRAME_READY or either B1 result. State and every
 * output are unchanged on failure. Separate instances are independent;
 * serialize calls that advance the same instance. */
enum v34_data_receive_result
v34_data_channel_receiver_advance(
	struct v34_data_channel_receiver *receiver,
	const struct qam_point
		normalized[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct v34_survivor_decision *current,
	struct qam_point
		decision_targets[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct v34_received_mapping_frame *received);

#endif /* V34_DATA_CHANNEL_RECEIVER_H */
