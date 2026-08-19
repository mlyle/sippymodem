// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_RATE_PATH_H
#define V32_RATE_PATH_H

#include "conditioning_path.h"
#include "four_point_receiver.h"
#include "rate_signal.h"

#include <stdbool.h>
#include <stdint.h>

enum v32_rate_path_result {
	V32_RATE_PATH_OK,
	V32_RATE_PATH_COMPLETE,
	V32_RATE_PATH_INVALID_ARGUMENT,
	V32_RATE_PATH_INVALID_STATE,
};

/* Own the continuous post-conditioning receive lane through the first
 * complete repeated rate signal. */
struct v32_rate_path {
	struct v32_receive_path receive;
	struct v32_four_point_receiver four_point;
	struct v32_rate_signal_detector detector;
	uint64_t observation_epoch;
};

/* Preserve the aligned detector and differential framing for the later E
 * sequence.  A recovered observation may already be pending at the boundary. */
struct v32_rate_path_handoff {
	struct v32_receive_path receive;
	struct v32_four_point_receiver four_point;
	struct v32_rate_signal_detector detector;
	struct v32_rate_offer offer;
	struct v32_equalizer_observation observation;
	uint64_t observation_epoch;
	bool observation_ready;
};

enum v32_rate_path_result v32_rate_path_init(
	struct v32_rate_path *path,
	const struct v32_conditioning_path_handoff *conditioning);
bool v32_rate_path_is_valid(
	const struct v32_rate_path *path);
bool v32_rate_path_is_complete(
	const struct v32_rate_path *path);

/* Consume one 8 kHz receive sample after the corresponding transmit sample.
 * COMPLETE consumes the decision which completes the second identical
 * 16-bit rate word. */
enum v32_rate_path_result v32_rate_path_sample(
	struct v32_rate_path *path, int16_t received,
	const struct v32_echo_reference *accepted_reference);

bool v32_rate_path_handoff_is_valid(
	const struct v32_rate_path_handoff *handoff);
enum v32_rate_path_result v32_rate_path_handoff(
	const struct v32_rate_path *path,
	struct v32_rate_path_handoff *handoff);

#endif
