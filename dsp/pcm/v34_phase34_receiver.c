// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase34_receiver.h"

#include "integer_math.h"
#include "memory_region.h"
#include "v34_constellation.h"

#include <stddef.h>
#include <string.h>

enum {
	V34_PHASE34_DATA_DECODER_POWER_Q15 = 0x2000,
};

static uint16_t decoder_power(
	const struct v34_phase34_receiver *receiver)
{
	return v34_training_decoder_power(
		receiver->line.timing.timing.nominal_symbol_rate,
		receiver->tracking_symbols);
}

static bool semantic_state_is_valid(
	const struct v34_phase34_receiver *receiver)
{
	if (receiver->stage == V34_PHASE34_RECEIVER_PHASE3)
		return memory_region_is_zero(
			       &receiver->phase3_handoff,
			       sizeof(receiver->phase3_handoff)) &&
		       v34_phase3_decision_receiver_is_valid(
			       &receiver->semantic.phase3) &&
		       !v34_phase3_decision_receiver_is_complete(
			       &receiver->semantic.phase3) &&
		       receiver->semantic.phase3.transmitting_role ==
			       receiver->transmitting_profile.local_role;
	if (!v34_phase3_decision_handoff_is_valid(
		    &receiver->phase3_handoff))
		return false;
	if (receiver->stage == V34_PHASE34_RECEIVER_PHASE4)
		return v34_phase4_decision_receiver_is_valid(
			       &receiver->semantic.phase4) &&
		       receiver->semantic.phase4.mp_receiver.transmitting_role ==
			       receiver->transmitting_profile.local_role &&
		       receiver->semantic.phase4.mp_receiver.constellation ==
			       receiver->phase4_constellation;
	return receiver->stage == V34_PHASE34_RECEIVER_DATA &&
	       !receiver->rate_renegotiation.acquire_phase &&
	       v34_phase3_s_hunt_is_valid(&receiver->rate_renegotiation) &&
	       v34_data_decision_receiver_is_valid(
		       &receiver->semantic.data) &&
	       receiver->semantic.data.data_receiver.data_receiver
		       .transmitting_role ==
		       receiver->transmitting_profile.local_role &&
	       receiver->semantic.data.data_receiver.data_receiver.schedule
		       .geometry
		       .symbol_rate == receiver->transmitting_profile.symbol_rate;
}

static bool precoder_coefficients_match(
	const struct v34_q14_coefficient left[V34_PRECODER_TAP_COUNT],
	const struct v34_q14_coefficient right[V34_PRECODER_TAP_COUNT])
{
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap)
		if (left[tap].real != right[tap].real ||
		    left[tap].imaginary != right[tap].imaginary)
			return false;
	return true;
}

static bool precoder_coefficients_are_zero(
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT])
{
	static const struct v34_q14_coefficient zero[V34_PRECODER_TAP_COUNT];

	return precoder_coefficients_match(coefficients, zero);
}

static uint32_t scaled_coefficient_magnitude(int16_t coefficient)
{
	int32_t scaled = arithmetic_shift_right_u32(
		(uint32_t)(int32_t)coefficient, 2);

	return scaled < 0 ? (uint32_t)-scaled : (uint32_t)scaled;
}

static bool feedback_training_supports_precoding(
	const struct v34_adaptive_equalizer *equalizer,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT])
{
	const struct v34_adaptive_equalizer_quality *quality =
		&equalizer->quality;
	uint32_t coefficient_magnitude = 0;
	int32_t gate = (int32_t)quality->decision_metric -
		quality->feedback_metric;

	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
		coefficient_magnitude +=
			scaled_coefficient_magnitude(coefficients[tap].real);
		coefficient_magnitude +=
			scaled_coefficient_magnitude(coefficients[tap].imaginary);
	}
	if (gate < 1)
		gate = quality->feedback_metric;
	gate -= quality->decision_metric >> 2;
	if (coefficient_magnitude > 8000u &&
	    quality->decision_metric > 200 && gate > 0)
		coefficient_magnitude = 0x5000u;
	return quality->decision_metric < 0x0200 &&
	       quality->feedback_metric < quality->decision_metric &&
	       coefficient_magnitude < 0x4000u;
}

static enum v34_adaptive_equalizer_feedback_mode expected_feedback_mode(
	const struct v34_phase34_receiver *receiver)
{
	/* The data decoder reconstructs the transmitter's precoder before its
	 * trellis decision and returns the inverse-precoded survivor as the
	 * equalizer target. Applying the same frozen filter in this front end
	 * would add the precoder prediction twice. */
	return receiver->feedback_training_complete ?
		V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE :
		V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN;
}

