// SPDX-License-Identifier: BSD-3-Clause

#include "conditioning_receiver.h"

#include "constellation.h"
#include "decision.h"
#include "memory_region.h"
#include "rate_signal.h"

#include <limits.h>
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

static bool observation_is_valid(
	const struct v32_equalizer_observation *observation)
{
	return observation != NULL && observation->epoch != 0 &&
	       observation->phase <= INT16_MAX &&
	       observation->magnitude <= INT16_MAX;
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
	    receiver->previous_quadrant >= QPSK_QUADRANT_COUNT ||
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
	    receiver->has_previous_quadrant !=
		    (receiver->observation_epoch != 0) ||
	    (!receiver->has_previous_quadrant &&
	     (receiver->previous_quadrant != 0 ||
	      receiver->observation_lineage != 0)))
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
	    !receiver->has_previous_quadrant ||
	    (receiver->stable_trn_symbols != 0 &&
	     receiver->previous_quadrant !=
		     (uint8_t)((receiver->reference.trn_encoder.phase_quadrant +
				receiver->quadrant_offset) & 3u)))
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
	struct v32_conditioning_receiver *receiver)
{
	struct v32_conditioning_receiver reset;

	if (v32_conditioning_receiver_init(
		    &reset, receiver->source_role) !=
	    V32_CONDITIONING_RECEIVER_OK)
		return -1;
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
	uint8_t observed_quadrant, uint16_t matched_symbols,
	uint8_t *expected_quadrant)
{
	uint8_t expected;

	if (expected_quadrant == NULL || matched_symbols == 0 ||
	    matched_symbols >
		    V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS + 1u ||
	    v32_conditioning_transmitter_init(
		    &receiver->reference, receiver->source_role,
		    V32_CONDITIONING_TRN_MAX_SYMBOLS) !=
		    V32_CONDITIONING_OK ||
	    skip_symbols(
		    &receiver->reference,
		    (uint16_t)(V32_CONDITIONING_FIXED_SYMBOLS + matched_symbols),
		    &expected) != 0)
		return -1;
	receiver->trn_symbols = matched_symbols;
	receiver->quadrant_offset =
		(uint8_t)((observed_quadrant - expected) & 3u);
	receiver->pending_quadrant_offset = receiver->quadrant_offset;
	receiver->confirmed_trn_symbols = matched_symbols;
	receiver->stable_trn_symbols = matched_symbols;
	receiver->alternating_transitions = 0;
	receiver->s_bar_symbols = 0;
	receiver->prefix_match = 0;
	receiver->phase = V32_CONDITIONING_TRAIN_TRN;
	*expected_quadrant = expected;
	return 0;
}

struct training_symbol {
	uint8_t reference_quadrant;
	bool confirmed;
	bool reset;
};

static int train_symbol(
	struct v32_conditioning_receiver *receiver,
	uint8_t observed_quadrant, struct training_symbol *symbol)
{
	uint8_t expected_quadrant;
	uint8_t observed_expected;
	uint8_t candidate_offset;
	bool confirmed = false;

	if (symbol == NULL || v32_conditioning_transmitter_next(
		    &receiver->reference, &expected_quadrant) !=
		    V32_CONDITIONING_OK)
		return -1;
	observed_expected =
		(uint8_t)((expected_quadrant + receiver->quadrant_offset) & 3u);
	if (observed_quadrant == observed_expected) {
		receiver->consecutive_mismatches = 0;
		receiver->pending_quadrant_offset = receiver->quadrant_offset;
		receiver->confirmed_trn_symbols++;
		receiver->stable_trn_symbols++;
		confirmed = true;
	} else {
		receiver->stable_trn_symbols = 0;
		candidate_offset = (uint8_t)(
			(observed_quadrant - expected_quadrant) & 3u);
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
			confirmed = true;
		}
	}
	receiver->trn_symbols++;
	*symbol = (struct training_symbol){
		.reference_quadrant = expected_quadrant,
		.confirmed = confirmed,
	};
	if (training_quality_is_sufficient(receiver))
		receiver->phase = V32_CONDITIONING_READY;
	else if (receiver->trn_symbols == V32_CONDITIONING_TRN_MAX_SYMBOLS)
		symbol->reset = true;
	return 0;
}

