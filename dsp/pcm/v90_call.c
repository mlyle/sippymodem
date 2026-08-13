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
	const union v90_call_phase3_configuration *phase3,
	const struct v90_call_analog_phase4_configuration *phase4,
	enum g711_law law)
{
	return v90_info1a_is_valid(&info1->analog) &&
	       phase3->analog.nominal_peak_amplitude > 0 &&
	       phase4->training_parameters.kind == V90_CP_TRAINING &&
	       phase4->training_parameters.law == law &&
	       !phase4->training_parameters.request_silence &&
	       !phase4->training_parameters.acknowledges_peer &&
	       v90_cp_parameters_are_valid(&phase4->training_parameters);
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
			       &configuration->phase3,
			       &configuration->analog_phase4,
			       configuration->law);
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
			       &call->phase3_configuration,
			       &call->analog_phase4_configuration,
			       call->law) :
		       digital_configuration_is_valid(
			       &call->local_info1,
			       &call->phase3_configuration);
}

static bool analog_mixed_is_valid(const struct v90_call *call)
{
	const struct v90_call_analog_mixed_state *mixed =
		&call->state.mixed.analog;
	const struct v90_phase3_sign_receiver *signs =
		&mixed->phase3.receive.signs;
	bool transmit_complete =
		mixed->phase3.transmit_stage ==
			V90_PHASE3_ANALOG_TRANSMIT_RETURN &&
		v90_phase3_return_line_tx_is_handoff_ready(
			&mixed->phase3.transmit.returning);

	return v90_phase3_analog_station_is_valid(&mixed->phase3) &&
	       mixed->phase3.receive_stage ==
		       V90_PHASE3_ANALOG_RECEIVE_SIGNS &&
	       v90_phase3_sign_receiver_is_complete(signs) &&
	       v90_phase4_ri_rx_is_valid(&mixed->receive) &&
	       mixed->receive.stage == V90_PHASE4_RI &&
	       mixed->receive.law == signs->law &&
	       mixed->receive.training_ucode == signs->training_ucode &&
	       (call->failed || !transmit_complete);
}

