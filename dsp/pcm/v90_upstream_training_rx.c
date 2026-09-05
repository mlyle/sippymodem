// SPDX-License-Identifier: BSD-3-Clause

#include "v90_upstream_training_rx.h"

#include "v34_constellation.h"

#include <stddef.h>
#include <string.h>

enum {
	V90_UPSTREAM_INITIAL_DECODER_POWER_Q15 = 0x7000,
	V90_UPSTREAM_MIDDLE_DECODER_POWER_Q15 = 0x4000,
	V90_UPSTREAM_FINAL_DECODER_POWER_Q15 = 0x2000,
	/* Silence between the analogue modem's V.90 return intervals must not
	 * pull the trained equalizer toward an arbitrary constellation point. */
	V90_UPSTREAM_MIN_TRACKING_POWER =
		(V34_TRAINING_FIXED_AMPLITUDE / 2) *
		(V34_TRAINING_FIXED_AMPLITUDE / 2),
};

static uint16_t decoder_power(
	const struct v90_upstream_training_rx *receiver)
{
	uint16_t period = receiver->line.timing.timing.nominal_symbol_rate;

	if (receiver->tracking_symbols >= period)
		return V90_UPSTREAM_FINAL_DECODER_POWER_Q15;
	if (receiver->tracking_symbols >= (uint16_t)(period >> 1u))
		return V90_UPSTREAM_MIDDLE_DECODER_POWER_Q15;
	return V90_UPSTREAM_INITIAL_DECODER_POWER_Q15;
}

static bool semantic_state_is_valid(
	const struct v90_upstream_training_rx *receiver)
{
	if (!v34_phase3_decision_receiver_is_valid(&receiver->initial) ||
	    receiver->initial.transmitting_role !=
		    receiver->transmitting_profile.local_role ||
	    receiver->initial.md_symbols != 0u)
		return false;
	if (receiver->stage == V90_UPSTREAM_TRAINING_RX_INITIAL)
		return receiver->initial.stage ==
			       V34_PHASE3_DECISION_RECEIVER_S ||
		       receiver->initial.stage ==
			       V34_PHASE3_DECISION_RECEIVER_PP;
	return receiver->stage == V90_UPSTREAM_TRAINING_RX_TRACKING &&
	       receiver->initial.stage == V34_PHASE3_DECISION_RECEIVER_J &&
	       receiver->initial.j.received_symbols <=
		       V34_TRAINING_SEQUENCE_MIN_SYMBOLS;
}

bool v90_upstream_training_rx_is_valid(
	const struct v90_upstream_training_rx *receiver)
{
	if (receiver == NULL ||
	    !v34_phase3_profile_is_valid(&receiver->transmitting_profile) ||
	    !v34_line_receiver_matches_profile(
		    &receiver->line, &receiver->transmitting_profile) ||
	    receiver->line.equalizer.feedback_mode !=
		    V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE ||
	    !semantic_state_is_valid(receiver) ||
	    receiver->decoder_power_q15 != decoder_power(receiver))
		return false;
	if (receiver->initial.stage == V34_PHASE3_DECISION_RECEIVER_S)
		return receiver->tracking_symbols == 0u;
	return true;
}

bool v90_upstream_training_rx_has_failed(
	const struct v90_upstream_training_rx *receiver)
{
	return v90_upstream_training_rx_is_valid(receiver) && receiver->failed;
}

bool v90_upstream_training_rx_is_tracking(
	const struct v90_upstream_training_rx *receiver)
{
	return v90_upstream_training_rx_is_valid(receiver) && !receiver->failed &&
	       receiver->stage == V90_UPSTREAM_TRAINING_RX_TRACKING;
}

enum v90_upstream_training_rx_result v90_upstream_training_rx_init(
	struct v90_upstream_training_rx *receiver,
	const struct v34_phase3_profile *transmitting_profile,
	int16_t initial_receive_gain)
{
	struct v90_upstream_training_rx initialized;

	if (receiver == NULL ||
	    !v34_phase3_profile_is_valid(transmitting_profile) ||
	    initial_receive_gain <= 0)
		return V90_UPSTREAM_TRAINING_RX_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	initialized.transmitting_profile = *transmitting_profile;
	initialized.decoder_power_q15 =
		V90_UPSTREAM_INITIAL_DECODER_POWER_Q15;
	initialized.stage = V90_UPSTREAM_TRAINING_RX_INITIAL;
	if (v34_line_receiver_init(
		    &initialized.line, transmitting_profile,
		    initial_receive_gain, false,
		    V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE) !=
		    V34_LINE_RECEIVER_OK ||
	    v34_phase3_decision_receiver_init_suffix(
		    &initialized.initial,
		    transmitting_profile->local_role, 0u,
		    V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS) !=
		    V34_PHASE3_DECISION_RECEIVER_OK ||
	    !v90_upstream_training_rx_is_valid(&initialized))
		return V90_UPSTREAM_TRAINING_RX_INVALID_ARGUMENT;
	*receiver = initialized;
	return V90_UPSTREAM_TRAINING_RX_OK;
}

static void advance_tracking(struct v90_upstream_training_rx *receiver)
{
	uint16_t period = receiver->line.timing.timing.nominal_symbol_rate;

	if (receiver->tracking_symbols < period)
		receiver->tracking_symbols++;
	receiver->decoder_power_q15 = decoder_power(receiver);
}