static bool tracking_state_is_valid(
	const struct v34_phase34_receiver *receiver)
{
	bool awaiting_phase4_s =
		receiver->stage == V34_PHASE34_RECEIVER_PHASE4 &&
		receiver->semantic.phase4.stage ==
			V34_PHASE4_DECISION_RECEIVER_ANSWER_S;
	bool expected =
		(receiver->stage != V34_PHASE34_RECEIVER_PHASE3 &&
		 (!awaiting_phase4_s || !receiver->phase4_s_rearmed)) ||
		(receiver->stage == V34_PHASE34_RECEIVER_PHASE3 &&
		 receiver->semantic.phase3.stage >=
			 V34_PHASE3_DECISION_RECEIVER_PP);

	if (receiver->tracking_started != expected)
		return false;
	return receiver->tracking_started || receiver->tracking_symbols == 0u;
}

static bool phase4_s_rearm_state_is_valid(
	const struct v34_phase34_receiver *receiver)
{
	enum v34_station_role role;

	if (receiver->stage == V34_PHASE34_RECEIVER_PHASE3)
		return !receiver->phase4_s_rearmed;
	role = receiver->phase3_handoff.j.sequence.role;
	if (role != V34_ANSWERING_STATION)
		return !receiver->phase4_s_rearmed;
	return receiver->stage == V34_PHASE34_RECEIVER_PHASE4 &&
		       receiver->semantic.phase4.stage ==
			       V34_PHASE4_DECISION_RECEIVER_ANSWER_S ?
		       true : receiver->phase4_s_rearmed;
}

bool v34_phase34_receiver_is_valid(
	const struct v34_phase34_receiver *receiver)
{
	if (receiver == NULL || !receiver->initialized ||
	    receiver->pending_head >= V34_PHASE34_PENDING_FRAMES ||
	    receiver->pending_count > V34_PHASE34_PENDING_FRAMES ||
	    receiver->confirmed_count > receiver->pending_count ||
	    (receiver->stage != V34_PHASE34_RECEIVER_DATA && receiver->pending_count != 0u) ||
	    !v34_phase3_profile_is_valid(&receiver->transmitting_profile) ||
	    !v34_constellation_size_is_valid(receiver->phase4_constellation) ||
	    !v34_line_receiver_matches_profile(
		    &receiver->line, &receiver->transmitting_profile) ||
	    receiver->line.equalizer.feedback_mode !=
		    expected_feedback_mode(receiver) ||
	    !semantic_state_is_valid(receiver) ||
	    !phase4_s_rearm_state_is_valid(receiver) ||
	    !tracking_state_is_valid(receiver))
		return false;
	if (receiver->data_erasure &&
	    receiver->stage != V34_PHASE34_RECEIVER_DATA)
		return false;
	if ((!receiver->feedback_training_complete &&
	     !precoder_coefficients_are_zero(
		     receiver->precoder_coefficients)) ||
	    (receiver->feedback_training_complete &&
	     receiver->stage == V34_PHASE34_RECEIVER_PHASE3))
		return false;
	if (receiver->stage == V34_PHASE34_RECEIVER_DATA &&
	    (!receiver->feedback_training_complete ||
	     !precoder_coefficients_match(
		     receiver->precoder_coefficients,
		     receiver->semantic.data.data_receiver.channel_receiver
			     .receive_precoder.coefficients)))
		return false;
	if (!v34_receive_timing_is_acquired(&receiver->line.timing))
		return receiver->line.equalizer.epoch == 0u &&
		       receiver->line.equalizer.inputs_since_observation == 0u &&
		       !receiver->line.equalizer.decision_pending &&
		       receiver->stage == V34_PHASE34_RECEIVER_PHASE3 &&
		       (receiver->semantic.phase3.stage ==
				V34_PHASE3_DECISION_RECEIVER_S ||
			receiver->semantic.phase3.stage ==
				V34_PHASE3_DECISION_RECEIVER_REPEATED_S) &&
		       receiver->semantic.phase3.s.buffered_decisions == 0u;
	return true;
}

bool v34_phase34_receiver_has_failed(
	const struct v34_phase34_receiver *receiver)
{
	return v34_phase34_receiver_is_valid(receiver) && receiver->failed;
}

bool v34_phase34_receiver_is_complete(
	const struct v34_phase34_receiver *receiver)
{
	return v34_phase34_receiver_is_valid(receiver) && !receiver->failed &&
	       receiver->stage == V34_PHASE34_RECEIVER_PHASE4 &&
	       v34_phase4_decision_receiver_is_complete(
		       &receiver->semantic.phase4);
}

