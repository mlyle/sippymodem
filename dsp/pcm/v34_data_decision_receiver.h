// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_DECISION_RECEIVER_H
#define V34_DATA_DECISION_RECEIVER_H

#include "v34_adaptive_equalizer.h"
#include "v34_data_channel_receiver.h"

#include <stdbool.h>

enum {
	/* Unit-power Phase 3/4 points occupy this fixed receive radius. */
	V34_DATA_DECISION_FIXED_AMPLITUDE = 3238,
};

enum v34_data_decision_receiver_result {
	/* The first 2-D symbol was retained without completing an interval. */
	V34_DATA_DECISION_RECEIVER_OK,
	V34_DATA_DECISION_RECEIVER_INTERVAL_READY,
	V34_DATA_DECISION_RECEIVER_MAPPING_FRAME_READY,
	V34_DATA_DECISION_RECEIVER_B1_IN_PROGRESS,
	V34_DATA_DECISION_RECEIVER_B1_COMPLETE,
	V34_DATA_DECISION_RECEIVER_INVALID_ARGUMENT,
	V34_DATA_DECISION_RECEIVER_INVALID_STATE,
	V34_DATA_DECISION_RECEIVER_NUMERIC_FAILURE,
	V34_DATA_DECISION_RECEIVER_OUT_OF_RANGE,
};

/* Own the fixed-observation semantic data path. The first point of each 4-D
 * interval is retained until the second permits one joint channel decision. */
struct v34_data_decision_receiver {
	struct v34_data_channel_receiver data_receiver;
	struct qam_point first_normalized;
	bool has_first_symbol;
};

struct v34_data_decision_output {
	struct v34_adaptive_equalizer_decision equalizer;
	struct v34_survivor_decision current;
	struct v34_received_mapping_frame received;
};

enum v34_data_decision_receiver_result
v34_data_decision_receiver_init(
	struct v34_data_decision_receiver *receiver,
	const struct v34_power_normalizer *normalizer,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection,
	enum v34_station_role transmitting_role);

bool v34_data_decision_receiver_is_valid(
	const struct v34_data_decision_receiver *receiver);
bool v34_data_decision_receiver_is_receiving_b1(
	const struct v34_data_decision_receiver *receiver);

/* Consume one pending fixed equalizer observation. The first symbol returns a
 * SKIP decision, matching the V.34 demapper's pair cadence. The second symbol
 * returns the present-time channel target and current decision with full
 * adaptation; frame results additionally return `received`. Fields that are
 * not meaningful for a result are zero. Rejection leaves owner and output
 * unchanged. */
enum v34_data_decision_receiver_result
v34_data_decision_receiver_observe(
	struct v34_data_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	struct v34_data_decision_output *output);

#endif /* V34_DATA_DECISION_RECEIVER_H */
