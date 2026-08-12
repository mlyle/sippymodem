// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE3_J_RECEIVER_H
#define V34_PHASE3_J_RECEIVER_H

#include "v34_phase3_j_sequence.h"
#include "v34_phase3_trn.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE3_J_RECEIVER_PATTERN_REPETITIONS = 3,
	V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS =
		V34_PHASE3_J_PATTERN_SYMBOLS *
		V34_PHASE3_J_RECEIVER_PATTERN_REPETITIONS,
	V34_PHASE3_J_RECEIVER_MAX_ERRORS = 2,
};

enum v34_phase3_j_receiver_result {
	V34_PHASE3_J_RECEIVER_OK,
	/* This decision completed a qualified J window. */
	V34_PHASE3_J_RECEIVER_DETECTED,
	/* J was already detected; no decision was consumed. */
	V34_PHASE3_J_RECEIVER_COMPLETE,
	V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT,
	V34_PHASE3_J_RECEIVER_OUT_OF_RANGE,
};

struct v34_phase3_j_detection {
	/* Expected peer state immediately after the detected whole J window.
	 * remote_constellation is the Phase 4 selection signalled by J. */
	struct v34_phase3_j_sequence sequence;
	uint32_t trn_symbols;
	uint8_t errors;
};

bool v34_phase3_j_detection_is_valid(
	const struct v34_phase3_j_detection *detection);

/* Locate the TRN-to-J transition from hard four-point decisions. A rolling
 * three-pattern window rejects continued TRN and correlates both possible J
 * selections. Candidate symbols are generated from retained TRN boundary
 * states, so no pattern or scrambler-state table is stored. */
struct v34_phase3_j_receiver {
	struct v34_phase3_trn trn;
	struct v34_phase3_trn
		boundaries[V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS];
	uint8_t decisions[V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS];
	struct v34_phase3_j_detection detection;
	uint32_t received_symbols;
	uint8_t next_decision;
	uint8_t buffered_decisions;
	bool complete;
};

enum v34_phase3_j_receiver_result v34_phase3_j_receiver_init(
	struct v34_phase3_j_receiver *receiver,
	enum v34_station_role transmitting_role);

bool v34_phase3_j_receiver_is_valid(
	const struct v34_phase3_j_receiver *receiver);
bool v34_phase3_j_receiver_is_complete(
	const struct v34_phase3_j_receiver *receiver);

/* Accept one training-constellation decision. TRN and J use quarter point
 * zero; a different valid point counts as a correlation error. Detection is
 * disabled until at least 512 preceding TRN decisions have been accepted. */
enum v34_phase3_j_receiver_result v34_phase3_j_receiver_receive(
	struct v34_phase3_j_receiver *receiver, uint8_t quarter_point,
	uint8_t clockwise_quarter_turns);

/* Copy the selected constellation, exact post-J sequence state, transition
 * position, and error count. A false return leaves the output unchanged. */
bool v34_phase3_j_receiver_get_detection(
	const struct v34_phase3_j_receiver *receiver,
	struct v34_phase3_j_detection *detection);

const char *v34_phase3_j_receiver_result_name(
	enum v34_phase3_j_receiver_result result);

#endif /* V34_PHASE3_J_RECEIVER_H */
