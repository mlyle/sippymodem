// SPDX-License-Identifier: BSD-3-Clause

#include "conditioning_receiver.h"

#include "constellation.h"
#include "rate_signal.h"

#include <math.h>
#include <stddef.h>

enum {
	V32_CONDITIONING_FIXED_SYMBOLS =
		V32_CONDITIONING_S_SYMBOLS +
		V32_CONDITIONING_S_BAR_SYMBOLS,
	V32_CONDITIONING_MISMATCH_RESTART = 3,
	/* Receiver quality budget: leave no more observations unresolved than
	 * the standard's 256-symbol special TRN prefix. */
	V32_CONDITIONING_MAX_UNRESOLVED_TRN_SYMBOLS =
		V32_CONDITIONING_TRN_PREFIX_SYMBOLS,
	/* Receiver stability guard: require a clean TRN tail spanning the same
	 * sixteen symbols as the following minimum two-word rate evidence. This
	 * is a receiver quality policy, not an additional wire requirement. */
	V32_CONDITIONING_STABLE_TAIL_SYMBOLS =
		2 * V32_RATE_SIGNAL_DIBITS,
};

static const double V32_CONDITIONING_GAIN_ADAPTATION = 1.0 / 32.0;
static const double V32_CONDITIONING_MIN_POINT_POWER = 1e-12;

static bool role_is_valid(enum v32_transmitter_role role)
{
	return role == V32_CALLING_TRANSMITTER ||
	       role == V32_ANSWERING_TRANSMITTER;
}

static bool phase_is_valid(
	enum v32_conditioning_receiver_phase phase)
{
	return phase >= V32_CONDITIONING_SEARCH_S_BAR &&
	       phase <= V32_CONDITIONING_READY;
}

static bool point_has_power(const struct qam_point *point)
{
	double power;

	if (point == NULL || !isfinite(point->in_phase) ||
	    !isfinite(point->quadrature))
		return false;
	power = point->in_phase * point->in_phase +
		point->quadrature * point->quadrature;
	return isfinite(power) && power > V32_CONDITIONING_MIN_POINT_POWER;
}

static bool pattern_is_valid(
	const struct v32_conditioning_receiver *receiver)
{
	for (size_t index = 0;
	     index < V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS;
	     ++index) {
		if (receiver->prefix_turns[index] >=
			    QPSK_QUADRANT_COUNT ||
		    receiver->prefix_fallback[index] > index)
			return false;
	}
	return true;
}

static bool training_quality_is_sufficient(
	const struct v32_conditioning_receiver *receiver)
{
	return receiver->trn_symbols >=
			V32_CONDITIONING_TRN_MIN_SYMBOLS &&
	       receiver->confirmed_trn_symbols <= receiver->trn_symbols &&
	       receiver->trn_symbols - receiver->confirmed_trn_symbols <=
		       V32_CONDITIONING_MAX_UNRESOLVED_TRN_SYMBOLS &&
	       receiver->stable_trn_symbols >=
			V32_CONDITIONING_STABLE_TAIL_SYMBOLS;
}

bool v32_conditioning_receiver_is_valid(
	const struct v32_conditioning_receiver *receiver)
{
	bool training;

	if (receiver == NULL || !role_is_valid(receiver->source_role) ||
	    !phase_is_valid(receiver->phase) || !pattern_is_valid(receiver) ||
	    receiver->observed_quadrant >= QPSK_QUADRANT_COUNT ||
	    receiver->quadrant_offset >= QPSK_QUADRANT_COUNT ||
	    receiver->pending_quadrant_offset >=
		    QPSK_QUADRANT_COUNT ||
	    receiver->alternating_transitions >
		    V32_CONDITIONING_S_EPOCH_TRANSITIONS ||
	    receiver->s_bar_symbols >
		    V32_CONDITIONING_S_BAR_SYMBOLS ||
	    receiver->previous_turn >= QPSK_QUADRANT_COUNT ||
	    receiver->prefix_match >=
		    V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS ||
	    receiver->consecutive_mismatches >=
		    V32_CONDITIONING_MISMATCH_RESTART ||
	    (receiver->has_previous_point &&
	     !point_has_power(&receiver->previous_point)))
		return false;
	if (receiver->phase == V32_CONDITIONING_COUNT_S_BAR) {
		if (receiver->s_bar_symbols == 0 ||
		    receiver->prefix_match != 0)
			return false;
	} else if (receiver->s_bar_symbols != 0) {
		return false;
	}
	training = receiver->phase == V32_CONDITIONING_TRAIN_TRN ||
		   receiver->phase == V32_CONDITIONING_READY;
	if (!training)
		return receiver->trn_symbols == 0 &&
		       receiver->consecutive_mismatches == 0 &&
		       receiver->confirmed_trn_symbols == 0 &&
		       receiver->stable_trn_symbols == 0;
	if (!v32_conditioning_transmitter_is_valid(
		    &receiver->reference) ||
	    !qam_gain_is_valid(&receiver->gain) ||
	    receiver->reference.trn_symbols !=
		    V32_CONDITIONING_TRN_MAX_SYMBOLS ||
	    receiver->reference.emitted_symbols !=
		    V32_CONDITIONING_FIXED_SYMBOLS + receiver->trn_symbols ||
	    receiver->trn_symbols == 0 ||
	    receiver->trn_symbols >
		    V32_CONDITIONING_TRN_MAX_SYMBOLS ||
	    receiver->confirmed_trn_symbols == 0 ||
	    receiver->confirmed_trn_symbols > receiver->trn_symbols ||
	    receiver->stable_trn_symbols >
		    receiver->confirmed_trn_symbols ||
	    ((receiver->consecutive_mismatches == 0) !=
	     (receiver->stable_trn_symbols != 0)) ||
	    !receiver->has_previous_point)
		return false;
	if (receiver->phase == V32_CONDITIONING_READY)
		return training_quality_is_sufficient(receiver);
	return receiver->trn_symbols < V32_CONDITIONING_TRN_MAX_SYMBOLS &&
	       !training_quality_is_sufficient(receiver);
}

