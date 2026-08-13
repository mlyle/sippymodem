// SPDX-License-Identifier: BSD-3-Clause

#include "v90_call.h"

#include "v90_phase3_entry.h"
#include "v90_phase3_handoff.h"

#include <stddef.h>

static bool role_is_valid(enum v90_modem_role role)
{
	return role == V90_ANALOG_MODEM || role == V90_DIGITAL_MODEM;
}

static bool law_is_valid(enum g711_law law)
{
	return law == G711_LAW_PCMU || law == G711_LAW_PCMA;
}

static enum g711_law info0_law(const struct v90_info0d *info0)
{
	return info0->pcm_law == V90_PCM_A_LAW ?
		       G711_LAW_PCMA : G711_LAW_PCMU;
}

static bool analog_configuration_is_valid(
	const union v90_call_local_info1 *info1,
	const union v90_call_phase3_configuration *phase3)
{
	return v90_info1a_is_valid(&info1->analog) &&
	       phase3->analog.nominal_peak_amplitude > 0;
}

static bool digital_configuration_is_valid(
	const union v90_call_local_info1 *info1,
	const union v90_call_phase3_configuration *phase3)
{
	return v34_info1c_is_valid(&info1->digital) &&
	       v90_phase3_analog_rx_configuration_is_valid(
		       &phase3->digital.receive) &&
	       v90_jd_parameters_are_valid(&phase3->digital.jd);
}

static bool configuration_is_valid(
	const struct v90_call_configuration *configuration)
{
	if (configuration == NULL ||
	    !role_is_valid(configuration->local_role) ||
	    !law_is_valid(configuration->law) ||
	    configuration->phase2_nominal_amplitude <= 0)
		return false;
	if (configuration->local_role == V90_ANALOG_MODEM)
		return v90_info0_is_valid(
			       &configuration->local_info0.analog) &&
		       analog_configuration_is_valid(
			       &configuration->local_info1,
			       &configuration->phase3);
	return v90_info0d_is_valid(
		       &configuration->local_info0.digital) &&
	       info0_law(&configuration->local_info0.digital) ==
		       configuration->law &&
	       digital_configuration_is_valid(
		       &configuration->local_info1,
		       &configuration->phase3);
}

static bool stored_configuration_is_valid(const struct v90_call *call)
{
	if (!role_is_valid(call->local_role) || !law_is_valid(call->law))
		return false;
	return call->local_role == V90_ANALOG_MODEM ?
		       analog_configuration_is_valid(
			       &call->local_info1,
			       &call->phase3_configuration) :
		       digital_configuration_is_valid(
			       &call->local_info1,
			       &call->phase3_configuration);
}

bool v90_call_is_valid(const struct v90_call *call)
{
	bool child_valid;
	bool child_failed;

	if (call == NULL || !stored_configuration_is_valid(call))
		return false;
	switch (call->stage) {
	case V90_CALL_PHASE2:
		child_valid = v90_phase2_is_valid(&call->state.phase2) &&
			      call->state.phase2.local_role == call->local_role;
		child_failed = v90_phase2_has_failed(&call->state.phase2);
		break;
	case V90_CALL_PHASE3:
		if (call->local_role == V90_ANALOG_MODEM) {
			child_valid = v90_phase3_analog_station_is_valid(
				&call->state.phase3.analog);
			child_failed = v90_phase3_analog_station_has_failed(
				&call->state.phase3.analog);
		} else {
			child_valid = v90_phase3_digital_station_is_valid(
				&call->state.phase3.digital);
			child_failed = v90_phase3_digital_station_has_failed(
				&call->state.phase3.digital);
		}
		break;
	default:
		return false;
	}
	return child_valid && (call->failed || !child_failed);
}