enum v32_conditioning_receiver_result
v32_conditioning_receiver_observe(
	struct v32_conditioning_receiver *receiver,
	const struct v32_equalizer_observation *observation,
	struct v32_conditioning_symbol_decision *decision)
{
	struct v32_conditioning_receiver next;
	struct v32_conditioning_symbol_decision next_decision;
	struct v32_quadrant_decision quadrant_decision;
	struct training_symbol training_symbol = { 0 };
	uint8_t current_quadrant;
	uint8_t expected_quadrant = 0;
	uint8_t turn;
	bool known_reference = false;
	bool confirmed_reference = false;
	bool reset = false;

	if (receiver == NULL || observation == NULL || decision == NULL ||
	    memory_regions_overlap(
		    receiver, sizeof(*receiver), observation,
		    sizeof(*observation)) ||
	    memory_regions_overlap(
		    receiver, sizeof(*receiver), decision,
		    sizeof(*decision)) ||
	    memory_regions_overlap(
		    observation, sizeof(*observation), decision,
		    sizeof(*decision)))
		return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	if (!v32_conditioning_receiver_is_valid(receiver) ||
	    !observation_is_valid(observation) ||
	    (receiver->has_previous_quadrant &&
	     (receiver->observation_epoch == UINT64_MAX ||
	      observation->epoch != receiver->observation_epoch + 1u)))
		return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	if (v32_conditioning_receiver_is_complete(receiver))
		return V32_CONDITIONING_RECEIVER_COMPLETE;
	if (v32_decide_4_point(
		    observation, false, &quadrant_decision) != V32_DECISION_OK)
		return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	current_quadrant = quadrant_decision.quadrant;
	if (!next.has_previous_quadrant) {
		next.has_previous_quadrant = true;
	} else {
		turn = (uint8_t)((current_quadrant -
				  next.previous_quadrant) & 3u);
		if (next.phase == V32_CONDITIONING_SEARCH_S_BAR ||
		    next.phase == V32_CONDITIONING_MATCH_TRN) {
			bool prefix_ready = observe_prefix_turn(&next, turn);
			bool epoch = observe_s(&next, turn);

			if (prefix_ready &&
			    begin_training(
				    &next, current_quadrant,
				    V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS + 1u,
				    &expected_quadrant) != 0)
				return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
			known_reference = prefix_ready;
			confirmed_reference = prefix_ready;
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
			} else if (begin_training(
				   &next, current_quadrant, 1,
				   &expected_quadrant) != 0) {
				return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
			} else {
				known_reference = true;
				confirmed_reference = true;
			}
		} else {
			if (train_symbol(
				    &next, current_quadrant,
				    &training_symbol) != 0)
				return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
			expected_quadrant = training_symbol.reference_quadrant;
			known_reference = true;
			confirmed_reference = training_symbol.confirmed;
			reset = training_symbol.reset;
		}
	}
	if (known_reference) {
		if (v32_decide_known_quadrant(
			    observation, expected_quadrant, true,
			    &quadrant_decision) != V32_DECISION_OK)
			return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	}
	next_decision = (struct v32_conditioning_symbol_decision){
		.equalizer = quadrant_decision.equalizer,
		.reference_quadrant = expected_quadrant,
		.reference_known = known_reference,
		.reference_confirmed = confirmed_reference,
	};
	if (reset && reset_search(&next) != 0)
		return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	/* A maximum-length unqualified TRN restarts acquisition after accepting
	 * its final observation.  Retain the continuous observation coordinate
	 * across that protocol-state reset. */
	next.previous_quadrant = current_quadrant;
	next.observation_epoch = observation->epoch;
	next.observation_lineage = observation->lineage;
	next.has_previous_quadrant = true;
	if (!v32_conditioning_receiver_is_valid(&next))
		return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*decision = next_decision;
	return V32_CONDITIONING_RECEIVER_OK;
}

enum v32_conditioning_receiver_result
v32_conditioning_receiver_framing_state(
	const struct v32_conditioning_receiver *receiver,
	struct v32_symbol_decoder *decoder)
{
	struct v32_symbol_decoder next_decoder;

	if (!v32_conditioning_receiver_is_complete(receiver) ||
	    decoder == NULL || memory_regions_overlap(
		    receiver, sizeof(*receiver), decoder, sizeof(*decoder)))
		return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	if (v32_symbol_decoder_init(
		    &next_decoder, receiver->source_role, 0) !=
		    V32_FRAMING_OK)
		return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	next_decoder.descrambler = receiver->reference.trn_encoder.scrambler;
	*decoder = next_decoder;
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
