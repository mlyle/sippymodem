// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_LINE_RECEIVER_H
#define V34_DATA_LINE_RECEIVER_H

#include "v34_data_decision_receiver.h"
#include "v34_data_receive.h"
#include "v34_line_receiver.h"
#include "v34_phase3_s_receiver.h"

#include <stdbool.h>
#include <stdint.h>

struct v34_data_line_receiver_configuration {
	struct v34_phase3_profile transmitting_profile;
	struct v34_frame_geometry geometry;
	struct v34_power_normalizer normalizer;
	struct v34_q14_coefficient
		precoder_coefficients[V34_PRECODER_TAP_COUNT];
	enum v34_trellis_selection trellis_selection;
	bool expanded_shaping;
	bool detect_rate_renegotiation;
};

struct v34_data_rate_renegotiation_monitor {
	struct v34_phase3_s_hunt s;
	bool enabled;
};

/* Own the continuous fixed sample-to-data path after Phase 4 E. The timing,
 * carrier, and fractionally spaced equalizer are copied at one exact sample
 * boundary and remain the same physical lineage through B1, payload, and an
 * optional online S/S-bar rate-renegotiation marker. */
struct v34_data_line_receiver {
	struct v34_line_receiver line;
	struct v34_data_decision_receiver decisions;
	struct v34_data_rate_renegotiation_monitor rate_renegotiation;
	struct v34_phase3_profile transmitting_profile;
	struct v34_frame_geometry geometry;
	struct v34_phase3_s_adaptation first_symbol_adaptation;
	bool first_symbol_freezes_adaptation;
	bool first_symbol_is_control;
	bool failed;
};

enum v34_data_receive_result v34_data_line_receiver_init(
	struct v34_data_line_receiver *receiver,
	const struct v34_line_receiver *line,
	const struct v34_data_line_receiver_configuration *configuration);

bool v34_data_line_receiver_is_valid(
	const struct v34_data_line_receiver *receiver);
bool v34_data_line_receiver_has_failed(
	const struct v34_data_line_receiver *receiver);
bool v34_data_line_receiver_is_receiving_b1(
	const struct v34_data_line_receiver *receiver);
bool v34_data_line_receiver_detects_rate_renegotiation(
	const struct v34_data_line_receiver *receiver);
bool v34_data_line_receiver_has_rate_renegotiation_candidate(
	const struct v34_data_line_receiver *receiver);
bool v34_data_line_receiver_rate_renegotiation_is_complete(
	const struct v34_data_line_receiver *receiver);
bool v34_data_line_receiver_get_rate_renegotiation_detection(
	const struct v34_data_line_receiver *receiver,
	struct v34_phase3_s_detection *detection);

/* Consume one signed line sample. `current` changes for every interval result;
 * `received` changes only for a mapping-frame or B1 result. Invalid calls and
 * calls after failure leave the receiver and both outputs unchanged. */
enum v34_data_receive_result v34_data_line_receiver_sample(
	struct v34_data_line_receiver *receiver, int16_t sample,
	struct v34_survivor_decision *current,
	struct v34_received_mapping_frame *received);

#endif /* V34_DATA_LINE_RECEIVER_H */
