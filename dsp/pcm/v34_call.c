// SPDX-License-Identifier: BSD-3-Clause

#include "v34_call.h"

#include "memory_region.h"
#include "v34_frame_schedule.h"

#include <stddef.h>
#include <string.h>

/* 11.3.2.2.1 and 11.4.2.1.1 specify 600 ms plus RTT. Allow 100 ms of
 * compatibility grace for late S/S-bar. The countdown's final 8 ms are
 * reserved for a marker already complete at that deadline: the caller
 * needs 16T of peer TRN to reject local echo (6.67 ms at 2400 baud). */
static const uint32_t PEER_S_CONFIRM_SAMPLES = 8u * 8u;
static const uint32_t PEER_S_WAIT_SAMPLES = (600u + 100u + 8u) * 8u;

static enum v34_call_result phase3_result(
	enum v34_phase3_exchange_result result);

static bool initialize_echo(struct pcm_host_echo *echo)
{
	/* Preserve near-return coverage. The shared scan independently locates
	 * delayed returns; peer ranging only determines the training deadline. */
	return pcm_host_echo_init(echo) == PCM_HOST_ECHO_OK &&
	       pcm_host_echo_seed_window_delay(echo, 1u) == PCM_HOST_ECHO_OK;
}

static enum v34_phase3_exchange_result initialize_phase3(
	struct v34_call *call, const struct v34_phase3_profile *local,
	const struct v34_phase3_profile *peer, int16_t gain)
{
	struct v34_phase3_exchange_configuration config =
		call->phase3_configuration;

	config.extend_training = true;
	config.round_trip_delay_samples = call->round_trip_delay_samples;
	return v34_phase3_exchange_init(
		&call->state.phase3, local, peer, &config, gain);
}

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
	    !pcm_host_echo_is_valid(&call->echo) ||
	    call->echo_quiet_samples > PCM_ECHO_REFERENCE_HISTORY_SAMPLES ||
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

static bool echo_is_training(const struct v34_call *call)
{
	if (call->stage != V34_CALL_PHASE3 ||
	    call->state.phase3.transmit.stage !=
		    V34_TRAINING_TRANSMITTER_PHASE3 ||
	    call->state.phase3.transmit.phase3.stage ==
		    V34_PHASE3_TRANSMITTER_J ||
	    (call->state.phase3.transmit.phase3.stage ==
		     V34_PHASE3_TRANSMITTER_TRN &&
	     call->state.phase3.transmit.phase3.trn_symbols ==
		     call->state.phase3.trn_target_symbols))
		return false;
	return call->state.phase3.transmit_started;
}

static bool echo_training_can_finish(const struct v34_call *call)
{
	const struct v34_training_transmitter *transmit;
	const struct v34_phase3_profile *profile;
	uint64_t minimum_samples;
	uint32_t return_guard;

	if (!echo_is_training(call))
		return false;
	transmit = &call->state.phase3.transmit;
	if (transmit->phase3.stage != V34_PHASE3_TRANSMITTER_TRN ||
	    transmit->phase3.trn_symbols < V34_TRAINING_SEQUENCE_MIN_SYMBOLS)
		return false;
	if (pcm_host_echo_is_ready(&call->echo))
		return !pcm_host_echo_has_weak_margin(&call->echo);
	profile = &transmit->line.profile;
	minimum_samples = ((uint64_t)V34_TRAINING_SEQUENCE_MIN_SYMBOLS *
		V34_PHASE2_SAMPLE_RATE * profile->symbol_rate_denominator +
		profile->symbol_rate_numerator - 1u) /
		profile->symbol_rate_numerator;
	/* A quiet prefix can precede a delayed return. Retain the shared
	 * 700 ms acquisition horizon, or a longer measured RTD plus 50 ms,
	 * before accepting a complete 512T interval of negligible input. */
	return_guard = call->round_trip_delay_samples +
		V34_PHASE2_SAMPLE_RATE / 20u;
	if (return_guard < PCM_ECHO_REFERENCE_REQUIRED_DELAY_SAMPLES)
		return_guard = PCM_ECHO_REFERENCE_REQUIRED_DELAY_SAMPLES;
	return call->echo_quiet_samples >= minimum_samples &&
	       transmit->line.generated_samples >= return_guard + minimum_samples;
}

