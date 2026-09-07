// SPDX-License-Identifier: BSD-3-Clause

#include "v34_call.h"

#include "memory_region.h"

#include <stddef.h>
#include <string.h>

static enum v34_call_result phase3_result(
	enum v34_phase3_exchange_result result);

static bool configuration_is_valid(
	const struct v34_call_configuration *configuration)
{
	if (configuration == NULL ||
	    !v34_station_role_is_valid(configuration->local_role) ||
	    !v34_info0_is_valid(&configuration->local_info0) ||
	    configuration->phase2_nominal_amplitude <= 0 ||
	    configuration->phase3.sample_rate != V34_PHASE2_SAMPLE_RATE ||
	    configuration->phase3.nominal_peak_amplitude <= 0 ||
	    !v34_constellation_size_is_valid(
		    configuration->phase3.requested_remote_constellation) ||
	    !v34_mp_record_is_valid(&configuration->phase4.transmit_mp) ||
	    v34_mp_record_common_parameters(
		    &configuration->phase4.transmit_mp)
		    ->acknowledges_peer_mp)
		return false;
	return configuration->local_role == V34_CALLING_STATION ?
		       v34_info1c_is_valid(
			       &configuration->local_info1.calling) :
		       v34_info1a_is_valid(
			       &configuration->local_info1.answering);
}

static bool stored_configuration_is_valid(const struct v34_call *call)
{
	if (!v34_station_role_is_valid(call->local_role) ||
	    call->phase2_nominal_amplitude <= 0 ||
	    !v34_info0_is_valid(&call->local_info0) ||
	    (call->peer_info0_stored &&
	     !v34_info0_is_valid(&call->peer_info0)) ||
	    !v34_full_retrain_is_valid(&call->retrain) ||
	    call->retrain.detector.tone.local_role != call->local_role ||
	    !v34_echo_canceller_is_valid(&call->echo) ||
	    (!call->round_trip_delay_stored &&
	     call->round_trip_delay_samples != 0u) ||
	    call->round_trip_delay_samples >
		    PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES -
			    PHASE2_RANGING_TURNAROUND_SAMPLES ||
	    call->phase3_configuration.sample_rate !=
		    V34_PHASE2_SAMPLE_RATE ||
	    call->phase3_configuration.nominal_peak_amplitude <= 0 ||
	    !v34_constellation_size_is_valid(
		    call->phase3_configuration.requested_remote_constellation) ||
	    !v34_mp_record_is_valid(
		    &call->phase4_configuration.transmit_mp) ||
	    v34_mp_record_common_parameters(
		    &call->phase4_configuration.transmit_mp)
		    ->acknowledges_peer_mp)
		return false;
	return call->local_role == V34_CALLING_STATION ?
		       v34_info1c_is_valid(&call->local_info1.calling) :
		       v34_info1a_is_valid(&call->local_info1.answering);
}

static bool echo_adaptation_is_expected(const struct v34_call *call)
{
	if (call->stage != V34_CALL_PHASE3 ||
	    call->state.phase3.transmit.stage !=
		    V34_TRAINING_TRANSMITTER_PHASE3 ||
	    call->state.phase3.transmit.phase3.stage ==
		    V34_PHASE3_TRANSMITTER_J)
		return false;
	return call->local_role == V34_ANSWERING_STATION ||
	       call->state.phase3.transmit_started;
}

static bool add_caller_retrain_fallbacks(struct v34_call *call)
{
	struct v34_info1c selected = call->local_info1.calling;
	uint8_t projected_ceiling = 0;
	unsigned int highest_offered = 0;
	bool offered = false;

	if (!v34_phase2_info1c_is_consistent(
		    &call->local_info0, &call->peer_info0, &selected))
		return false;
	for (unsigned int index = 0; index < V34_SYMBOL_RATE_COUNT; ++index) {
		uint8_t projected = selected.answer_transmit[index]
					    .projected_data_rate_2400_bps_units;

		if (projected == 0)
			continue;
		offered = true;
		highest_offered = index;
		if (projected > projected_ceiling)
			projected_ceiling = projected;
	}
	if (!offered)
		return true;
	for (unsigned int index = 0; index <= highest_offered; ++index) {
		struct v34_info1c candidate;
		struct v34_info1_probe_result *probe;
		uint8_t limit;

		if (selected.answer_transmit[index]
			    .projected_data_rate_2400_bps_units != 0)
			continue;
		limit = v34_phase2_projected_rate_limit(
			(enum v34_symbol_rate)index);
		if (limit == 0)
			continue;
		candidate = selected;
		probe = &candidate.answer_transmit[index];
		probe->high_carrier = index == V34_SYMBOL_RATE_2743;
		probe->projected_data_rate_2400_bps_units =
			projected_ceiling < limit ? projected_ceiling : limit;
		if (!v34_phase2_info1c_is_consistent(
			    &call->local_info0, &call->peer_info0, &candidate)) {
			probe->high_carrier = !probe->high_carrier;
			if (!v34_phase2_info1c_is_consistent(
				    &call->local_info0, &call->peer_info0,
				    &candidate))
				continue;
		}
		selected = candidate;
	}
	call->local_info1.calling = selected;
	return true;
}