enum v34_phase34_receiver_result v34_phase34_receiver_init(
	struct v34_phase34_receiver *receiver,
	const struct v34_phase3_profile *transmitting_profile,
	enum v34_constellation_size phase4_constellation,
	int16_t initial_receive_gain)
{
	struct v34_phase34_receiver initialized;

	if (receiver == NULL ||
	    !v34_phase3_profile_is_valid(transmitting_profile) ||
	    !v34_constellation_size_is_valid(phase4_constellation) ||
	    initial_receive_gain <= 0)
		return V34_PHASE34_RECEIVER_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	initialized.transmitting_profile = *transmitting_profile;
	initialized.phase4_constellation = phase4_constellation;
	initialized.stage = V34_PHASE34_RECEIVER_PHASE3;
	if (v34_line_receiver_init(
		    &initialized.line, transmitting_profile,
		    initial_receive_gain, false,
		    V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN) !=
		    V34_LINE_RECEIVER_OK ||
	    v34_phase3_decision_receiver_init_suffix(
		    &initialized.semantic.phase3,
		    transmitting_profile->local_role,
		    transmitting_profile->md_symbol_count,
		    V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS) !=
		    V34_PHASE3_DECISION_RECEIVER_OK)
		return V34_PHASE34_RECEIVER_INVALID_ARGUMENT;
	initialized.initialized = true;
	if (!v34_phase34_receiver_is_valid(&initialized))
		return V34_PHASE34_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE34_RECEIVER_OK;
}

static enum v34_phase34_receiver_result fail(
	struct v34_phase34_receiver *receiver,
	enum v34_phase34_receiver_result result)
{
	receiver->failed = true;
	return result;
}

static enum v34_phase34_receiver_result discard_and_fail(
	struct v34_phase34_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	enum v34_phase34_receiver_result result)
{
	if (v34_adaptive_equalizer_discard(
		    &receiver->line.equalizer, observation) !=
	    V34_ADAPTIVE_EQUALIZER_OK)
		result = V34_PHASE34_RECEIVER_NUMERIC_FAILURE;
	return fail(receiver, result);
}

static void advance_tracking(struct v34_phase34_receiver *receiver)
{
	uint16_t period;

	if (!receiver->tracking_started)
		return;
	period = receiver->line.timing.timing.nominal_symbol_rate;
	if (receiver->tracking_symbols < period)
		receiver->tracking_symbols++;
}

static enum v34_phase34_receiver_result phase3_result(
	enum v34_phase3_decision_receiver_result result)
{
	switch (result) {
	case V34_PHASE3_DECISION_RECEIVER_OK:
		return V34_PHASE34_RECEIVER_OK;
	case V34_PHASE3_DECISION_RECEIVER_S_DETECTED:
		return V34_PHASE34_RECEIVER_PHASE3_S_DETECTED;
	case V34_PHASE3_DECISION_RECEIVER_MD_COMPLETE:
		return V34_PHASE34_RECEIVER_MD_COMPLETE;
	case V34_PHASE3_DECISION_RECEIVER_REPEATED_S_DETECTED:
		return V34_PHASE34_RECEIVER_REPEATED_S_DETECTED;
	case V34_PHASE3_DECISION_RECEIVER_PP_COMPLETE:
		return V34_PHASE34_RECEIVER_PP_COMPLETE;
	case V34_PHASE3_DECISION_RECEIVER_J_DETECTED:
		return V34_PHASE34_RECEIVER_J_DETECTED;
	case V34_PHASE3_DECISION_RECEIVER_OUT_OF_RANGE:
		return V34_PHASE34_RECEIVER_OUT_OF_RANGE;
	case V34_PHASE3_DECISION_RECEIVER_ALREADY_COMPLETE:
	case V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT:
	case V34_PHASE3_DECISION_RECEIVER_INVALID_STATE:
		break;
	}
	return V34_PHASE34_RECEIVER_NUMERIC_FAILURE;
}

static bool semantic_result_consumed(
	enum v34_phase34_receiver_result result)
{
	switch (result) {
	case V34_PHASE34_RECEIVER_OK:
	case V34_PHASE34_RECEIVER_PHASE3_S_DETECTED:
	case V34_PHASE34_RECEIVER_MD_COMPLETE:
	case V34_PHASE34_RECEIVER_REPEATED_S_DETECTED:
	case V34_PHASE34_RECEIVER_PP_COMPLETE:
	case V34_PHASE34_RECEIVER_J_DETECTED:
	case V34_PHASE34_RECEIVER_PHASE4_S_DETECTED:
	case V34_PHASE34_RECEIVER_TRN_COMPLETE:
	case V34_PHASE34_RECEIVER_RECORD_READY:
	case V34_PHASE34_RECEIVER_E_COMPLETE:
	case V34_PHASE34_RECEIVER_INTERVAL_READY:
	case V34_PHASE34_RECEIVER_MAPPING_FRAME_READY:
	case V34_PHASE34_RECEIVER_B1_IN_PROGRESS:
	case V34_PHASE34_RECEIVER_B1_COMPLETE:
		return true;
	default:
		return false;
	}
}

static bool execute_receive_action(
	struct v34_phase34_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	const struct v34_receive_action *output)
{
	bool was_tracking = receiver->tracking_started;

