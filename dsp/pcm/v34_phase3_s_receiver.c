// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_s_receiver.h"

#include "v34_constellation.h"

#include <limits.h>
#include <stddef.h>

enum {
	V34_PHASE3_S_BOUNDARY_ANCHOR_SYMBOLS = 2,
	V34_PHASE3_S_METRIC_SHIFT = 14,
	/* Require one fixed startup hypothesis to beat its opposite by the
	 * receiver's relative-decision margin. */
	V34_PHASE3_S_METRIC_MARGIN = 0x80,
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
	    receiver->pending_symbols >
		    V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS ||
	    receiver->post_marker_symbols >
		    V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS ||
	    (receiver->buffered_decisions < window &&
	     receiver->next_decision != receiver->buffered_decisions) ||
	    (receiver->detection_pending &&
	     (receiver->complete || receiver->buffered_decisions != window ||
	      receiver->pending_symbols >=
		      V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS ||
	      !v34_phase3_s_detection_is_valid(
		      &receiver->pending_detection) ||
	      (!receiver->pending_metric &&
	       receiver->pending_detection.s_bar_errors == 0))) ||
	    (!receiver->detection_pending &&
	     (receiver->pending_symbols != 0 ||
	      receiver->pending_metric ||
	      receiver->pending_detection.clockwise_quarter_turn_offset != 0 ||
	      receiver->pending_detection.s_errors != 0 ||
	      receiver->pending_detection.s_bar_errors != 0)) ||
	    (!receiver->complete && receiver->post_marker_symbols != 0) ||
	    (receiver->complete &&
	     (receiver->buffered_decisions != window ||
	      !v34_phase3_s_detection_is_valid(&receiver->detection) ||
	      receiver->detection.s_errors > allowed_s_errors(receiver))) ||
	    (!receiver->complete &&
	     (receiver->detection.clockwise_quarter_turn_offset != 0 ||
	      receiver->detection.s_errors != 0 ||
	      receiver->detection.s_bar_errors != 0)))
		return false;
	if (!receiver->metric_enabled && receiver->pending_metric)
		return false;
	for (size_t index = 0; index < V34_PHASE3_S_SEQUENCE_SYMBOLS;
	     ++index)
		if (!receiver->metric_enabled && receiver->decisions[index] >= 16u)
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

enum v34_phase3_s_receiver_result
v34_phase3_s_receiver_init_fixed_suffix(
	struct v34_phase3_s_receiver *receiver, uint8_t s_symbols)
{
	struct v34_phase3_s_receiver initialized = {
		.s_symbols = s_symbols,
		.metric_enabled = true,
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
		(receiver->next_decision + index) % receive_window(receiver)] &
	       0x0fu;
}

