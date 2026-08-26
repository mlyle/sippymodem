// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_j_receiver.h"

#include <limits.h>
#include <stddef.h>

enum {
	V34_PHASE3_J_DECODED_HISTORY_BITS =
		V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS * 2,
};

#define V34_PHASE3_J_DECODED_HISTORY_MASK \
	((UINT64_C(1) << V34_PHASE3_J_DECODED_HISTORY_BITS) - 1u)
#define V34_PHASE3_J_QUALIFIED_HISTORY_MASK \
	((UINT32_C(1) << V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS) - 1u)

_Static_assert(
	V34_PHASE3_J_DECODED_HISTORY_BITS < 64,
	"V.34 J decoded history must fit its owner");

static bool role_is_valid(enum v34_station_role role)
{
	return role == V34_CALLING_STATION ||
	       role == V34_ANSWERING_STATION;
}

bool v34_phase3_j_detection_is_valid(
	const struct v34_phase3_j_detection *detection)
{
	return detection != NULL &&
	       detection->preceding_symbols >=
		       V34_PHASE3_TRN_MIN_SYMBOLS +
		       V34_PHASE3_J_RECEIVER_ACQUISITION_SYMBOLS &&
	       detection->errors <= V34_PHASE3_J_RECEIVER_MAX_ERRORS &&
	       v34_phase3_j_sequence_at_pattern_boundary(
		       &detection->sequence);
}

