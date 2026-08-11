// SPDX-License-Identifier: BSD-3-Clause

#include "conditioning_receiver.h"

#include "constellation.h"

#include <math.h>
#include <stddef.h>

enum {
	V32_CONDITIONING_FIXED_SYMBOLS =
		SIPPYMODEM_V32_CONDITIONING_S_SYMBOLS +
		SIPPYMODEM_V32_CONDITIONING_S_BAR_SYMBOLS,
	V32_CONDITIONING_MISMATCH_RESTART = 3,
	V32_CONDITIONING_MAX_PHASE_CORRECTIONS = 8,
};

static const double V32_CONDITIONING_GAIN_ADAPTATION = 1.0 / 32.0;
static const double V32_CONDITIONING_MIN_POINT_POWER = 1e-12;

static bool role_is_valid(enum sippymodem_v32_transmitter_role role)
{
	return role == SIPPYMODEM_V32_CALLING_TRANSMITTER ||
	       role == SIPPYMODEM_V32_ANSWERING_TRANSMITTER;
}

static bool phase_is_valid(
	enum sippymodem_v32_conditioning_receiver_phase phase)
{
	return phase >= SIPPYMODEM_V32_CONDITIONING_SEARCH_S_BAR &&
	       phase <= SIPPYMODEM_V32_CONDITIONING_READY;
}

static bool point_has_power(const struct sippymodem_qam_point *point)
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
	const struct sippymodem_v32_conditioning_receiver *receiver)
{
	for (size_t index = 0;
	     index < SIPPYMODEM_V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS;
	     ++index) {
		if (receiver->prefix_turns[index] >=
			    SIPPYMODEM_QPSK_QUADRANT_COUNT ||
		    receiver->prefix_fallback[index] > index)
			return false;
	}
	return true;
}

bool sippymodem_v32_conditioning_receiver_is_valid(
	const struct sippymodem_v32_conditioning_receiver *receiver)
{
	bool training;

	if (receiver == NULL || !role_is_valid(receiver->source_role) ||
	    !phase_is_valid(receiver->phase) || !pattern_is_valid(receiver) ||
	    receiver->observed_quadrant >= SIPPYMODEM_QPSK_QUADRANT_COUNT ||
	    receiver->quadrant_offset >= SIPPYMODEM_QPSK_QUADRANT_COUNT ||
	    receiver->pending_quadrant_offset >=
		    SIPPYMODEM_QPSK_QUADRANT_COUNT ||
	    receiver->alternating_transitions >
		    SIPPYMODEM_V32_CONDITIONING_S_BAR_ACQUIRE_TRANSITIONS ||
	    receiver->previous_turn >= SIPPYMODEM_QPSK_QUADRANT_COUNT ||
	    receiver->prefix_match >=
		    SIPPYMODEM_V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS ||
	    receiver->consecutive_mismatches >=
		    V32_CONDITIONING_MISMATCH_RESTART ||
	    receiver->phase_corrections >
		    V32_CONDITIONING_MAX_PHASE_CORRECTIONS ||
	    (receiver->has_previous_point &&
	     !point_has_power(&receiver->previous_point)))
		return false;
	training = receiver->phase == SIPPYMODEM_V32_CONDITIONING_TRAIN_TRN ||
		   receiver->phase == SIPPYMODEM_V32_CONDITIONING_READY;
	if (!training)
		return receiver->trn_symbols == 0 &&
		       receiver->consecutive_mismatches == 0 &&
		       receiver->phase_corrections == 0;
	if (!sippymodem_v32_conditioning_transmitter_is_valid(
		    &receiver->reference) ||
	    !sippymodem_qam_gain_is_valid(&receiver->gain) ||
	    receiver->reference.trn_symbols !=
		    SIPPYMODEM_V32_CONDITIONING_TRN_MIN_SYMBOLS ||
	    receiver->reference.emitted_symbols !=
		    V32_CONDITIONING_FIXED_SYMBOLS + receiver->trn_symbols ||
	    receiver->trn_symbols <
		    SIPPYMODEM_V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS + 1u ||
	    receiver->trn_symbols >
		    SIPPYMODEM_V32_CONDITIONING_TRN_MIN_SYMBOLS ||
	    !receiver->has_previous_point)
		return false;
	return (receiver->phase == SIPPYMODEM_V32_CONDITIONING_READY) ==
	       (receiver->trn_symbols ==
		SIPPYMODEM_V32_CONDITIONING_TRN_MIN_SYMBOLS);
}

bool sippymodem_v32_conditioning_receiver_is_complete(
	const struct sippymodem_v32_conditioning_receiver *receiver)
{
	return sippymodem_v32_conditioning_receiver_is_valid(receiver) &&
	       receiver->phase == SIPPYMODEM_V32_CONDITIONING_READY;
}

static int skip_symbols(
	struct sippymodem_v32_conditioning_transmitter *transmitter,
	uint16_t count, uint8_t *last_quadrant)
{
	for (uint16_t symbol = 0; symbol < count; ++symbol)
		if (sippymodem_v32_conditioning_transmitter_next(
			    transmitter, last_quadrant) !=
		    SIPPYMODEM_V32_CONDITIONING_OK)
			return -1;
	return 0;
}

