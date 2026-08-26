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
	/* Twelve two-bit wire decisions recover the 23-bit self-synchronizing
	 * scrambler state; one earlier rotation is needed in differential mode. */
	V34_PHASE3_J_RECEIVER_BOUNDARY_SYMBOLS =
		(V34_SCRAMBLER_BITS + 1) / 2,
	V34_PHASE3_J_RECEIVER_ROTATION_SYMBOLS =
		1 + V34_PHASE3_J_RECEIVER_BOUNDARY_SYMBOLS +
		V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS,
	V34_PHASE3_J_RECEIVER_ROTATION_BYTES =
		(V34_PHASE3_J_RECEIVER_ROTATION_SYMBOLS + 3) / 4,
	/* A later J window follows twelve recovery symbols, rounded to the next
	 * complete eight-symbol Table 18 boundary. */
	V34_PHASE3_J_RECEIVER_ACQUISITION_SYMBOLS =
		2 * V34_PHASE3_J_PATTERN_SYMBOLS,
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
	/* Symbols preceding the qualified J window. */
	uint32_t preceding_symbols;
	uint8_t errors;
};

bool v34_phase3_j_detection_is_valid(
	const struct v34_phase3_j_detection *detection);

/* Locate J from hard four-point decisions. A packed rolling history rebuilds
 * the wire scrambler boundary from either preceding TRN or preceding J, so the
 * first complete J window is accepted without predicting its transition epoch
 * and a later clean window can recover from a damaged boundary. */
struct v34_phase3_j_receiver {
	struct v34_phase3_j_detection detection;
	enum v34_station_role transmitting_role;
	uint32_t qualified_history;
	uint32_t received_symbols;
	/* Four two-bit absolute rotations per byte, oldest first. */
	uint8_t rotation_history[V34_PHASE3_J_RECEIVER_ROTATION_BYTES];
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
 * disabled until at least 512 decisions precede the candidate J window. */
enum v34_phase3_j_receiver_result v34_phase3_j_receiver_receive(
	struct v34_phase3_j_receiver *receiver, uint8_t quarter_point,
	uint8_t clockwise_quarter_turns);

/* Copy the selected constellation, exact post-J sequence state, qualified
 * window position, and error count. A false return leaves output unchanged. */
bool v34_phase3_j_receiver_get_detection(
	const struct v34_phase3_j_receiver *receiver,
	struct v34_phase3_j_detection *detection);

#endif /* V34_PHASE3_J_RECEIVER_H */