bool v34_call_is_valid(const struct v34_call *call)
{
	bool child_valid;
	bool child_failed;
	enum v34_station_role child_role;

	if (call == NULL || !stored_configuration_is_valid(call))
		return false;
	switch (call->stage) {
	case V34_CALL_PHASE2:
		child_valid = v34_phase2_is_valid(&call->state.phase2);
		child_failed = v34_phase2_has_failed(&call->state.phase2);
		child_role = call->state.phase2.local_role;
		break;
	case V34_CALL_PHASE3:
		child_valid = v34_phase3_exchange_is_valid(&call->state.phase3);
		child_failed = v34_phase3_exchange_has_failed(&call->state.phase3);
		child_role = call->state.phase3.local_role;
		break;
	case V34_CALL_PHASE4:
		child_valid = v34_phase4_exchange_is_valid(&call->state.phase4);
		child_failed = v34_phase4_exchange_has_failed(&call->state.phase4);
		child_role = v34_station_peer_role(
			call->state.phase4.receive.transmitting_profile.local_role);
		break;
	case V34_CALL_RETRAIN_SILENCE:
		child_valid = call->peer_info0_stored && call->retrain_active &&
			      v34_full_retrain_response_is_active(
				      &call->retrain);
		child_failed = false;
		child_role = call->local_role;
		break;
	case V34_CALL_CLEARDOWN:
		child_valid = v34_cleardown_is_valid(&call->state.cleardown);
		child_failed = call->state.cleardown.failed;
		child_role = call->state.cleardown.transmit_line.profile.local_role;
		break;
	default:
		return false;
	}
	return child_valid && child_role == call->local_role &&
	       v34_full_retrain_response_is_active(&call->retrain) ==
		       (call->stage == V34_CALL_RETRAIN_SILENCE) &&
	       v34_echo_canceller_is_adapting(&call->echo) ==
		       echo_adaptation_is_expected(call) &&
	       (call->round_trip_delay_stored ||
		(call->stage == V34_CALL_PHASE2 &&
		 call->state.phase2.stage == V34_PHASE2_RANGING)) &&
	       (call->failed || !child_failed);
}

enum v34_call_result v34_call_init(
	struct v34_call *call,
	const struct v34_call_configuration *configuration)
{
	struct v34_call initialized = { 0 };

	if (call == NULL || !configuration_is_valid(configuration))
		return V34_CALL_INVALID_ARGUMENT;
	if (v34_phase2_init(
		    &initialized.state.phase2, configuration->local_role,
		    configuration->phase2_nominal_amplitude,
		    &configuration->local_info0) != V34_PHASE2_OK)
		return V34_CALL_INVALID_ARGUMENT;
	initialized.local_info1 = configuration->local_info1;
	initialized.local_info0 = configuration->local_info0;
	initialized.phase3_configuration = configuration->phase3;
	initialized.phase4_configuration = configuration->phase4;
	initialized.phase2_nominal_amplitude =
		configuration->phase2_nominal_amplitude;
	initialized.local_role = configuration->local_role;
	initialized.stage = V34_CALL_PHASE2;
	if (v34_full_retrain_init(
		    &initialized.retrain,
		    configuration->local_role) != V34_PHASE2_TONE_OK ||
	    v34_echo_canceller_init(&initialized.echo) !=
		    V34_ECHO_CANCELLER_OK ||
	    v34_echo_canceller_freeze(&initialized.echo) !=
		    V34_ECHO_CANCELLER_OK)
		return V34_CALL_INVALID_ARGUMENT;
	if (!v34_call_is_valid(&initialized))
		return V34_CALL_NUMERIC_FAILURE;
	*call = initialized;
	return V34_CALL_OK;
}

