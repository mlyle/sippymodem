// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_s_receiver.h"

#include "memory_region.h"
#include "v34_constellation.h"

#include <limits.h>
#include <math.h>
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

bool v34_phase3_s_prefix_is_valid(
	const struct v34_phase3_s_prefix *prefix)
{
	if (prefix == NULL ||
	    prefix->recurrence_points > V34_PHASE3_S_PREFIX_RECURRENCE_LAG ||
	    prefix->recurrence_hits > V34_PHASE3_S_PREFIX_RECURRENCE_HITS ||
	    prefix->recurrence_qualified !=
		    (prefix->recurrence_hits ==
		     V34_PHASE3_S_PREFIX_RECURRENCE_HITS) ||
	    prefix->alternating_turns > V34_PHASE3_S_PREFIX_TURNS ||
	    prefix->complete !=
		    (prefix->alternating_turns == V34_PHASE3_S_PREFIX_TURNS &&
		     (!prefix->require_recurrence ||
		      prefix->recurrence_qualified)))
		return false;
	for (uint8_t index = 0; index < prefix->recurrence_points; ++index)
		if (!point_is_finite(&prefix->recurrence[index]))
			return false;
	for (uint8_t index = prefix->recurrence_points;
	     index < V34_PHASE3_S_PREFIX_RECURRENCE_LAG; ++index)
		if (prefix->recurrence[index].in_phase != 0.0 ||
		    prefix->recurrence[index].quadrature != 0.0)
			return false;
	if (!prefix->previous_valid)
		return prefix->previous.in_phase == 0.0 &&
		       prefix->previous.quadrature == 0.0 &&
		       prefix->previous_turn == 0u &&
		       prefix->alternating_turns == 0u &&
		       prefix->recurrence_points == 0u &&
		       prefix->recurrence_hits == 0u &&
		       !prefix->recurrence_qualified;
	if (prefix->recurrence_points == 0u)
		return false;
	return point_is_finite(&prefix->previous) &&
	       prefix->previous_turn < 4u &&
	       (prefix->alternating_turns == 0u ||
		(prefix->previous_turn == 1u ||
		 prefix->previous_turn == 3u));
}

enum v34_phase3_s_prefix_result v34_phase3_s_prefix_init(
	struct v34_phase3_s_prefix *prefix)
{
	if (prefix == NULL)
		return V34_PHASE3_S_PREFIX_INVALID_ARGUMENT;
	*prefix = (struct v34_phase3_s_prefix){ 0 };
	return V34_PHASE3_S_PREFIX_OK;
}

enum v34_phase3_s_prefix_result v34_phase3_s_prefix_init_recurrent(
	struct v34_phase3_s_prefix *prefix)
{
	if (prefix == NULL)
		return V34_PHASE3_S_PREFIX_INVALID_ARGUMENT;
	*prefix = (struct v34_phase3_s_prefix){
		.require_recurrence = true,
	};
	return V34_PHASE3_S_PREFIX_OK;
}

static bool point_has_power(
	const struct qam_point *point, double minimum_point_power)
{
	double power = point->in_phase * point->in_phase +
		       point->quadrature * point->quadrature;

	return isfinite(power) && power >= minimum_point_power;
}

static bool points_recur(
	const struct qam_point *previous,
	const struct qam_point *delayed,
	const struct qam_point *point, double minimum_point_power)
{
	double point_power = point->in_phase * point->in_phase +
			     point->quadrature * point->quadrature;
	double previous_power = previous->in_phase * previous->in_phase +
				 previous->quadrature * previous->quadrature;
	double delayed_power = delayed->in_phase * delayed->in_phase +
				delayed->quadrature * delayed->quadrature;
	double point_scale = sqrt(point_power);
	double delayed_scale = sqrt(delayed_power);
	double correlation;
	double delta_in_phase = point->in_phase - previous->in_phase;
	double delta_quadrature = point->quadrature - previous->quadrature;
	double transition_power =
		delta_in_phase * delta_in_phase +
		delta_quadrature * delta_quadrature;

	if (!point_has_power(previous, minimum_point_power) ||
	    !point_has_power(delayed, minimum_point_power) ||
	    !isfinite(point_scale) || !isfinite(delayed_scale) ||
	    point_scale == 0.0 || delayed_scale == 0.0 ||
	    !isfinite(transition_power))
		return false;
	correlation =
		(point->in_phase / point_scale) *
			(delayed->in_phase / delayed_scale) +
		(point->quadrature / point_scale) *
			(delayed->quadrature / delayed_scale);
	return isfinite(correlation) && correlation >= 0.98 &&
	       transition_power >= 0.02 * (point_power + previous_power);
}