static bool digital_mixed_is_valid(const struct v90_call *call)
{
	const struct v90_call_digital_mixed_state *mixed =
		&call->state.mixed.digital;
	bool receive_complete =
		mixed->phase3.receive_stage ==
			V90_PHASE3_DIGITAL_RECEIVE_RETURN &&
		v90_phase3_return_rx_is_complete(
			&mixed->phase3.receive.returning.points);

	return v90_phase3_digital_station_is_valid(&mixed->phase3) &&
	       mixed->phase3.transmit_started &&
	       mixed->phase3.transmit.stage ==
		       V90_PHASE3_DIGITAL_TX_COMPLETE &&
	       v90_phase4_ri_tx_is_valid(&mixed->transmit) &&
	       mixed->transmit.stage == V90_PHASE4_RI &&
	       !mixed->transmit.finish_requested &&
	       mixed->transmit.law == mixed->phase3.law &&
	       mixed->transmit.training_ucode ==
		       mixed->phase3.training_ucode &&
	       (call->failed || !receive_complete);
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
	case V90_CALL_PHASE3_MIXED:
		child_valid = call->local_role == V90_ANALOG_MODEM ?
			      analog_mixed_is_valid(call) :
			      digital_mixed_is_valid(call);
		child_failed = call->local_role == V90_ANALOG_MODEM ?
			       v90_phase3_analog_station_has_failed(
				       &call->state.mixed.analog.phase3) :
			       v90_phase3_digital_station_has_failed(
				       &call->state.mixed.digital.phase3);
		break;
	case V90_CALL_PHASE4:
		if (call->local_role == V90_ANALOG_MODEM) {
			child_valid = v90_phase4_analog_station_is_valid(
				&call->state.phase4.analog);
			child_failed = v90_phase4_analog_station_has_failed(
				&call->state.phase4.analog);
		} else {
			child_valid = v90_phase4_digital_station_is_valid(
				&call->state.phase4.digital);
			child_failed = v90_phase4_digital_station_has_failed(
				&call->state.phase4.digital);
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
	if (configuration->local_role == V90_ANALOG_MODEM) {
		initialized.local_info1.analog =
			configuration->local_info1.analog;
		initialized.phase3_configuration.analog =
			configuration->phase3.analog;
		initialized.analog_phase4_configuration =
			configuration->analog_phase4;
	} else {
		initialized.local_info1.digital =
			configuration->local_info1.digital;
		initialized.phase3_configuration.digital =
			configuration->phase3.digital;
	}
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

bool v90_call_phase4_is_ready(const struct v90_call *call)
{
	return v90_call_is_valid(call) && !call->failed &&
	       call->stage == V90_CALL_PHASE4;
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

static enum v90_call_result analog_phase3_result(
	enum v90_phase3_analog_station_result result)
{
	switch (result) {
	case V90_PHASE3_ANALOG_STATION_OK:
		return V90_CALL_OK;
	case V90_PHASE3_ANALOG_STATION_FAILED:
		return V90_CALL_FAILED;
	case V90_PHASE3_ANALOG_STATION_OUT_OF_RANGE:
		return V90_CALL_OUT_OF_RANGE;
	case V90_PHASE3_ANALOG_STATION_CLIPPING_RISK:
		return V90_CALL_CLIPPING_RISK;
	case V90_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH:
		return V90_CALL_SEQUENCE_MISMATCH;
	case V90_PHASE3_ANALOG_STATION_COMPLETE:
	case V90_PHASE3_ANALOG_STATION_INVALID_STATE:
		return V90_CALL_INVALID_STATE;
	case V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT:
	case V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE:
		break;
	}
	return V90_CALL_NUMERIC_FAILURE;
}

static enum v90_call_result digital_phase3_result(
	enum v90_phase3_digital_station_result result)
{
	switch (result) {
	case V90_PHASE3_DIGITAL_STATION_OK:
		return V90_CALL_OK;
	case V90_PHASE3_DIGITAL_STATION_FAILED:
		return V90_CALL_FAILED;
	case V90_PHASE3_DIGITAL_STATION_OUT_OF_RANGE:
		return V90_CALL_OUT_OF_RANGE;
	case V90_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH:
		return V90_CALL_SEQUENCE_MISMATCH;
	case V90_PHASE3_DIGITAL_STATION_UNSUPPORTED:
		return V90_CALL_UNUSABLE_SELECTION;
	case V90_PHASE3_DIGITAL_STATION_COMPLETE:
	case V90_PHASE3_DIGITAL_STATION_INVALID_STATE:
		return V90_CALL_INVALID_STATE;
	case V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT:
	case V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE:
		break;
	}
	return V90_CALL_NUMERIC_FAILURE;
}

static enum v90_call_result enter_mixed_phase3(struct v90_call *call)
{
	struct v90_call next = *call;

	next.state.mixed = (union v90_call_mixed_state){ 0 };
	if (call->local_role == V90_ANALOG_MODEM) {
		struct v90_phase3_analog_receive_handoff handoff;
		struct v90_call_analog_mixed_state mixed = {
			.phase3 = call->state.phase3.analog,
		};

		if (v90_phase3_analog_station_receive_handoff(
			    &call->state.phase3.analog, &handoff) !=
			    V90_PHASE3_ANALOG_STATION_OK)
			return V90_CALL_INVALID_STATE;
		if (v90_phase4_ri_rx_init(
			    &mixed.receive, handoff.law,
			    handoff.training_ucode) != V90_PHASE4_RI_OK)
			return V90_CALL_NUMERIC_FAILURE;
		next.state.mixed.analog = mixed;
	} else {
		struct v90_phase3_digital_transmit_handoff handoff;
		struct v90_call_digital_mixed_state mixed = {
			.phase3 = call->state.phase3.digital,
		};

		if (v90_phase3_digital_station_transmit_handoff(
			    &call->state.phase3.digital, &handoff) !=
			    V90_PHASE3_DIGITAL_STATION_OK)
			return V90_CALL_INVALID_STATE;
		if (v90_phase4_ri_tx_init(
			    &mixed.transmit, handoff.law,
			    handoff.training_ucode) != V90_PHASE4_RI_OK)
			return V90_CALL_NUMERIC_FAILURE;
		next.state.mixed.digital = mixed;
	}
	next.stage = V90_CALL_PHASE3_MIXED;
	if (!v90_call_is_valid(&next))
		return V90_CALL_NUMERIC_FAILURE;
	*call = next;
	return V90_CALL_OK;
}

static enum v90_call_result analog_phase4_result(
	enum v90_phase4_analog_station_result result)
{
	switch (result) {
	case V90_PHASE4_ANALOG_STATION_OK:
		return V90_CALL_OK;
	case V90_PHASE4_ANALOG_STATION_INVALID_PARAMETERS:
		return V90_CALL_UNUSABLE_SELECTION;
	case V90_PHASE4_ANALOG_STATION_OUT_OF_RANGE:
		return V90_CALL_OUT_OF_RANGE;
	case V90_PHASE4_ANALOG_STATION_CLIPPING_RISK:
		return V90_CALL_CLIPPING_RISK;
	case V90_PHASE4_ANALOG_STATION_SEQUENCE_MISMATCH:
		return V90_CALL_SEQUENCE_MISMATCH;
	case V90_PHASE4_ANALOG_STATION_FAILED:
		return V90_CALL_FAILED;
	case V90_PHASE4_ANALOG_STATION_COMPLETE:
	case V90_PHASE4_ANALOG_STATION_INVALID_STATE:
		return V90_CALL_INVALID_STATE;
	case V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT:
	case V90_PHASE4_ANALOG_STATION_NUMERIC_FAILURE:
		break;
	}
	return V90_CALL_NUMERIC_FAILURE;
}

static enum v90_call_result enter_phase4(struct v90_call *call)
{
	struct v90_call next = *call;

	next.state.phase4 = (union v90_call_phase4_state){ 0 };
	if (call->local_role == V90_ANALOG_MODEM) {
		const struct v90_call_analog_mixed_state *mixed =
			&call->state.mixed.analog;
		struct v90_phase3_analog_receive_handoff receive_handoff;
		struct v34_training_line transmit_handoff;
		struct v90_phase4_analog_station station;
		enum v90_call_result result;

		if (v90_phase3_analog_station_transmit_handoff(
			    &mixed->phase3, &transmit_handoff) !=
			    V90_PHASE3_ANALOG_STATION_OK ||
		    v90_phase3_analog_station_receive_handoff(
			    &mixed->phase3, &receive_handoff) !=
			    V90_PHASE3_ANALOG_STATION_OK)
			return V90_CALL_INVALID_STATE;
		result = analog_phase4_result(v90_phase4_analog_station_resume(
			&station, &transmit_handoff, &mixed->receive,
			&receive_handoff.jd,
			&call->analog_phase4_configuration
				.training_parameters));
		if (result != V90_CALL_OK)
			return result;
		next.state.phase4.analog = station;
	} else {
		const struct v90_call_digital_mixed_state *mixed =
			&call->state.mixed.digital;
		struct v90_phase3_return_line_handoff receive_handoff;
		struct v90_phase4_digital_station station;

		if (v90_phase3_digital_station_receive_handoff(
			    &mixed->phase3, &receive_handoff) !=
			    V90_PHASE3_DIGITAL_STATION_OK)
			return V90_CALL_INVALID_STATE;
		if (v90_phase4_digital_station_resume(
			    &station, &receive_handoff,
			    &call->phase3_configuration.digital.jd,
			    &mixed->transmit,
			    call->phase3_configuration.digital.receive
				    .tracking_equalizer_adaptation) !=
			    V90_PHASE4_DIGITAL_STATION_OK)
			return V90_CALL_NUMERIC_FAILURE;
		next.state.phase4.digital = station;
	}
	next.stage = V90_CALL_PHASE4;
	if (!v90_call_is_valid(&next))
		return V90_CALL_NUMERIC_FAILURE;
	*call = next;
	return V90_CALL_OK;
}

static enum v90_call_result advance_phase3(struct v90_call *call)
{
	enum v90_call_result result;

	if (call->stage == V90_CALL_PHASE3) {
		bool leading_direction_complete =
			call->local_role == V90_ANALOG_MODEM ?
				v90_phase3_analog_station_receive_complete(
					&call->state.phase3.analog) :
				v90_phase3_digital_station_transmit_complete(
					&call->state.phase3.digital);

		if (!leading_direction_complete)
			return V90_CALL_OK;
		result = enter_mixed_phase3(call);
		if (result != V90_CALL_OK)
			return result;
	}
	if (call->local_role == V90_ANALOG_MODEM) {
		if (!v90_phase3_analog_station_transmit_complete(
			    &call->state.mixed.analog.phase3))
			return V90_CALL_OK;
	} else if (!v90_phase3_digital_station_receive_complete(
			   &call->state.mixed.digital.phase3)) {
		return V90_CALL_OK;
	}
	return enter_phase4(call);
}

static enum v90_call_result sample_phase3(
	struct v90_call *call, uint8_t received_octet,
	uint8_t *transmit_octet)
{
	enum v90_call_result result;

	if (call->local_role == V90_ANALOG_MODEM) {
		int16_t sample;

		result = analog_phase3_result(
			v90_phase3_analog_station_transmit(
				&call->state.phase3.analog, &sample));
		if (result != V90_CALL_OK)
			return result;
		*transmit_octet = encode_sample(call->law, sample);
		result = analog_phase3_result(
			v90_phase3_analog_station_receive(
				&call->state.phase3.analog, received_octet));
	} else {
		result = digital_phase3_result(
			v90_phase3_digital_station_transmit(
				&call->state.phase3.digital, transmit_octet));
		if (result != V90_CALL_OK)
			return result;
		result = digital_phase3_result(
			v90_phase3_digital_station_receive(
				&call->state.phase3.digital,
				decode_octet(call->law, received_octet)));
	}
	return result == V90_CALL_OK ? advance_phase3(call) : result;
}

static enum v90_call_result receive_mixed_ri(
	struct v90_phase4_ri_rx *receive, uint8_t network_octet)
{
	struct v90_phase4_ri_rx next = *receive;
	enum v90_phase4_ri_result result =
		v90_phase4_ri_rx_receive(&next, network_octet);

	if (result == V90_PHASE4_RI_OK) {
		*receive = next;
		return V90_CALL_OK;
	}
	if (result == V90_PHASE4_RI_TRANSITION_DETECTED ||
	    result == V90_PHASE4_RI_INVALID_SEQUENCE)
		return V90_CALL_SEQUENCE_MISMATCH;
	return result == V90_PHASE4_RI_DONE ?
		       V90_CALL_INVALID_STATE : V90_CALL_NUMERIC_FAILURE;
}

static enum v90_call_result sample_mixed_phase3(
	struct v90_call *call, uint8_t received_octet,
	uint8_t *transmit_octet)
{
	enum v90_call_result result;

	if (call->local_role == V90_ANALOG_MODEM) {
		struct v90_call_analog_mixed_state *mixed =
			&call->state.mixed.analog;
		int16_t sample;

		result = analog_phase3_result(
			v90_phase3_analog_station_transmit(
				&mixed->phase3, &sample));
		if (result != V90_CALL_OK)
			return result;
		*transmit_octet = encode_sample(call->law, sample);
		result = receive_mixed_ri(&mixed->receive, received_octet);
	} else {
		struct v90_call_digital_mixed_state *mixed =
			&call->state.mixed.digital;

		if (v90_phase4_ri_tx_next(
			    &mixed->transmit, transmit_octet) != V90_PHASE4_RI_OK)
			return V90_CALL_NUMERIC_FAILURE;
		result = digital_phase3_result(
			v90_phase3_digital_station_receive(
				&mixed->phase3,
				decode_octet(call->law, received_octet)));
	}
	return result == V90_CALL_OK ? advance_phase3(call) : result;
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
	if (v90_call_phase4_is_ready(call))
		return V90_CALL_PHASE4_READY;
	generated = encode_sample(call->law, 0);
	emitted.processed = call->stage;
	switch (call->stage) {
	case V90_CALL_PHASE2:
		result = sample_phase2(
			call, received_octet, &generated, &emitted);
		break;
	case V90_CALL_PHASE3:
		result = sample_phase3(
			call, received_octet, &generated);
		break;
	case V90_CALL_PHASE3_MIXED:
		result = sample_mixed_phase3(
			call, received_octet, &generated);
		break;
	case V90_CALL_PHASE4:
	default:
		return V90_CALL_NUMERIC_FAILURE;
	}
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
	case V90_CALL_PHASE4_READY:
		return "phase 4 ready";
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