enum v34_call_result v34_call_init_from_phase2(
	struct v34_call *call,
	const struct v34_call_configuration *configuration,
	const struct v34_phase2_handoff *handoff)
{
	struct v34_call initialized = { 0 };
	struct v34_phase3_profile caller;
	struct v34_phase3_profile answerer;
	const struct v34_phase3_profile *local;
	const struct v34_phase3_profile *peer;
	enum v34_phase3_exchange_result result;

	if (call == NULL || !configuration_is_valid(configuration) ||
	    handoff == NULL ||
	    configuration->local_role != handoff->local_role ||
	    memory_regions_overlap(
		    call, sizeof(*call), configuration, sizeof(*configuration)) ||
	    memory_regions_overlap(
		    call, sizeof(*call), handoff, sizeof(*handoff)))
		return V34_CALL_INVALID_ARGUMENT;
	if (v34_phase2_handoff_profiles(handoff, &caller, &answerer) !=
	    V34_PHASE2_SELECTION_OK)
		return V34_CALL_INVALID_ARGUMENT;
	local = handoff->local_role == V34_CALLING_STATION ?
			&caller : &answerer;
	peer = handoff->local_role == V34_CALLING_STATION ?
		       &answerer : &caller;
	initialized.local_info0 = handoff->local_info0;
	initialized.peer_info0 = handoff->peer_info0;
	initialized.local_info1 = handoff->local_role == V34_CALLING_STATION ?
		(union v34_local_info1){ .calling = handoff->caller_info1c } :
		(union v34_local_info1){ .answering = handoff->answerer_info1a };
	initialized.phase3_configuration = configuration->phase3;
	initialized.phase4_configuration = configuration->phase4;
	initialized.round_trip_delay_samples =
		handoff->round_trip_delay_samples;
	initialized.phase2_nominal_amplitude =
		configuration->phase2_nominal_amplitude;
	initialized.local_role = handoff->local_role;
	initialized.stage = V34_CALL_PHASE3;
	initialized.peer_info0_stored = true;
	initialized.round_trip_delay_stored = true;
	result = v34_phase3_exchange_init(
		&initialized.state.phase3, local, peer,
		&initialized.phase3_configuration, handoff->receive_gain);
	if (result != V34_PHASE3_EXCHANGE_OK)
		return phase3_result(result);
	if (v34_full_retrain_init(
		    &initialized.retrain, initialized.local_role) !=
		    V34_PHASE2_TONE_OK ||
	    v34_echo_canceller_init(&initialized.echo) !=
		    V34_ECHO_CANCELLER_OK ||
	    (initialized.local_role == V34_CALLING_STATION &&
	     v34_echo_canceller_freeze(&initialized.echo) !=
		     V34_ECHO_CANCELLER_OK))
		return V34_CALL_NUMERIC_FAILURE;
	if (!v34_call_is_valid(&initialized))
		return V34_CALL_NUMERIC_FAILURE;
	*call = initialized;
	return V34_CALL_OK;
}

static enum v34_call_result begin_retrain(
	struct v34_call *call, bool retain_peer_tone)
{
	struct v34_call next = *call;

	if (!next.peer_info0_stored) {
		if (next.stage != V34_CALL_PHASE2 ||
		    v34_phase2_peer_info0(
			    &next.state.phase2, &next.peer_info0) !=
			    V34_PHASE2_OK)
			return V34_CALL_INVALID_STATE;
		next.peer_info0_stored = true;
	}
	if (!next.round_trip_delay_stored)
		return V34_CALL_INVALID_STATE;
	/* A forced initial cadence remains exact.  Once the peer requests a full
	 * retrain, let it select a compatible slower probe result instead of
	 * rejecting an otherwise valid INFO1a fallback. */
	if (next.local_role == V34_CALLING_STATION &&
	    !add_caller_retrain_fallbacks(&next))
		return V34_CALL_INVALID_STATE;
	if (v34_full_retrain_begin_response(
		    &next.retrain, retain_peer_tone) != V34_PHASE2_TONE_OK ||
	    v34_echo_canceller_init(&next.echo) !=
		    V34_ECHO_CANCELLER_OK ||
	    v34_echo_canceller_freeze(&next.echo) !=
		    V34_ECHO_CANCELLER_OK)
		return V34_CALL_NUMERIC_FAILURE;
	next.retrain_active = true;
	next.stage = V34_CALL_RETRAIN_SILENCE;
	if (!v34_call_is_valid(&next))
		return V34_CALL_NUMERIC_FAILURE;
	*call = next;
	return V34_CALL_OK;
}

