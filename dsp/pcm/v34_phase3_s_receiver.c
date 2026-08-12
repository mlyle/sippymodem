// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_s_receiver.h"

#include <limits.h>
#include <stddef.h>

enum {
	V34_PHASE3_S_BOUNDARY_ANCHOR_SYMBOLS = 2,
};

_Static_assert(
	V34_PHASE3_S_SEQUENCE_SYMBOLS <= UINT8_MAX,
	"V.34 S/S-bar receive history must fit its counters");
_Static_assert(
	(unsigned int)V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS >=
		(unsigned int)V34_PHASE3_S_BOUNDARY_ANCHOR_SYMBOLS,
	"V.34 S suffix must retain its transition anchor");

bool v34_phase3_s_detection_is_valid(
	const struct v34_phase3_s_detection *detection)
{
	return detection != NULL &&
	       detection->clockwise_quarter_turn_offset < 4u &&
	       detection->s_errors <= V34_PHASE3_S_RECEIVER_MAX_S_ERRORS &&
	       detection->s_bar_errors <=
		       V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS;
}

static bool s_suffix_is_valid(uint8_t symbols)
{
	return symbols >= V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS &&
	       symbols <= V34_PHASE3_S_SYMBOLS;
}

static size_t receive_window(
	const struct v34_phase3_s_receiver *receiver)
{
	return receiver->s_symbols + V34_PHASE3_S_BAR_SYMBOLS;
}

static unsigned int allowed_s_errors(
	const struct v34_phase3_s_receiver *receiver)
{
	return receiver->s_symbols * V34_PHASE3_S_RECEIVER_MAX_S_ERRORS /
	       V34_PHASE3_S_SYMBOLS;
}

bool v34_phase3_s_receiver_is_valid(
	const struct v34_phase3_s_receiver *receiver)
{
	size_t window;

	if (receiver == NULL || !s_suffix_is_valid(receiver->s_symbols))
		return false;
	window = receive_window(receiver);
	if (receiver->next_decision >= window ||
	    receiver->buffered_decisions > window ||
	    (receiver->buffered_decisions < window &&
	     receiver->next_decision != receiver->buffered_decisions) ||
	    (receiver->complete &&
	     (receiver->buffered_decisions != window ||
	      !v34_phase3_s_detection_is_valid(&receiver->detection) ||
	      receiver->detection.s_errors > allowed_s_errors(receiver))) ||
	    (!receiver->complete &&
	     (receiver->detection.clockwise_quarter_turn_offset != 0 ||
	      receiver->detection.s_errors != 0 ||
	      receiver->detection.s_bar_errors != 0)))
		return false;
	for (size_t index = 0; index < V34_PHASE3_S_SEQUENCE_SYMBOLS;
	     ++index)
		if (receiver->decisions[index] >= 16u)
			return false;
	return true;
}

bool v34_phase3_s_receiver_is_complete(
	const struct v34_phase3_s_receiver *receiver)
{
	return v34_phase3_s_receiver_is_valid(receiver) &&
	       receiver->complete;
}

enum v34_phase3_s_receiver_result v34_phase3_s_receiver_init(
	struct v34_phase3_s_receiver *receiver)
{
	return v34_phase3_s_receiver_init_suffix(
		receiver, V34_PHASE3_S_SYMBOLS);
}

enum v34_phase3_s_receiver_result v34_phase3_s_receiver_init_suffix(
	struct v34_phase3_s_receiver *receiver, uint8_t s_symbols)
{
	struct v34_phase3_s_receiver initialized = {
		.s_symbols = s_symbols,
	};

	if (receiver == NULL || !s_suffix_is_valid(s_symbols))
		return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE3_S_RECEIVER_OK;
}

static uint8_t buffered_decision(
	const struct v34_phase3_s_receiver *receiver, size_t index)
{
	return receiver->decisions[
		(receiver->next_decision + index) % receive_window(receiver)];
}

