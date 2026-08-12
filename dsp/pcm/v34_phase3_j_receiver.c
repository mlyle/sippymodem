// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_j_receiver.h"

#include <limits.h>
#include <stddef.h>

_Static_assert(
	V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS <= UINT8_MAX,
	"V.34 J receive window must fit its counters");
_Static_assert(
	V34_PHASE3_J_RECEIVER_MAX_ERRORS <
		V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS,
	"V.34 J correlation must retain matching evidence");

static bool detection_is_valid(
	const struct v34_phase3_j_detection *detection)
{
	return detection != NULL &&
	       detection->trn_symbols >= V34_PHASE3_TRN_MIN_SYMBOLS &&
	       detection->errors <= V34_PHASE3_J_RECEIVER_MAX_ERRORS &&
	       v34_phase3_j_sequence_at_pattern_boundary(
		       &detection->sequence);
}

static bool ring_is_valid(const struct v34_phase3_j_receiver *receiver)
{
	uint32_t expected_buffered = receiver->received_symbols <
					     V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS ?
				     receiver->received_symbols :
				     V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS;
	uint8_t expected_next = (uint8_t)(
		receiver->received_symbols %
		V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS);

	if (receiver->buffered_decisions != expected_buffered ||
	    receiver->next_decision != expected_next)
		return false;
	for (size_t index = 0;
	     index < V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS; ++index) {
		bool populated = receiver->buffered_decisions ==
					 V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS ||
			 index < receiver->buffered_decisions;

		if (receiver->decisions[index] >= 16u ||
		    (populated &&
		     (!v34_phase3_trn_is_valid(&receiver->boundaries[index]) ||
		      receiver->boundaries[index].role != receiver->trn.role)))
			return false;
	}
	return true;
}

bool v34_phase3_j_receiver_is_valid(
	const struct v34_phase3_j_receiver *receiver)
{
	if (receiver == NULL || !v34_phase3_trn_is_valid(&receiver->trn) ||
	    !ring_is_valid(receiver))
		return false;
	if (!receiver->complete)
		return receiver->detection.trn_symbols == 0 &&
		       receiver->detection.errors == 0;
	return receiver->buffered_decisions ==
		       V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS &&
	       detection_is_valid(&receiver->detection) &&
	       receiver->detection.sequence.role == receiver->trn.role &&
	       receiver->detection.trn_symbols <=
		       UINT32_MAX - V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS &&
	       receiver->detection.trn_symbols +
		       V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS ==
		       receiver->received_symbols;
}

bool v34_phase3_j_receiver_is_complete(
	const struct v34_phase3_j_receiver *receiver)
{
	return v34_phase3_j_receiver_is_valid(receiver) &&
	       receiver->complete;
}

enum v34_phase3_j_receiver_result v34_phase3_j_receiver_init(
	struct v34_phase3_j_receiver *receiver,
	enum v34_station_role transmitting_role)
{
	struct v34_phase3_j_receiver initialized = { 0 };

	if (receiver == NULL ||
	    v34_phase3_trn_init(&initialized.trn, transmitting_role) !=
		    V34_PHASE3_TRN_OK ||
	    !v34_phase3_j_receiver_is_valid(&initialized))
		return V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE3_J_RECEIVER_OK;
}

static uint8_t buffered_decision(
	const struct v34_phase3_j_receiver *receiver, size_t index)
{
	return receiver->decisions[
		(receiver->next_decision + index) %
		V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS];
}

static const struct v34_phase3_trn *window_boundary(
	const struct v34_phase3_j_receiver *receiver)
{
	return &receiver->boundaries[receiver->next_decision];
}

static bool correlate_candidate(
	const struct v34_phase3_j_receiver *receiver,
	enum v34_constellation_size constellation,
	struct v34_phase3_j_detection *detection)
{
	struct v34_phase3_j_sequence sequence;
	uint8_t errors = 0;