enum v34_call_result v34_call_start_retrain(struct v34_call *call)
{
	if (!v34_call_is_valid(call))
		return V34_CALL_INVALID_ARGUMENT;
	if (!v34_call_is_online(call))
		return V34_CALL_INVALID_STATE;
	return begin_retrain(call, false);
}

enum v34_call_result v34_call_start_cleardown(struct v34_call *call)
{
	struct v34_cleardown cleardown;
	const struct v34_phase4_exchange *online;

	if (!v34_call_is_valid(call))
		return V34_CALL_INVALID_ARGUMENT;
	if (call->stage == V34_CALL_CLEARDOWN && !call->failed)
		return V34_CALL_OK;
	if (!v34_call_is_online(call))
		return V34_CALL_INVALID_STATE;
	online = &call->state.phase4;
	if (v34_cleardown_init(
		    &cleardown, &online->transmit.data.line,
		    &online->receive.line,
		    v34_mp_record_common_parameters(&online->transmit_mp)) !=
		    V34_CLEARDOWN_OK)
		return V34_CALL_NUMERIC_FAILURE;
	call->state = (union v34_call_state){ .cleardown = cleardown };
	call->stage = V34_CALL_CLEARDOWN;
	return V34_CALL_OK;
}

bool v34_call_has_failed(const struct v34_call *call)
{
	return v34_call_is_valid(call) && call->failed;
}

bool v34_call_is_online(const struct v34_call *call)
{
	return v34_call_is_valid(call) && !call->failed &&
	       call->stage == V34_CALL_PHASE4 &&
	       v34_phase4_exchange_is_online(&call->state.phase4);
}

bool v34_call_get_data_startup(
	const struct v34_call *call, struct v34_data_startup *startup)
{
	return v34_call_is_valid(call) && !call->failed &&
	       call->stage == V34_CALL_PHASE4 && startup != NULL &&
	       !memory_regions_overlap(
		       call, sizeof(*call), startup, sizeof(*startup)) &&
	       v34_phase4_exchange_get_data_startup(
		       &call->state.phase4, startup);
}

static bool outputs_overlap(
	const struct v34_call *call, const int16_t *sample,
	const struct v34_call_observation *observation,
	const struct v34_received_mapping_frame *received)
{
	const struct memory_region regions[] = {
		{ call, sizeof(*call) },
		{ sample, sizeof(*sample) },
		{ observation, sizeof(*observation) },
		{ received, sizeof(*received) },
	};

	return memory_region_set_overlaps(regions, sizeof regions / sizeof *regions);
}

static enum v34_call_result phase3_result(
	enum v34_phase3_exchange_result result)
{
	switch (result) {
	case V34_PHASE3_EXCHANGE_OK:
		return V34_CALL_OK;
	case V34_PHASE3_EXCHANGE_FAILED:
		return V34_CALL_FAILED;
	case V34_PHASE3_EXCHANGE_INVALID_STATE:
		return V34_CALL_INVALID_STATE;
	case V34_PHASE3_EXCHANGE_OUT_OF_RANGE:
		return V34_CALL_OUT_OF_RANGE;
	case V34_PHASE3_EXCHANGE_CLIPPING_RISK:
		return V34_CALL_CLIPPING_RISK;
	case V34_PHASE3_EXCHANGE_SEQUENCE_MISMATCH:
		return V34_CALL_SEQUENCE_MISMATCH;
	case V34_PHASE3_EXCHANGE_INVALID_ARGUMENT:
	case V34_PHASE3_EXCHANGE_NUMERIC_FAILURE:
		break;
	}
	return V34_CALL_NUMERIC_FAILURE;
}

