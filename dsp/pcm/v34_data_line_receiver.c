// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_line_receiver.h"

#include "v34_constellation.h"
#include "v34_survivor.h"

#include <stddef.h>

_Static_assert(
	V34_PHASE3_S_HUNT_CANDIDATE_PREFIX_SYMBOLS *
		(V34_PHASE3_S_RECEIVER_MAX_S_ERRORS + 1u) <=
		(V34_SURVIVOR_TRACEBACK_INTERVALS - 1u) *
			V34_2D_SYMBOLS_PER_4D_INTERVAL,
	"error-bounded online S must be gated before traceback can publish it");

static bool monitor_is_valid(
	const struct v34_data_rate_renegotiation_monitor *monitor)
{
	if (!monitor->enabled)
		return true;
	return !monitor->s.acquire_phase &&
	       v34_phase3_s_hunt_is_valid(&monitor->s);
}

static const struct v34_frame_geometry *data_geometry(
	const struct v34_data_line_receiver *receiver)
{
	return &receiver->decisions.data_receiver.data_receiver.schedule.geometry;
}

static bool first_symbol_state_is_valid(
	const struct v34_data_line_receiver *receiver)
{
	const struct v34_phase3_s_adaptation *adaptation =
		&receiver->first_symbol_adaptation;
	bool has_first = receiver->decisions.has_first_symbol;

	if (has_first != receiver->line.equalizer.retained.pending)
		return false;
	if (!has_first)
		return !receiver->first_symbol_freezes_adaptation &&
		       !receiver->first_symbol_is_control && !adaptation->adapt &&
		       adaptation->target.in_phase == 0 &&
		       adaptation->target.quadrature == 0;
	if (adaptation->adapt && !receiver->first_symbol_is_control)
		return false;
	if (!adaptation->adapt &&
	    (adaptation->target.in_phase != 0 ||
	     adaptation->target.quadrature != 0))
		return false;
	return !receiver->first_symbol_is_control ||
	       receiver->first_symbol_freezes_adaptation;
}

bool v34_data_line_receiver_is_valid(
	const struct v34_data_line_receiver *receiver)
{
	return receiver != NULL &&
	       v34_phase3_profile_is_valid(&receiver->transmitting_profile) &&
	       v34_frame_geometry_is_valid(&receiver->geometry) &&
	       receiver->transmitting_profile.symbol_rate ==
		       receiver->geometry.symbol_rate &&
	       v34_line_receiver_matches_profile(
		       &receiver->line, &receiver->transmitting_profile) &&
	       v34_receive_timing_is_acquired(&receiver->line.timing) &&
	       receiver->line.equalizer.feedback_mode ==
		       V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE &&
	       !receiver->line.equalizer.decision_pending &&
	       v34_data_decision_receiver_is_valid(&receiver->decisions) &&
	       receiver->decisions.data_receiver.data_receiver.transmitting_role ==
		       receiver->transmitting_profile.local_role &&
	       v34_frame_geometry_is_equal(
		       &receiver->geometry, data_geometry(receiver)) &&
	       monitor_is_valid(&receiver->rate_renegotiation) &&
	       first_symbol_state_is_valid(receiver);
}

static bool initialize_monitor(
	struct v34_data_rate_renegotiation_monitor *monitor)
{
	struct v34_data_rate_renegotiation_monitor initialized = { 0 };

	if (v34_phase3_s_hunt_init(&initialized.s) !=
	    V34_PHASE3_S_RECEIVER_OK)
		return false;
	initialized.enabled = true;
	if (!monitor_is_valid(&initialized))
		return false;
	*monitor = initialized;
	return true;
}