bool v32_conditioning_receiver_is_complete(
	const struct v32_conditioning_receiver *receiver)
{
	return v32_conditioning_receiver_is_valid(receiver) &&
	       receiver->phase == V32_CONDITIONING_READY;
}

static int skip_symbols(
	struct v32_conditioning_transmitter *transmitter,
	uint16_t count, uint8_t *last_quadrant)
{
	for (uint16_t symbol = 0; symbol < count; ++symbol)
		if (v32_conditioning_transmitter_next(
			    transmitter, last_quadrant) !=
		    V32_CONDITIONING_OK)
			return -1;
	return 0;
}

static int build_prefix_pattern(
	struct v32_conditioning_receiver *receiver)
{
	struct v32_conditioning_transmitter transmitter;
	uint8_t previous;

	if (v32_conditioning_transmitter_init(
		    &transmitter, receiver->source_role,
		    V32_CONDITIONING_TRN_MIN_SYMBOLS) !=
		    V32_CONDITIONING_OK ||
	    skip_symbols(&transmitter, V32_CONDITIONING_FIXED_SYMBOLS + 1u,
			 &previous) != 0)
		return -1;
	for (size_t index = 0;
	     index < V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS;
	     ++index) {
		uint8_t current;
		size_t fallback = 0;

		if (v32_conditioning_transmitter_next(
			    &transmitter, &current) !=
		    V32_CONDITIONING_OK)
			return -1;
		receiver->prefix_turns[index] =
			(uint8_t)((current - previous) & 3u);
		previous = current;
		if (index != 0) {
			fallback = receiver->prefix_fallback[index - 1u];
			while (fallback > 0 &&
			       receiver->prefix_turns[index] !=
				       receiver->prefix_turns[fallback])
				fallback =
					receiver->prefix_fallback[fallback - 1u];
			if (receiver->prefix_turns[index] ==
			    receiver->prefix_turns[fallback])
				fallback++;
		}
		receiver->prefix_fallback[index] = (uint8_t)fallback;
	}
	return 0;
}

enum v32_conditioning_receiver_result
v32_conditioning_receiver_init(
	struct v32_conditioning_receiver *receiver,
	enum v32_transmitter_role source_role)
{
	struct v32_conditioning_receiver initialized = {
		.source_role = source_role,
		.phase = V32_CONDITIONING_SEARCH_S_BAR,
	};

	if (receiver == NULL || !role_is_valid(source_role) ||
	    build_prefix_pattern(&initialized) != 0 ||
	    !v32_conditioning_receiver_is_valid(&initialized))
		return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V32_CONDITIONING_RECEIVER_OK;
}

static int reset_search(
	struct v32_conditioning_receiver *receiver,
	const struct qam_point *point)
{
	struct v32_conditioning_receiver reset;

	if (v32_conditioning_receiver_init(
		    &reset, receiver->source_role) !=
	    V32_CONDITIONING_RECEIVER_OK)
		return -1;
	reset.previous_point = *point;
	reset.has_previous_point = true;
	*receiver = reset;
	return 0;
}