static enum v34_call_result phase4_result(
	enum v34_phase4_exchange_result result)
{
	switch (result) {
	case V34_PHASE4_EXCHANGE_OK:
		return V34_CALL_OK;
	case V34_PHASE4_EXCHANGE_SOURCE_FAILURE:
		return V34_CALL_SOURCE_FAILURE;
	case V34_PHASE4_EXCHANGE_FAILED:
		return V34_CALL_FAILED;
	case V34_PHASE4_EXCHANGE_INVALID_STATE:
		return V34_CALL_INVALID_STATE;
	case V34_PHASE4_EXCHANGE_NO_COMMON_RATE:
		return V34_CALL_NO_COMMON_RATE;
	case V34_PHASE4_EXCHANGE_OUT_OF_RANGE:
		return V34_CALL_OUT_OF_RANGE;
	case V34_PHASE4_EXCHANGE_CLIPPING_RISK:
		return V34_CALL_CLIPPING_RISK;
	case V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH:
		return V34_CALL_SEQUENCE_MISMATCH;
	case V34_PHASE4_EXCHANGE_INVALID_ARGUMENT:
	case V34_PHASE4_EXCHANGE_NUMERIC_FAILURE:
		break;
	}
	return V34_CALL_NUMERIC_FAILURE;
}

static enum v34_call_result enter_phase3(struct v34_call *call)
{
	struct v34_phase3_profile local;
	struct v34_phase3_profile peer;
	enum v34_phase3_exchange_result result;
	int16_t receive_gain;

	if (v34_phase2_peer_info0(
		    &call->state.phase2, &call->peer_info0) != V34_PHASE2_OK ||
	    v34_phase2_profiles(
		    &call->state.phase2, &local, &peer) != V34_PHASE2_OK ||
	    v34_phase2_receive_gain(
		    &call->state.phase2, &receive_gain) != V34_PHASE2_OK)
		return V34_CALL_NUMERIC_FAILURE;
	call->peer_info0_stored = true;
	result = v34_phase3_exchange_init(
		&call->state.phase3, &local, &peer,
		&call->phase3_configuration, receive_gain);
	if (result != V34_PHASE3_EXCHANGE_OK)
		return phase3_result(result);
	if (v34_echo_canceller_init(&call->echo) !=
		    V34_ECHO_CANCELLER_OK)
		return V34_CALL_NUMERIC_FAILURE;
	/* The caller retains pass-through until its own Phase 3 transmission.
	 * The answering peer then turns off J and leaves a training interval
	 * before it may answer the caller's later J. */
	if (call->local_role == V34_CALLING_STATION &&
	    v34_echo_canceller_freeze(&call->echo) !=
		    V34_ECHO_CANCELLER_OK)
		return V34_CALL_NUMERIC_FAILURE;
	call->stage = V34_CALL_PHASE3;
	return V34_CALL_OK;
}

static bool select_local_preemphasis(
	struct v34_call *call, union v34_local_info1 *selection);

static enum v34_call_result answer_info1c(struct v34_call *call)
{
	union v34_local_info1 selection = call->local_info1;
	struct v34_info0 peer_info0;
	struct v34_info1c peer;
	enum v34_phase2_selection_result result;

	if (v34_phase2_peer_info0(
		    &call->state.phase2, &peer_info0) != V34_PHASE2_OK ||
	    v34_phase2_peer_info1c(&call->state.phase2, &peer) !=
		    V34_PHASE2_OK)
		return V34_CALL_SEQUENCE_MISMATCH;
	result = v34_phase2_select_answerer_info1a(
		&peer_info0, &call->local_info0, &peer,
		&call->local_info1.answering, &selection.answering);
	if (result == V34_PHASE2_SELECTION_NO_COMMON_PROFILE)
		return begin_retrain(call, false);
	if (result != V34_PHASE2_SELECTION_OK ||
	    !select_local_preemphasis(call, &selection))
		return V34_CALL_SEQUENCE_MISMATCH;
	if (v34_phase2_answerer_reply(
		    &call->state.phase2, &selection.answering) != V34_PHASE2_OK)
		return V34_CALL_SEQUENCE_MISMATCH;
	/* Keep a conservative fallback for the rest of this call so another
	 * retrain cannot immediately return to the withdrawn profile. */
	call->local_info1 = selection;
	return V34_CALL_OK;
}

static bool select_local_preemphasis(
	struct v34_call *call, union v34_local_info1 *selection)
{
	struct v34_phase2_probe_receiver_measurement measurement;
	union v34_local_info1 selected = *selection;