static int build_prefix_pattern(
	struct sippymodem_v32_conditioning_receiver *receiver)
{
	struct sippymodem_v32_conditioning_transmitter transmitter;
	uint8_t previous;

	if (sippymodem_v32_conditioning_transmitter_init(
		    &transmitter, receiver->source_role,
		    SIPPYMODEM_V32_CONDITIONING_TRN_MIN_SYMBOLS) !=
		    SIPPYMODEM_V32_CONDITIONING_OK ||
	    skip_symbols(&transmitter, V32_CONDITIONING_FIXED_SYMBOLS + 1u,
			 &previous) != 0)
		return -1;
	for (size_t index = 0;
	     index < SIPPYMODEM_V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS;
	     ++index) {
		uint8_t current;
		size_t fallback = 0;

		if (sippymodem_v32_conditioning_transmitter_next(
			    &transmitter, &current) !=
		    SIPPYMODEM_V32_CONDITIONING_OK)
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

enum sippymodem_v32_conditioning_receiver_result
sippymodem_v32_conditioning_receiver_init(
	struct sippymodem_v32_conditioning_receiver *receiver,
	enum sippymodem_v32_transmitter_role source_role)
{
	struct sippymodem_v32_conditioning_receiver initialized = {
		.source_role = source_role,
		.phase = SIPPYMODEM_V32_CONDITIONING_SEARCH_S_BAR,
	};

	if (receiver == NULL || !role_is_valid(source_role) ||
	    build_prefix_pattern(&initialized) != 0 ||
	    !sippymodem_v32_conditioning_receiver_is_valid(&initialized))
		return SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK;
}

static int reset_search(
	struct sippymodem_v32_conditioning_receiver *receiver,
	const struct sippymodem_qam_point *point)
{
	struct sippymodem_v32_conditioning_receiver reset;

	if (sippymodem_v32_conditioning_receiver_init(
		    &reset, receiver->source_role) !=
	    SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK)
		return -1;
	reset.previous_point = *point;
	reset.has_previous_point = true;
	*receiver = reset;
	return 0;
}

static void observe_s_bar(
	struct sippymodem_v32_conditioning_receiver *receiver, uint8_t turn)
{
	if ((turn & 1u) != 0) {
		if (receiver->alternating_transitions == 0 ||
		    turn != receiver->previous_turn) {
			if (receiver->alternating_transitions <
			    SIPPYMODEM_V32_CONDITIONING_S_BAR_ACQUIRE_TRANSITIONS)
				receiver->alternating_transitions++;
		} else {
			receiver->alternating_transitions = 1;
		}
	} else {
		receiver->alternating_transitions = 0;
	}
	receiver->previous_turn = turn;
	if (receiver->alternating_transitions ==
	    SIPPYMODEM_V32_CONDITIONING_S_BAR_ACQUIRE_TRANSITIONS)
		receiver->phase = SIPPYMODEM_V32_CONDITIONING_MATCH_TRN;
}

static bool observe_prefix_turn(
	struct sippymodem_v32_conditioning_receiver *receiver, uint8_t turn)
{
	while (receiver->prefix_match > 0 &&
	       turn != receiver->prefix_turns[receiver->prefix_match])
		receiver->prefix_match =
			receiver->prefix_fallback[receiver->prefix_match - 1u];
	if (turn == receiver->prefix_turns[receiver->prefix_match])
		receiver->prefix_match++;
	return receiver->prefix_match ==
	       SIPPYMODEM_V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS;
}

static int begin_training(
	struct sippymodem_v32_conditioning_receiver *receiver,
	const struct sippymodem_qam_point *point)
{
	struct sippymodem_qam_point expected_point;
	uint8_t expected_quadrant;
	uint16_t matched_symbols =
		SIPPYMODEM_V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS + 1u;

	if (sippymodem_v32_conditioning_transmitter_init(
		    &receiver->reference, receiver->source_role,
		    SIPPYMODEM_V32_CONDITIONING_TRN_MIN_SYMBOLS) !=
		    SIPPYMODEM_V32_CONDITIONING_OK ||
	    skip_symbols(
		    &receiver->reference,
		    (uint16_t)(V32_CONDITIONING_FIXED_SYMBOLS + matched_symbols),
		    &expected_quadrant) != 0 ||
	    sippymodem_v32_4800_constellation_point(
		    expected_quadrant, &expected_point) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK ||
	    sippymodem_qam_gain_init_from_reference(
		    &receiver->gain, point, &expected_point,
		    V32_CONDITIONING_GAIN_ADAPTATION) != SIPPYMODEM_QAM_GAIN_OK)
		return -1;
	receiver->trn_symbols = matched_symbols;
	receiver->quadrant_offset =
		(uint8_t)((receiver->observed_quadrant - expected_quadrant) & 3u);
	receiver->pending_quadrant_offset = receiver->quadrant_offset;
	receiver->prefix_match = 0;
	receiver->phase = SIPPYMODEM_V32_CONDITIONING_TRAIN_TRN;
	return 0;
}

static int train_symbol(
	struct sippymodem_v32_conditioning_receiver *receiver,
	const struct sippymodem_qam_point *point)
{
	struct sippymodem_qam_point expected_point;
	uint8_t expected_quadrant;
	uint8_t observed_expected;
	uint8_t candidate_offset;

	if (sippymodem_v32_conditioning_transmitter_next(
		    &receiver->reference, &expected_quadrant) !=
		    SIPPYMODEM_V32_CONDITIONING_OK ||
	    sippymodem_v32_4800_constellation_point(
		    expected_quadrant, &expected_point) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK)
		return -1;
	observed_expected =
		(uint8_t)((expected_quadrant + receiver->quadrant_offset) & 3u);
	if (receiver->observed_quadrant == observed_expected) {
		receiver->consecutive_mismatches = 0;
		receiver->pending_quadrant_offset = receiver->quadrant_offset;
		if (sippymodem_qam_gain_track(
			    &receiver->gain, point, &expected_point) !=
		    SIPPYMODEM_QAM_GAIN_OK)
			return -1;
	} else {
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
			if (receiver->phase_corrections ==
			    V32_CONDITIONING_MAX_PHASE_CORRECTIONS)
				return 1;
			receiver->quadrant_offset =
				receiver->pending_quadrant_offset;
			receiver->consecutive_mismatches = 0;
			receiver->phase_corrections++;
			if (sippymodem_qam_gain_track(
				    &receiver->gain, point, &expected_point) !=
			    SIPPYMODEM_QAM_GAIN_OK)
				return -1;
		}
	}
	receiver->trn_symbols++;
	if (receiver->trn_symbols ==
	    SIPPYMODEM_V32_CONDITIONING_TRN_MIN_SYMBOLS)
		receiver->phase = SIPPYMODEM_V32_CONDITIONING_READY;
	return 0;
}

enum sippymodem_v32_conditioning_receiver_result
sippymodem_v32_conditioning_receiver_observe(
	struct sippymodem_v32_conditioning_receiver *receiver,
	const struct sippymodem_qam_point *point, bool *ready)
{
	struct sippymodem_v32_conditioning_receiver next;
	uint8_t turn;
	int training_result = 0;

	if (!sippymodem_v32_conditioning_receiver_is_valid(receiver) ||
	    !point_has_power(point) || ready == NULL)
		return SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	if (sippymodem_v32_conditioning_receiver_is_complete(receiver))
		return SIPPYMODEM_V32_CONDITIONING_RECEIVER_COMPLETE;
	next = *receiver;
	if (!next.has_previous_point) {
		next.previous_point = *point;
		next.has_previous_point = true;
	} else {
		if (sippymodem_qam_nearest_quarter_turn(
			    &next.previous_point, point, &turn) !=
		    SIPPYMODEM_QAM_OK)
			return SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
		next.observed_quadrant =
			(uint8_t)((next.observed_quadrant + turn) & 3u);
		if (next.phase == SIPPYMODEM_V32_CONDITIONING_SEARCH_S_BAR) {
			bool prefix_ready = observe_prefix_turn(&next, turn);

			observe_s_bar(&next, turn);
			if (prefix_ready && begin_training(&next, point) != 0)
				return SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
		} else if (next.phase == SIPPYMODEM_V32_CONDITIONING_MATCH_TRN) {
			if (observe_prefix_turn(&next, turn) &&
			    begin_training(&next, point) != 0)
				return SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
		} else {
			training_result = train_symbol(&next, point);
			if (training_result < 0)
				return SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
			if (training_result > 0 && reset_search(&next, point) != 0)
				return SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
		}
		next.previous_point = *point;
	}
	if (!sippymodem_v32_conditioning_receiver_is_valid(&next))
		return SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*ready = next.phase == SIPPYMODEM_V32_CONDITIONING_READY;
	return SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK;
}

enum sippymodem_v32_conditioning_receiver_result
sippymodem_v32_conditioning_receiver_framing_state(
	const struct sippymodem_v32_conditioning_receiver *receiver,
	struct sippymodem_v32_symbol_decoder *decoder,
	struct sippymodem_qam_gain *gain)
{
	struct sippymodem_v32_symbol_decoder next_decoder;

	if (!sippymodem_v32_conditioning_receiver_is_complete(receiver) ||
	    decoder == NULL || gain == NULL ||
	    sippymodem_v32_symbol_decoder_init(
		    &next_decoder, receiver->source_role,
		    receiver->reference.trn_encoder.phase_quadrant) !=
		    SIPPYMODEM_V32_FRAMING_OK)
		return SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	next_decoder.descrambler = receiver->reference.trn_encoder.scrambler;
	*decoder = next_decoder;
	*gain = receiver->gain;
	return SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK;
}

const char *sippymodem_v32_conditioning_receiver_result_name(
	enum sippymodem_v32_conditioning_receiver_result result)
{
	switch (result) {
	case SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK:
		return "ok";
	case SIPPYMODEM_V32_CONDITIONING_RECEIVER_COMPLETE:
		return "complete";
	case SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