enum v90_call_result v90_call_init(
	struct v90_call *call,
	const struct v90_call_configuration *configuration)
{
	struct v90_call initialized = { 0 };
	enum v90_phase2_result result;

	if (call == NULL || !configuration_is_valid(configuration))
		return V90_CALL_INVALID_ARGUMENT;
	if (configuration->local_role == V90_ANALOG_MODEM)
		result = v90_phase2_init_analog(
			&initialized.state.phase2,
			configuration->phase2_nominal_amplitude,
			&configuration->local_info0.analog);
	else
		result = v90_phase2_init_digital(
			&initialized.state.phase2,
			configuration->phase2_nominal_amplitude,
			&configuration->local_info0.digital);
	if (result != V90_PHASE2_OK)
		return V90_CALL_INVALID_ARGUMENT;
	initialized.local_info1 = configuration->local_info1;
	initialized.phase3_configuration = configuration->phase3;
	initialized.local_role = configuration->local_role;
	initialized.law = configuration->law;
	initialized.stage = V90_CALL_PHASE2;
	if (!v90_call_is_valid(&initialized))
		return V90_CALL_NUMERIC_FAILURE;
	*call = initialized;
	return V90_CALL_OK;
}

bool v90_call_has_failed(const struct v90_call *call)
{
	return v90_call_is_valid(call) && call->failed;
}

bool v90_call_phase3_is_ready(const struct v90_call *call)
{
	return v90_call_is_valid(call) && !call->failed &&
	       call->stage == V90_CALL_PHASE3;
}

enum v90_call_stage v90_call_current_stage(const struct v90_call *call)
{
	return v90_call_is_valid(call) ? call->stage :
				       (enum v90_call_stage)-1;
}

static uint8_t encode_sample(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_encode_pcma(sample) : g711_encode_pcmu(sample);
}

static int16_t decode_octet(enum g711_law law, uint8_t octet)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(octet) : g711_decode_pcmu(octet);
}

static enum v90_call_result entry_result(
	enum v90_phase3_entry_result result)
{
	switch (result) {
	case V90_PHASE3_ENTRY_OK:
		return V90_CALL_OK;
	case V90_PHASE3_ENTRY_INVALID_STATE:
		return V90_CALL_INVALID_STATE;
	case V90_PHASE3_ENTRY_INVALID_SELECTION:
		return V90_CALL_INVALID_SELECTION;
	case V90_PHASE3_ENTRY_UNUSABLE_SELECTION:
		return V90_CALL_UNUSABLE_SELECTION;
	case V90_PHASE3_ENTRY_MEDIA_MISMATCH:
		return V90_CALL_MEDIA_MISMATCH;
	case V90_PHASE3_ENTRY_OUT_OF_RANGE:
		return V90_CALL_OUT_OF_RANGE;
	case V90_PHASE3_ENTRY_CLIPPING_RISK:
		return V90_CALL_CLIPPING_RISK;
	case V90_PHASE3_ENTRY_ROLE_MISMATCH:
		return V90_CALL_SEQUENCE_MISMATCH;
	case V90_PHASE3_ENTRY_INVALID_ARGUMENT:
	case V90_PHASE3_ENTRY_INTERNAL_ERROR:
		break;
	}
	return V90_CALL_NUMERIC_FAILURE;
}

static enum v90_call_result enter_phase3(struct v90_call *call)
{
	struct v90_phase3_entry entry;
	struct v90_call next;
	enum v90_phase3_entry_result result =
		v90_phase3_entry_from_phase2(
			&entry, &call->state.phase2, call->law);

	if (result != V90_PHASE3_ENTRY_OK)
		return entry_result(result);
	next = *call;
	if (call->local_role == V90_ANALOG_MODEM) {
		struct v90_phase3_analog_station station;

		result = v90_phase3_entry_start_analog(
			&entry,
			call->phase3_configuration.analog
				.nominal_peak_amplitude,
			&station);
		if (result != V90_PHASE3_ENTRY_OK)
			return entry_result(result);
		next.state.phase3.analog = station;
	} else {
		struct v90_phase3_digital_station station;

		result = v90_phase3_entry_start_digital(
			&entry,
			&call->phase3_configuration.digital.receive,
			&call->phase3_configuration.digital.jd,
			&station);
		if (result != V90_PHASE3_ENTRY_OK)
			return entry_result(result);
		next.state.phase3.digital = station;
	}
	next.stage = V90_CALL_PHASE3;
	if (!v90_call_is_valid(&next))
		return V90_CALL_NUMERIC_FAILURE;
	*call = next;
	return V90_CALL_OK;
}