	if (v34_phase2_measure_remote_probe(
		    &call->state.phase2, &measurement) != V34_PHASE2_OK)
		return false;
	if (call->local_role == V34_CALLING_STATION) {
		for (unsigned int index = 0; index < V34_SYMBOL_RATE_COUNT;
		     ++index) {
			struct v34_info1_probe_result *probe =
				&selected.calling.answer_transmit[index];

			if (probe->projected_data_rate_2400_bps_units == 0)
				continue;
			if (v34_phase2_select_preemphasis(
				    &measurement, (enum v34_symbol_rate)index,
				    &probe->preemphasis_filter_index) !=
			    V34_PHASE2_SELECTION_OK)
				return false;
		}
	} else {
		struct v34_info1_probe_result *probe =
			&selected.answering.call_transmit;

		if (probe->projected_data_rate_2400_bps_units != 0 &&
		    v34_phase2_select_preemphasis(
			    &measurement,
			    selected.answering.call_to_answer_symbol_rate,
			    &probe->preemphasis_filter_index) !=
			    V34_PHASE2_SELECTION_OK)
			return false;
	}
	*selection = selected;
	return true;
}

static enum v34_call_result advance_phase2(
	struct v34_call *call, const struct v34_phase2_event *event)
{
	enum v34_phase2_result result;

	if (event->kind == V34_PHASE2_EVENT_FAILED)
		return V34_CALL_FAILED;
	if (v34_phase2_is_ready_for_info1(&call->state.phase2)) {
		if (call->local_role == V34_CALLING_STATION &&
		    !select_local_preemphasis(call, &call->local_info1))
			return V34_CALL_NUMERIC_FAILURE;
		result = call->local_role == V34_CALLING_STATION ?
			v34_phase2_start_caller_info1(
				&call->state.phase2,
				&call->local_info1.calling) :
			v34_phase2_start_answerer_info1(&call->state.phase2);
		if (result != V34_PHASE2_OK)
			return V34_CALL_SEQUENCE_MISMATCH;
	}
	if (call->local_role == V34_ANSWERING_STATION &&
	    event->kind == V34_PHASE2_EVENT_PEER_INFO1C_RECEIVED)
		return answer_info1c(call);
	return v34_phase2_is_ready_for_phase3(&call->state.phase2) ?
		       enter_phase3(call) : V34_CALL_OK;
}

static enum v34_call_result sample_phase2(
	struct v34_call *call, int16_t received_sample,
	int16_t *transmit_sample, struct v34_call_observation *observation)
{
	enum v34_phase2_result result = v34_phase2_sample(
		&call->state.phase2, received_sample, transmit_sample,
		&observation->events.phase2);

	if (result != V34_PHASE2_OK)
		return V34_CALL_NUMERIC_FAILURE;
	if (observation->events.phase2.kind ==
	    V34_PHASE2_EVENT_INITIAL_PROBE_STARTED) {
		call->round_trip_delay_samples =
			observation->events.phase2.round_trip_delay_samples;
		call->round_trip_delay_stored = true;
	}
	return advance_phase2(call, &observation->events.phase2);
}

static enum v34_call_result enter_phase4(struct v34_call *call)
{
	struct v34_phase3_exchange_handoff handoff;
	enum v34_phase4_exchange_result result;

	if (v34_phase3_exchange_handoff(
		    &call->state.phase3, &handoff) != V34_PHASE3_EXCHANGE_OK)
		return V34_CALL_NUMERIC_FAILURE;
	result = v34_phase4_exchange_init(
		&call->state.phase4, &handoff,
		&call->phase4_configuration,
		call->round_trip_delay_samples);
	if (result != V34_PHASE4_EXCHANGE_OK)
		return phase4_result(result);
	call->stage = V34_CALL_PHASE4;
	return V34_CALL_OK;
}

static enum v34_call_result sample_phase3(
	struct v34_call *call, int16_t received_sample,
	int16_t *transmit_sample, struct v34_call_observation *observation)
{
	enum v34_phase3_exchange_result result =
		v34_phase3_exchange_sample(
			&call->state.phase3, received_sample, transmit_sample,
			&observation->events.phase3);
	enum v34_call_result translated = phase3_result(result);

	if (translated != V34_CALL_OK)
		return translated;
	return v34_phase3_exchange_is_handoff_ready(&call->state.phase3) ?
		       enter_phase4(call) : V34_CALL_OK;
}

