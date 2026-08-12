// SPDX-License-Identifier: BSD-3-Clause

#include "v34_call.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static bool role_is_valid(enum v34_station_role role)
{
	return role == V34_CALLING_STATION ||
	       role == V34_ANSWERING_STATION;
}

static const struct v34_mp_parameters *mp_common(
	const struct v34_mp_record *record)
{
	return record->type == V34_MP_RECORD_TYPE0 ?
		       &record->parameters.type0 :
		       &record->parameters.type1.common;
}

static bool configuration_is_valid(
	const struct v34_call_configuration *configuration)
{
	const struct v34_phase3_line_receiver_configuration *phase3;
	const struct v34_training_receiver_configuration *phase4;

	if (configuration == NULL ||
	    !role_is_valid(configuration->local_role) ||
	    !v34_info0_is_valid(&configuration->local_info0) ||
	    configuration->phase2_nominal_amplitude <= 0 ||
	    configuration->phase3.sample_rate != V34_PHASE2_SAMPLE_RATE ||
	    configuration->phase3.nominal_peak_amplitude <= 0 ||
	    !v34_constellation_size_is_valid(
		    configuration->phase3.requested_remote_constellation) ||
	    !v34_phase3_line_receiver_configuration_is_valid(
		    &configuration->phase3.receive) ||
	    !v34_training_receiver_configuration_is_valid(
		    &configuration->phase4.receive) ||
	    !v34_mp_record_is_valid(&configuration->phase4.transmit_mp) ||
	    mp_common(&configuration->phase4.transmit_mp)
		    ->acknowledges_peer_mp ||
	    !isfinite(configuration->phase4.data_equalizer_adaptation) ||
	    configuration->phase4.data_equalizer_adaptation < 0.0 ||
	    configuration->phase4.data_equalizer_adaptation > 1.0)
		return false;
	phase3 = &configuration->phase3.receive;
	phase4 = &configuration->phase4.receive;
	if (phase3->s_gain_adaptation != phase4->phase3.s_gain_adaptation ||
	    phase3->pp_equalizer_adaptation !=
		    phase4->phase3.pp_equalizer_adaptation ||
	    phase3->tracking_equalizer_adaptation !=
		    phase4->phase3.tracking_equalizer_adaptation)
		return false;
	return configuration->local_role == V34_CALLING_STATION ?
		       v34_info1c_is_valid(
			       &configuration->local_info1.calling) :
		       v34_info1a_is_valid(
			       &configuration->local_info1.answering);
}

static bool stored_configuration_is_valid(const struct v34_call *call)
{
	const struct v34_phase3_line_receiver_configuration *phase3 =
		&call->phase3_configuration.receive;
	const struct v34_training_receiver_configuration *phase4 =
		&call->phase4_configuration.receive;

	if (!role_is_valid(call->local_role) ||
	    call->phase3_configuration.sample_rate !=
		    V34_PHASE2_SAMPLE_RATE ||
	    call->phase3_configuration.nominal_peak_amplitude <= 0 ||
	    !v34_constellation_size_is_valid(
		    call->phase3_configuration.requested_remote_constellation) ||
	    !v34_phase3_line_receiver_configuration_is_valid(phase3) ||
	    !v34_training_receiver_configuration_is_valid(phase4) ||
	    !v34_mp_record_is_valid(
		    &call->phase4_configuration.transmit_mp) ||
	    mp_common(&call->phase4_configuration.transmit_mp)
		    ->acknowledges_peer_mp ||
	    !isfinite(call->phase4_configuration.data_equalizer_adaptation) ||
	    call->phase4_configuration.data_equalizer_adaptation < 0.0 ||
	    call->phase4_configuration.data_equalizer_adaptation > 1.0 ||
	    phase3->s_gain_adaptation != phase4->phase3.s_gain_adaptation ||
	    phase3->pp_equalizer_adaptation !=
		    phase4->phase3.pp_equalizer_adaptation ||
	    phase3->tracking_equalizer_adaptation !=
		    phase4->phase3.tracking_equalizer_adaptation)
		return false;
	return call->local_role == V34_CALLING_STATION ?
		       v34_info1c_is_valid(&call->local_info1.calling) :
		       v34_info1a_is_valid(&call->local_info1.answering);
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
		child_role = call->state.phase4.local_role;
		break;
	default:
		return false;
	}
	return child_valid && child_role == call->local_role &&
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
	initialized.phase3_configuration = configuration->phase3;
	initialized.phase4_configuration = configuration->phase4;
	initialized.local_role = configuration->local_role;
	initialized.stage = V34_CALL_PHASE2;
	if (!v34_call_is_valid(&initialized))
		return V34_CALL_NUMERIC_FAILURE;
	*call = initialized;
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