enum v34_data_receive_result v34_data_line_receiver_init(
	struct v34_data_line_receiver *receiver,
	const struct v34_line_receiver *line,
	const struct v34_data_line_receiver_configuration *configuration)
{
	struct v34_data_line_receiver initialized = { 0 };

	if (receiver == NULL || line == NULL || configuration == NULL ||
	    !v34_phase3_profile_is_valid(
		    &configuration->transmitting_profile) ||
	    !v34_frame_geometry_is_valid(&configuration->geometry) ||
	    configuration->transmitting_profile.symbol_rate !=
		    configuration->geometry.symbol_rate ||
	    !v34_line_receiver_matches_profile(
		    line, &configuration->transmitting_profile) ||
	    !v34_receive_timing_is_acquired(&line->timing) ||
	    line->equalizer.feedback_mode !=
		    V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE ||
	    line->equalizer.decision_pending)
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	initialized.line = *line;
	initialized.transmitting_profile =
		configuration->transmitting_profile;
	initialized.geometry = configuration->geometry;
	if (v34_data_decision_receiver_init(
		    &initialized.decisions, &configuration->normalizer,
		    &configuration->geometry, configuration->expanded_shaping,
		    configuration->precoder_coefficients,
		    configuration->trellis_selection,
		    configuration->transmitting_profile.local_role) !=
		    V34_DATA_RECEIVE_OK ||
	    (configuration->detect_rate_renegotiation &&
	     !initialize_monitor(&initialized.rate_renegotiation)) ||
	    !v34_data_line_receiver_is_valid(&initialized))
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_DATA_RECEIVE_OK;
}

bool v34_data_line_receiver_has_failed(
	const struct v34_data_line_receiver *receiver)
{
	return v34_data_line_receiver_is_valid(receiver) && receiver->failed;
}

bool v34_data_line_receiver_is_receiving_b1(
	const struct v34_data_line_receiver *receiver)
{
	return v34_data_line_receiver_is_valid(receiver) && !receiver->failed &&
	       v34_data_decision_receiver_is_receiving_b1(&receiver->decisions);
}

bool v34_data_line_receiver_detects_rate_renegotiation(
	const struct v34_data_line_receiver *receiver)
{
	return v34_data_line_receiver_is_valid(receiver) &&
	       receiver->rate_renegotiation.enabled;
}

bool v34_data_line_receiver_has_rate_renegotiation_candidate(
	const struct v34_data_line_receiver *receiver)
{
	return v34_data_line_receiver_detects_rate_renegotiation(receiver) &&
	       receiver->rate_renegotiation.s.candidate;
}

bool v34_data_line_receiver_rate_renegotiation_is_complete(
	const struct v34_data_line_receiver *receiver)
{
	return v34_data_line_receiver_detects_rate_renegotiation(receiver) &&
	       v34_phase3_s_hunt_is_complete(
		       &receiver->rate_renegotiation.s);
}

bool v34_data_line_receiver_get_rate_renegotiation_detection(
	const struct v34_data_line_receiver *receiver,
	struct v34_phase3_s_detection *detection)
{
	return v34_data_line_receiver_rate_renegotiation_is_complete(receiver) &&
	       v34_phase3_s_hunt_get_detection(
		       &receiver->rate_renegotiation.s, detection);
}

static enum v34_data_receive_result observe_rate_renegotiation(
	struct v34_data_rate_renegotiation_monitor *monitor,
	const struct v34_adaptive_equalizer_observation *observation,
	struct v34_phase3_s_adaptation *adaptation, bool *control)
{
	enum v34_phase3_s_receiver_result result;

	*adaptation = (struct v34_phase3_s_adaptation){ 0 };
	*control = false;
	if (!monitor->enabled)
		return V34_DATA_RECEIVE_OK;
	if (v34_phase3_s_hunt_is_complete(&monitor->s))
		return V34_DATA_RECEIVE_RATE_RENEGOTIATION;
	result = v34_phase3_s_hunt_receive(
		&monitor->s, observation->decision, adaptation, control);
	if (result == V34_PHASE3_S_RECEIVER_DETECTED)
		return V34_DATA_RECEIVE_RATE_RENEGOTIATION;
	if (result != V34_PHASE3_S_RECEIVER_OK)
		return V34_DATA_RECEIVE_NUMERIC_FAILURE;
	return V34_DATA_RECEIVE_OK;
}

static bool result_is_terminal(enum v34_data_receive_result result)
{
	return result == V34_DATA_RECEIVE_FAILED ||
	       result == V34_DATA_RECEIVE_NUMERIC_FAILURE ||
	       result == V34_DATA_RECEIVE_OUT_OF_RANGE;
}