static void remember_recurrence(
	struct v34_phase3_s_prefix *prefix,
	const struct qam_point *point)
{
	if (prefix->recurrence_points < V34_PHASE3_S_PREFIX_RECURRENCE_LAG) {
		prefix->recurrence[prefix->recurrence_points++] = *point;
		return;
	}
	prefix->recurrence[0] = prefix->recurrence[1];
	prefix->recurrence[1] = *point;
}

enum v34_phase3_s_prefix_result v34_phase3_s_prefix_receive(
	struct v34_phase3_s_prefix *prefix,
	const struct qam_point *point, double minimum_point_power)
{
	struct v34_phase3_s_prefix next;
	uint8_t turn;
	bool recurrence_just_qualified = false;

	if (!v34_phase3_s_prefix_is_valid(prefix) ||
	    !point_is_finite(point) || !isfinite(minimum_point_power) ||
	    minimum_point_power <= 0.0)
		return V34_PHASE3_S_PREFIX_INVALID_ARGUMENT;
	if (prefix->complete)
		return V34_PHASE3_S_PREFIX_COMPLETE;
	if (!point_has_power(point, minimum_point_power)) {
		bool require_recurrence = prefix->require_recurrence;

		*prefix = (struct v34_phase3_s_prefix){
			.require_recurrence = require_recurrence,
		};
		return V34_PHASE3_S_PREFIX_OK;
	}
	next = *prefix;
	if (!next.previous_valid ||
	    !point_has_power(&next.previous, minimum_point_power)) {
		bool require_recurrence = next.require_recurrence;

		next = (struct v34_phase3_s_prefix){
			.previous = *point,
			.previous_valid = true,
			.require_recurrence = require_recurrence,
		};
		remember_recurrence(&next, point);
		*prefix = next;
		return V34_PHASE3_S_PREFIX_OK;
	}
	if (qam_nearest_quarter_turn(&next.previous, point, &turn) != QAM_OK)
		return V34_PHASE3_S_PREFIX_INVALID_ARGUMENT;
	if (turn == 1u || turn == 3u) {
		if (next.alternating_turns == 0u ||
		    turn != next.previous_turn) {
			if (next.alternating_turns <
			    V34_PHASE3_S_PREFIX_TURNS)
				next.alternating_turns++;
		}
		else
			next.alternating_turns = 1u;
		next.previous_turn = turn;
	} else {
		next.previous_turn = 0u;
		next.alternating_turns = 0u;
	}
	if (next.recurrence_points == V34_PHASE3_S_PREFIX_RECURRENCE_LAG &&
	    (!next.recurrence_qualified || next.require_recurrence)) {
		if (points_recur(
			    &next.previous, &next.recurrence[0], point,
			    minimum_point_power)) {
			if (next.recurrence_hits <
			    V34_PHASE3_S_PREFIX_RECURRENCE_HITS)
				next.recurrence_hits++;
		} else {
			next.recurrence_hits = 0u;
			next.recurrence_qualified = false;
		}
		if (!next.recurrence_qualified &&
		    next.recurrence_hits ==
		    V34_PHASE3_S_PREFIX_RECURRENCE_HITS) {
			next.recurrence_qualified = true;
			recurrence_just_qualified = true;
		}
	}
	remember_recurrence(&next, point);
	next.previous = *point;
	next.complete =
		next.alternating_turns == V34_PHASE3_S_PREFIX_TURNS &&
		(!next.require_recurrence || next.recurrence_qualified);
	if (!v34_phase3_s_prefix_is_valid(&next))
		return V34_PHASE3_S_PREFIX_INVALID_ARGUMENT;
	*prefix = next;
	if (next.complete)
		return V34_PHASE3_S_PREFIX_DETECTED;
	return recurrence_just_qualified ?
		       V34_PHASE3_S_PREFIX_REBASE_TIMING :
		       V34_PHASE3_S_PREFIX_OK;
}

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
	       receiver->pending_detection.s_errors == 0 &&
	       receiver->pending_detection.s_bar_errors == 0))) ||
	    (!receiver->detection_pending &&
	     (receiver->pending_symbols != 0 ||
	      receiver->pending_metric ||
	      receiver->pending_detection.clockwise_quarter_turn_offset != 0 ||
	      receiver->pending_detection.s_errors != 0 ||
	      receiver->pending_detection.s_bar_errors != 0)) ||
	    (!receiver->target_seeded &&
	     receiver->next_target_rotation != 0u) ||
	    (receiver->target_seeded &&
	     receiver->next_target_rotation >= 4u) ||
	    (receiver->target_seeded && !receiver->metric_enabled) ||
	    (receiver->complete && receiver->target_seeded) ||
	    (!receiver->complete && receiver->post_marker_symbols != 0) ||
	    (receiver->complete &&
	     (receiver->buffered_decisions != window ||
	      !v34_phase3_s_detection_is_valid(&receiver->detection))) ||
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

		for (size_t symbol = 0; symbol < window; ++symbol) {
			if (symbol_matches(receiver, symbol, offset, metric))
				continue;
			if (symbol < receiver->s_symbols)
				candidate.s_errors++;
			else
				candidate.s_bar_errors++;
		}
		if (v34_phase3_s_detection_is_valid(&candidate)) {
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

static bool metric_confirms_complete_s_bar(
	const struct v34_phase3_s_detection *hard,
	const struct v34_phase3_s_detection *metric)
{
	return hard->clockwise_quarter_turn_offset ==
		       metric->clockwise_quarter_turn_offset &&
	       metric->s_bar_errors == 0u;
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

static bool s_bar_target_is_qualified(
	struct v34_analytic_sample observation,
	struct v34_analytic_sample target,
	struct v34_analytic_sample opposite)
{
	uint64_t target_metric = metric(observation, target);
	uint64_t opposite_metric = metric(observation, opposite);

	/* Stop training only when the S-bar opposite clearly beats the expected
	 * S target. Ambiguous S observations still need to train the equalizer;
	 * requiring a clean S decision here can prevent carrier acquisition on a
	 * channel which needs that adaptation most. */
	return target_metric > opposite_metric &&
	       target_metric - opposite_metric > V34_PHASE3_S_METRIC_MARGIN;
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
	receiver->next_target_rotation = 0u;
	receiver->detection_pending = false;
	receiver->pending_metric = false;
	receiver->target_seeded = false;
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
			    false) ||
		    (metric_found &&
		     metric_confirms_complete_s_bar(
			     &hard_detection, &metric_detection))) {
			complete_detection(&next, &hard_detection, 0);
		} else if (!next.detection_pending || next.pending_metric) {
			next.pending_detection = hard_detection;
			next.pending_symbols = 0;
			next.detection_pending = true;
			next.pending_metric = false;
		} else if (detection_errors(&hard_detection) <
			   detection_errors(&next.pending_detection)) {
			next.pending_detection = hard_detection;
			next.pending_symbols = 0;
		}
	} else if (metric_found && !next.complete) {
		if (metric_detection.s_bar_errors == 0u) {
			complete_detection(&next, &metric_detection, 0);
		} else if (!next.detection_pending) {
			next.pending_detection = metric_detection;
			next.pending_symbols = 0;
			next.detection_pending = true;
			next.pending_metric = true;
		} else if (next.pending_metric &&
			   detection_errors(&metric_detection) <
				   detection_errors(&next.pending_detection)) {
			next.pending_detection = metric_detection;
			next.pending_symbols = 0;
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
	    receiver->target_seeded ||
	    receiver->metric_enabled || quarter_point >= 4u ||
	    clockwise_quarter_turns >= 4u)
		return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
	return receive_decision(
		receiver,
		(uint8_t)(quarter_point * 4u + clockwise_quarter_turns), 0u);
}

