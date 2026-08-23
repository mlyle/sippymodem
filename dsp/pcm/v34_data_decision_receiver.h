// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_DECISION_RECEIVER_H
#define V34_DATA_DECISION_RECEIVER_H

#include "v34_adaptive_equalizer.h"
#include "v34_data_channel_receiver.h"
#include "v34_data_points.h"
#include "v34_data_receive.h"

#include <stdbool.h>

enum {
	/* Unit-power Phase 3/4 points occupy this fixed receive radius. */
	V34_DATA_DECISION_FIXED_AMPLITUDE = 3238,
};

/* Own the fixed-observation semantic data path. The first point of each 4-D
 * interval is retained until the second permits one joint channel decision. */
struct v34_data_decision_receiver {
	struct v34_data_channel_receiver data_receiver;
	/* B1 is a complete known all-one data frame.  Retain an independent
	 * transmit-side mapper so its exact negotiated points can train the
	 * continuous receive equalizer without trusting noisy hard decisions. */
	struct v34_data_points b1_reference;
	struct qam_point first_normalized;
	bool has_first_symbol;
	bool b1_reference_active;
};

struct v34_data_decision_output {
	struct v34_adaptive_equalizer_decision equalizer;
	struct v34_survivor_decision current;
	struct v34_received_mapping_frame received;
};

enum v34_data_receive_result
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

/* Consume one pending fixed equalizer observation. During the physical B1
 * frame, each symbol returns its exact known target with full adaptation and
 * the semantic decoder advances from those known points. Ordinary data keeps
 * the pair cadence: the first symbol returns SKIP and the second returns the
 * present-time hard-decision target with full adaptation. Frame results also
 * return `received`; other fields are zero. Rejection leaves owner and output
 * unchanged. */
enum v34_data_receive_result
v34_data_decision_receiver_observe(
	struct v34_data_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	struct v34_data_decision_output *output);

#endif /* V34_DATA_DECISION_RECEIVER_H */
