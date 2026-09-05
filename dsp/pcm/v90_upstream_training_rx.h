// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_UPSTREAM_TRAINING_RX_H
#define V90_UPSTREAM_TRAINING_RX_H

#include "v34_line_receiver.h"
#include "v34_phase3_decision_receiver.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_upstream_training_rx_stage {
	V90_UPSTREAM_TRAINING_RX_INITIAL,
	V90_UPSTREAM_TRAINING_RX_TRACKING,
};

enum v90_upstream_training_rx_result {
	V90_UPSTREAM_TRAINING_RX_OK,
	V90_UPSTREAM_TRAINING_RX_FAILED,
	V90_UPSTREAM_TRAINING_RX_INVALID_ARGUMENT,
	V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE,
};

/* Train the conventional upstream's fixed V.34 physical receiver from its
 * initial S through PP, then retain decision-directed tracking across the
 * V.90-specific Ja and return epochs. Protocol parsers remain separate from
 * this physical owner. */
struct v90_upstream_training_rx {
	struct v34_line_receiver line;
	struct v34_phase3_decision_receiver initial;
	struct v34_phase3_profile transmitting_profile;
	uint32_t tracking_symbols;
	uint16_t decoder_power_q15;
	enum v90_upstream_training_rx_stage stage;
	bool failed;
};

enum v90_upstream_training_rx_result v90_upstream_training_rx_init(
	struct v90_upstream_training_rx *receiver,
	const struct v34_phase3_profile *transmitting_profile,
	int16_t initial_receive_gain);
bool v90_upstream_training_rx_is_valid(
	const struct v90_upstream_training_rx *receiver);
bool v90_upstream_training_rx_has_failed(
	const struct v90_upstream_training_rx *receiver);
bool v90_upstream_training_rx_is_tracking(
	const struct v90_upstream_training_rx *receiver);

/* Consume exactly one signed 8-kHz sample. Once PP completes, later calls
 * continue bounded decision-directed tracking and normally return OK. */
enum v90_upstream_training_rx_result v90_upstream_training_rx_sample(
	struct v90_upstream_training_rx *receiver, int16_t sample);

#endif /* V90_UPSTREAM_TRAINING_RX_H */
