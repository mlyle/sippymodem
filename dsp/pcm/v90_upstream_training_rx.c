// SPDX-License-Identifier: BSD-3-Clause

#include "v90_upstream_training_rx.h"

#include "memory_region.h"
#include "v34_constellation.h"

#include <stddef.h>
#include <string.h>

enum {
	/* Preserve 48 symbols of past-channel span for delayed upstream echoes,
	 * while ordinary V.34 retains its 80-tap/20-symbol geometry. */
	V90_UPSTREAM_EQUALIZER_TAPS = 128,
	/* Silence between the analogue modem's V.90 return intervals must not
	 * pull the trained equalizer toward an arbitrary constellation point. */
	V90_UPSTREAM_MIN_TRACKING_POWER =
		(V34_TRAINING_FIXED_AMPLITUDE / 2) *
		(V34_TRAINING_FIXED_AMPLITUDE / 2),
};

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

static bool history_is_valid(
	const struct v90_upstream_training_rx *receiver)
{
	return receiver->next_recent_decision <
		       V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS &&
	       receiver->recent_decision_count <=
		       V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS &&
	       (receiver->recent_decision_count ==
			V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS ||
		receiver->next_recent_decision ==
			receiver->recent_decision_count);
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
	    !history_is_valid(receiver))
		return false;
	if (receiver->initial.stage == V34_PHASE3_DECISION_RECEIVER_S)
		return receiver->tracking_symbols == 0u;
	return true;
}

bool v90_upstream_training_rx_recent_decision(
	const struct v90_upstream_training_rx *receiver, size_t index,
	struct v34_analytic_sample *decision)
{
	size_t oldest;

	if (!v90_upstream_training_rx_is_valid(receiver) || decision == NULL ||
	    index >= receiver->recent_decision_count)
		return false;
	oldest = (receiver->next_recent_decision +
		  V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS -
		  receiver->recent_decision_count) %
		 V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS;
	*decision = receiver->recent_decisions[
		(oldest + index) % V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS];
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
	    v34_equalizer_init_geometry(
		    &initialized.line.equalizer.equalizer,
		    V90_UPSTREAM_EQUALIZER_TAPS,
		    V90_UPSTREAM_TRAINING_RX_DECISION_DELAY) != V34_EQUALIZER_OK ||
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
}

static void remember_decision(
	struct v90_upstream_training_rx *receiver,
	struct v34_analytic_sample decision)
{
	receiver->recent_decisions[receiver->next_recent_decision] = decision;
	receiver->next_recent_decision++;
	if (receiver->next_recent_decision ==
	    V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS)
		receiver->next_recent_decision = 0;
	if (receiver->recent_decision_count <
	    V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS)
		receiver->recent_decision_count++;
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
		    v90_upstream_training_rx_decoder_power(receiver)) !=
	    V34_LINE_RECEIVER_OK)
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
		v90_upstream_training_rx_decoder_power(receiver), &action);
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
				v90_upstream_training_rx_decoder_power(receiver),
				&action);

		if (result != V34_PHASE3_DECISION_RECEIVER_OK)
			return V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE;
		return resolve_action(receiver, observation, &action);
	}
	if (observation_power(observation) <
	    V90_UPSTREAM_MIN_TRACKING_POWER) {
		if (v34_receive_timing_set_feedback_held(
			    &receiver->line.timing, true) != V34_RECEIVE_TIMING_OK)
			return V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE;
		action = v34_receive_action_only(V34_RECEIVE_DISCARD);
		return resolve_action(receiver, observation, &action);
	}
	if (v34_receive_timing_set_feedback_held(
		    &receiver->line.timing, false) != V34_RECEIVE_TIMING_OK)
		return V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE;
	{
		struct v34_fixed_training_decision hard;

		if (v34_training_constellation_fixed_slice(
			    V34_CONSTELLATION_4_POINT, observation->decision,
			    &hard) != V34_CONSTELLATION_OK)
			return V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE;
		action = v34_receive_action_commit(
			observation, hard.target,
			v90_upstream_training_rx_decoder_power(receiver),
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
	struct v90_upstream_training_rx *receiver, int16_t sample,
	struct v34_analytic_sample *decision)
{
	struct v90_upstream_training_rx next;
	struct v34_adaptive_equalizer_observation observation = { 0 };
	enum v34_line_receiver_result line_result;
	enum v90_upstream_training_rx_result result;
	bool was_tracking;

	if (!v90_upstream_training_rx_is_valid(receiver) || decision == NULL ||
	    memory_regions_overlap(receiver, sizeof(*receiver),
				   decision, sizeof(*decision)))
		return V90_UPSTREAM_TRAINING_RX_INVALID_ARGUMENT;
	if (receiver->failed)
		return V90_UPSTREAM_TRAINING_RX_FAILED;
	next = *receiver;
	was_tracking = next.stage == V90_UPSTREAM_TRAINING_RX_TRACKING;
	line_result = v34_line_receiver_sample(
		&next.line, sample, &observation);
	/* The lower S-carrier edge has a qualified raw analytic window. Bring
	 * an insufficient Phase 2 gain seed into that measured receive domain
	 * before the short qualified S suffix, without training on pre-S echo. */
	if (!receiver->line.timing.s_detector.acquired &&
	    next.line.timing.s_detector.acquired &&
	    v34_gain_control_refresh_level(&next.line.timing.gain) !=
		    V34_GAIN_CONTROL_OK) {
		receiver->failed = true;
		return V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE;
	}
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
	if (was_tracking)
		remember_decision(&next, observation.decision);
	if (!v90_upstream_training_rx_is_valid(&next)) {
		receiver->failed = true;
		return V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE;
	}
	*receiver = next;
	if (was_tracking) {
		*decision = observation.decision;
		return V90_UPSTREAM_TRAINING_RX_DECISION_READY;
	}
	return result;
}

enum v90_upstream_training_rx_result v90_upstream_training_rx_advance(
	struct v90_upstream_training_rx *receiver, int16_t sample)
{
	struct v90_upstream_training_rx next;
	struct v34_adaptive_equalizer_observation observation = { 0 };
	struct v34_receive_action discard =
		v34_receive_action_only(V34_RECEIVE_DISCARD);
	enum v34_line_receiver_result line_result;

	if (!v90_upstream_training_rx_is_valid(receiver))
		return V90_UPSTREAM_TRAINING_RX_INVALID_ARGUMENT;
	if (receiver->failed)
		return V90_UPSTREAM_TRAINING_RX_FAILED;
	if (receiver->stage != V90_UPSTREAM_TRAINING_RX_TRACKING)
		return V90_UPSTREAM_TRAINING_RX_INVALID_ARGUMENT;
	next = *receiver;
	line_result = v34_line_receiver_sample(
		&next.line, sample, &observation);
	if (line_result == V34_LINE_RECEIVER_OBSERVATION_READY)
		line_result = v34_line_receiver_resolve(
			&next.line, &observation, &discard,
			v90_upstream_training_rx_decoder_power(&next));
	if (line_result != V34_LINE_RECEIVER_OK) {
		next.failed = true;
		if (v90_upstream_training_rx_is_valid(&next))
			*receiver = next;
		else
			receiver->failed = true;
		return V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE;
	}
	if (v34_adaptive_equalizer_observation_is_valid(&observation))
		remember_decision(&next, observation.decision);
	if (!v90_upstream_training_rx_is_valid(&next)) {
		receiver->failed = true;
		return V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE;
	}
	*receiver = next;
	return V90_UPSTREAM_TRAINING_RX_OK;
}