	if (v34_phase3_j_sequence_init_from_trn(
		    &sequence, window_boundary(receiver), constellation) !=
	    V34_PHASE3_J_SEQUENCE_OK)
		return false;
	for (size_t symbol = 0;
	     symbol < V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS; ++symbol) {
		struct qam_point point;
		uint8_t rotation;

		if (v34_phase3_j_sequence_next(
			    &sequence, &point, &rotation) !=
		    V34_PHASE3_J_SEQUENCE_OK)
			return false;
		if (buffered_decision(receiver, symbol) != rotation)
			errors++;
	}
	*detection = (struct v34_phase3_j_detection){
		.sequence = sequence,
		.trn_symbols = receiver->received_symbols -
			       V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS,
		.errors = errors,
	};
	return true;
}

static bool correlate(
	const struct v34_phase3_j_receiver *receiver,
	struct v34_phase3_j_detection *detection)
{
	static const enum v34_constellation_size CONSTELLATIONS[] = {
		V34_CONSTELLATION_4_POINT,
		V34_CONSTELLATION_16_POINT,
	};
	struct v34_phase3_j_detection best = { 0 };
	bool found = false;
	bool ambiguous = false;

	for (size_t index = 0;
	     index < sizeof(CONSTELLATIONS) / sizeof(CONSTELLATIONS[0]);
	     ++index) {
		struct v34_phase3_j_detection candidate;

		if (!correlate_candidate(
			    receiver, CONSTELLATIONS[index], &candidate) ||
		    candidate.errors > V34_PHASE3_J_RECEIVER_MAX_ERRORS)
			continue;
		if (!found || candidate.errors < best.errors) {
			best = candidate;
			found = true;
			ambiguous = false;
		} else if (candidate.errors == best.errors) {
			ambiguous = true;
		}
	}
	if (!found || ambiguous)
		return false;
	*detection = best;
	return true;
}

enum v34_phase3_j_receiver_result v34_phase3_j_receiver_receive(
	struct v34_phase3_j_receiver *receiver, uint8_t quarter_point,
	uint8_t clockwise_quarter_turns)
{
	struct v34_phase3_j_receiver next;
	struct v34_phase3_j_detection detection;
	struct qam_point expected;
	uint8_t expected_rotation;

	if (!v34_phase3_j_receiver_is_valid(receiver) || quarter_point >= 4u ||
	    clockwise_quarter_turns >= 4u)
		return V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT;
	if (receiver->complete)
		return V34_PHASE3_J_RECEIVER_COMPLETE;
	if (receiver->received_symbols == UINT32_MAX)
		return V34_PHASE3_J_RECEIVER_OUT_OF_RANGE;
	next = *receiver;
	next.boundaries[next.next_decision] = next.trn;
	next.decisions[next.next_decision] =
		(uint8_t)(quarter_point * 4u + clockwise_quarter_turns);
	if (v34_phase3_trn_next(
		    &next.trn, &expected, &expected_rotation) !=
	    V34_PHASE3_TRN_OK)
		return V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT;
	next.received_symbols++;
	next.next_decision++;
	if (next.next_decision == V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS)
		next.next_decision = 0;
	if (next.buffered_decisions < V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS)
		next.buffered_decisions++;
	if (next.buffered_decisions ==
		    V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS &&
	    next.received_symbols - V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS >=
		    V34_PHASE3_TRN_MIN_SYMBOLS &&
	    correlate(&next, &detection)) {
		next.detection = detection;
		next.complete = true;
	}
	if (!v34_phase3_j_receiver_is_valid(&next))
		return V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	return next.complete ? V34_PHASE3_J_RECEIVER_DETECTED :
			       V34_PHASE3_J_RECEIVER_OK;
}

bool v34_phase3_j_receiver_get_detection(
	const struct v34_phase3_j_receiver *receiver,
	struct v34_phase3_j_detection *detection)
{
	if (!v34_phase3_j_receiver_is_complete(receiver) || detection == NULL)
		return false;
	*detection = receiver->detection;
	return true;
}

const char *v34_phase3_j_receiver_result_name(
	enum v34_phase3_j_receiver_result result)
{
	switch (result) {
	case V34_PHASE3_J_RECEIVER_OK:
		return "ok";
	case V34_PHASE3_J_RECEIVER_DETECTED:
		return "detected";
	case V34_PHASE3_J_RECEIVER_COMPLETE:
		return "complete";
	case V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_PHASE3_J_RECEIVER_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