static bool observe_s(
	struct v32_conditioning_receiver *receiver, uint8_t turn)
{
	bool epoch = receiver->phase == V32_CONDITIONING_MATCH_TRN &&
		     receiver->alternating_transitions ==
			     V32_CONDITIONING_S_EPOCH_TRANSITIONS &&
		     (turn & 1u) != 0 && turn == receiver->previous_turn;

	if ((turn & 1u) != 0) {
		if (receiver->alternating_transitions == 0 ||
		    turn != receiver->previous_turn) {
			if (receiver->alternating_transitions <
			    V32_CONDITIONING_S_EPOCH_TRANSITIONS)
				receiver->alternating_transitions++;
		} else {
			receiver->alternating_transitions = 1;
		}
	} else {
		receiver->alternating_transitions = 0;
	}
	receiver->previous_turn = turn;
	if (receiver->phase == V32_CONDITIONING_SEARCH_S_BAR &&
	    receiver->alternating_transitions ==
		    V32_CONDITIONING_S_DETECT_TRANSITIONS)
		receiver->phase = V32_CONDITIONING_MATCH_TRN;
	return epoch;
}

static bool observe_prefix_turn(
	struct v32_conditioning_receiver *receiver, uint8_t turn)
{
	while (receiver->prefix_match > 0 &&
	       turn != receiver->prefix_turns[receiver->prefix_match])
		receiver->prefix_match =
			receiver->prefix_fallback[receiver->prefix_match - 1u];
	if (turn == receiver->prefix_turns[receiver->prefix_match])
		receiver->prefix_match++;
	return receiver->prefix_match ==
	       V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS;
}

static int begin_training(
	struct v32_conditioning_receiver *receiver,
	const struct qam_point *point, uint16_t matched_symbols)
{
	struct qam_point expected_point;
	uint8_t expected_quadrant;

	if (matched_symbols == 0 ||
	    matched_symbols >
		    V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS + 1u ||
	    v32_conditioning_transmitter_init(
		    &receiver->reference, receiver->source_role,
		    V32_CONDITIONING_TRN_MAX_SYMBOLS) !=
		    V32_CONDITIONING_OK ||
	    skip_symbols(
		    &receiver->reference,
		    (uint16_t)(V32_CONDITIONING_FIXED_SYMBOLS + matched_symbols),
		    &expected_quadrant) != 0 ||
	    v32_4800_constellation_point(
		    expected_quadrant, &expected_point) !=
		    V32_CONSTELLATION_OK ||
	    qam_gain_init_from_reference(
		    &receiver->gain, point, &expected_point,
		    V32_CONDITIONING_GAIN_ADAPTATION) != QAM_GAIN_OK)
		return -1;
	receiver->trn_symbols = matched_symbols;
	receiver->quadrant_offset =
		(uint8_t)((receiver->observed_quadrant - expected_quadrant) & 3u);
	receiver->pending_quadrant_offset = receiver->quadrant_offset;
	receiver->confirmed_trn_symbols = matched_symbols;
	receiver->stable_trn_symbols = matched_symbols;
	receiver->alternating_transitions = 0;
	receiver->s_bar_symbols = 0;
	receiver->prefix_match = 0;
	receiver->phase = V32_CONDITIONING_TRAIN_TRN;
	return 0;
}

static int train_symbol(
	struct v32_conditioning_receiver *receiver,
	const struct qam_point *point)
{
	struct qam_point expected_point;
	uint8_t expected_quadrant;
	uint8_t observed_expected;
	uint8_t candidate_offset;

	if (v32_conditioning_transmitter_next(
		    &receiver->reference, &expected_quadrant) !=
		    V32_CONDITIONING_OK ||
	    v32_4800_constellation_point(
		    expected_quadrant, &expected_point) !=
		    V32_CONSTELLATION_OK)
		return -1;
	observed_expected =
		(uint8_t)((expected_quadrant + receiver->quadrant_offset) & 3u);
	if (receiver->observed_quadrant == observed_expected) {
		receiver->consecutive_mismatches = 0;
		receiver->pending_quadrant_offset = receiver->quadrant_offset;
		receiver->confirmed_trn_symbols++;
		receiver->stable_trn_symbols++;
		if (qam_gain_track(
			    &receiver->gain, point, &expected_point) !=
		    QAM_GAIN_OK)
			return -1;
	} else {
		receiver->stable_trn_symbols = 0;
		candidate_offset = (uint8_t)(
			(receiver->observed_quadrant - expected_quadrant) & 3u);
		if (receiver->consecutive_mismatches == 0 ||
		    receiver->pending_quadrant_offset != candidate_offset) {
			receiver->pending_quadrant_offset = candidate_offset;
			receiver->consecutive_mismatches = 1;
		} else {
			receiver->consecutive_mismatches++;
		}
		if (receiver->consecutive_mismatches ==
		    V32_CONDITIONING_MISMATCH_RESTART) {
			receiver->quadrant_offset =
				receiver->pending_quadrant_offset;
			receiver->consecutive_mismatches = 0;
			/* All three observations established the new phase. Count
			 * them as confirmed evidence, including the two held while
			 * distinguishing a phase change from an isolated error. */
			receiver->confirmed_trn_symbols +=
				V32_CONDITIONING_MISMATCH_RESTART;
			receiver->stable_trn_symbols =
				V32_CONDITIONING_MISMATCH_RESTART;
			if (qam_gain_track(
				    &receiver->gain, point, &expected_point) !=
			    QAM_GAIN_OK)
				return -1;
		}
	}
	receiver->trn_symbols++;
	if (training_quality_is_sufficient(receiver))
		receiver->phase = V32_CONDITIONING_READY;
	else if (receiver->trn_symbols ==
		 V32_CONDITIONING_TRN_MAX_SYMBOLS)
		return 1;
	return 0;
}