static enum v34_call_result sample_phase4(
	struct v34_call *call, int16_t received_sample,
	v34_mapping_frame_source source, void *source_context,
	int16_t *transmit_sample, struct v34_call_observation *observation,
	struct v34_received_mapping_frame *received)
{
	enum v34_phase4_exchange_result result =
		v34_phase4_exchange_sample(
			&call->state.phase4, received_sample, source,
			source_context, transmit_sample,
			&observation->events.phase4, received);
	enum v34_call_result translated = phase4_result(result);

	if (translated != V34_CALL_OK)
		return translated;
	if (!v34_phase4_exchange_needs_data_preparation(
		    &call->state.phase4))
		return V34_CALL_OK;
	result = v34_phase4_exchange_prepare_data_startup(
		&call->state.phase4);
	return phase4_result(result);
}

static bool phase4_receive_has_failed(
	const struct v34_phase4_exchange *exchange)
{
	return v34_phase34_receiver_has_failed(&exchange->receive);
}

static enum v34_call_result sample_retrain_silence(
	struct v34_call *call, int16_t received_sample,
	int16_t *transmit_sample)
{
	struct v34_phase2 restarted;
	struct v34_full_retrain next_retrain = call->retrain;
	enum v34_full_retrain_response_event event;

	*transmit_sample = 0;
	if (v34_full_retrain_sample_response(
		    &next_retrain, received_sample, &event) !=
	    V34_PHASE2_TONE_OK)
		return V34_CALL_NUMERIC_FAILURE;
	if (event == V34_FULL_RETRAIN_RESPONSE_CONTINUES) {
		call->retrain = next_retrain;
		return V34_CALL_OK;
	}
	if (v34_phase2_init_retrain(
		    &restarted, call->local_role,
		    call->phase2_nominal_amplitude, &call->local_info0,
		    &call->peer_info0, call->round_trip_delay_samples) !=
		    V34_PHASE2_OK)
		return V34_CALL_NUMERIC_FAILURE;
	if (event ==
	    V34_FULL_RETRAIN_RESPONSE_COMPLETES_WITH_PEER_TONE) {
		restarted.state.ranging.info0_exchange.transport.tone_detector =
			next_retrain.detector.tone;
		if (!v34_phase2_is_valid(&restarted))
			return V34_CALL_NUMERIC_FAILURE;
	}
	call->retrain = next_retrain;
	call->state.phase2 = restarted;
	call->stage = V34_CALL_PHASE2;
	return V34_CALL_OK;
}

static bool retrain_detection_is_active(const struct v34_call *call)
{
	/* A peer can repeat its request after one side has advanced into Phase 3
	 * while the other is still restarting Phase 2.  Treat that as the same
	 * full-retrain transaction; ordinary initial Phase 3 remains unarmed. */
	if (call->stage == V34_CALL_PHASE3)
		return call->retrain_active;
	/* A peer cannot request an online retrain while it is still sending its
	 * Phase 4 E or B1.  Arm only after both local directions are online;
	 * earlier Phase 4 waveforms can otherwise qualify as a retrain tone. */
	if (call->stage == V34_CALL_PHASE4)
		return !call->retrain_active &&
		       call->state.phase4.online_announced;
	return call->stage == V34_CALL_PHASE2 &&
	       call->state.phase2.stage == V34_PHASE2_INFO1 &&
	       phase2_info1_is_waiting_for_peer(
		       &call->state.phase2.state.info1.control);
}

static enum v34_call_result observe_retrain_tone(
	struct v34_call *call, int16_t received_sample)
{
	bool detected;

	if (v34_retrain_detector_sample(
		    &call->retrain.detector, received_sample,
		    &detected) != V34_PHASE2_TONE_OK)
		return V34_CALL_NUMERIC_FAILURE;
	return detected ? begin_retrain(call, true) : V34_CALL_OK;
}

static enum v34_call_result reset_retrain_detector(struct v34_call *call)
{
	if (v34_retrain_detector_reset(
		    &call->retrain.detector) != V34_PHASE2_TONE_OK)
		return V34_CALL_NUMERIC_FAILURE;
	return V34_CALL_OK;
}

enum v34_call_result v34_call_sample(
	struct v34_call *call, int16_t received_sample,
	v34_mapping_frame_source source, void *source_context,
	int16_t *transmit_sample, struct v34_call_observation *observation,
	struct v34_received_mapping_frame *received)
{
	struct v34_call_observation emitted = { 0 };
	struct v34_received_mapping_frame decoded;
	enum v34_call_result result;
	int16_t conditioned = received_sample;
	int16_t generated = 0;
	bool echo_processed;
	bool retrain_requested = false;