	if (v34_line_receiver_resolve(
		    &receiver->line, observation, output,
		    decoder_power(receiver)) != V34_LINE_RECEIVER_OK)
		return false;
	if (output->action == V34_RECEIVE_BEGIN_TRACKING) {
		receiver->tracking_started = true;
		receiver->tracking_symbols = 0u;
		return true;
	}
	if (output->action == V34_RECEIVE_RESTART_PATH) {
		receiver->tracking_started = false;
		receiver->tracking_symbols = 0u;
		return true;
	}
	if (was_tracking)
		advance_tracking(receiver);
	return true;
}

static enum v34_phase34_receiver_result enter_phase4(
	struct v34_phase34_receiver *receiver,
	const struct v34_phase3_decision_receiver *phase3)
{
	struct v34_phase3_decision_handoff handoff;
	struct v34_phase4_decision_receiver phase4;

	if (v34_phase3_decision_receiver_handoff(phase3, &handoff) !=
		    V34_PHASE3_DECISION_RECEIVER_OK ||
	    v34_phase4_decision_receiver_init(
		    &phase4, &handoff.j.sequence,
		    receiver->phase4_constellation) !=
		    V34_PHASE4_DECISION_RECEIVER_OK)
		return V34_PHASE34_RECEIVER_NUMERIC_FAILURE;
	receiver->semantic =
		(union v34_phase34_receiver_semantic_state){ .phase4 = phase4 };
	receiver->phase3_handoff = handoff;
	receiver->stage = V34_PHASE34_RECEIVER_PHASE4;
	return V34_PHASE34_RECEIVER_J_DETECTED;
}

static enum v34_phase34_receiver_result receive_phase3(
	struct v34_phase34_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation)
{
	struct v34_phase3_decision_receiver phase3 =
		receiver->semantic.phase3;
	struct v34_receive_action output;
	enum v34_phase3_decision_receiver_result result =
		v34_phase3_decision_receiver_observe(
			&phase3, observation, decoder_power(receiver),
			&output);
	enum v34_phase34_receiver_result mapped = phase3_result(result);

	if (!semantic_result_consumed(mapped))
		return discard_and_fail(receiver, observation, mapped);
	if (!execute_receive_action(receiver, observation, &output))
		return fail(receiver, V34_PHASE34_RECEIVER_NUMERIC_FAILURE);
	if (result == V34_PHASE3_DECISION_RECEIVER_J_DETECTED) {
		enum v34_phase34_receiver_result transition =
			enter_phase4(receiver, &phase3);

		if (transition != V34_PHASE34_RECEIVER_J_DETECTED)
			return fail(receiver, transition);
		return transition;
	}
	receiver->semantic.phase3 = phase3;
	return mapped;
}

static enum v34_phase34_receiver_result phase4_result(
	enum v34_phase4_decision_receiver_result result)
{
	switch (result) {
	case V34_PHASE4_DECISION_RECEIVER_OK:
		return V34_PHASE34_RECEIVER_OK;
	case V34_PHASE4_DECISION_RECEIVER_S_DETECTED:
		return V34_PHASE34_RECEIVER_PHASE4_S_DETECTED;
	case V34_PHASE4_DECISION_RECEIVER_TRN_COMPLETE:
		return V34_PHASE34_RECEIVER_TRN_COMPLETE;
	case V34_PHASE4_DECISION_RECEIVER_RECORD_READY:
		return V34_PHASE34_RECEIVER_RECORD_READY;
	case V34_PHASE4_DECISION_RECEIVER_E_COMPLETE:
		return V34_PHASE34_RECEIVER_E_COMPLETE;
	case V34_PHASE4_DECISION_RECEIVER_ALREADY_COMPLETE:
	case V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT:
	case V34_PHASE4_DECISION_RECEIVER_INVALID_STATE:
		break;
	}
	return V34_PHASE34_RECEIVER_NUMERIC_FAILURE;
}

static enum v34_phase34_receiver_result receive_phase4(
	struct v34_phase34_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	struct v34_phase34_receive_output *output)
{
	struct v34_phase4_decision_receiver phase4 =
		receiver->semantic.phase4;
	struct v34_receive_action decision;
	struct v34_mp_receive_event emitted = { 0 };
	enum v34_phase4_decision_receiver_result result =
		v34_phase4_decision_receiver_observe(
			&phase4, observation, decoder_power(receiver),
			&decision, &emitted);
	enum v34_phase34_receiver_result mapped = phase4_result(result);

	if (!semantic_result_consumed(mapped))
		return discard_and_fail(receiver, observation, mapped);
	if (!execute_receive_action(receiver, observation, &decision))
		return fail(receiver, V34_PHASE34_RECEIVER_NUMERIC_FAILURE);
	receiver->semantic.phase4 = phase4;
	if (result == V34_PHASE4_DECISION_RECEIVER_RECORD_READY)
		output->mp = emitted;
	return mapped;
}