static uint8_t expected_rotation(
	const struct v34_phase3_s_receiver *receiver,
	size_t symbol, uint8_t offset)
{
	unsigned int s_bar_offset =
		symbol >= receiver->s_symbols ? 2u : 0u;
	size_t absolute_symbol =
		V34_PHASE3_S_SYMBOLS - receiver->s_symbols + symbol;
	unsigned int rotation =
		offset + s_bar_offset + 4u - (absolute_symbol & 1u);

	return (uint8_t)(rotation & 3u);
}

static bool boundary_matches(
	const struct v34_phase3_s_receiver *receiver, uint8_t offset)
{
	const size_t first =
		receiver->s_symbols - V34_PHASE3_S_BOUNDARY_ANCHOR_SYMBOLS;
	const size_t end =
		receiver->s_symbols + V34_PHASE3_S_BOUNDARY_ANCHOR_SYMBOLS;

	for (size_t symbol = first; symbol < end; ++symbol)
		if (buffered_decision(receiver, symbol) !=
		    expected_rotation(receiver, symbol, offset))
			return false;
	return true;
}

static bool correlate(
	const struct v34_phase3_s_receiver *receiver,
	struct v34_phase3_s_detection *detection)
{
	struct v34_phase3_s_detection best = { 0 };
	unsigned int best_errors = UINT_MAX;
	size_t window = receive_window(receiver);
	bool found = false;

	for (uint8_t offset = 0; offset < 4u; ++offset) {
		struct v34_phase3_s_detection candidate = {
			.clockwise_quarter_turn_offset = offset,
		};

		if (!boundary_matches(receiver, offset))
			continue;

		for (size_t symbol = 0; symbol < window; ++symbol) {
			if (buffered_decision(receiver, symbol) ==
			    expected_rotation(receiver, symbol, offset))
				continue;
			if (symbol < receiver->s_symbols)
				candidate.s_errors++;
			else
				candidate.s_bar_errors++;
		}
		if (candidate.s_errors <= allowed_s_errors(receiver) &&
		    v34_phase3_s_detection_is_valid(&candidate)) {
			unsigned int errors =
				candidate.s_errors + candidate.s_bar_errors;

			if (!found || errors < best_errors) {
				best = candidate;
				best_errors = errors;
				found = true;
			}
		}
	}
	if (found)
		*detection = best;
	return found;
}

enum v34_phase3_s_receiver_result v34_phase3_s_receiver_receive(
	struct v34_phase3_s_receiver *receiver, uint8_t quarter_point,
	uint8_t clockwise_quarter_turns)
{
	struct v34_phase3_s_receiver next;
	struct v34_phase3_s_detection detection;
	size_t window;
	uint8_t decision;

	if (!v34_phase3_s_receiver_is_valid(receiver) || quarter_point >= 4u ||
	    clockwise_quarter_turns >= 4u)
		return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
	if (receiver->complete)
		return V34_PHASE3_S_RECEIVER_COMPLETE;
	window = receive_window(receiver);
	decision = (uint8_t)(quarter_point * 4u + clockwise_quarter_turns);
	next = *receiver;
	next.decisions[next.next_decision] = decision;
	next.next_decision++;
	if (next.next_decision == window)
		next.next_decision = 0;
	if (next.buffered_decisions < window)
		next.buffered_decisions++;
	if (next.buffered_decisions == window &&
	    correlate(&next, &detection)) {
		next.detection = detection;
		next.complete = true;
	}
	if (!v34_phase3_s_receiver_is_valid(&next))
		return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	return next.complete ? V34_PHASE3_S_RECEIVER_DETECTED :
			       V34_PHASE3_S_RECEIVER_OK;
}

bool v34_phase3_s_receiver_get_detection(
	const struct v34_phase3_s_receiver *receiver,
	struct v34_phase3_s_detection *detection)
{
	if (!v34_phase3_s_receiver_is_complete(receiver) || detection == NULL)
		return false;
	*detection = receiver->detection;
	return true;
}

const char *v34_phase3_s_receiver_result_name(
	enum v34_phase3_s_receiver_result result)
{
	switch (result) {
	case V34_PHASE3_S_RECEIVER_OK:
		return "ok";
	case V34_PHASE3_S_RECEIVER_DETECTED:
		return "detected";
	case V34_PHASE3_S_RECEIVER_COMPLETE:
		return "complete";
	case V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
