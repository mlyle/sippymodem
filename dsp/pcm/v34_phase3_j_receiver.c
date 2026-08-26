// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_j_receiver.h"

#include <limits.h>
#include <stddef.h>

enum {
	V34_PHASE3_J_ROTATION_CAPACITY =
		V34_PHASE3_J_RECEIVER_ROTATION_BYTES * 4,
	V34_PHASE3_J_ROTATION_PADDING =
		V34_PHASE3_J_ROTATION_CAPACITY -
		V34_PHASE3_J_RECEIVER_ROTATION_SYMBOLS,
};

#define V34_PHASE3_J_QUALIFIED_HISTORY_MASK \
	((UINT32_C(1) << V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS) - 1u)

_Static_assert(
	V34_PHASE3_J_ROTATION_PADDING < 4,
	"V.34 J rotation history must waste less than one packed byte");

static bool role_is_valid(enum v34_station_role role)
{
	return role == V34_CALLING_STATION ||
	       role == V34_ANSWERING_STATION;
}

bool v34_phase3_j_detection_is_valid(
	const struct v34_phase3_j_detection *detection)
{
	return detection != NULL &&
	       detection->preceding_symbols >= V34_PHASE3_TRN_MIN_SYMBOLS &&
	       detection->errors <= V34_PHASE3_J_RECEIVER_MAX_ERRORS &&
	       v34_phase3_j_sequence_at_pattern_boundary(
		       &detection->sequence);
}

static uint8_t expected_buffered(uint32_t received_symbols)
{
	return (uint8_t)(received_symbols <
				 V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS ?
			       received_symbols :
			       V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS);
}

static bool detection_is_clear(
	const struct v34_phase3_j_detection *detection)
{
	return detection->preceding_symbols == 0u && detection->errors == 0u &&
	       detection->sequence.scrambler.history == 0u &&
	       detection->sequence.scrambler.polynomial ==
		       V34_POLYNOMIAL_GPC &&
	       detection->sequence.role == V34_CALLING_STATION &&
	       detection->sequence.remote_constellation == 0 &&
	       detection->sequence.previous_clockwise_quarter_turns == 0u &&
	       detection->sequence.symbol_in_pattern == 0u;
}

static bool rotation_history_is_valid(
	const struct v34_phase3_j_receiver *receiver)
{
	uint32_t retained = receiver->received_symbols <
				    V34_PHASE3_J_ROTATION_CAPACITY ?
				receiver->received_symbols :
				V34_PHASE3_J_ROTATION_CAPACITY;
	uint32_t unused = V34_PHASE3_J_ROTATION_CAPACITY - retained;

	for (uint32_t index = 0; index < unused; ++index) {
		unsigned int shift = 2u * (3u - index % 4u);

		if (((receiver->rotation_history[index / 4u] >> shift) & 3u) !=
		    0u)
			return false;
	}
	return true;
}

bool v34_phase3_j_receiver_is_valid(
	const struct v34_phase3_j_receiver *receiver)
{
	if (receiver == NULL || !role_is_valid(receiver->transmitting_role) ||
	    receiver->buffered_decisions !=
		    expected_buffered(receiver->received_symbols) ||
	    (receiver->qualified_history &
	     ~V34_PHASE3_J_QUALIFIED_HISTORY_MASK) != 0u ||
	    !rotation_history_is_valid(receiver))
		return false;
	if (!receiver->complete)
		return detection_is_clear(&receiver->detection);
	return receiver->buffered_decisions ==
		       V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS &&
	       v34_phase3_j_detection_is_valid(&receiver->detection) &&
	       receiver->detection.sequence.role ==
		       receiver->transmitting_role &&
	       receiver->detection.preceding_symbols <=
		       UINT32_MAX - V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS &&
	       receiver->detection.preceding_symbols +
		       V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS ==
		       receiver->received_symbols;
}

bool v34_phase3_j_receiver_is_complete(
	const struct v34_phase3_j_receiver *receiver)
{
	return v34_phase3_j_receiver_is_valid(receiver) && receiver->complete;
}

enum v34_phase3_j_receiver_result v34_phase3_j_receiver_init(
	struct v34_phase3_j_receiver *receiver,
	enum v34_station_role transmitting_role)
{
	struct v34_phase3_j_receiver initialized = {
		.transmitting_role = transmitting_role,
	};

	if (receiver == NULL || !role_is_valid(transmitting_role) ||
	    !v34_phase3_j_receiver_is_valid(&initialized))
		return V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE3_J_RECEIVER_OK;
}

static uint8_t retained_rotation(
	const struct v34_phase3_j_receiver *receiver, size_t index)
{
	size_t packed = V34_PHASE3_J_ROTATION_PADDING + index;
	unsigned int shift = 2u * (3u - (unsigned int)(packed % 4u));

	return (uint8_t)(
		(receiver->rotation_history[packed / 4u] >> shift) & 3u);
}

static void retain_rotation(
	struct v34_phase3_j_receiver *receiver, uint8_t rotation)
{
	for (size_t index = 0;
	     index + 1u < V34_PHASE3_J_RECEIVER_ROTATION_BYTES; ++index)
		receiver->rotation_history[index] = (uint8_t)(
			(receiver->rotation_history[index] << 2u) |
			(receiver->rotation_history[index + 1u] >> 6u));
	receiver->rotation_history[
		V34_PHASE3_J_RECEIVER_ROTATION_BYTES - 1u] = (uint8_t)(
		(receiver->rotation_history[
			 V34_PHASE3_J_RECEIVER_ROTATION_BYTES - 1u]
		 << 2u) |
		rotation);
}