static enum v90_call_result advance_phase2(
	struct v90_call *call, const struct v90_phase2_event *event)
{
	enum v90_phase2_result result;

	if (event->kind == V90_PHASE2_EVENT_FAILED)
		return V90_CALL_FAILED;
	if (v90_phase2_is_ready_for_info1(&call->state.phase2)) {
		result = call->local_role == V90_ANALOG_MODEM ?
			v90_phase2_start_analog_info1(&call->state.phase2) :
			v90_phase2_start_digital_info1(
				&call->state.phase2,
				&call->local_info1.digital);
		if (result != V90_PHASE2_OK)
			return V90_CALL_SEQUENCE_MISMATCH;
	}
	if (call->local_role == V90_ANALOG_MODEM &&
	    event->kind == V90_PHASE2_EVENT_PEER_INFO1D_RECEIVED &&
	    v90_phase2_reply_analog(
		    &call->state.phase2, &call->local_info1.analog) !=
		    V90_PHASE2_OK)
		return V90_CALL_SEQUENCE_MISMATCH;
	return v90_phase2_is_ready_for_phase3(&call->state.phase2) ?
		       enter_phase3(call) : V90_CALL_OK;
}

static enum v90_call_result sample_phase2(
	struct v90_call *call, uint8_t received_octet,
	uint8_t *transmit_octet, struct v90_call_observation *observation)
{
	int16_t transmitted_sample;
	enum v90_phase2_result result = v90_phase2_sample(
		&call->state.phase2,
		decode_octet(call->law, received_octet),
		&transmitted_sample, &observation->phase2);

	if (result != V90_PHASE2_OK)
		return V90_CALL_NUMERIC_FAILURE;
	*transmit_octet = encode_sample(call->law, transmitted_sample);
	return advance_phase2(call, &observation->phase2);
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
	const struct v90_call *call, const uint8_t *transmit_octet,
	const struct v90_call_observation *observation)
{
	return regions_overlap(
		       call, sizeof(*call),
		       transmit_octet, sizeof(*transmit_octet)) ||
	       regions_overlap(
		       call, sizeof(*call),
		       observation, sizeof(*observation)) ||
	       regions_overlap(
		       transmit_octet, sizeof(*transmit_octet),
		       observation, sizeof(*observation));
}

enum v90_call_result v90_call_octet(
	struct v90_call *call, uint8_t received_octet,
	uint8_t *transmit_octet,
	struct v90_call_observation *observation)
{
	struct v90_call_observation emitted = { 0 };
	uint8_t generated;
	enum v90_call_result result;

	if (!v90_call_is_valid(call) || transmit_octet == NULL ||
	    observation == NULL ||
	    outputs_overlap(call, transmit_octet, observation))
		return V90_CALL_INVALID_ARGUMENT;
	if (call->failed)
		return V90_CALL_FAILED;
	if (v90_call_phase3_is_ready(call))
		return V90_CALL_PHASE3_READY;
	generated = encode_sample(call->law, 0);
	emitted.processed = call->stage;
	result = sample_phase2(
		call, received_octet, &generated, &emitted);
	if (result != V90_CALL_OK)
		call->failed = true;
	emitted.next = call->stage;
	if (!v90_call_is_valid(call)) {
		call->failed = true;
		return V90_CALL_NUMERIC_FAILURE;
	}
	*transmit_octet = generated;
	*observation = emitted;
	return result;
}

const char *v90_call_result_name(enum v90_call_result result)
{
	switch (result) {
	case V90_CALL_OK:
		return "ok";
	case V90_CALL_PHASE3_READY:
		return "phase 3 ready";
	case V90_CALL_FAILED:
		return "failed";
	case V90_CALL_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_CALL_INVALID_STATE:
		return "invalid state";
	case V90_CALL_INVALID_SELECTION:
		return "invalid selection";
	case V90_CALL_UNUSABLE_SELECTION:
		return "unusable selection";
	case V90_CALL_MEDIA_MISMATCH:
		return "media mismatch";
	case V90_CALL_OUT_OF_RANGE:
		return "out of range";
	case V90_CALL_CLIPPING_RISK:
		return "clipping risk";
	case V90_CALL_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V90_CALL_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