enum v34_phase3_s_receiver_result
v34_phase3_s_receiver_receive_adaptive(
	struct v34_phase3_s_receiver *receiver, uint8_t quarter_point,
	uint8_t clockwise_quarter_turns,
	struct v34_analytic_sample observation,
	struct v34_phase3_s_adaptation *adaptation)
{
	struct v34_phase3_s_receiver next;
	struct v34_phase3_s_adaptation generated = { 0 };
	struct v34_analytic_sample opposite;
	struct v34_analytic_sample target;
	enum v34_phase3_s_receiver_result result;
	uint8_t evidence;
	uint8_t rotation;

	if (receiver == NULL || adaptation == NULL ||
	    memory_regions_overlap(
		    receiver, receiver == NULL ? 0u : sizeof(*receiver),
		    adaptation, sizeof(*adaptation)) ||
	    !v34_phase3_s_receiver_is_valid(receiver) ||
	    !receiver->metric_enabled || quarter_point >= 4u ||
	    clockwise_quarter_turns >= 4u ||
	    !observation_evidence(observation, &evidence))
		return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
	if (receiver->complete)
		return V34_PHASE3_S_RECEIVER_COMPLETE;
	next = *receiver;
	rotation = next.target_seeded ? next.next_target_rotation :
				       clockwise_quarter_turns;
	if (v34_training_constellation_fixed_point(
		    V34_CONSTELLATION_4_POINT, 0u, rotation, &target) !=
	    V34_CONSTELLATION_OK ||
	    v34_training_constellation_fixed_point(
		    V34_CONSTELLATION_4_POINT, 0u,
		    (uint8_t)((rotation + 2u) & 3u), &opposite) !=
		    V34_CONSTELLATION_OK)
		return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
	next.target_seeded = true;
	/* S alternates between two absolute quarter rotations. The bounded
	 * detector window is even, so its cursor retains parity after wrapping. */
	next.next_target_rotation = (uint8_t)(
		(rotation + ((next.next_decision & 1u) != 0u ? 1u : 3u)) &
		3u);
	result = receive_decision(
		&next,
		(uint8_t)(quarter_point * 4u + clockwise_quarter_turns),
		evidence);
	if (result != V34_PHASE3_S_RECEIVER_OK &&
	    result != V34_PHASE3_S_RECEIVER_DETECTED)
		return result;
	if (result == V34_PHASE3_S_RECEIVER_DETECTED) {
		next.target_seeded = false;
		next.next_target_rotation = 0u;
	} else if (!next.detection_pending &&
		   !s_bar_target_is_qualified(observation, target, opposite)) {
		generated.target = target;
		generated.adapt = true;
	}
	if (!v34_phase3_s_receiver_is_valid(&next))
		return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*adaptation = generated;
	return result;
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

static bool hunt_prefix_is_candidate(
	const struct v34_phase3_s_prefix *prefix)
{
	return v34_phase3_s_prefix_is_valid(prefix) &&
	       prefix->alternating_turns == V34_PHASE3_S_PREFIX_TURNS &&
	       prefix->recurrence_hits >=
		       V34_PHASE3_S_HUNT_CANDIDATE_RECURRENCE_HITS;
}

bool v34_phase3_s_hunt_is_valid(
	const struct v34_phase3_s_hunt *hunt)
{
	bool first_complete;
	bool second_complete;

	if (hunt == NULL ||
	    !v34_phase3_s_receiver_is_valid(&hunt->marker[0]) ||
	    !v34_phase3_s_receiver_is_valid(&hunt->marker[1]) ||
	    hunt->marker[0].s_symbols !=
		    V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS ||
	    hunt->marker[1].s_symbols !=
		    V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS ||
	    hunt->marker[0].metric_enabled || hunt->marker[1].metric_enabled ||
	    hunt->marker[0].target_seeded || hunt->marker[1].target_seeded ||
	    !v34_phase3_s_prefix_is_valid(&hunt->prefix) ||
	    hunt->candidate_symbols > V34_PHASE3_S_HUNT_CANDIDATE_SYMBOLS ||
	    hunt->post_marker_symbols >
		    V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS ||
	    hunt->recent_observation_count >
		    V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS ||
	    hunt->next_target_rotation >= 4u ||
	    hunt->candidate != (hunt->candidate_symbols != 0u) ||
	    (hunt->complete &&
	     (!hunt->candidate ||
	      hunt->post_marker_symbols > hunt->recent_observation_count)) ||
	    (hunt->complete ?
		     !v34_phase3_s_detection_is_valid(&hunt->detection) :
		     hunt->detection.clockwise_quarter_turn_offset != 0u ||
		     hunt->detection.s_errors != 0u ||
		     hunt->detection.s_bar_errors != 0u))
		return false;
	first_complete =
		v34_phase3_s_receiver_is_complete(&hunt->marker[0]);
	second_complete =
		v34_phase3_s_receiver_is_complete(&hunt->marker[1]);
	if (hunt->complete != (first_complete || second_complete) ||
	    (!hunt->complete && hunt->post_marker_symbols != 0u) ||
	    (hunt->complete &&
	     !((first_complete &&
		hunt->post_marker_symbols ==
			hunt->marker[0].post_marker_symbols) ||
	       (second_complete &&
		hunt->post_marker_symbols ==
			hunt->marker[1].post_marker_symbols))))
		return false;
	if (!hunt->candidate)
		return hunt->candidate_rotation[0] == 0u &&
		       hunt->candidate_rotation[1] == 0u &&
		       hunt->next_target_rotation == 0u &&
		       !hunt->s_bar_seen &&
		       !first_complete && !second_complete;
	return hunt_prefix_is_candidate(&hunt->prefix) &&
	       hunt->candidate_rotation[0] < 4u &&
	       hunt->candidate_rotation[1] < 4u &&
	       (((hunt->candidate_rotation[0] + 1u) & 3u) ==
			hunt->candidate_rotation[1] ||
		((hunt->candidate_rotation[0] + 3u) & 3u) ==
			hunt->candidate_rotation[1]);
}

bool v34_phase3_s_hunt_is_complete(
	const struct v34_phase3_s_hunt *hunt)
{
	return v34_phase3_s_hunt_is_valid(hunt) && hunt->complete;
}

static bool reset_hunt(struct v34_phase3_s_hunt *hunt, bool acquire_phase)
{
	struct v34_phase3_s_hunt reset = { .acquire_phase = acquire_phase };

	if (v34_phase3_s_receiver_init_suffix(
		    &reset.marker[0],
		    V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS) !=
		    V34_PHASE3_S_RECEIVER_OK ||
	    v34_phase3_s_receiver_init_suffix(
		    &reset.marker[1],
		    V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS) !=
		    V34_PHASE3_S_RECEIVER_OK ||
	    v34_phase3_s_prefix_init_recurrent(&reset.prefix) !=
		    V34_PHASE3_S_PREFIX_OK)
		return false;
	*hunt = reset;
	return true;
}

enum v34_phase3_s_receiver_result v34_phase3_s_hunt_init(
	struct v34_phase3_s_hunt *hunt)
{
	struct v34_phase3_s_hunt initialized;

