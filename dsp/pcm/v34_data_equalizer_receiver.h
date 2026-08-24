// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_EQUALIZER_RECEIVER_H
#define V34_DATA_EQUALIZER_RECEIVER_H

#include "qam_equalizer.h"
#include "v34_data_channel_receiver.h"
#include "v34_data_receive.h"
#include "v34_equalizer_handoff.h"

#include <stdbool.h>
#include <stdint.h>

/* Own the symbol-spaced equalizer and normalized-symbol data path. The first
 * equalized symbol and its delayed gradient remain explicit until the second
 * symbol permits one joint four-dimensional decision. */
struct v34_data_equalizer_receiver {
	struct qam_equalizer equalizer;
	struct qam_equalizer_observation first_observation;
	struct v34_data_channel_receiver data_receiver;
	double adaptation;
};

/* Continue from a completed Phase 4 E detector. The handoff already retains
 * the first equalizer-delay B1 inputs, so no outputs are discarded. */
enum v34_data_receive_result
v34_data_equalizer_receiver_init_from_handoff(
	struct v34_data_equalizer_receiver *receiver,
	const struct v34_equalizer_handoff *handoff,
	const struct v34_power_normalizer *normalizer,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection,
	enum v34_station_role transmitting_role,
	double adaptation);

bool v34_data_equalizer_receiver_is_valid(
	const struct v34_data_equalizer_receiver *receiver);
bool v34_data_equalizer_receiver_is_receiving_b1(
	const struct v34_data_equalizer_receiver *receiver);

/* Accept one finite, carrier- and timing-recovered QAM point. `current`
 * changes for every interval result. `received` changes only for a mapping or
 * B1 result. Receiver state and both outputs are unchanged on failure. */
enum v34_data_receive_result
v34_data_equalizer_receive_point(
	struct v34_data_equalizer_receiver *receiver,
	const struct qam_point *received_point,
	struct v34_survivor_decision *current,
	struct v34_received_mapping_frame *received);

#endif /* V34_DATA_EQUALIZER_RECEIVER_H */