static uint8_t expected_buffered(uint32_t received_symbols)
{
	uint32_t decoded = received_symbols == 0u ? 0u : received_symbols - 1u;

	return (uint8_t)(decoded < V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS ?
				 decoded :
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

bool v34_phase3_j_receiver_is_valid(
	const struct v34_phase3_j_receiver *receiver)
{
	if (receiver == NULL || !role_is_valid(receiver->transmitting_role) ||
	    !v34_transmit_scrambler_is_valid(
		    &receiver->descrambler, receiver->transmitting_role) ||
	    receiver->buffered_decisions !=
		    expected_buffered(receiver->received_symbols) ||
	    receiver->previous_clockwise_quarter_turns >= 4u ||
	    (receiver->decoded_history &
	     ~V34_PHASE3_J_DECODED_HISTORY_MASK) != 0u ||
	    (receiver->qualified_history &
	     ~V34_PHASE3_J_QUALIFIED_HISTORY_MASK) != 0u ||
	    receiver->has_previous_rotation !=
		    (receiver->received_symbols != 0u))
		return false;
	if (!receiver->complete)
		return detection_is_clear(&receiver->detection);
	return receiver->buffered_decisions ==
		       V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS &&
	       v34_phase3_j_detection_is_valid(&receiver->detection) &&
	       receiver->detection.sequence.role ==
		       receiver->transmitting_role &&
	       receiver->detection.sequence.scrambler.history ==
		       receiver->descrambler.history &&
	       receiver->detection.sequence.scrambler.polynomial ==
		       receiver->descrambler.polynomial &&
	       receiver->detection.sequence
		       .previous_clockwise_quarter_turns ==
		       receiver->previous_clockwise_quarter_turns &&
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
	    v34_transmit_scrambler_init(
		    &initialized.descrambler, transmitting_role) !=
		    V34_FRAMING_OK ||
	    !v34_phase3_j_receiver_is_valid(&initialized))
		return V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE3_J_RECEIVER_OK;
}

static uint8_t decoded_symbol(
	const struct v34_phase3_j_receiver *receiver, size_t index)
{
	unsigned int shift = 2u *
		(V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS - 1u - index);

	return (uint8_t)((receiver->decoded_history >> shift) & 3u);
}

static bool decision_is_qualified(
	const struct v34_phase3_j_receiver *receiver, size_t index)
{
	unsigned int shift =
		V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS - 1u - index;

	return ((receiver->qualified_history >> shift) & 1u) != 0u;
}

static uint8_t pattern_symbol(
	enum v34_constellation_size constellation, size_t index)
{
	uint16_t pattern = constellation == V34_CONSTELLATION_4_POINT ?
		V34_PHASE3_J_4_POINT_PATTERN :
		V34_PHASE3_J_16_POINT_PATTERN;
	unsigned int symbol =
		(unsigned int)(index % V34_PHASE3_J_PATTERN_SYMBOLS);
	unsigned int shift =
		2u * (V34_PHASE3_J_PATTERN_SYMBOLS - 1u - symbol);

	return (uint8_t)((pattern >> shift) & 3u);
}

static uint8_t candidate_errors(
	const struct v34_phase3_j_receiver *receiver,
	enum v34_constellation_size constellation)
{
	uint8_t errors = 0;

	for (size_t index = 0;
	     index < V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS; ++index) {
		if (!decision_is_qualified(receiver, index) ||
		    decoded_symbol(receiver, index) !=
			    pattern_symbol(constellation, index))
			errors++;
	}
	return errors;
}

static bool correlate(
	const struct v34_phase3_j_receiver *receiver,
	struct v34_phase3_j_detection *detection)
{
	uint8_t four_errors = candidate_errors(
		receiver, V34_CONSTELLATION_4_POINT);
	uint8_t sixteen_errors = candidate_errors(
		receiver, V34_CONSTELLATION_16_POINT);
	struct v34_phase3_j_detection candidate = {
		.sequence = {
			.scrambler = receiver->descrambler,
			.role = receiver->transmitting_role,
			.remote_constellation = four_errors < sixteen_errors ?
				V34_CONSTELLATION_4_POINT :
				V34_CONSTELLATION_16_POINT,
			.previous_clockwise_quarter_turns =
				receiver->previous_clockwise_quarter_turns,
		},
		.preceding_symbols = receiver->received_symbols -
			V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS,
		.errors = four_errors < sixteen_errors ?
			four_errors : sixteen_errors,
	};

	if (four_errors == sixteen_errors ||
	    !v34_phase3_j_detection_is_valid(&candidate))
		return false;
	*detection = candidate;
	return true;
}

enum v34_phase3_j_receiver_result v34_phase3_j_receiver_receive(
	struct v34_phase3_j_receiver *receiver, uint8_t quarter_point,
	uint8_t clockwise_quarter_turns)
{
	struct v34_phase3_j_receiver next;
	struct v34_phase3_j_detection detection;
	uint8_t decoded;

	if (!v34_phase3_j_receiver_is_valid(receiver) || quarter_point >= 4u ||
	    clockwise_quarter_turns >= 4u)
		return V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT;
	if (receiver->complete)
		return V34_PHASE3_J_RECEIVER_COMPLETE;
	if (receiver->received_symbols == UINT32_MAX)
		return V34_PHASE3_J_RECEIVER_OUT_OF_RANGE;
	next = *receiver;
	next.received_symbols++;
	if (!next.has_previous_rotation) {
		next.previous_clockwise_quarter_turns =
			clockwise_quarter_turns;
		next.has_previous_rotation = true;
	} else {
		if (v34_descramble_differential_dibit(
			    &next.descrambler,
			    next.previous_clockwise_quarter_turns,
			    clockwise_quarter_turns, &decoded) != V34_FRAMING_OK)
			return V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT;
		next.previous_clockwise_quarter_turns =
			clockwise_quarter_turns;
		next.decoded_history =
			((next.decoded_history << 2u) | decoded) &
			V34_PHASE3_J_DECODED_HISTORY_MASK;
		next.qualified_history =
			((next.qualified_history << 1u) |
			 (quarter_point == 0u ? 1u : 0u)) &
			V34_PHASE3_J_QUALIFIED_HISTORY_MASK;
		if (next.buffered_decisions <
		    V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS)
			next.buffered_decisions++;
	}
	if (next.buffered_decisions ==
		    V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS &&
	    next.received_symbols - V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS >=
		    V34_PHASE3_TRN_MIN_SYMBOLS +
		    V34_PHASE3_J_RECEIVER_ACQUISITION_SYMBOLS &&
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