static bool constrain_caller_info1(struct v34_call *call)
{
	struct v34_info0 peer;
	struct v34_info1c selected = {
		.common = call->local_info1.calling.common,
	};

	if (v34_phase2_peer_info0(&call->state.phase2, &peer) != V34_PHASE2_OK)
		return false;
	if (!peer.transmit_power_reduction_supported) {
		selected.common.minimum_peer_power_reduction_db = 0;
		selected.common.additional_peer_power_reduction_db = 0;
	}
	for (unsigned int index = 0; index < V34_SYMBOL_RATE_COUNT; ++index) {
		struct v34_info1_probe_result *probe =
			&selected.answer_transmit[index];

		*probe = call->local_info1.calling.answer_transmit[index];
		if (!peer.constellation_1664_supported &&
		    probe->projected_data_rate_2400_bps_units > 12)
			probe->projected_data_rate_2400_bps_units = 12;
		/* Retain the configured carrier where possible, then try the
		 * alternate. Unsupported baud rates remain withdrawn from INFO1c. */
		if (!v34_phase2_info1c_is_consistent(
			    &call->local_info0, &peer, &selected)) {
			probe->high_carrier = !probe->high_carrier;
			if (!v34_phase2_info1c_is_consistent(
				    &call->local_info0, &peer, &selected))
				*probe = (struct v34_info1_probe_result){ 0 };
		}
	}
	call->local_info1.calling = selected;
	return true;
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
		limit = v34_frame_geometry_maximum_rate(
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
		child_failed = call->state.phase3.failed;
		child_role = call->state.phase3.local_role;
		break;
	case V34_CALL_PHASE4:
		child_valid = v34_phase4_exchange_is_valid(&call->state.phase4);
		child_failed = call->state.phase4.failed;
		child_role = v34_station_peer_role(
			call->state.phase4.receive.transmitting_profile.local_role);
		break;
	case V34_CALL_RETRAIN_SILENCE:
		child_valid = call->peer_info0_stored && call->retrain_active &&
			      call->retrain.response_silence_samples_remaining !=
				      0u;
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
	       call->peer_s_remaining_samples <=
		       PEER_S_WAIT_SAMPLES + call->round_trip_delay_samples &&
	       (call->peer_s_remaining_samples == 0u ||
		(call->stage == V34_CALL_PHASE3 ||
		 (call->local_role == V34_CALLING_STATION &&
		  call->stage == V34_CALL_PHASE4))) &&
	       call->phase4_e_remaining_samples <= 30u * 8000u &&
	       (call->phase4_e_remaining_samples == 0u ||
		(call->stage == V34_CALL_PHASE4 &&
		 !call->state.phase4.receive_data_active)) &&
	       (call->retrain.response_silence_samples_remaining != 0u) ==
		       (call->stage == V34_CALL_RETRAIN_SILENCE) &&
	       (call->round_trip_delay_stored ||
		call->stage == V34_CALL_RETRAIN_SILENCE ||
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
	    !initialize_echo(&initialized.echo))
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
	result = initialize_phase3(
		&initialized, local, peer, handoff->receive_gain);
	if (result != V34_PHASE3_EXCHANGE_OK)
		return phase3_result(result);
	if (v34_full_retrain_init(
		    &initialized.retrain, initialized.local_role) !=
		    V34_PHASE2_TONE_OK ||
	    !initialize_echo(&initialized.echo))
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
	/* A forced initial cadence remains exact. On full retrain, let the peer
	 * select a compatible slower probe result instead of
	 * rejecting an otherwise valid INFO1a fallback. */
	if (next.local_role == V34_CALLING_STATION &&
	    !add_caller_retrain_fallbacks(&next))
		return V34_CALL_INVALID_STATE;
	if (v34_full_retrain_begin_response(
		    &next.retrain, retain_peer_tone) != V34_PHASE2_TONE_OK ||
	    !initialize_echo(&next.echo))
		return V34_CALL_NUMERIC_FAILURE;
	next.retrain_active = true;
	next.echo_quiet_samples = 0;
	next.peer_s_remaining_samples = 0u;
	next.phase4_e_remaining_samples = 0u;
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
	case V34_PHASE4_EXCHANGE_PEER_CLEARDOWN:
		return V34_CALL_PEER_CLEARDOWN;
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
	result = initialize_phase3(call, &local, &peer, receive_gain);
	if (result != V34_PHASE3_EXCHANGE_OK)
		return phase3_result(result);
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

	if (event->kind == V34_PHASE2_EVENT_FAILED) {
		/* A missing L1 can leave the stations in different ranging epochs.
		 * Reuse the PCM call's full-retrain recovery; retain the acquisition
		 * failure and let the host's whole-call deadline bound retries. */
		if (event->failure == V34_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND) {
			/* A missing initial L1 leaves the first ranging estimate
			 * unconfirmed, just as it does in the PCM call owner. */
			if (call->state.phase2.stage == V34_PHASE2_INITIAL_PROBE) {
				call->round_trip_delay_samples = 0u;
				call->round_trip_delay_stored = false;
			}
			return begin_retrain(call, false);
		}
		/* V.34 11.2.2.1.5: missing final Tone A requires full retrain.
		 * Keep the completed INFO0/ranging agreement and common recovery path. */
		if (call->local_role == V34_CALLING_STATION &&
		    event->failure == V34_PHASE2_FAILURE_LOCAL_PROBE_LIMIT)
			return begin_retrain(call, false);
		return V34_CALL_FAILED;
	}
	/* V.34 11.2.2.2.4: an answerer missing INFO1c must initiate
	 * full retrain or send INFOMARKSa, not retain Tone A indefinitely. */
	if (event->kind == V34_PHASE2_EVENT_INFO1_RECOVERY_STARTED) {
		if (call->local_role == V34_ANSWERING_STATION)
			return begin_retrain(call, false);
		if (v34_retrain_detector_reset(&call->retrain.detector) !=
		    V34_PHASE2_TONE_OK)
			return V34_CALL_NUMERIC_FAILURE;
	}
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
		/* The configured caller INFO1 template precedes peer capability
		 * discovery. Bound it before INFO1 or a missing-probe retrain. */
		if (call->local_role == V34_CALLING_STATION &&
		    !constrain_caller_info1(call))
			return V34_CALL_NUMERIC_FAILURE;
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
	if (!call->state.phase4.data_preparation_active) {
		result = v34_phase4_exchange_begin_data_preparation(
			&call->state.phase4, &call->data_preparation);
		if (result != V34_PHASE4_EXCHANGE_OK)
			return phase4_result(result);
	}
	/* One generated frame per real line sample bounds preparation work in
	 * every host fragment, independently of its packet size. Keep transmitting
	 * ordinary MP until both directions and the clipping preflight are ready. */
	struct v34_data_startup startup;
	enum v34_data_startup_result prepared = v34_data_startup_step_preparation(
		&call->data_preparation, 1, &startup);

	if (prepared == V34_DATA_STARTUP_IN_PROGRESS)
		return V34_CALL_OK;
	if (prepared != V34_DATA_STARTUP_OK)
		return prepared == V34_DATA_STARTUP_OUT_OF_RANGE ? V34_CALL_OUT_OF_RANGE :
			V34_CALL_NUMERIC_FAILURE;
	result = v34_phase4_exchange_complete_data_preparation(&call->state.phase4, &startup);
	return phase4_result(result);
}

/* Bound each station's wait for S/S-bar from its first transmitted J:
 * Phase 3 for the answerer (11.3.2.2.1), Phase 4 for the caller (11.4.2.1.1).
 * Use the full-retrain alternative permitted by 11.3.2.2 and 11.4.2.1,
 * preserving INFO0 and the measured delay. The later E deadline cannot
 * cover a missing S marker. */
static bool peer_s_has_timed_out(
	struct v34_call *call, const struct v34_call_observation *observation)
{
	if (call->local_role == V34_ANSWERING_STATION &&
	    observation->processed == V34_CALL_PHASE3 &&
	    (observation->events.phase3 &
	     V34_PHASE3_EXCHANGE_EVENT_PEER_S_DETECTED) != 0) {
		call->peer_s_remaining_samples = 0u;
		return false;
	}
	if (observation->processed == V34_CALL_PHASE3 &&
	    (observation->events.phase3 &
	     V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_J_STARTED) != 0) {
		call->peer_s_remaining_samples =
			PEER_S_WAIT_SAMPLES + call->round_trip_delay_samples;
	} else if (observation->processed == V34_CALL_PHASE4 &&
		   (observation->events.phase4 &
		    V34_PHASE4_EXCHANGE_EVENT_PEER_S_DETECTED) != 0) {
		call->peer_s_remaining_samples = 0u;
	} else if (call->peer_s_remaining_samples != 0u) {
		--call->peer_s_remaining_samples;
		if (call->peer_s_remaining_samples == 0u)
			return true;
		/* Without a complete marker, recover at the original deadline.
		 * This tail is consumed once; another candidate cannot renew it. */
		if (call->peer_s_remaining_samples == PEER_S_CONFIRM_SAMPLES) {
			const struct v34_phase4_decision_receiver *receiver =
				&call->state.phase4.receive.semantic.phase4;

			return call->local_role != V34_CALLING_STATION ||
			       call->stage != V34_CALL_PHASE4 ||
			       receiver->stage != V34_PHASE4_DECISION_RECEIVER_ANSWER_S ||
			       !v34_phase3_s_hunt_is_complete(&receiver->answer_s);
		}
	}
	return false;
}

/* V.34 11.4.2: start after J-prime or S-bar has finished. Initial
 * callers allow two round trips; answerers allow three. The peer's INFO0
 * CME bit selects the extended deadline. Online responders retain 11.6.2.2. */
static bool phase4_e_has_timed_out(struct v34_call *call, uint32_t events)
{
	const struct v34_phase4_exchange *exchange = &call->state.phase4;
	uint32_t start_event = exchange->rate_renegotiation ?
		V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_S_BAR_STARTED :
		V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_TRN_STARTED;

	if (exchange->receive_data_active) {
		call->phase4_e_remaining_samples = 0u;
	} else if ((events & start_event) != 0) {
		bool cme = exchange->rate_renegotiation ?
			(call->local_info0.cme_modem || call->peer_info0.cme_modem) :
			call->peer_info0.cme_modem;
		uint32_t round_trips = exchange->rate_renegotiation ||
			call->local_role == V34_ANSWERING_STATION ? 3u : 2u;

		call->phase4_e_remaining_samples = cme ?
			30u * V34_PHASE2_SAMPLE_RATE :
			V34_PHASE2_SAMPLE_RATE * 5u / 2u +
				round_trips * call->round_trip_delay_samples;
	} else if (call->phase4_e_remaining_samples != 0u &&
		   --call->phase4_e_remaining_samples == 0u) {
		return true;
	}
	return false;
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
	 * earlier Phase 4 waveforms can otherwise qualify as a retrain tone.
	 * An online rate renegotiation retains the full-retrain recovery path. */
	if (call->stage == V34_CALL_PHASE4)
		return !call->retrain_active &&
		       (call->state.phase4.online_announced ||
			call->state.phase4.rate_renegotiation);
	return call->stage == V34_CALL_PHASE2 &&
	       call->state.phase2.stage == V34_PHASE2_INFO1 &&
	       phase2_info1_is_waiting_for_peer(
		       &call->state.phase2.state.info1.control);
}

static enum v34_call_result observe_retrain_tone(
	struct v34_call *call, int16_t received_sample)
{
	bool detected;

	if (call->stage == V34_CALL_PHASE2 &&
	    call->local_role == V34_CALLING_STATION &&
	    call->state.phase2.stage == V34_PHASE2_INFO1 &&
	    call->state.phase2.state.info1.control.phase ==
		    PHASE2_INFO1_RECOVERING) {
		/* A synchronized INFO frame owns its carrier, including zero-bit
		 * runs that resemble Tone A. Qualify tone only outside that frame. */
		if (call->state.phase2.state.info1.control.receiver.frame.stage !=
		    V34_PHASE2_INFO_SEARCH_SYNC)
			return v34_retrain_detector_reset(&call->retrain.detector) ==
				       V34_PHASE2_TONE_OK ?
				       V34_CALL_OK : V34_CALL_NUMERIC_FAILURE;
		/* V.34 11.2.2.1.6 conditions Tone A reception after the INFO1
		 * deadline, including a peer still holding its earlier Tone A.
		 * Ordinary online retrains retain their silence preamble. */
		if (v34_retrain_detector_sample_tone(
			    &call->retrain.detector, received_sample, &detected) !=
		    V34_PHASE2_TONE_OK)
			return V34_CALL_NUMERIC_FAILURE;
	} else if (v34_retrain_detector_sample(
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

static const struct v34_phase34_receiver *data_receiver(const struct v34_call *call)
{
	if (call->stage != V34_CALL_PHASE4 ||
	    call->state.phase4.receive.stage != V34_PHASE34_RECEIVER_DATA ||
	    call->state.phase4.receive.data_erasure)
		return NULL;
	return &call->state.phase4.receive;
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
	enum pcm_host_echo_mode echo_mode;
	bool echo_advanced;
	bool retrain_requested = false;
	struct v34_line_receiver before;
	bool observe_echo;

	if (!v34_call_is_valid(call) || transmit_sample == NULL ||
	    observation == NULL || received == NULL ||
	    outputs_overlap(call, transmit_sample, observation, received))
		return V34_CALL_INVALID_ARGUMENT;
	if (call->failed)
		return V34_CALL_FAILED;
	if (echo_training_can_finish(call) &&
	    v34_phase3_exchange_finish_training(&call->state.phase3) !=
		    V34_PHASE3_EXCHANGE_OK) {
		call->failed = true;
		return V34_CALL_NUMERIC_FAILURE;
	}
	emitted.processed = call->stage;
	/* Every consumed host sample retains its actual transmit reference,
	 * including Phase 2 and silence. TRAIN preserves raw receive input;
	 * Each duplex phase monitors the frozen model. */
	echo_mode = echo_is_training(call) ? PCM_HOST_ECHO_TRAIN :
		(call->stage == V34_CALL_PHASE2 ||
		 call->stage == V34_CALL_RETRAIN_SILENCE) ?
			PCM_HOST_ECHO_BYPASS : PCM_HOST_ECHO_FILTER_DATA;
	if (pcm_host_echo_predict(
		    &call->echo, echo_mode, received_sample, &conditioned) !=
	    PCM_HOST_ECHO_OK) {
		call->failed = true;
		return V34_CALL_NUMERIC_FAILURE;
	}
	const struct v34_phase34_receiver *data = data_receiver(call);
	observe_echo = data != NULL;
	if (observe_echo)
		before = data->line;
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
		/* Finish both B1 directions before recovery so the peer can observe
		 * the complete retrain silence. Gross errors in this known all-one
		 * frame must not become an online data connection. */
		if (result == V34_CALL_OK &&
		    (emitted.events.phase4 & V34_PHASE4_EXCHANGE_EVENT_ONLINE) != 0) {
			const struct v34_data_receiver *b1 = &call->state.phase4.receive
				.semantic.data.data_receiver.data_receiver;

			if (!v34_data_receiver_b1_is_qualified(b1))
				retrain_requested = true;
		}
		if (result == V34_CALL_OK &&
		    phase4_e_has_timed_out(call, emitted.events.phase4))
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
	if (result == V34_CALL_OK && peer_s_has_timed_out(call, &emitted))
		retrain_requested = true;
	data = data_receiver(call);
	if (result == V34_CALL_OK && observe_echo && data != NULL) {
		v34_echo_timing_prepare(&call->echo_timing, &call->echo, echo_mode,
			&before, &data->semantic.data, received_sample, conditioned);
		v34_echo_timing_observe(&call->echo_timing, &call->echo, &data->line);
	} else {
		v34_echo_timing_reset(&call->echo_timing);
	}
	echo_advanced = pcm_host_echo_advance_predicted(
		&call->echo, echo_mode, received_sample, conditioned,
		generated) == PCM_HOST_ECHO_OK;
	if (!echo_advanced)
		result = V34_CALL_NUMERIC_FAILURE;
	if (echo_mode == PCM_HOST_ECHO_TRAIN &&
	    received_sample > -PCM_HOST_ECHO_READY_MIN_PEAK &&
	    received_sample < PCM_HOST_ECHO_READY_MIN_PEAK) {
		if (call->echo_quiet_samples < PCM_ECHO_REFERENCE_HISTORY_SAMPLES)
			call->echo_quiet_samples++;
	} else {
		call->echo_quiet_samples = 0;
	}
	if (echo_advanced && retrain_requested)
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
	else if (result == V34_CALL_OK && call->stage == V34_CALL_PHASE4 &&
		 call->state.phase4.transmit_b1_complete &&
		 v34_retrain_detector_observe_silence(&call->retrain.detector,
			 conditioned) != V34_PHASE2_TONE_OK)
		result = V34_CALL_NUMERIC_FAILURE;
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