static enum v34_phase34_receiver_result data_result(
	enum v34_data_receive_result result)
{
	switch (result) {
	case V34_DATA_RECEIVE_OK:
		return V34_PHASE34_RECEIVER_OK;
	case V34_DATA_RECEIVE_INTERVAL_READY:
		return V34_PHASE34_RECEIVER_INTERVAL_READY;
	case V34_DATA_RECEIVE_MAPPING_FRAME_READY:
		return V34_PHASE34_RECEIVER_MAPPING_FRAME_READY;
	case V34_DATA_RECEIVE_B1_IN_PROGRESS:
		return V34_PHASE34_RECEIVER_B1_IN_PROGRESS;
	case V34_DATA_RECEIVE_B1_COMPLETE:
		return V34_PHASE34_RECEIVER_B1_COMPLETE;
	case V34_DATA_RECEIVE_RATE_RENEGOTIATION:
		/* The aggregate resolves online markers before data decisions. */
		break;
	case V34_DATA_RECEIVE_OUT_OF_RANGE:
		return V34_PHASE34_RECEIVER_OUT_OF_RANGE;
	case V34_DATA_RECEIVE_FAILED:
	case V34_DATA_RECEIVE_INVALID_ARGUMENT:
	case V34_DATA_RECEIVE_INVALID_STATE:
	case V34_DATA_RECEIVE_NUMERIC_FAILURE:
		break;
	}
	return V34_PHASE34_RECEIVER_NUMERIC_FAILURE;
}

static enum v34_phase34_receiver_result receive_data(
	struct v34_phase34_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	struct v34_phase34_receive_output *output)
{
	struct v34_data_decision_receiver data = receiver->semantic.data;
	struct v34_data_decision_output decoded;
	struct v34_adaptive_equalizer_decision decision;
	struct v34_phase3_s_adaptation adaptation;
	bool control = false;

	if (!receiver->data_erasure &&
	    !v34_data_decision_receiver_is_receiving_b1(&data)) {
		enum v34_phase3_s_receiver_result marker =
			v34_phase3_s_hunt_receive(&receiver->rate_renegotiation,
				observation->decision, &adaptation, &control);

		if (marker == V34_PHASE3_S_RECEIVER_DETECTED) {
			struct v34_phase4_decision_receiver phase4;

			if (v34_adaptive_equalizer_discard(&receiver->line.equalizer,
				    observation) != V34_ADAPTIVE_EQUALIZER_OK ||
			    v34_phase4_decision_receiver_resume_online(&phase4,
				    receiver->transmitting_profile.local_role) !=
				    V34_PHASE4_DECISION_RECEIVER_OK)
				return fail(receiver, V34_PHASE34_RECEIVER_NUMERIC_FAILURE);
			receiver->semantic = (union v34_phase34_receiver_semantic_state){
				.phase4 = phase4,
			};
			receiver->phase4_constellation = V34_CONSTELLATION_4_POINT;
			receiver->stage = V34_PHASE34_RECEIVER_PHASE4;
			receiver->data_erasure = false;
			receiver->pending_head = 0u;
			receiver->pending_count = 0u;
			receiver->confirmed_count = 0u;
			return V34_PHASE34_RECEIVER_RATE_RENEGOTIATION;
		}
		if (marker != V34_PHASE3_S_RECEIVER_OK)
			return discard_and_fail(receiver, observation,
				V34_PHASE34_RECEIVER_NUMERIC_FAILURE);
	}
	enum v34_data_receive_result result =
		v34_data_decision_receiver_observe(
			&data, observation, &decoded);
	enum v34_phase34_receiver_result mapped = data_result(result);

	if (!semantic_result_consumed(mapped))
		return discard_and_fail(receiver, observation, mapped);
	/* A missing line fragment does not stop the peer's mapping schedule.
	 * Consume its symbol positions without learning from erased decisions or
	 * exposing their frames. Otherwise a short gap permanently offsets data. */
	if (receiver->data_erasure) {
		if (v34_adaptive_equalizer_discard(
			    &receiver->line.equalizer, observation) !=
		    V34_ADAPTIVE_EQUALIZER_OK)
			return fail(receiver, V34_PHASE34_RECEIVER_NUMERIC_FAILURE);
		receiver->semantic.data = data;
		return V34_PHASE34_RECEIVER_OK;
	}
	/* Preserve the established V.34 schedule: advance the first symbol
	 * without adaptation, then train from the current 4-D decision. */
	decision = decoded.equalizer[result == V34_DATA_RECEIVE_OK ? 0u : 1u];
	if (decision.adaptation != V34_ADAPTIVE_EQUALIZER_SKIP)
		decision.decoder_power_q15 = V34_PHASE34_DATA_DECODER_POWER_Q15;
	if (v34_adaptive_equalizer_commit(
		    &receiver->line.equalizer, &decision) !=
	    V34_ADAPTIVE_EQUALIZER_OK)
		return fail(receiver, V34_PHASE34_RECEIVER_NUMERIC_FAILURE);
	advance_tracking(receiver);
	receiver->semantic.data = data;
	if (result != V34_DATA_RECEIVE_OK) {
		output->current = decoded.current;
		if (result != V34_DATA_RECEIVE_INTERVAL_READY)
			output->received = decoded.received;
	}
	return mapped;
}