static uint8_t buffered_metric_evidence(
	const struct v34_phase3_s_receiver *receiver, size_t index)
{
	return receiver->decisions[
		(receiver->next_decision + index) % receive_window(receiver)] >>
	       4u;
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

static bool symbol_matches(
	const struct v34_phase3_s_receiver *receiver, size_t symbol,
	uint8_t offset, bool metric)
{
	uint8_t expected = expected_rotation(receiver, symbol, offset);

	return metric ?
		       (buffered_metric_evidence(receiver, symbol) &
			(1u << expected)) != 0u :
		       buffered_decision(receiver, symbol) == expected;
}

static bool boundary_matches(
	const struct v34_phase3_s_receiver *receiver, uint8_t offset,
	bool metric)
{
	const size_t first =
		receiver->s_symbols - V34_PHASE3_S_BOUNDARY_ANCHOR_SYMBOLS;
	const size_t end =
		receiver->s_symbols + V34_PHASE3_S_BOUNDARY_ANCHOR_SYMBOLS;

	for (size_t symbol = first; symbol < end; ++symbol)
		if (!symbol_matches(receiver, symbol, offset, metric))
			return false;
	return true;
}

static bool s_tail_matches(
	const struct v34_phase3_s_receiver *receiver, uint8_t offset,
	bool metric)
{
	const size_t first =
		receiver->s_symbols - V34_PHASE3_S_BOUNDARY_ANCHOR_SYMBOLS;

	for (size_t symbol = first; symbol < receiver->s_symbols; ++symbol)
		if (!symbol_matches(receiver, symbol, offset, metric))
			return false;
	return true;
}

static bool correlate(
	const struct v34_phase3_s_receiver *receiver,
	struct v34_phase3_s_detection *detection, bool metric)
{
	struct v34_phase3_s_detection best = { 0 };
	unsigned int best_errors = UINT_MAX;
	size_t window = receive_window(receiver);
	bool found = false;

	for (uint8_t offset = 0; offset < 4u; ++offset) {
		struct v34_phase3_s_detection candidate = {
			.clockwise_quarter_turn_offset = offset,
		};

		if (!s_tail_matches(receiver, offset, metric))
			continue;

		for (size_t symbol = 0; symbol < window; ++symbol) {
			if (symbol_matches(receiver, symbol, offset, metric))
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

static unsigned int detection_errors(
	const struct v34_phase3_s_detection *detection)
{
	return detection->s_errors + detection->s_bar_errors;
}

static uint64_t metric(
	struct v34_analytic_sample observation,
	struct v34_analytic_sample reference)
{
	int32_t in_phase = (int16_t)(
		(uint16_t)observation.in_phase - (uint16_t)reference.in_phase);
	int32_t quadrature = (int16_t)(
		(uint16_t)observation.quadrature -
		(uint16_t)reference.quadrature);

	return ((uint64_t)((int64_t)in_phase * in_phase) +
		(uint64_t)((int64_t)quadrature * quadrature)) >>
	       V34_PHASE3_S_METRIC_SHIFT;
}

static bool observation_evidence(
	struct v34_analytic_sample observation, uint8_t *generated)
{
	uint8_t evidence = 0;

	for (uint8_t rotation = 0; rotation < 2u; ++rotation) {
		struct v34_analytic_sample first;
		struct v34_analytic_sample opposite;
		uint64_t first_metric;
		uint64_t opposite_metric;

		if (v34_training_constellation_fixed_point(
			    V34_CONSTELLATION_4_POINT, 0, rotation, &first) !=
			    V34_CONSTELLATION_OK ||
		    v34_training_constellation_fixed_point(
			    V34_CONSTELLATION_4_POINT, 0,
			    (uint8_t)(rotation + 2u), &opposite) !=
			    V34_CONSTELLATION_OK)
			return false;
		first_metric = metric(observation, first);
		opposite_metric = metric(observation, opposite);
		if (opposite_metric > first_metric &&
		    opposite_metric - first_metric >
			    V34_PHASE3_S_METRIC_MARGIN)
			evidence |= (uint8_t)(1u << rotation);
		else if (first_metric > opposite_metric &&
			 first_metric - opposite_metric >
				 V34_PHASE3_S_METRIC_MARGIN)
			evidence |= (uint8_t)(1u << (rotation + 2u));
	}
	*generated = evidence;
	return true;
}

static void complete_detection(
	struct v34_phase3_s_receiver *receiver,
	const struct v34_phase3_s_detection *detection,
	uint8_t post_marker_symbols)
{
	receiver->detection = *detection;
	receiver->pending_detection =
		(struct v34_phase3_s_detection){ 0 };
	receiver->pending_symbols = 0;
	receiver->post_marker_symbols = post_marker_symbols;
	receiver->detection_pending = false;
	receiver->pending_metric = false;
	receiver->complete = true;
}

static enum v34_phase3_s_receiver_result receive_decision(
	struct v34_phase3_s_receiver *receiver, uint8_t decision,
	uint8_t metric_evidence)
{
	struct v34_phase3_s_receiver next;
	struct v34_phase3_s_detection hard_detection;
	struct v34_phase3_s_detection metric_detection;
	size_t window;
	bool hard_found = false;
	bool metric_found = false;

	if (receiver->complete)
		return V34_PHASE3_S_RECEIVER_COMPLETE;
	window = receive_window(receiver);
	next = *receiver;
	next.decisions[next.next_decision] =
		(uint8_t)(decision | (metric_evidence << 4u));
	next.next_decision++;
	if (next.next_decision == window)
		next.next_decision = 0;
	if (next.buffered_decisions < window)
		next.buffered_decisions++;
	if (next.detection_pending)
		next.pending_symbols++;
	if (next.buffered_decisions == window) {
		hard_found = correlate(&next, &hard_detection, false);
		metric_found = next.metric_enabled &&
			correlate(&next, &metric_detection, true);
	}
	if (hard_found) {
		if (boundary_matches(
			    &next,
			    hard_detection.clockwise_quarter_turn_offset,
			    false)) {
			complete_detection(&next, &hard_detection, 0);
		} else if (!next.detection_pending || next.pending_metric) {
			next.pending_detection = hard_detection;
			next.pending_symbols = 0;
			next.detection_pending = true;
			next.pending_metric = false;
		} else if (detection_errors(&hard_detection) <
			   detection_errors(&next.pending_detection)) {
			next.pending_detection = hard_detection;
		}
	} else if (metric_found && !next.complete) {
		if (!next.detection_pending) {
			next.pending_detection = metric_detection;
			next.pending_symbols = 0;
			next.detection_pending = true;
			next.pending_metric = true;
		} else if (next.pending_metric &&
			   detection_errors(&metric_detection) <
				   detection_errors(&next.pending_detection)) {
			next.pending_detection = metric_detection;
		}
	}
	if (next.detection_pending &&
	    next.pending_symbols >=
		    V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS)
		complete_detection(
			&next, &next.pending_detection, next.pending_symbols);
	if (!v34_phase3_s_receiver_is_valid(&next))
		return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	return next.complete ? V34_PHASE3_S_RECEIVER_DETECTED :
			       V34_PHASE3_S_RECEIVER_OK;
}

enum v34_phase3_s_receiver_result v34_phase3_s_receiver_receive(
	struct v34_phase3_s_receiver *receiver, uint8_t quarter_point,
	uint8_t clockwise_quarter_turns)
{
	if (!v34_phase3_s_receiver_is_valid(receiver) ||
	    receiver->metric_enabled || quarter_point >= 4u ||
	    clockwise_quarter_turns >= 4u)
		return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
	return receive_decision(
		receiver,
		(uint8_t)(quarter_point * 4u + clockwise_quarter_turns), 0u);
}

enum v34_phase3_s_receiver_result v34_phase3_s_receiver_receive_fixed(
	struct v34_phase3_s_receiver *receiver, uint8_t quarter_point,
	uint8_t clockwise_quarter_turns,
	struct v34_analytic_sample observation)
{
	uint8_t evidence;

	if (!v34_phase3_s_receiver_is_valid(receiver) ||
	    !receiver->metric_enabled || quarter_point >= 4u ||
	    clockwise_quarter_turns >= 4u ||
	    !observation_evidence(observation, &evidence))
		return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
	return receive_decision(
		receiver,
		(uint8_t)(quarter_point * 4u + clockwise_quarter_turns),
		evidence);
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
