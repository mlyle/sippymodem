// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_LINE_RECEIVER_H
#define V34_LINE_RECEIVER_H

#include "v34_adaptive_equalizer.h"
#include "v34_phase3_profile.h"
#include "v34_receive_timing.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_line_receiver_result {
	V34_LINE_RECEIVER_OK,
	V34_LINE_RECEIVER_TIMING_ACQUIRED,
	V34_LINE_RECEIVER_OBSERVATION_READY,
	V34_LINE_RECEIVER_INVALID_ARGUMENT,
	V34_LINE_RECEIVER_INVALID_STATE,
	V34_LINE_RECEIVER_NUMERIC_FAILURE,
};

/* Own one continuous fixed-point path from signed 8-kHz samples through
 * timing recovery and the fractionally spaced adaptive equalizer. Semantic
 * training and data receivers resolve every published observation before
 * another line sample is accepted. */
struct v34_line_receiver {
	struct v34_receive_timing timing;
	struct v34_adaptive_equalizer equalizer;
	/* Retain the last published signal observation across semantic resolve.
	 * Its lineage identifies the observed state, not a reset generation. */
	struct v34_adaptive_equalizer_observation latest_observation;
	bool initialized;
};

enum v34_line_receiver_result v34_line_receiver_init(
	struct v34_line_receiver *receiver,
	const struct v34_phase3_profile *transmitting_profile,
	int16_t initial_receive_gain, bool reanchor_at_s,
	enum v34_adaptive_equalizer_feedback_mode feedback_mode);
bool v34_line_receiver_is_valid(
	const struct v34_line_receiver *receiver);
bool v34_line_receiver_matches_profile(
	const struct v34_line_receiver *receiver,
	const struct v34_phase3_profile *transmitting_profile);

/* Consume one signed 8-kHz sample. OBSERVATION_READY leaves exactly one
 * adaptive-equalizer decision pending; the caller must commit or discard it
 * before supplying another sample. The zero-filled equalizer prefix is owned
 * and discarded here, so the first published observation contains signal. */
enum v34_line_receiver_result v34_line_receiver_sample(
	struct v34_line_receiver *receiver, int16_t sample,
	struct v34_adaptive_equalizer_observation *observation);

/* Resolve the one pending observation using a semantic receiver's action.
 * BEGIN_TRACKING also freezes the trained gain. RESTART_PATH preserves the
 * selected profile, current gain, equalizer geometry, and feedback policy while
 * starting a fresh physical receive epoch. Successful calls consume the
 * observation; rejected calls leave the receiver unchanged. */
enum v34_line_receiver_result v34_line_receiver_resolve(
	struct v34_line_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	const struct v34_receive_action *action,
	uint16_t decoder_power_q15);

#endif /* V34_LINE_RECEIVER_H */