static enum v34_phase34_receiver_result receive_observation(
	struct v34_phase34_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	struct v34_phase34_receive_output *output)
{
	switch (receiver->stage) {
	case V34_PHASE34_RECEIVER_PHASE3:
		return receive_phase3(receiver, observation);
	case V34_PHASE34_RECEIVER_PHASE4:
		return receive_phase4(receiver, observation, output);
	case V34_PHASE34_RECEIVER_DATA:
		return receive_data(receiver, observation, output);
	}
	return fail(receiver, V34_PHASE34_RECEIVER_NUMERIC_FAILURE);
}

static enum v34_phase34_receiver_result receive_line_sample(
	struct v34_phase34_receiver *receiver, int16_t sample,
	struct v34_phase34_receive_output *output)
{
	struct v34_adaptive_equalizer_observation observation;
	enum v34_line_receiver_result line_result;
	enum v34_phase34_receiver_result result;

	line_result = v34_line_receiver_sample(
		&receiver->line, sample, &observation);
	if (line_result == V34_LINE_RECEIVER_OK)
		return V34_PHASE34_RECEIVER_OK;
	if (line_result == V34_LINE_RECEIVER_TIMING_ACQUIRED)
		return V34_PHASE34_RECEIVER_TIMING_ACQUIRED;
	if (line_result != V34_LINE_RECEIVER_OBSERVATION_READY)
		return fail(receiver, V34_PHASE34_RECEIVER_NUMERIC_FAILURE);
	result = receive_observation(receiver, &observation, output);
	if (!v34_phase34_receiver_is_valid(receiver))
		return V34_PHASE34_RECEIVER_INVALID_STATE;
	return result;
}

static enum v34_phase34_receiver_result defer_control_frames(
	struct v34_phase34_receiver *receiver,
	enum v34_phase34_receiver_result result,
	struct v34_phase34_receive_output *output)
{
	bool candidate = receiver->rate_renegotiation.candidate;

	if (receiver->data_erasure)
		return result;
	if (result != V34_PHASE34_RECEIVER_OK &&
	    result != V34_PHASE34_RECEIVER_INTERVAL_READY &&
	    result != V34_PHASE34_RECEIVER_MAPPING_FRAME_READY)
		return result;
	if (result == V34_PHASE34_RECEIVER_MAPPING_FRAME_READY &&
	    (candidate || receiver->pending_count != 0u)) {
		if (receiver->pending_count == V34_PHASE34_PENDING_FRAMES)
			return fail(receiver, V34_PHASE34_RECEIVER_NUMERIC_FAILURE);
		receiver->pending_frames[(receiver->pending_head + receiver->pending_count) %
			V34_PHASE34_PENDING_FRAMES] = output->received;
		receiver->pending_count++;
		/* The current interval was decoded even though its completed
		 * mapping frame must wait for the control hypothesis. */
		result = V34_PHASE34_RECEIVER_INTERVAL_READY;
	}
	if (!candidate)
		receiver->confirmed_count = receiver->pending_count;
	if (receiver->confirmed_count != 0u) {
		output->received = receiver->pending_frames[receiver->pending_head];
		receiver->pending_head = (uint8_t)((receiver->pending_head + 1u) %
			V34_PHASE34_PENDING_FRAMES);
		receiver->pending_count--;
		receiver->confirmed_count--;
		result = V34_PHASE34_RECEIVER_MAPPING_FRAME_READY;
	}
	return result;
}

enum v34_phase34_receiver_result v34_phase34_receiver_sample(
	struct v34_phase34_receiver *receiver, int16_t sample,
	struct v34_phase34_receive_output *output)
{
	if (output == NULL ||
	    memory_regions_overlap(
		    receiver, receiver == NULL ? 0 : sizeof(*receiver),
		    output, sizeof(*output)))
		return V34_PHASE34_RECEIVER_INVALID_ARGUMENT;
	if (!v34_phase34_receiver_is_valid(receiver))
		return V34_PHASE34_RECEIVER_INVALID_STATE;
	if (receiver->failed)
		return V34_PHASE34_RECEIVER_FAILED;
	if (v34_phase34_receiver_is_complete(receiver))
		return V34_PHASE34_RECEIVER_COMPLETE;
	if (receiver->stage == V34_PHASE34_RECEIVER_PHASE4 &&
	    receiver->semantic.phase4.record_pending)
		return V34_PHASE34_RECEIVER_INVALID_STATE;
	if (receiver->stage == V34_PHASE34_RECEIVER_PHASE4 &&
	    receiver->semantic.phase4.stage ==
		    V34_PHASE4_DECISION_RECEIVER_ANSWER_S &&
	    !receiver->phase4_s_rearmed)
		return V34_PHASE34_RECEIVER_INVALID_STATE;
	enum v34_phase34_receiver_result result =
		receive_line_sample(receiver, sample, output);