	if (hunt == NULL || !reset_hunt(&initialized, false) ||
	    !v34_phase3_s_hunt_is_valid(&initialized))
		return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
	*hunt = initialized;
	return V34_PHASE3_S_RECEIVER_OK;
}

enum v34_phase3_s_receiver_result v34_phase3_s_hunt_init_training(
	struct v34_phase3_s_hunt *hunt)
{
	enum v34_phase3_s_receiver_result result = v34_phase3_s_hunt_init(hunt);

	if (result == V34_PHASE3_S_RECEIVER_OK)
		hunt->acquire_phase = true;
	return result;
}

static struct qam_point normalized_fixed_observation(
	struct v34_analytic_sample observation)
{
	return (struct qam_point){
		.in_phase = observation.in_phase /
			(double)V34_TRAINING_FIXED_AMPLITUDE,
		.quadrature = observation.quadrature /
			(double)V34_TRAINING_FIXED_AMPLITUDE,
	};
}

static bool try_start_hunt_candidate(
	struct v34_phase3_s_hunt *hunt,
	const struct v34_fixed_training_decision *current)
{
	uint8_t previous_quarter;
	uint8_t previous_rotation;
	uint8_t current_rotation = current->clockwise_quarter_turns;
	uint8_t delta;

	if (hunt->prefix.recurrence_points !=
		    V34_PHASE3_S_PREFIX_RECURRENCE_LAG ||
	    v34_training_constellation_slice(
		    V34_CONSTELLATION_4_POINT,
		    &hunt->prefix.recurrence[0], &previous_quarter,
		    &previous_rotation) != V34_CONSTELLATION_OK ||
	    previous_quarter != 0u)
		return false;
	delta = (uint8_t)((previous_rotation + 4u -
			   current->clockwise_quarter_turns) & 3u);
	if (delta != 1u && delta != 3u) {
		const struct qam_point *points = hunt->prefix.recurrence;
		int sign = hunt->prefix.previous_turn == 1u ? 1 : -1;
		struct qam_point aligned = {
			.in_phase = points[1].in_phase - sign * points[0].quadrature,
			.quadrature = points[1].quadrature + sign * points[0].in_phase,
		};

		/* Unacquired S can recur near quadrant boundaries while its hard
		 * decisions are not adjacent. During training, align the previous
		 * point by the measured quarter turn and slice their sum: this
		 * selects the nearest adjacent target pair jointly. Do not let
		 * this acquisition rule interrupt ordinary online QAM data. */
		if (!hunt->acquire_phase)
			return reset_hunt(hunt, false);
		if (v34_training_constellation_slice(
			    V34_CONSTELLATION_4_POINT, &aligned,
			    &previous_quarter, &current_rotation) != V34_CONSTELLATION_OK)
			return false;
		previous_rotation = (uint8_t)(
			(current_rotation + hunt->prefix.previous_turn) & 3u);
	}
	if (v34_phase3_s_receiver_init_suffix(
		    &hunt->marker[0],
		    V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS) !=
		    V34_PHASE3_S_RECEIVER_OK ||
	    v34_phase3_s_receiver_init_suffix(
		    &hunt->marker[1],
		    V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS) !=
		    V34_PHASE3_S_RECEIVER_OK)
		return false;
	hunt->candidate_rotation[0] = current_rotation;
	hunt->candidate_rotation[1] = previous_rotation;
	hunt->next_target_rotation = previous_rotation;
	hunt->candidate = true;
	hunt->candidate_symbols = V34_PHASE3_S_HUNT_CANDIDATE_SYMBOLS;
	return true;
}

static bool prepare_hunt_adaptation(
	struct v34_phase3_s_hunt *hunt,
	struct v34_analytic_sample observation,
	struct v34_phase3_s_adaptation *adaptation)
{
	struct v34_analytic_sample target;
	struct v34_analytic_sample opposite;
	uint8_t rotation = hunt->next_target_rotation;