enum v32_conditioning_receiver_result
v32_conditioning_receiver_observe(
	struct v32_conditioning_receiver *receiver,
	const struct qam_point *point, bool *ready)
{
	struct v32_conditioning_receiver next;
	uint8_t turn;
	int training_result = 0;

	if (!v32_conditioning_receiver_is_valid(receiver) ||
	    !point_has_power(point) || ready == NULL)
		return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	if (v32_conditioning_receiver_is_complete(receiver))
		return V32_CONDITIONING_RECEIVER_COMPLETE;
	next = *receiver;
	if (!next.has_previous_point) {
		next.previous_point = *point;
		next.has_previous_point = true;
	} else {
		if (qam_nearest_quarter_turn(
			    &next.previous_point, point, &turn) !=
		    QAM_OK)
			return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
		next.observed_quadrant =
			(uint8_t)((next.observed_quadrant + turn) & 3u);
		if (next.phase == V32_CONDITIONING_SEARCH_S_BAR ||
		    next.phase == V32_CONDITIONING_MATCH_TRN) {
			bool prefix_ready = observe_prefix_turn(&next, turn);
			bool epoch = observe_s(&next, turn);

			if (prefix_ready &&
			    begin_training(
				    &next, point,
				    V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS + 1u) != 0)
				return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
			if (!prefix_ready && epoch) {
				next.phase = V32_CONDITIONING_COUNT_S_BAR;
				next.alternating_transitions = 0;
				next.s_bar_symbols = 1;
				next.prefix_match = 0;
			}
		} else if (next.phase == V32_CONDITIONING_COUNT_S_BAR) {
			if (next.s_bar_symbols <
			    V32_CONDITIONING_S_BAR_SYMBOLS) {
				next.s_bar_symbols++;
			} else if (begin_training(&next, point, 1) != 0) {
				return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
			}
		} else {
			training_result = train_symbol(&next, point);
			if (training_result < 0)
				return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
			if (training_result > 0 && reset_search(&next, point) != 0)
				return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
		}
		next.previous_point = *point;
	}
	if (!v32_conditioning_receiver_is_valid(&next))
		return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*ready = next.phase == V32_CONDITIONING_READY;
	return V32_CONDITIONING_RECEIVER_OK;
}

enum v32_conditioning_receiver_result
v32_conditioning_receiver_framing_state(
	const struct v32_conditioning_receiver *receiver,
	struct v32_symbol_decoder *decoder,
	struct qam_gain *gain)
{
	struct v32_symbol_decoder next_decoder;

	if (!v32_conditioning_receiver_is_complete(receiver) ||
	    decoder == NULL || gain == NULL ||
	    v32_symbol_decoder_init(
		    &next_decoder, receiver->source_role,
		    receiver->reference.trn_encoder.phase_quadrant) !=
		    V32_FRAMING_OK)
		return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	next_decoder.descrambler = receiver->reference.trn_encoder.scrambler;
	*decoder = next_decoder;
	*gain = receiver->gain;
	return V32_CONDITIONING_RECEIVER_OK;
}

const char *v32_conditioning_receiver_result_name(
	enum v32_conditioning_receiver_result result)
{
	switch (result) {
	case V32_CONDITIONING_RECEIVER_OK:
		return "ok";
	case V32_CONDITIONING_RECEIVER_COMPLETE:
		return "complete";
	case V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
