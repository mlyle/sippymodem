// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE3_S_RECEIVER_H
#define V34_PHASE3_S_RECEIVER_H

#include "v34_phase3_s_sequence.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE3_S_RECEIVER_MAX_S_ERRORS = 8,
	V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS = 2,
};

enum v34_phase3_s_receiver_result {
	V34_PHASE3_S_RECEIVER_OK,
	/* This symbol completed a qualified S/S-bar marker. */
	V34_PHASE3_S_RECEIVER_DETECTED,
	/* A marker was already detected; no symbol was consumed. */
	V34_PHASE3_S_RECEIVER_COMPLETE,
	V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT,
};

struct v34_phase3_s_detection {
	/* Received rotation of the first point in S, including any fixed
	 * quarter-turn ambiguity left by the carrier/equalizer front end. */
	uint8_t clockwise_quarter_turn_offset;
	uint8_t s_errors;
	uint8_t s_bar_errors;
};

bool v34_phase3_s_detection_is_valid(
	const struct v34_phase3_s_detection *detection);

/* Bounded hard-decision history for locating the complete 128T S followed by
 * 16T S-bar marker at an arbitrary input boundary. The last two S and first
 * two S-bar decisions form an exact boundary anchor; bounded errors are
 * tolerated elsewhere. Expected rotations are generated during correlation,
 * so no sequence table is stored. */
struct v34_phase3_s_receiver {
	uint8_t decisions[V34_PHASE3_S_SEQUENCE_SYMBOLS];
	struct v34_phase3_s_detection detection;
	uint8_t next_decision;
	uint8_t buffered_decisions;
	bool complete;
};

enum v34_phase3_s_receiver_result v34_phase3_s_receiver_init(
	struct v34_phase3_s_receiver *receiver);

bool v34_phase3_s_receiver_is_valid(
	const struct v34_phase3_s_receiver *receiver);
bool v34_phase3_s_receiver_is_complete(
	const struct v34_phase3_s_receiver *receiver);

/* Accept one hard decision in the training-constellation convention. S and
 * S-bar use quarter point zero; a different valid point counts as a symbol
 * error. Labels outside 0..3 and already-complete calls consume nothing. The
 * full window is required so the following PP symbol remains unconsumed for
 * the equalizer-training owner. */
enum v34_phase3_s_receiver_result v34_phase3_s_receiver_receive(
	struct v34_phase3_s_receiver *receiver, uint8_t quarter_point,
	uint8_t clockwise_quarter_turns);

/* Copy the completed correlation result. A false return leaves the output
 * unchanged. */
bool v34_phase3_s_receiver_get_detection(
	const struct v34_phase3_s_receiver *receiver,
	struct v34_phase3_s_detection *detection);

const char *v34_phase3_s_receiver_result_name(
	enum v34_phase3_s_receiver_result result);

#endif /* V34_PHASE3_S_RECEIVER_H */