static bool decision_is_qualified(
	const struct v34_phase3_j_receiver *receiver, size_t index)
{
	unsigned int shift =
		V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS - 1u - index;

	return ((receiver->qualified_history >> shift) & 1u) != 0u;
}

static bool reconstruct_boundary_scrambler(
	const struct v34_phase3_j_receiver *receiver, bool differential,
	uint8_t rotation_offset, struct v34_scrambler *scrambler)
{
	uint8_t ignored;

	if (v34_transmit_scrambler_init(
		    scrambler, receiver->transmitting_role) != V34_FRAMING_OK)
		return false;
	for (size_t index = 1;
	     index <= V34_PHASE3_J_RECEIVER_BOUNDARY_SYMBOLS; ++index) {
		uint8_t current = retained_rotation(receiver, index);
		uint8_t mapped = differential ?
			(uint8_t)((current + 4u -
				   retained_rotation(receiver, index - 1u)) &
				  3u) :
			(uint8_t)((current + 4u - rotation_offset) & 3u);

		if (v34_descramble_mapped_dibit(
			    scrambler, mapped, &ignored) != V34_FRAMING_OK)
			return false;
	}
	return true;
}

static bool correlate_candidate(
	const struct v34_phase3_j_receiver *receiver,
	enum v34_constellation_size constellation, bool differential,
	uint8_t rotation_offset, struct v34_phase3_j_detection *detection)
{
	struct v34_phase3_j_sequence sequence = {
		.role = receiver->transmitting_role,
		.remote_constellation = constellation,
		.previous_clockwise_quarter_turns = retained_rotation(
			receiver, V34_PHASE3_J_RECEIVER_BOUNDARY_SYMBOLS),
	};
	uint8_t errors = 0;

	if (!reconstruct_boundary_scrambler(
		    receiver, differential, rotation_offset,
		    &sequence.scrambler))
		return false;
	for (size_t symbol = 0;
	     symbol < V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS; ++symbol) {
		struct v34_analytic_sample point;
		uint8_t expected_rotation;

		if (v34_phase3_j_sequence_next_fixed(
			    &sequence, &point, &expected_rotation) !=
			    V34_PHASE3_J_SEQUENCE_OK)
			return false;
		if (!decision_is_qualified(receiver, symbol) ||
		    retained_rotation(
			    receiver,
			    V34_PHASE3_J_RECEIVER_BOUNDARY_SYMBOLS + 1u +
				    symbol) != expected_rotation)
			errors++;
	}
	*detection = (struct v34_phase3_j_detection){
		.sequence = sequence,
		.preceding_symbols = receiver->received_symbols -
			V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS,
		.errors = errors,
	};
	return v34_phase3_j_detection_is_valid(detection);
}

static bool detections_are_equal(
	const struct v34_phase3_j_detection *left,
	const struct v34_phase3_j_detection *right)
{
	return left->preceding_symbols == right->preceding_symbols &&
	       left->errors == right->errors &&
	       v34_phase3_j_sequence_is_equal(
		       &left->sequence, &right->sequence);
}

static void consider_candidate(
	const struct v34_phase3_j_detection *candidate,
	struct v34_phase3_j_detection *best, bool *found, bool *ambiguous)
{
	if (!*found || candidate->errors < best->errors) {
		*best = *candidate;
		*found = true;
		*ambiguous = false;
	} else if (candidate->errors == best->errors &&
		   !detections_are_equal(candidate, best)) {
		*ambiguous = true;
	}
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

		for (uint8_t offset = 0; offset < 4u; ++offset) {
			if (correlate_candidate(
				    receiver, CONSTELLATIONS[index], false, offset,
				    &candidate))
				consider_candidate(
					&candidate, &best, &found, &ambiguous);
		}
		if (correlate_candidate(
			    receiver, CONSTELLATIONS[index], true, 0,
			    &candidate))
			consider_candidate(
				&candidate, &best, &found, &ambiguous);
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

	if (!v34_phase3_j_receiver_is_valid(receiver) || quarter_point >= 4u ||
	    clockwise_quarter_turns >= 4u)
		return V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT;
	if (receiver->complete)
		return V34_PHASE3_J_RECEIVER_COMPLETE;
	if (receiver->received_symbols == UINT32_MAX)
		return V34_PHASE3_J_RECEIVER_OUT_OF_RANGE;
	next = *receiver;
	retain_rotation(&next, clockwise_quarter_turns);
	next.received_symbols++;
	next.qualified_history =
		((next.qualified_history << 1u) |
		 (quarter_point == 0u ? 1u : 0u)) &
		V34_PHASE3_J_QUALIFIED_HISTORY_MASK;
	if (next.buffered_decisions < V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS)
		next.buffered_decisions++;
	if (next.buffered_decisions ==
		    V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS &&
	    next.received_symbols >= V34_PHASE3_J_RECEIVER_ROTATION_SYMBOLS &&
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