static enum v34_data_receive_result fail_observation(
	struct v34_data_line_receiver *receiver,
	struct v34_data_line_receiver *next,
	const struct v34_adaptive_equalizer_observation *observation,
	enum v34_data_receive_result result)
{
	if (next->line.equalizer.decision_pending) {
		if (next->line.equalizer.retained.pending) {
			const struct v34_adaptive_equalizer_decision skipped[2] = {
				{
					.observation =
						next->decisions.first_observation,
					.adaptation =
						V34_ADAPTIVE_EQUALIZER_SKIP,
				},
				{
					.observation = *observation,
					.adaptation =
						V34_ADAPTIVE_EQUALIZER_SKIP,
				},
			};

			if (v34_adaptive_equalizer_commit_pair(
				    &next->line.equalizer, skipped) !=
				    V34_ADAPTIVE_EQUALIZER_OK ||
			    v34_data_decision_receiver_cancel_interval(
				    &next->decisions) != V34_DATA_RECEIVE_OK)
				result = V34_DATA_RECEIVE_NUMERIC_FAILURE;
			else {
				next->first_symbol_adaptation =
					(struct v34_phase3_s_adaptation){ 0 };
				next->first_symbol_freezes_adaptation = false;
				next->first_symbol_is_control = false;
			}
		} else if (v34_adaptive_equalizer_discard(
				   &next->line.equalizer, observation) !=
			   V34_ADAPTIVE_EQUALIZER_OK) {
			result = V34_DATA_RECEIVE_NUMERIC_FAILURE;
		}
	}
	next->failed = true;
	if (v34_data_line_receiver_is_valid(next))
		*receiver = *next;
	else
		receiver->failed = true;
	return result;
}

static struct v34_adaptive_equalizer_decision skip_decision(
	const struct v34_adaptive_equalizer_observation *observation)
{
	return (struct v34_adaptive_equalizer_decision){
		.observation = *observation,
		.adaptation = V34_ADAPTIVE_EQUALIZER_SKIP,
	};
}

static void apply_symbol_policy(
	struct v34_adaptive_equalizer_decision *decision,
	const struct v34_phase3_s_adaptation *adaptation, bool control,
	bool freeze)
{
	if (control && adaptation->adapt) {
		*decision = (struct v34_adaptive_equalizer_decision){
			.observation = decision->observation,
			.target = adaptation->target,
			.decoder_power_q15 =
				V34_DATA_DECISION_DECODER_POWER_Q15,
			.adaptation = V34_ADAPTIVE_EQUALIZER_CENTER_TAPS,
		};
	} else if (freeze || control) {
		*decision = skip_decision(&decision->observation);
	}
}

static void clear_first_symbol_policy(
	struct v34_data_line_receiver *receiver)
{
	receiver->first_symbol_adaptation =
		(struct v34_phase3_s_adaptation){ 0 };
	receiver->first_symbol_freezes_adaptation = false;
	receiver->first_symbol_is_control = false;
}

static bool complete_rate_renegotiation_boundary(
	struct v34_data_line_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation)
{
	if (receiver->line.equalizer.retained.pending) {
		struct v34_adaptive_equalizer_decision decisions[2] = {
			{
				.observation =
					receiver->decisions.first_observation,
				.adaptation = V34_ADAPTIVE_EQUALIZER_SKIP,
			},
			{
				.observation = *observation,
				.adaptation = V34_ADAPTIVE_EQUALIZER_SKIP,
			},
		};

		apply_symbol_policy(
			&decisions[0], &receiver->first_symbol_adaptation,
			receiver->first_symbol_is_control,
			receiver->first_symbol_freezes_adaptation);
		if (v34_adaptive_equalizer_commit_pair(
			    &receiver->line.equalizer, decisions) !=
			    V34_ADAPTIVE_EQUALIZER_OK ||
		    v34_data_decision_receiver_cancel_interval(
			    &receiver->decisions) != V34_DATA_RECEIVE_OK)
			return false;
		clear_first_symbol_policy(receiver);
		return true;
	}
	return v34_adaptive_equalizer_discard(
		       &receiver->line.equalizer, observation) ==
	       V34_ADAPTIVE_EQUALIZER_OK;
}

enum v34_data_receive_result v34_data_line_receiver_sample(
	struct v34_data_line_receiver *receiver, int16_t sample,
	struct v34_survivor_decision *current,
	struct v34_received_mapping_frame *received)
{
	struct v34_data_line_receiver next;
	struct v34_adaptive_equalizer_observation observation;
	struct v34_data_decision_output decoded;
	struct v34_phase3_s_adaptation rrn_adaptation = { 0 };
	enum v34_line_receiver_result line_result;
	enum v34_data_receive_result result;
	bool rrn_control = false;
	bool freeze;
	bool candidate_before;
	bool candidate_after;
	bool had_first;
	bool marker_was_complete;