	/* A rejected S hypothesis must delay data, not silently discard it.
	 * Only a complete marker discards quarantined control frames. */
	return receiver->stage == V34_PHASE34_RECEIVER_DATA ?
		defer_control_frames(receiver, result, output) : result;
}

enum v34_phase34_receiver_result v34_phase34_receiver_rearm_phase4_s(
	struct v34_phase34_receiver *receiver)
{
	struct v34_phase34_receiver next;

	if (!v34_phase34_receiver_is_valid(receiver) || receiver->failed ||
	    receiver->stage != V34_PHASE34_RECEIVER_PHASE4 ||
	    receiver->semantic.phase4.stage !=
		    V34_PHASE4_DECISION_RECEIVER_ANSWER_S ||
	    receiver->phase4_s_rearmed ||
	    receiver->line.equalizer.decision_pending ||
	    receiver->line.equalizer.inputs_since_observation != 0u ||
	    !v34_receive_timing_is_acquired(&receiver->line.timing) ||
	    receiver->line.equalizer.epoch <
		    V34_ADAPTIVE_EQUALIZER_FIRST_SIGNAL_EPOCH)
		return V34_PHASE34_RECEIVER_INVALID_STATE;
	next = *receiver;
	next.tracking_started = false;
	next.tracking_symbols = 0u;
	next.phase4_s_rearmed = true;
	if (!v34_phase34_receiver_is_valid(&next))
		return V34_PHASE34_RECEIVER_NUMERIC_FAILURE;
	*receiver = next;
	return V34_PHASE34_RECEIVER_OK;
}

static enum v34_phase34_receiver_result phase4_control(
	struct v34_phase34_receiver *receiver,
	enum v34_phase4_decision_receiver_result (*control)(
		struct v34_phase4_decision_receiver *))
{
	if (!v34_phase34_receiver_is_valid(receiver) || receiver->failed ||
	    receiver->stage != V34_PHASE34_RECEIVER_PHASE4)
		return V34_PHASE34_RECEIVER_INVALID_STATE;
	return control(&receiver->semantic.phase4) ==
		       V34_PHASE4_DECISION_RECEIVER_OK ?
		       V34_PHASE34_RECEIVER_OK :
		       V34_PHASE34_RECEIVER_INVALID_STATE;
}

enum v34_phase34_receiver_result v34_phase34_receiver_continue_mp(
	struct v34_phase34_receiver *receiver)
{
	return phase4_control(
		receiver, v34_phase4_decision_receiver_continue_mp);
}

enum v34_phase34_receiver_result v34_phase34_receiver_expect_e(
	struct v34_phase34_receiver *receiver)
{
	return phase4_control(receiver, v34_phase4_decision_receiver_expect_e);
}

enum v34_phase34_receiver_result
v34_phase34_receiver_finish_feedback_training(
	struct v34_phase34_receiver *receiver, bool allow_precoder,
	struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT])
{
	struct v34_phase34_receiver next;
	struct v34_q14_coefficient learned[V34_PRECODER_TAP_COUNT];
	struct v34_q14_coefficient selected[V34_PRECODER_TAP_COUNT] = { 0 };
	bool select_precoder;

	if (coefficients == NULL ||
	    memory_regions_overlap(
		    receiver, receiver == NULL ? 0 : sizeof(*receiver),
		    coefficients, sizeof(selected)))
		return V34_PHASE34_RECEIVER_INVALID_ARGUMENT;
	if (!v34_phase34_receiver_is_valid(receiver) || receiver->failed ||
	    receiver->stage != V34_PHASE34_RECEIVER_PHASE4 ||
	    receiver->feedback_training_complete)
		return V34_PHASE34_RECEIVER_INVALID_STATE;
	next = *receiver;
	if (v34_adaptive_equalizer_finish_feedback_training(
		    &next.line.equalizer, learned) != V34_ADAPTIVE_EQUALIZER_OK)
		return V34_PHASE34_RECEIVER_INVALID_STATE;
	select_precoder = allow_precoder &&
		feedback_training_supports_precoding(&next.line.equalizer, learned);
	if (select_precoder)
		memcpy(selected, learned, sizeof(selected));
	memcpy(next.precoder_coefficients, selected, sizeof(selected));
	next.feedback_training_complete = true;
	if (!v34_phase34_receiver_is_valid(&next))
		return V34_PHASE34_RECEIVER_NUMERIC_FAILURE;
	*receiver = next;
	memcpy(coefficients, selected, sizeof(selected));
	return V34_PHASE34_RECEIVER_OK;
}

