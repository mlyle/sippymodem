// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_RECEIVE_PATH_H
#define V32_RECEIVE_PATH_H

#include "echo_frontend.h"
#include "equalizer.h"
#include "line_receiver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V32_RECEIVE_PATH_RECOVERED_CAPACITY =
		V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES,
};

enum v32_receive_path_result {
	V32_RECEIVE_PATH_OK,
	V32_RECEIVE_PATH_OBSERVATION_READY,
	V32_RECEIVE_PATH_DECISION_PENDING,
	V32_RECEIVE_PATH_INVALID_ARGUMENT,
	V32_RECEIVE_PATH_INVALID_STATE,
};

/*
 * Own the continuous physical receive lane through the next slicer decision.
 * Timing recovery may publish a block of samples at once, so the path retains
 * the bounded remainder while the caller decides and commits each observation.
 */
struct v32_receive_path {
	struct v32_echo_frontend echo;
	struct v32_line_receiver line;
	struct v32_equalizer equalizer;
	int16_t recovered[V32_RECEIVE_PATH_RECOVERED_CAPACITY];
	size_t recovered_index;
	size_t recovered_count;
	bool echo_training;
};

enum v32_receive_path_result v32_receive_path_init(
	struct v32_receive_path *path,
	uint16_t near_delay_symbols, uint16_t round_trip_symbols);
bool v32_receive_path_is_valid(
	const struct v32_receive_path *path);

/* Pause the post-echo lane before the local transmitter's reference prefill.
 * Adaptation may then start and change gain without clearing learned state.
 * Finishing freezes the echo coefficients and resumes the same downstream
 * owner. Every transition requires an open slicer decision boundary and the
 * end of the current three-sample echo group. */
enum v32_receive_path_result v32_receive_path_begin_echo_training(
	struct v32_receive_path *path);
enum v32_receive_path_result v32_receive_path_start_echo_adaptation(
	struct v32_receive_path *path, double adaptation);
enum v32_receive_path_result v32_receive_path_finish_echo_training(
	struct v32_receive_path *path);

/* Change only the gain-control lifecycle at an open receive boundary. */
enum v32_receive_path_result v32_receive_path_freeze_gain(
	struct v32_receive_path *path);
enum v32_receive_path_result v32_receive_path_reacquire_gain(
	struct v32_receive_path *path);

/*
 * Consume one 8 kHz receive sample after the corresponding transmit sample.
 * A reference is supplied exactly when that transmit sample accepted a point.
 * OBSERVATION_READY blocks further samples until commit. Rejected calls leave
 * the owner and output unchanged.
 */
enum v32_receive_path_result v32_receive_path_sample(
	struct v32_receive_path *path, int16_t received,
	const struct v32_echo_reference *accepted_reference,
	struct v32_equalizer_observation *observation);

/*
 * Commit the pending slicer decision, then drain retained timing samples up to
 * the next decision boundary. OBSERVATION_READY publishes that next decision;
 * OK means the path is ready for another line sample.
 */
enum v32_receive_path_result v32_receive_path_commit(
	struct v32_receive_path *path,
	const struct v32_equalizer_decision *decision,
	struct v32_equalizer_observation *observation);

/*
 * Commit at a protocol boundary, change equalizer adaptation before any
 * retained timing samples are observed, then drain toward the next decision.
 * This preserves the exact sample epoch when training changes to rate/data.
 */
enum v32_receive_path_result v32_receive_path_commit_adaptation(
	struct v32_receive_path *path,
	const struct v32_equalizer_decision *decision,
	enum v32_equalizer_adaptation adaptation,
	struct v32_equalizer_observation *observation);

/* Change adaptation at an open decision boundary.  A pending decision must
 * instead use commit_adaptation so no recovered sample crosses the boundary. */
enum v32_receive_path_result v32_receive_path_set_adaptation(
	struct v32_receive_path *path,
	enum v32_equalizer_adaptation adaptation);

#endif