	if (!v34_data_line_receiver_is_valid(receiver) || current == NULL ||
	    received == NULL)
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	if (receiver->failed)
		return V34_DATA_RECEIVE_FAILED;
	marker_was_complete =
		v34_data_line_receiver_rate_renegotiation_is_complete(receiver);

	/* Even a latched marker leaves this call owning one input sample.
	 * Preserve the physical clock before handing that state to control. */
	next = *receiver;
	line_result = v34_line_receiver_sample(
		&next.line, sample, &observation);
	if (line_result == V34_LINE_RECEIVER_OK) {
		*receiver = next;
		return marker_was_complete ?
			V34_DATA_RECEIVE_RATE_RENEGOTIATION : V34_DATA_RECEIVE_OK;
	}
	if (line_result != V34_LINE_RECEIVER_OBSERVATION_READY) {
		receiver->failed = true;
		return V34_DATA_RECEIVE_NUMERIC_FAILURE;
	}
	candidate_before = next.rate_renegotiation.enabled &&
		next.rate_renegotiation.s.candidate;
	result = v34_data_decision_receiver_is_receiving_b1(&next.decisions) ?
		V34_DATA_RECEIVE_OK :
		observe_rate_renegotiation(
			&next.rate_renegotiation, &observation,
			&rrn_adaptation, &rrn_control);
	if (v34_data_decision_receiver_is_receiving_b1(&next.decisions)) {
		rrn_adaptation = (struct v34_phase3_s_adaptation){ 0 };
		rrn_control = false;
	}
	if (result == V34_DATA_RECEIVE_RATE_RENEGOTIATION) {
		if (!complete_rate_renegotiation_boundary(
			    &next, &observation) ||
		    !v34_data_line_receiver_is_valid(&next))
			return fail_observation(
				receiver, &next, &observation,
				V34_DATA_RECEIVE_NUMERIC_FAILURE);
		*receiver = next;
		return result;
	}
	if (result != V34_DATA_RECEIVE_OK)
		return fail_observation(receiver, &next, &observation, result);
	candidate_after = next.rate_renegotiation.enabled &&
		next.rate_renegotiation.s.candidate;
	had_first = next.decisions.has_first_symbol;
	result = v34_data_decision_receiver_observe(
		&next.decisions, &observation, &decoded);
	if (result == V34_DATA_RECEIVE_INVALID_ARGUMENT ||
	    result == V34_DATA_RECEIVE_INVALID_STATE)
		result = V34_DATA_RECEIVE_NUMERIC_FAILURE;
	if (result_is_terminal(result))
		return fail_observation(receiver, &next, &observation, result);
	freeze = candidate_before || candidate_after;
	if (!had_first) {
		if (result != V34_DATA_RECEIVE_OK ||
		    v34_adaptive_equalizer_retain_observation(
			    &next.line.equalizer, &observation) !=
			    V34_ADAPTIVE_EQUALIZER_OK)
			return fail_observation(
				receiver, &next, &observation,
				V34_DATA_RECEIVE_NUMERIC_FAILURE);
		next.first_symbol_adaptation = rrn_adaptation;
		next.first_symbol_freezes_adaptation = freeze;
		next.first_symbol_is_control = rrn_control;
	} else {
		freeze = freeze || next.first_symbol_freezes_adaptation;
		apply_symbol_policy(
			&decoded.equalizer[0], &next.first_symbol_adaptation,
			next.first_symbol_is_control, freeze);
		apply_symbol_policy(
			&decoded.equalizer[1], &rrn_adaptation, rrn_control,
			freeze);
		if (v34_adaptive_equalizer_commit_pair(
			    &next.line.equalizer, decoded.equalizer) !=
		    V34_ADAPTIVE_EQUALIZER_OK)
			return fail_observation(
				receiver, &next, &observation,
				V34_DATA_RECEIVE_NUMERIC_FAILURE);
		clear_first_symbol_policy(&next);
	}
	if (!v34_data_line_receiver_is_valid(&next))
		return fail_observation(
			receiver, &next, &observation,
			V34_DATA_RECEIVE_NUMERIC_FAILURE);
	*receiver = next;
	if (result != V34_DATA_RECEIVE_OK) {
		*current = decoded.current;
		if (v34_data_receive_emits_mapping_frame(result))
			*received = decoded.received;
	}
	return result;
}