enum v34_phase34_receiver_result v34_phase34_receiver_start_data(
	struct v34_phase34_receiver *receiver,
	const struct v34_power_normalizer *normalizer,
	const struct v34_frame_geometry *geometry, bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection)
{
	struct v34_phase34_receiver next;
	struct v34_data_decision_receiver data;

	if (normalizer == NULL || geometry == NULL || coefficients == NULL ||
	    memory_regions_overlap(
		    receiver, receiver == NULL ? 0 : sizeof(*receiver),
		    normalizer, sizeof(*normalizer)) ||
	    memory_regions_overlap(
		    receiver, receiver == NULL ? 0 : sizeof(*receiver),
		    geometry, sizeof(*geometry)) ||
	    memory_regions_overlap(
		    receiver, receiver == NULL ? 0 : sizeof(*receiver),
		    coefficients,
		    sizeof(*coefficients) * V34_PRECODER_TAP_COUNT))
		return V34_PHASE34_RECEIVER_INVALID_ARGUMENT;
	if (!v34_phase34_receiver_is_valid(receiver) || receiver->failed ||
	    !v34_phase34_receiver_is_complete(receiver) ||
	    !receiver->feedback_training_complete)
		return V34_PHASE34_RECEIVER_INVALID_STATE;
	if (geometry->symbol_rate != receiver->transmitting_profile.symbol_rate)
		return V34_PHASE34_RECEIVER_INVALID_ARGUMENT;
	if (!precoder_coefficients_match(
		    receiver->precoder_coefficients, coefficients))
		return V34_PHASE34_RECEIVER_INVALID_ARGUMENT;
	if (v34_data_decision_receiver_init(
		    &data, normalizer, geometry, expanded_shaping,
		    coefficients, selection,
		    receiver->transmitting_profile.local_role) !=
	    V34_DATA_RECEIVE_OK)
		return V34_PHASE34_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	/* E changes the semantic decoder, not the physical receive epoch.  Keep
	 * the active timing schedule and its learned fractional-clock state
	 * continuous through B1. */
	if (v34_adaptive_equalizer_set_feedback_mode(
		    &next.line.equalizer, V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE) !=
	    V34_ADAPTIVE_EQUALIZER_OK)
		return V34_PHASE34_RECEIVER_INVALID_STATE;
	next.semantic =
		(union v34_phase34_receiver_semantic_state){ .data = data };
	if (v34_phase3_s_hunt_init(&next.rate_renegotiation) !=
	    V34_PHASE3_S_RECEIVER_OK)
		return V34_PHASE34_RECEIVER_NUMERIC_FAILURE;
	next.stage = V34_PHASE34_RECEIVER_DATA;
	if (!v34_phase34_receiver_is_valid(&next))
		return V34_PHASE34_RECEIVER_NUMERIC_FAILURE;
	*receiver = next;
	return V34_PHASE34_RECEIVER_OK;
}

enum v34_phase34_receiver_result
v34_phase34_receiver_set_data_timing_frozen(
	struct v34_phase34_receiver *receiver, bool frozen)
{
	struct v34_phase34_receiver next;

	if (!v34_phase34_receiver_is_valid(receiver) ||
	    receiver->stage != V34_PHASE34_RECEIVER_DATA)
		return V34_PHASE34_RECEIVER_INVALID_STATE;
	next = *receiver;
	if (v34_receive_timing_set_data_frozen(&next.line.timing, frozen) !=
		    V34_RECEIVE_TIMING_OK ||
	    !v34_phase34_receiver_is_valid(&next))
		return V34_PHASE34_RECEIVER_INVALID_STATE;
	*receiver = next;
	return V34_PHASE34_RECEIVER_OK;
}

enum v34_phase34_receiver_result
v34_phase34_receiver_set_data_erasure(
	struct v34_phase34_receiver *receiver, bool erasure)
{
	struct v34_phase34_receiver next;

	if (!v34_phase34_receiver_is_valid(receiver) ||
	    receiver->stage != V34_PHASE34_RECEIVER_DATA ||
	    receiver->line.equalizer.decision_pending)
		return V34_PHASE34_RECEIVER_INVALID_STATE;
	next = *receiver;
	next.data_erasure = erasure;
	if (!v34_phase34_receiver_is_valid(&next))
		return V34_PHASE34_RECEIVER_INVALID_STATE;
	*receiver = next;
	return V34_PHASE34_RECEIVER_OK;
}