	if (!v34_call_is_valid(call) || transmit_sample == NULL ||
	    observation == NULL || received == NULL ||
	    outputs_overlap(call, transmit_sample, observation, received))
		return V34_CALL_INVALID_ARGUMENT;
	if (call->failed)
		return V34_CALL_FAILED;
	emitted.processed = call->stage;
	echo_processed = (emitted.processed == V34_CALL_PHASE3 ||
			  emitted.processed == V34_CALL_PHASE4 ||
			  emitted.processed == V34_CALL_CLEARDOWN) &&
			 (call->echo.adapting || call->echo.filtering);
	if (echo_processed &&
	    v34_echo_canceller_predict(
		    &call->echo, received_sample, &conditioned) !=
		    V34_ECHO_CANCELLER_OK) {
		call->failed = true;
		return V34_CALL_NUMERIC_FAILURE;
	}
	switch (call->stage) {
	case V34_CALL_PHASE2:
		result = sample_phase2(
			call, received_sample, &generated, &emitted);
		break;
	case V34_CALL_PHASE3:
		result = sample_phase3(
			call, conditioned, &generated, &emitted);
		break;
	case V34_CALL_PHASE4:
		result = sample_phase4(
			call, conditioned, source, source_context,
			&generated, &emitted, &decoded);
		if (result == V34_CALL_SOURCE_FAILURE)
			return result;
		if (result != V34_CALL_OK &&
		    phase4_receive_has_failed(&call->state.phase4))
			retrain_requested = true;
		break;
	case V34_CALL_RETRAIN_SILENCE:
		result = sample_retrain_silence(
			call, received_sample, &generated);
		break;
	case V34_CALL_CLEARDOWN:
		{
			enum v34_cleardown_result cleared = v34_cleardown_sample(
				&call->state.cleardown, conditioned, &generated);

			result = cleared == V34_CLEARDOWN_OK ? V34_CALL_OK :
				cleared == V34_CLEARDOWN_COMPLETE ?
					V34_CALL_CLEARDOWN_COMPLETE :
					V34_CALL_NUMERIC_FAILURE;
		}
		break;
	default:
		return V34_CALL_INVALID_STATE;
	}
	if (emitted.processed == V34_CALL_PHASE3 &&
	    call->local_role == V34_CALLING_STATION &&
	    (emitted.events.phase3 &
	     V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_STARTED) != 0) {
		if (v34_echo_canceller_init(&call->echo) !=
		    V34_ECHO_CANCELLER_OK)
			result = V34_CALL_NUMERIC_FAILURE;
		else
			echo_processed = true;
	}
	if (echo_processed &&
	    v34_echo_canceller_advance(
		    &call->echo, received_sample, conditioned, generated) !=
		    V34_ECHO_CANCELLER_OK)
		result = V34_CALL_NUMERIC_FAILURE;
	else if (v34_echo_canceller_is_adapting(&call->echo) &&
		 !echo_adaptation_is_expected(call) &&
		 v34_echo_canceller_freeze(&call->echo) !=
			 V34_ECHO_CANCELLER_OK)
		result = V34_CALL_NUMERIC_FAILURE;
	else if (retrain_requested)
		result = begin_retrain(call, false);
	if (result == V34_CALL_OK && call->stage == V34_CALL_PHASE4 &&
	    call->state.phase4.online_announced)
		call->retrain_active = false;
	if (result == V34_CALL_OK && emitted.processed != call->stage &&
		 call->stage != V34_CALL_RETRAIN_SILENCE)
		result = reset_retrain_detector(call);
	else if (result == V34_CALL_OK && emitted.processed == call->stage &&
		 retrain_detection_is_active(call))
		result = observe_retrain_tone(call, conditioned);
	if (result != V34_CALL_OK && result != V34_CALL_CLEARDOWN_COMPLETE)
		call->failed = true;
	emitted.next = call->stage;
	if (!v34_call_is_valid(call)) {
		call->failed = true;
		return V34_CALL_NUMERIC_FAILURE;
	}
	*transmit_sample = generated;
	*observation = emitted;
	if (emitted.processed == V34_CALL_PHASE4 &&
	    (emitted.events.phase4 &
	     V34_PHASE4_EXCHANGE_EVENT_MAPPING_FRAME_RECEIVED) != 0)
		*received = decoded;
	return result;
}
