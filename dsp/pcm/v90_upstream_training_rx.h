// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_UPSTREAM_TRAINING_RX_H
#define V90_UPSTREAM_TRAINING_RX_H

#include "v34_line_receiver.h"
#include "v34_phase3_decision_receiver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	/* The return detector can report S-bar after CPt is already in flight.
	 * Retain enough fixed decisions to recover the bounded semantic prefix
	 * without replaying samples through the physical receiver. */
	V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS = 256,
};

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
	struct v34_analytic_sample
		recent_decisions[V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS];
	uint32_t tracking_symbols;
	uint16_t decoder_power_q15;
	uint16_t next_recent_decision;
	uint16_t recent_decision_count;
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

/* Copy one retained fixed decision in oldest-to-newest order. Rejected calls
 * leave the destination unchanged. */
bool v90_upstream_training_rx_recent_decision(
	const struct v90_upstream_training_rx *receiver, size_t index,
	struct v34_analytic_sample *decision);

/* Consume exactly one signed 8-kHz sample. Once PP completes, later calls
 * continue bounded decision-directed tracking and normally return OK. */
enum v90_upstream_training_rx_result v90_upstream_training_rx_sample(
	struct v90_upstream_training_rx *receiver, int16_t sample);

/* Advance the trained physical clock without interpreting a symbol. This is
 * used after the return S-bar boundary while the peer has already begun CPt
 * but the local digital DIL segment is still draining. */
enum v90_upstream_training_rx_result v90_upstream_training_rx_advance(
	struct v90_upstream_training_rx *receiver, int16_t sample);

#endif /* V90_UPSTREAM_TRAINING_RX_H */