	if (hunt->s_bar_seen)
		return true;
	/* The candidate's first observation was already offered for adaptation
	 * when it was opened. Leave the final S suffix untouched: a non-causal
	 * equalizer begins to see S-bar before the hard-decision boundary. */
	if (V34_PHASE3_S_HUNT_CANDIDATE_SYMBOLS -
		    hunt->candidate_symbols +
		    1u >=
	    V34_PHASE3_S_HUNT_ADAPTATION_SYMBOLS)
		return true;
	if (v34_training_constellation_fixed_point(
		    V34_CONSTELLATION_4_POINT, 0u, rotation, &target) !=
		    V34_CONSTELLATION_OK ||
	    v34_training_constellation_fixed_point(
		    V34_CONSTELLATION_4_POINT, 0u,
		    (uint8_t)((rotation + 2u) & 3u), &opposite) !=
		    V34_CONSTELLATION_OK)
		return false;
	if (s_bar_target_is_qualified(observation, target, opposite)) {
		hunt->s_bar_seen = true;
		hunt->next_target_rotation = 0u;
		return true;
	}
	adaptation->target = target;
	adaptation->adapt = true;
	hunt->next_target_rotation =
		rotation == hunt->candidate_rotation[0] ?
			hunt->candidate_rotation[1] :
			hunt->candidate_rotation[0];
	return true;
}

static uint8_t mapped_hunt_rotation(
	const struct v34_phase3_s_hunt *hunt, size_t hypothesis,
	uint8_t rotation)
{
	uint8_t current = hunt->candidate_rotation[0];
	uint8_t previous = hunt->candidate_rotation[1];

	if (rotation == current)
		return hypothesis == 0u ? 0u : 3u;
	if (rotation == previous)
		return hypothesis == 0u ? 3u : 0u;
	if (rotation == ((current + 2u) & 3u))
		return hypothesis == 0u ? 2u : 1u;
	return hypothesis == 0u ? 1u : 2u;
}

static bool complete_hunt(
	struct v34_phase3_s_hunt *hunt, size_t hypothesis)
{
	struct v34_phase3_s_detection detection;

