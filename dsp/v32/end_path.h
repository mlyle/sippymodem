// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_END_PATH_H
#define V32_END_PATH_H

#include "rate_path.h"

#include <stdbool.h>
#include <stdint.h>

enum v32_end_path_result {
	V32_END_PATH_OK,
	V32_END_PATH_COMPLETE,
	V32_END_PATH_SEQUENCE_REJECTED,
	V32_END_PATH_INVALID_ARGUMENT,
	V32_END_PATH_INVALID_STATE,
};

/* Continue an aligned rate receiver through the selected E sequence.  The
 * initial rate may repeat or transition once to the selected rate before E. */
struct v32_end_path {
	struct v32_receive_path receive;
	struct v32_uncoded_receiver four_point;
	struct v32_rate_signal_detector detector;
	struct v32_rate_offer initial_offer;
	struct v32_rate_offer selected_offer;
	uint64_t observation_epoch;
	enum v32_equalizer_adaptation target_adaptation;
	bool end_received;
};

/* Preserve the continuous selected-rate framing and any observation already
 * recovered beyond E for the following B1 owner. */
struct v32_end_path_handoff {
	struct v32_receive_path receive;
	struct v32_uncoded_receiver four_point;
	struct v32_rate_offer selected_offer;
	struct v32_equalizer_observation observation;
	uint64_t observation_epoch;
	bool observation_ready;
};

enum v32_end_path_result v32_end_path_init(
	struct v32_end_path *path,
	const struct v32_rate_path_handoff *rate,
	const struct v32_rate_offer *selected_offer,
	enum v32_equalizer_adaptation adaptation);
/* Bounds/control checks for an admitted streaming owner. The full
 * is_valid predicate also audits stored echo samples and coefficients. */
bool v32_end_path_header_is_valid(
	const struct v32_end_path *path);
bool v32_end_path_is_valid(
	const struct v32_end_path *path);
bool v32_end_path_is_complete(
	const struct v32_end_path *path);

/* Set the mode for the next decision.  This lets the protocol owner place the
 * answering-side disable at its exact R3 boundary even if a decision is
 * already pending. */
enum v32_end_path_result v32_end_path_set_adaptation(
	struct v32_end_path *path,
	enum v32_equalizer_adaptation adaptation);

/* Consume one receive sample through the selected E sequence.  A detected
 * rate or E word outside the negotiated sequence returns SEQUENCE_REJECTED.
 * Earlier decisions recovered from the same timing block remain committed,
 * but the rejected decision stays pending and does not alter the semantic
 * decoder/detector; the call should abort. */
enum v32_end_path_result v32_end_path_sample(
	struct v32_end_path *path, int16_t received,
	const struct v32_echo_reference *accepted_reference);

bool v32_end_path_handoff_is_valid(
	const struct v32_end_path_handoff *handoff);
enum v32_end_path_result v32_end_path_handoff(
	const struct v32_end_path *path,
	struct v32_end_path_handoff *handoff);

#endif