enum v34_call_stage v34_call_current_stage(const struct v34_call *call)
{
	return v34_call_is_valid(call) ? call->stage :
				       (enum v34_call_stage)-1;
}

static bool regions_overlap(
	const void *left, size_t left_size,
	const void *right, size_t right_size)
{
	uintptr_t left_address = (uintptr_t)left;
	uintptr_t right_address = (uintptr_t)right;

	return left_address <= right_address ?
		       right_address - left_address < left_size :
		       left_address - right_address < right_size;
}

static bool outputs_overlap(
	const struct v34_call *call, const int16_t *sample,
	const struct v34_call_observation *observation,
	const struct v34_received_mapping_frame *received)
{
	return regions_overlap(call, sizeof(*call), sample, sizeof(*sample)) ||
	       regions_overlap(
		       call, sizeof(*call), observation, sizeof(*observation)) ||
	       regions_overlap(
		       call, sizeof(*call), received, sizeof(*received)) ||
	       regions_overlap(
		       sample, sizeof(*sample), observation,
		       sizeof(*observation)) ||
	       regions_overlap(
		       sample, sizeof(*sample), received, sizeof(*received)) ||
	       regions_overlap(
		       observation, sizeof(*observation), received,
		       sizeof(*received));
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

	if (v34_phase2_profiles(
		    &call->state.phase2, &local, &peer) != V34_PHASE2_OK)
		return V34_CALL_NUMERIC_FAILURE;
	result = v34_phase3_exchange_init(
		&call->state.phase3, &local, &peer,
		&call->phase3_configuration);
	if (result != V34_PHASE3_EXCHANGE_OK)
		return phase3_result(result);
	call->stage = V34_CALL_PHASE3;
	return V34_CALL_OK;
}

static enum v34_call_result advance_phase2(
	struct v34_call *call, const struct v34_phase2_event *event)
{
	enum v34_phase2_result result;

	if (event->kind == V34_PHASE2_EVENT_FAILED)
		return V34_CALL_FAILED;
	if (v34_phase2_is_ready_for_info1(&call->state.phase2)) {
		result = call->local_role == V34_CALLING_STATION ?
			v34_phase2_start_caller_info1(
				&call->state.phase2,
				&call->local_info1.calling) :
			v34_phase2_start_answerer_info1(&call->state.phase2);
		if (result != V34_PHASE2_OK)
			return V34_CALL_SEQUENCE_MISMATCH;
	}
	if (call->local_role == V34_ANSWERING_STATION &&
	    event->kind == V34_PHASE2_EVENT_PEER_INFO1C_RECEIVED &&
	    v34_phase2_answerer_reply(
		    &call->state.phase2, &call->local_info1.answering) !=
		    V34_PHASE2_OK)
		return V34_CALL_SEQUENCE_MISMATCH;
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
		&call->phase4_configuration);
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
	return phase4_result(v34_phase4_exchange_prepare_data_startup(
		&call->state.phase4));
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
	int16_t generated;

	if (!v34_call_is_valid(call) || transmit_sample == NULL ||
	    observation == NULL || received == NULL ||
	    outputs_overlap(call, transmit_sample, observation, received))
		return V34_CALL_INVALID_ARGUMENT;
	if (call->failed)
		return V34_CALL_FAILED;
	emitted.processed = call->stage;
	switch (call->stage) {
	case V34_CALL_PHASE2:
		result = sample_phase2(
			call, received_sample, &generated, &emitted);
		break;
	case V34_CALL_PHASE3:
		result = sample_phase3(
			call, received_sample, &generated, &emitted);
		break;
	case V34_CALL_PHASE4:
		result = sample_phase4(
			call, received_sample, source, source_context,
			&generated, &emitted, &decoded);
		if (result == V34_CALL_SOURCE_FAILURE)
			return result;
		break;
	default:
		return V34_CALL_INVALID_STATE;
	}
	if (result != V34_CALL_OK)
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

const char *v34_call_result_name(enum v34_call_result result)
{
	switch (result) {
	case V34_CALL_OK:
		return "ok";
	case V34_CALL_SOURCE_FAILURE:
		return "mapping-frame source failure";
	case V34_CALL_FAILED:
		return "failed";
	case V34_CALL_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_CALL_INVALID_STATE:
		return "invalid state";
	case V34_CALL_NO_COMMON_RATE:
		return "no common rate";
	case V34_CALL_OUT_OF_RANGE:
		return "out of range";
	case V34_CALL_CLIPPING_RISK:
		return "clipping risk";
	case V34_CALL_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V34_CALL_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