	if (!v34_phase3_s_receiver_get_detection(
		    &hunt->marker[hypothesis], &detection))
		return false;
	for (uint8_t rotation = 0u; rotation < 4u; ++rotation) {
		if (mapped_hunt_rotation(hunt, hypothesis, rotation) !=
		    detection.clockwise_quarter_turn_offset)
			continue;
		detection.clockwise_quarter_turn_offset = rotation;
		hunt->detection = detection;
		hunt->post_marker_symbols =
			hunt->marker[hypothesis].post_marker_symbols;
		hunt->complete = true;
		return true;
	}
	return false;
}

static void remember_hunt_observation(
	struct v34_phase3_s_hunt *hunt,
	struct v34_analytic_sample observation)
{
	if (hunt->recent_observation_count ==
	    V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS) {
		for (size_t index = 1u;
		     index < V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS; ++index)
			hunt->recent_observation[index - 1u] =
				hunt->recent_observation[index];
		hunt->recent_observation_count--;
	}
	hunt->recent_observation[hunt->recent_observation_count++] = observation;
}

static enum v34_phase3_s_receiver_result receive_hunt_candidate(
	struct v34_phase3_s_hunt *hunt,
	const struct v34_fixed_training_decision *hard)
{
	for (size_t hypothesis = 0u; hypothesis < 2u; ++hypothesis) {
		enum v34_phase3_s_receiver_result result =
			v34_phase3_s_receiver_receive(
				&hunt->marker[hypothesis], hard->quarter_point,
				mapped_hunt_rotation(
					hunt, hypothesis,
					hard->clockwise_quarter_turns));

		if (result != V34_PHASE3_S_RECEIVER_OK &&
		    result != V34_PHASE3_S_RECEIVER_DETECTED)
			return result;
		if (result == V34_PHASE3_S_RECEIVER_DETECTED &&
		    !complete_hunt(hunt, hypothesis))
			return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
		if (result == V34_PHASE3_S_RECEIVER_DETECTED)
			return result;
	}
	return V34_PHASE3_S_RECEIVER_OK;
}

enum v34_phase3_s_receiver_result v34_phase3_s_hunt_receive(
	struct v34_phase3_s_hunt *hunt,
	struct v34_analytic_sample observation,
	struct v34_phase3_s_adaptation *adaptation, bool *control)
{
	enum {
		MINIMUM_POINT_POWER_Q8 = 64,
	};
	struct v34_phase3_s_hunt next;
	struct v34_phase3_s_adaptation generated = { 0 };
	struct v34_fixed_training_decision hard;
	struct qam_point normalized;
	enum v34_phase3_s_receiver_result result;
	enum v34_phase3_s_prefix_result prefix_result;
	bool generated_control = false;