static enum v90_upstream_training_rx_result resolve_action(
	struct v90_upstream_training_rx *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	const struct v34_receive_action *action)
{
	bool was_tracking = receiver->initial.stage >=
		V34_PHASE3_DECISION_RECEIVER_PP;

	if (v34_line_receiver_resolve(
		    &receiver->line, observation, action,
		    receiver->decoder_power_q15) != V34_LINE_RECEIVER_OK)
		return V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE;
	if (was_tracking && action->action == V34_RECEIVE_COMMIT)
		advance_tracking(receiver);
	return V90_UPSTREAM_TRAINING_RX_OK;
}

static enum v90_upstream_training_rx_result receive_initial(
	struct v90_upstream_training_rx *receiver,
	const struct v34_adaptive_equalizer_observation *observation)
{
	struct v34_receive_action action;
	enum v34_phase3_decision_receiver_result result;
	enum v90_upstream_training_rx_result resolved;

	result = v34_phase3_decision_receiver_observe(
		&receiver->initial, observation,
		receiver->decoder_power_q15, &action);
	if (result != V34_PHASE3_DECISION_RECEIVER_OK &&
	    result != V34_PHASE3_DECISION_RECEIVER_S_DETECTED &&
	    result != V34_PHASE3_DECISION_RECEIVER_PP_COMPLETE)
		return V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE;
	resolved = resolve_action(receiver, observation, &action);
	if (resolved != V90_UPSTREAM_TRAINING_RX_OK)
		return resolved;
	if (result != V34_PHASE3_DECISION_RECEIVER_PP_COMPLETE)
		return V90_UPSTREAM_TRAINING_RX_OK;
	receiver->stage = V90_UPSTREAM_TRAINING_RX_TRACKING;
	return V90_UPSTREAM_TRAINING_RX_OK;
}

static uint32_t observation_power(
	const struct v34_adaptive_equalizer_observation *observation)
{
	int32_t in_phase = observation->decision.in_phase;
	int32_t quadrature = observation->decision.quadrature;

	return (uint32_t)(in_phase * in_phase) +
	       (uint32_t)(quadrature * quadrature);
}

static enum v90_upstream_training_rx_result receive_tracking(
	struct v90_upstream_training_rx *receiver,
	const struct v34_adaptive_equalizer_observation *observation)
{
	struct v34_receive_action action;

	/* The first 512 symbols after PP are the known V.34 TRN sequence. Let
	 * the ordinary semantic receiver advance that exact GPA history, then
	 * retain decision-directed tracking through Ja and the return epochs. */
	if (receiver->initial.j.received_symbols <
	    V34_TRAINING_SEQUENCE_MIN_SYMBOLS) {
		enum v34_phase3_decision_receiver_result result =
			v34_phase3_decision_receiver_observe(
				&receiver->initial, observation,
				receiver->decoder_power_q15, &action);

		if (result != V34_PHASE3_DECISION_RECEIVER_OK)
			return V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE;
		return resolve_action(receiver, observation, &action);
	}
	if (observation_power(observation) <
	    V90_UPSTREAM_MIN_TRACKING_POWER) {
		action = v34_receive_action_only(V34_RECEIVE_DISCARD);
		return resolve_action(receiver, observation, &action);
	}
	{
		struct v34_fixed_training_decision hard;

		if (v34_training_constellation_fixed_slice(
			    V34_CONSTELLATION_4_POINT, observation->decision,
			    &hard) != V34_CONSTELLATION_OK)
			return V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE;
		action = v34_receive_action_commit(
			observation, hard.target, receiver->decoder_power_q15,
			V34_ADAPTIVE_EQUALIZER_FULL);
	}
	return resolve_action(receiver, observation, &action);
}

static enum v90_upstream_training_rx_result receive_observation(
	struct v90_upstream_training_rx *receiver,
	const struct v34_adaptive_equalizer_observation *observation)
{
	return receiver->stage == V90_UPSTREAM_TRAINING_RX_INITIAL ?
		receive_initial(receiver, observation) :
		receive_tracking(receiver, observation);
}

enum v90_upstream_training_rx_result v90_upstream_training_rx_sample(
	struct v90_upstream_training_rx *receiver, int16_t sample)
{
	struct v90_upstream_training_rx next;
	struct v34_adaptive_equalizer_observation observation;
	enum v34_line_receiver_result line_result;
	enum v90_upstream_training_rx_result result;

	if (!v90_upstream_training_rx_is_valid(receiver))
		return V90_UPSTREAM_TRAINING_RX_INVALID_ARGUMENT;
	if (receiver->failed)
		return V90_UPSTREAM_TRAINING_RX_FAILED;
	next = *receiver;
	line_result = v34_line_receiver_sample(
		&next.line, sample, &observation);
	if (line_result == V34_LINE_RECEIVER_OK) {
		*receiver = next;
		return V90_UPSTREAM_TRAINING_RX_OK;
	}
	if (line_result == V34_LINE_RECEIVER_TIMING_ACQUIRED) {
		*receiver = next;
		return V90_UPSTREAM_TRAINING_RX_OK;
	}
	if (line_result != V34_LINE_RECEIVER_OBSERVATION_READY) {
		receiver->failed = true;
		return V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE;
	}
	result = receive_observation(&next, &observation);
	if (result == V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE ||
	    !v90_upstream_training_rx_is_valid(&next)) {
		next.failed = true;
		if (v90_upstream_training_rx_is_valid(&next))
			*receiver = next;
		else
			receiver->failed = true;
		return V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE;
	}
	*receiver = next;
	return result;
}