	if (!v34_phase3_s_hunt_is_valid(hunt) || adaptation == NULL ||
	    control == NULL ||
	    memory_regions_overlap(hunt, sizeof(*hunt), adaptation,
			   sizeof(*adaptation)) ||
	    memory_regions_overlap(hunt, sizeof(*hunt), control,
			   sizeof(*control)) ||
	    memory_regions_overlap(adaptation, sizeof(*adaptation), control,
			   sizeof(*control)) ||
	    v34_training_constellation_fixed_slice(
		    V34_CONSTELLATION_4_POINT, observation, &hard) !=
		    V34_CONSTELLATION_OK)
		return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
	if (hunt->complete)
		return V34_PHASE3_S_RECEIVER_COMPLETE;
	next = *hunt;
	remember_hunt_observation(&next, observation);
	if (next.candidate) {
		if (!prepare_hunt_adaptation(
			    &next, observation, &generated))
			return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
		result = receive_hunt_candidate(&next, &hard);
		if (result != V34_PHASE3_S_RECEIVER_OK &&
		    result != V34_PHASE3_S_RECEIVER_DETECTED)
			return result;
		generated_control = true;
		if (result != V34_PHASE3_S_RECEIVER_DETECTED &&
		    --next.candidate_symbols == 0u &&
		    !reset_hunt(&next, next.acquire_phase))
			return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
	} else {
		/* Qualify recurrence from measured points. Slicing first would make
		 * unrelated data in alternating quadrants look perfectly periodic. */
		normalized = normalized_fixed_observation(observation);
		prefix_result = v34_phase3_s_prefix_receive(
			&next.prefix, &normalized,
			(double)MINIMUM_POINT_POWER_Q8 / 256.0);
		if (prefix_result != V34_PHASE3_S_PREFIX_OK &&
		    prefix_result != V34_PHASE3_S_PREFIX_DETECTED &&
		    prefix_result != V34_PHASE3_S_PREFIX_REBASE_TIMING)
			return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
		result = V34_PHASE3_S_RECEIVER_OK;
		if (hunt_prefix_is_candidate(&next.prefix) &&
		    !try_start_hunt_candidate(&next, &hard))
			return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
		if (next.candidate) {
			result = receive_hunt_candidate(&next, &hard);
			if (result != V34_PHASE3_S_RECEIVER_OK)
				return result;
			generated_control = true;
			if (v34_training_constellation_fixed_point(
				    V34_CONSTELLATION_4_POINT, 0u,
				    next.candidate_rotation[0], &generated.target) !=
			    V34_CONSTELLATION_OK)
				return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
			generated.adapt = true;
		}
	}
	if (!v34_phase3_s_hunt_is_valid(&next))
		return V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT;
	*hunt = next;
	*adaptation = generated;
	*control = generated_control;
	return result;
}

bool v34_phase3_s_hunt_get_detection(
	const struct v34_phase3_s_hunt *hunt,
	struct v34_phase3_s_detection *detection)
{
	if (!v34_phase3_s_hunt_is_valid(hunt) || detection == NULL ||
	    !hunt->complete)
		return false;
	*detection = hunt->detection;
	return true;
}

bool v34_phase3_s_hunt_get_post_marker_symbols(
	const struct v34_phase3_s_hunt *hunt, uint8_t *symbols)
{
	if (!v34_phase3_s_hunt_is_complete(hunt) || symbols == NULL)
		return false;
	*symbols = hunt->post_marker_symbols;
	return true;
}

bool v34_phase3_s_hunt_get_post_marker_observation(
	const struct v34_phase3_s_hunt *hunt, uint8_t index,
	struct v34_analytic_sample *observation)
{
	size_t first;

	if (!v34_phase3_s_hunt_is_complete(hunt) || observation == NULL ||
	    index >= hunt->post_marker_symbols)
		return false;
	first = hunt->recent_observation_count - hunt->post_marker_symbols;
	*observation = hunt->recent_observation[first + index];
	return true;
}
