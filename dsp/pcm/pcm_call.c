// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_call.h"

#include "memory_region.h"
#include "pcm_phase3_entry.h"
#include "pcm_phase3_handoff.h"

#include <stddef.h>

enum {
	V90_NETWORK_CHARACTERS_PER_SECOND = 8000,
	V90_UPSTREAM_RATE_UNIT = 2400,
};

static bool downstream_rate_mask_is_valid(uint32_t mask)
{
	return mask != 0 &&
	       (mask & ~(uint32_t)V90_JD_RATE_CAPABILITY_MASK) == 0;
}

static bool downstream_rate_is_enabled(
	uint32_t mask, uint8_t rate_index)
{
	return rate_index != 0 && rate_index <= V90_CP_RATE_INDEX_MAX &&
	       (mask & (UINT32_C(1) << (rate_index - 1u))) != 0;
}

static uint8_t highest_downstream_rate(uint32_t mask)
{
	for (uint8_t index = V90_CP_RATE_INDEX_MAX; index > 0; --index) {
		if (downstream_rate_is_enabled(mask, index))
			return index;
	}
	return 0;
}

static bool cp_supports_rate_mask(
	const struct v90_cp_parameters *parameters, uint32_t mask)
{
	struct v90_cp_parameters selected = *parameters;
	struct v90_pcm_mapping mapping;

	selected.selected_rate_index = highest_downstream_rate(mask);
	return selected.selected_rate_index != 0 &&
	       v90_pcm_mapping_init(&mapping, &selected) == V90_PCM_MAPPING_OK;
}

static enum g711_law info0_law(const struct v90_info0d *info0)
{
	return info0->pcm_law == V90_PCM_A_LAW ?
		       G711_LAW_PCMA : G711_LAW_PCMU;
}

static bool analog_configuration_is_valid(
	const union pcm_call_local_info1 *info1,
	const union pcm_call_phase3_configuration *phase3,
	const struct pcm_call_analog_phase4_configuration *phase4,
	enum g711_law law)
{
	return v90_info1a_is_valid(&info1->analog) &&
	       phase3->analog.nominal_peak_amplitude > 0 &&
	       phase4->training_parameters.kind == V90_CP_TRAINING &&
	       phase4->training_parameters.law == law &&
	       !phase4->training_parameters.request_silence &&
	       !phase4->training_parameters.acknowledges_peer &&
	       v90_cp_parameters_are_valid(&phase4->training_parameters) &&
	       phase4->data_parameters.kind == V90_CP_DATA &&
	       phase4->data_parameters.law == law &&
	       phase4->data_parameters.selected_rate_index != 0 &&
	       !phase4->data_parameters.request_silence &&
	       !phase4->data_parameters.acknowledges_peer &&
	       v90_cp_parameters_are_valid(&phase4->data_parameters);
}

static bool digital_configuration_is_valid(
	const union pcm_call_local_info1 *info1,
	const union pcm_call_phase3_configuration *phase3,
	const struct pcm_call_digital_phase4_configuration *phase4)
{
	const struct v90_mp_record *mp = &phase4->initial_mp;

	if (!v90_mp_record_is_valid(mp))
		return false;
	return v34_info1c_is_valid(&info1->digital) &&
	       v90_phase3_analog_rx_configuration_is_valid(
		       &phase3->digital.receive) &&
	       v90_jd_parameters_are_valid(&phase3->digital.jd) &&
	       !v90_mp_record_acknowledges_cp(mp);
}

static bool configuration_is_valid(
	const struct pcm_call_configuration *configuration)
{
	if (configuration == NULL ||
	    !pcm_modem_role_is_valid(configuration->local_role) ||
	    !g711_law_is_valid(configuration->law) ||
	    !downstream_rate_mask_is_valid(
		    configuration->enabled_downstream_rate_mask) ||
	    configuration->phase2_nominal_amplitude <= 0)
		return false;
	if (configuration->local_role == V90_ANALOG_MODEM)
		return v90_info0_is_valid(
			       &configuration->local_info0.analog) &&
		       downstream_rate_is_enabled(
			       configuration->enabled_downstream_rate_mask,
			       configuration->analog_phase4.data_parameters
				       .selected_rate_index) &&
		       cp_supports_rate_mask(
			       &configuration->analog_phase4.data_parameters,
			       configuration->enabled_downstream_rate_mask) &&
		       analog_configuration_is_valid(
			       &configuration->local_info1,
			       &configuration->phase3,
			       &configuration->analog_phase4,
			       configuration->law);
	return v90_info0d_is_valid(
		       &configuration->local_info0.digital) &&
	       info0_law(&configuration->local_info0.digital) ==
		       configuration->law &&
	       configuration->phase3.digital.jd
		       .data_rate_capability_mask ==
		       configuration->enabled_downstream_rate_mask &&
	       digital_configuration_is_valid(
		       &configuration->local_info1,
		       &configuration->phase3,
		       &configuration->digital_phase4);
}

static bool stored_configuration_is_valid(const struct pcm_call *call)
{
	if (!pcm_modem_role_is_valid(call->local_role) ||
	    !g711_law_is_valid(call->law) ||
	    !downstream_rate_mask_is_valid(
		    call->enabled_downstream_rate_mask))
		return false;
	return call->local_role == V90_ANALOG_MODEM ?
		       downstream_rate_is_enabled(
			       call->enabled_downstream_rate_mask,
			       call->analog_phase4_configuration.data_parameters
				       .selected_rate_index) &&
		       cp_supports_rate_mask(
			       &call->analog_phase4_configuration.data_parameters,
			       call->enabled_downstream_rate_mask) &&
		       analog_configuration_is_valid(
			       &call->local_info1,
			       &call->phase3_configuration,
			       &call->analog_phase4_configuration,
			       call->law) :
		       call->phase3_configuration.digital.jd
			       .data_rate_capability_mask ==
			       call->enabled_downstream_rate_mask &&
		       digital_configuration_is_valid(
			       &call->local_info1,
			       &call->phase3_configuration,
			       &call->digital_phase4_configuration);
}

static bool analog_mixed_is_valid(const struct pcm_call *call)
{
	const struct pcm_call_analog_mixed_state *mixed =
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

static bool digital_mixed_is_valid(const struct pcm_call *call)
{
	const struct pcm_call_digital_mixed_state *mixed =
		&call->state.mixed.digital;
	bool receive_complete =
		mixed->phase3.receive_stage ==
			V90_PHASE3_DIGITAL_RECEIVE_RETURN &&
		v90_phase3_return_rx_is_complete(
			&mixed->phase3.receive.returning.points);

	return v90_phase3_digital_station_is_valid(&mixed->phase3) &&
	       mixed->phase3.transmit_started &&
	       mixed->phase3.transmit.stage ==
		       PCM_PHASE3_DIGITAL_TX_COMPLETE &&
	       v90_phase4_ri_tx_is_valid(&mixed->transmit) &&
	       mixed->transmit.stage == V90_PHASE4_RI &&
	       !mixed->transmit.finish_requested &&
	       mixed->transmit.law == mixed->phase3.law &&
	       mixed->transmit.training_ucode ==
		       mixed->phase3.training_ucode &&
	       (call->failed || !receive_complete);
}

bool pcm_call_is_valid(const struct pcm_call *call)
{
	bool child_valid;
	bool child_failed;

	if (call == NULL || !stored_configuration_is_valid(call))
		return false;
	switch (call->stage) {
	case PCM_CALL_PHASE2:
		child_valid = pcm_phase2_is_valid(&call->state.phase2) &&
			      call->state.phase2.local_role == call->local_role;
		child_failed = pcm_phase2_has_failed(&call->state.phase2);
		break;
	case PCM_CALL_PHASE3:
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
	case PCM_CALL_PHASE3_MIXED:
		child_valid = call->local_role == V90_ANALOG_MODEM ?
			      analog_mixed_is_valid(call) :
			      digital_mixed_is_valid(call);
		child_failed = call->local_role == V90_ANALOG_MODEM ?
			       v90_phase3_analog_station_has_failed(
				       &call->state.mixed.analog.phase3) :
			       v90_phase3_digital_station_has_failed(
				       &call->state.mixed.digital.phase3);
		break;
	case PCM_CALL_PHASE4:
		if (call->local_role == V90_ANALOG_MODEM) {
			child_valid = v90_phase4_analog_startup_is_valid(
				&call->state.phase4.analog);
			child_failed = v90_phase4_analog_startup_has_failed(
				&call->state.phase4.analog);
		} else {
			child_valid = v90_phase4_digital_startup_is_valid(
				&call->state.phase4.digital);
			child_failed = v90_phase4_digital_startup_has_failed(
				&call->state.phase4.digital);
		}
		break;
	case PCM_CALL_DATA_PATH:
		if (call->local_role == V90_ANALOG_MODEM) {
			child_valid = v90_analog_path_is_valid(
				&call->state.data_path.analog);
			child_failed = v90_analog_path_has_failed(
				&call->state.data_path.analog);
		} else {
			child_valid = v90_digital_path_is_valid(
				&call->state.data_path.digital);
			child_failed = v90_digital_path_has_failed(
				&call->state.data_path.digital);
		}
		break;
	default:
		return false;
	}
	return child_valid && (call->failed || !child_failed);
}

enum pcm_call_result pcm_call_init(
	struct pcm_call *call,
	const struct pcm_call_configuration *configuration)
{
	struct pcm_call initialized = { 0 };
	enum pcm_phase2_result result;

	if (call == NULL || !configuration_is_valid(configuration))
		return PCM_CALL_INVALID_ARGUMENT;
	if (configuration->local_role == V90_ANALOG_MODEM)
		result = pcm_phase2_init_v90_analog(
			&initialized.state.phase2,
			configuration->phase2_nominal_amplitude,
			&configuration->local_info0.analog);
	else
		result = pcm_phase2_init_v90_digital(
			&initialized.state.phase2,
			configuration->phase2_nominal_amplitude,
			&configuration->local_info0.digital);
	if (result != PCM_PHASE2_OK)
		return PCM_CALL_INVALID_ARGUMENT;
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
		initialized.digital_phase4_configuration =
			configuration->digital_phase4;
	}
	initialized.local_role = configuration->local_role;
	initialized.law = configuration->law;
	initialized.enabled_downstream_rate_mask =
		configuration->enabled_downstream_rate_mask;
	initialized.stage = PCM_CALL_PHASE2;
	if (!pcm_call_is_valid(&initialized))
		return PCM_CALL_NUMERIC_FAILURE;
	*call = initialized;
	return PCM_CALL_OK;
}

bool pcm_call_has_failed(const struct pcm_call *call)
{
	return pcm_call_is_valid(call) && call->failed;
}

bool pcm_call_phase4_is_ready(const struct pcm_call *call)
{
	if (!pcm_call_is_valid(call) || call->failed ||
	    call->stage != PCM_CALL_PHASE4)
		return false;
	return call->local_role == V90_ANALOG_MODEM ?
		       v90_phase4_analog_startup_is_complete(
			       &call->state.phase4.analog) :
		       v90_phase4_digital_startup_is_complete(
			       &call->state.phase4.digital);
}

bool pcm_call_is_online(const struct pcm_call *call)
{
	if (!pcm_call_is_valid(call) || call->failed ||
	    call->stage != PCM_CALL_DATA_PATH)
		return false;
	return call->local_role == V90_ANALOG_MODEM ?
		       v90_analog_path_is_online(
			       &call->state.data_path.analog) :
		       v90_digital_path_is_online(
			       &call->state.data_path.digital);
}

enum pcm_call_stage pcm_call_current_stage(const struct pcm_call *call)
{
	return pcm_call_is_valid(call) ? call->stage :
				       (enum pcm_call_stage)-1;
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

static enum pcm_call_result entry_result(
	enum pcm_phase3_entry_result result)
{
	switch (result) {
	case PCM_PHASE3_ENTRY_OK:
		return PCM_CALL_OK;
	case PCM_PHASE3_ENTRY_INVALID_STATE:
		return PCM_CALL_INVALID_STATE;
	case PCM_PHASE3_ENTRY_INVALID_SELECTION:
		return PCM_CALL_INVALID_SELECTION;
	case PCM_PHASE3_ENTRY_UNUSABLE_SELECTION:
		return PCM_CALL_UNUSABLE_SELECTION;
	case PCM_PHASE3_ENTRY_MEDIA_MISMATCH:
		return PCM_CALL_MEDIA_MISMATCH;
	case PCM_PHASE3_ENTRY_OUT_OF_RANGE:
		return PCM_CALL_OUT_OF_RANGE;
	case PCM_PHASE3_ENTRY_CLIPPING_RISK:
		return PCM_CALL_CLIPPING_RISK;
	case PCM_PHASE3_ENTRY_ROLE_MISMATCH:
		return PCM_CALL_SEQUENCE_MISMATCH;
	case PCM_PHASE3_ENTRY_INVALID_ARGUMENT:
	case PCM_PHASE3_ENTRY_INTERNAL_ERROR:
		break;
	}
	return PCM_CALL_NUMERIC_FAILURE;
}

static enum pcm_call_result enter_phase3(struct pcm_call *call)
{
	struct pcm_phase3_entry entry;
	struct pcm_call next;
	enum pcm_phase3_entry_result result =
		pcm_phase3_entry_from_phase2(
			&entry, &call->state.phase2, call->law);

	if (result != PCM_PHASE3_ENTRY_OK)
		return entry_result(result);
	next = *call;
	if (call->local_role == V90_ANALOG_MODEM) {
		struct v90_phase3_analog_station station;

		result = pcm_phase3_entry_start_v90_analog(
			&entry,
			call->phase3_configuration.analog
				.nominal_peak_amplitude,
			&station);
		if (result != PCM_PHASE3_ENTRY_OK)
			return entry_result(result);
		next.state.phase3.analog = station;
	} else {
		struct v90_phase3_digital_station station;

		result = pcm_phase3_entry_start_v90_digital(
			&entry,
			&call->phase3_configuration.digital.receive,
			&call->phase3_configuration.digital.jd,
			&station);
		if (result != PCM_PHASE3_ENTRY_OK)
			return entry_result(result);
		next.state.phase3.digital = station;
	}
	next.stage = PCM_CALL_PHASE3;
	if (!pcm_call_is_valid(&next))
		return PCM_CALL_NUMERIC_FAILURE;
	*call = next;
	return PCM_CALL_OK;
}

static enum pcm_call_result advance_phase2(
	struct pcm_call *call, const struct pcm_phase2_event *event)
{
	enum pcm_phase2_result result;

	if (event->kind == PCM_PHASE2_EVENT_FAILED)
		return PCM_CALL_FAILED;
	if (pcm_phase2_is_ready_for_info1(&call->state.phase2)) {
		result = call->local_role == V90_ANALOG_MODEM ?
			pcm_phase2_start_analog_info1(&call->state.phase2) :
			pcm_phase2_start_v90_digital_info1(
				&call->state.phase2,
				&call->local_info1.digital);
		if (result != PCM_PHASE2_OK)
			return PCM_CALL_SEQUENCE_MISMATCH;
	}
	if (call->local_role == V90_ANALOG_MODEM &&
	    event->kind == PCM_PHASE2_EVENT_PEER_INFO1D_RECEIVED &&
	    pcm_phase2_reply_conventional(
		    &call->state.phase2, &call->local_info1.analog) !=
		    PCM_PHASE2_OK)
		return PCM_CALL_SEQUENCE_MISMATCH;
	return pcm_phase2_is_ready_for_phase3(&call->state.phase2) ?
		       enter_phase3(call) : PCM_CALL_OK;
}

static enum pcm_call_result sample_phase2(
	struct pcm_call *call, uint8_t received_octet,
	uint8_t *transmit_octet, struct pcm_call_observation *observation)
{
	int16_t transmitted_sample;
	enum pcm_phase2_result result = pcm_phase2_sample(
		&call->state.phase2,
		decode_octet(call->law, received_octet),
		&transmitted_sample, &observation->phase2);

	if (result != PCM_PHASE2_OK)
		return PCM_CALL_NUMERIC_FAILURE;
	*transmit_octet = encode_sample(call->law, transmitted_sample);
	return advance_phase2(call, &observation->phase2);
}

static enum pcm_call_result analog_phase3_result(
	enum v90_phase3_analog_station_result result)
{
	switch (result) {
	case V90_PHASE3_ANALOG_STATION_OK:
		return PCM_CALL_OK;
	case V90_PHASE3_ANALOG_STATION_FAILED:
		return PCM_CALL_FAILED;
	case V90_PHASE3_ANALOG_STATION_OUT_OF_RANGE:
		return PCM_CALL_OUT_OF_RANGE;
	case V90_PHASE3_ANALOG_STATION_CLIPPING_RISK:
		return PCM_CALL_CLIPPING_RISK;
	case V90_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH:
		return PCM_CALL_SEQUENCE_MISMATCH;
	case V90_PHASE3_ANALOG_STATION_COMPLETE:
	case V90_PHASE3_ANALOG_STATION_INVALID_STATE:
		return PCM_CALL_INVALID_STATE;
	case V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT:
	case V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE:
		break;
	}
	return PCM_CALL_NUMERIC_FAILURE;
}

static enum pcm_call_result digital_phase3_result(
	enum v90_phase3_digital_station_result result)
{
	switch (result) {
	case V90_PHASE3_DIGITAL_STATION_OK:
		return PCM_CALL_OK;
	case V90_PHASE3_DIGITAL_STATION_FAILED:
		return PCM_CALL_FAILED;
	case V90_PHASE3_DIGITAL_STATION_OUT_OF_RANGE:
		return PCM_CALL_OUT_OF_RANGE;
	case V90_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH:
		return PCM_CALL_SEQUENCE_MISMATCH;
	case V90_PHASE3_DIGITAL_STATION_UNSUPPORTED:
		return PCM_CALL_UNUSABLE_SELECTION;
	case V90_PHASE3_DIGITAL_STATION_COMPLETE:
	case V90_PHASE3_DIGITAL_STATION_INVALID_STATE:
		return PCM_CALL_INVALID_STATE;
	case V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT:
	case V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE:
		break;
	}
	return PCM_CALL_NUMERIC_FAILURE;
}

static enum pcm_call_result enter_mixed_phase3(struct pcm_call *call)
{
	struct pcm_call next = *call;

	next.state.mixed = (union pcm_call_mixed_state){ 0 };
	if (call->local_role == V90_ANALOG_MODEM) {
		struct v90_phase3_analog_receive_handoff handoff;
		struct pcm_call_analog_mixed_state mixed = {
			.phase3 = call->state.phase3.analog,
		};

		if (v90_phase3_analog_station_receive_handoff(
			    &call->state.phase3.analog, &handoff) !=
			    V90_PHASE3_ANALOG_STATION_OK)
			return PCM_CALL_INVALID_STATE;
		if (v90_phase4_ri_rx_init(
			    &mixed.receive, handoff.law,
			    handoff.training_ucode) != V90_PHASE4_RI_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		next.state.mixed.analog = mixed;
	} else {
		struct v90_phase3_digital_transmit_handoff handoff;
		struct pcm_call_digital_mixed_state mixed = {
			.phase3 = call->state.phase3.digital,
		};

		if (v90_phase3_digital_station_transmit_handoff(
			    &call->state.phase3.digital, &handoff) !=
			    V90_PHASE3_DIGITAL_STATION_OK)
			return PCM_CALL_INVALID_STATE;
		if (v90_phase4_ri_tx_init(
			    &mixed.transmit, handoff.law,
			    handoff.training_ucode) != V90_PHASE4_RI_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		next.state.mixed.digital = mixed;
	}
	next.stage = PCM_CALL_PHASE3_MIXED;
	if (!pcm_call_is_valid(&next))
		return PCM_CALL_NUMERIC_FAILURE;
	*call = next;
	return PCM_CALL_OK;
}

static enum pcm_call_result analog_phase4_result(
	enum v90_phase4_analog_station_result result)
{
	switch (result) {
	case V90_PHASE4_ANALOG_STATION_OK:
		return PCM_CALL_OK;
	case V90_PHASE4_ANALOG_STATION_INVALID_PARAMETERS:
		return PCM_CALL_UNUSABLE_SELECTION;
	case V90_PHASE4_ANALOG_STATION_OUT_OF_RANGE:
		return PCM_CALL_OUT_OF_RANGE;
	case V90_PHASE4_ANALOG_STATION_CLIPPING_RISK:
		return PCM_CALL_CLIPPING_RISK;
	case V90_PHASE4_ANALOG_STATION_SEQUENCE_MISMATCH:
		return PCM_CALL_SEQUENCE_MISMATCH;
	case V90_PHASE4_ANALOG_STATION_FAILED:
		return PCM_CALL_FAILED;
	case V90_PHASE4_ANALOG_STATION_COMPLETE:
	case V90_PHASE4_ANALOG_STATION_INVALID_STATE:
		return PCM_CALL_INVALID_STATE;
	case V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT:
	case V90_PHASE4_ANALOG_STATION_NUMERIC_FAILURE:
		break;
	}
	return PCM_CALL_NUMERIC_FAILURE;
}

static enum pcm_call_result enter_phase4(struct pcm_call *call)
{
	struct pcm_call next = *call;

	next.state.phase4 = (union pcm_call_phase4_state){ 0 };
	if (call->local_role == V90_ANALOG_MODEM) {
		const struct pcm_call_analog_mixed_state *mixed =
			&call->state.mixed.analog;
		struct v90_phase3_analog_receive_handoff receive_handoff;
		struct v34_training_line transmit_handoff;
		struct v90_phase4_analog_station station;
		struct v90_phase4_analog_startup startup;
		uint8_t selected_rate_index;
		enum pcm_call_result result;

		if (v90_phase3_analog_station_transmit_handoff(
			    &mixed->phase3, &transmit_handoff) !=
			    V90_PHASE3_ANALOG_STATION_OK ||
		    v90_phase3_analog_station_receive_handoff(
			    &mixed->phase3, &receive_handoff) !=
			    V90_PHASE3_ANALOG_STATION_OK)
			return PCM_CALL_INVALID_STATE;
		selected_rate_index = highest_downstream_rate(
			call->enabled_downstream_rate_mask &
			receive_handoff.jd.data_rate_capability_mask);
		if (selected_rate_index == 0)
			return PCM_CALL_UNUSABLE_SELECTION;
		next.analog_phase4_configuration.data_parameters
			.selected_rate_index = selected_rate_index;
		result = analog_phase4_result(v90_phase4_analog_station_resume(
			&station, &transmit_handoff, &mixed->receive,
			&receive_handoff.jd,
			&next.analog_phase4_configuration
				.training_parameters));
		if (result != PCM_CALL_OK)
			return result;
		if (v90_phase4_analog_startup_init(&startup, &station) !=
		    V90_PHASE4_STARTUP_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		next.state.phase4.analog = startup;
	} else {
		const struct pcm_call_digital_mixed_state *mixed =
			&call->state.mixed.digital;
		struct v90_phase3_return_line_handoff receive_handoff;
		struct v90_phase4_digital_station station;
		struct v90_phase4_digital_startup startup;

		if (v90_phase3_digital_station_receive_handoff(
			    &mixed->phase3, &receive_handoff) !=
			    V90_PHASE3_DIGITAL_STATION_OK)
			return PCM_CALL_INVALID_STATE;
		if (v90_phase4_digital_station_resume(
			    &station, &receive_handoff,
			    &call->phase3_configuration.digital.jd,
			    &mixed->transmit,
			    call->phase3_configuration.digital.receive
				    .tracking_equalizer_adaptation) !=
			    V90_PHASE4_DIGITAL_STATION_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		if (v90_phase4_digital_startup_init(
			    &startup, &station,
			    &call->digital_phase4_configuration.initial_mp) !=
		    V90_PHASE4_STARTUP_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		next.state.phase4.digital = startup;
	}
	next.stage = PCM_CALL_PHASE4;
	if (!pcm_call_is_valid(&next))
		return PCM_CALL_NUMERIC_FAILURE;
	*call = next;
	return PCM_CALL_OK;
}

static enum pcm_call_result advance_phase3(struct pcm_call *call)
{
	enum pcm_call_result result;

	if (call->stage == PCM_CALL_PHASE3) {
		bool leading_direction_complete =
			call->local_role == V90_ANALOG_MODEM ?
				v90_phase3_analog_station_receive_complete(
					&call->state.phase3.analog) :
				v90_phase3_digital_station_transmit_complete(
					&call->state.phase3.digital);

		if (!leading_direction_complete)
			return PCM_CALL_OK;
		result = enter_mixed_phase3(call);
		if (result != PCM_CALL_OK)
			return result;
	}
	if (call->local_role == V90_ANALOG_MODEM) {
		if (!v90_phase3_analog_station_transmit_complete(
			    &call->state.mixed.analog.phase3))
			return PCM_CALL_OK;
	} else if (!v90_phase3_digital_station_receive_complete(
			   &call->state.mixed.digital.phase3)) {
		return PCM_CALL_OK;
	}
	return enter_phase4(call);
}

static enum pcm_call_result sample_phase3(
	struct pcm_call *call, uint8_t received_octet,
	uint8_t *transmit_octet)
{
	enum pcm_call_result result;

	if (call->local_role == V90_ANALOG_MODEM) {
		int16_t sample;

		result = analog_phase3_result(
			v90_phase3_analog_station_transmit(
				&call->state.phase3.analog, &sample));
		if (result != PCM_CALL_OK)
			return result;
		*transmit_octet = encode_sample(call->law, sample);
		result = analog_phase3_result(
			v90_phase3_analog_station_receive(
				&call->state.phase3.analog, received_octet));
	} else {
		result = digital_phase3_result(
			v90_phase3_digital_station_transmit(
				&call->state.phase3.digital, transmit_octet));
		if (result != PCM_CALL_OK)
			return result;
		result = digital_phase3_result(
			v90_phase3_digital_station_receive(
				&call->state.phase3.digital,
				decode_octet(call->law, received_octet)));
	}
	return result == PCM_CALL_OK ? advance_phase3(call) : result;
}

static enum pcm_call_result receive_mixed_ri(
	struct v90_phase4_ri_rx *receive, uint8_t network_octet)
{
	struct v90_phase4_ri_rx next = *receive;
	enum v90_phase4_ri_result result =
		v90_phase4_ri_rx_receive(&next, network_octet);

	if (result == V90_PHASE4_RI_OK) {
		*receive = next;
		return PCM_CALL_OK;
	}
	if (result == V90_PHASE4_RI_TRANSITION_DETECTED ||
	    result == V90_PHASE4_RI_INVALID_SEQUENCE)
		return PCM_CALL_SEQUENCE_MISMATCH;
	return result == V90_PHASE4_RI_DONE ?
		       PCM_CALL_INVALID_STATE : PCM_CALL_NUMERIC_FAILURE;
}

static enum pcm_call_result sample_mixed_phase3(
	struct pcm_call *call, uint8_t received_octet,
	uint8_t *transmit_octet)
{
	enum pcm_call_result result;

	if (call->local_role == V90_ANALOG_MODEM) {
		struct pcm_call_analog_mixed_state *mixed =
			&call->state.mixed.analog;
		int16_t sample;

		result = analog_phase3_result(
			v90_phase3_analog_station_transmit(
				&mixed->phase3, &sample));
		if (result != PCM_CALL_OK)
			return result;
		*transmit_octet = encode_sample(call->law, sample);
		result = receive_mixed_ri(&mixed->receive, received_octet);
	} else {
		struct pcm_call_digital_mixed_state *mixed =
			&call->state.mixed.digital;

		if (v90_phase4_ri_tx_next(
			    &mixed->transmit, transmit_octet) != V90_PHASE4_RI_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		result = digital_phase3_result(
			v90_phase3_digital_station_receive(
				&mixed->phase3,
				decode_octet(call->law, received_octet)));
	}
	return result == PCM_CALL_OK ? advance_phase3(call) : result;
}

static enum pcm_call_result phase4_startup_result(
	enum v90_phase4_startup_result result)
{
	switch (result) {
	case V90_PHASE4_STARTUP_OK:
		return PCM_CALL_OK;
	case V90_PHASE4_STARTUP_FAILED:
		return PCM_CALL_FAILED;
	case V90_PHASE4_STARTUP_INVALID_PARAMETERS:
		return PCM_CALL_UNUSABLE_SELECTION;
	case V90_PHASE4_STARTUP_OUT_OF_RANGE:
		return PCM_CALL_OUT_OF_RANGE;
	case V90_PHASE4_STARTUP_CLIPPING_RISK:
		return PCM_CALL_CLIPPING_RISK;
	case V90_PHASE4_STARTUP_SEQUENCE_MISMATCH:
		return PCM_CALL_SEQUENCE_MISMATCH;
	case V90_PHASE4_STARTUP_DONE:
	case V90_PHASE4_STARTUP_INVALID_STATE:
		return PCM_CALL_INVALID_STATE;
	case V90_PHASE4_STARTUP_INVALID_ARGUMENT:
	case V90_PHASE4_STARTUP_NUMERIC_FAILURE:
		break;
	}
	return PCM_CALL_NUMERIC_FAILURE;
}

static enum pcm_call_result sample_phase4(
	struct pcm_call *call, uint8_t received_octet,
	uint8_t *transmit_octet)
{
	enum v90_phase4_startup_result result =
		call->local_role == V90_ANALOG_MODEM ?
			v90_phase4_analog_startup_octet(
				&call->state.phase4.analog,
				received_octet, transmit_octet) :
			v90_phase4_digital_startup_octet(
				&call->state.phase4.digital,
				received_octet, transmit_octet);

	return phase4_startup_result(result);
}

static enum pcm_call_result analog_path_result(
	enum v90_analog_path_result result)
{
	switch (result) {
	case V90_ANALOG_PATH_OK:
	case V90_ANALOG_PATH_DOWNSTREAM_FRAME:
		return PCM_CALL_OK;
	case V90_ANALOG_PATH_SOURCE_FAILURE:
		return PCM_CALL_SOURCE_FAILURE;
	case V90_ANALOG_PATH_FAILED:
		return PCM_CALL_FAILED;
	case V90_ANALOG_PATH_SEQUENCE_MISMATCH:
		return PCM_CALL_SEQUENCE_MISMATCH;
	case V90_ANALOG_PATH_INVALID_PARAMETERS:
	case V90_ANALOG_PATH_NO_COMMON_RATE:
		return PCM_CALL_UNUSABLE_SELECTION;
	case V90_ANALOG_PATH_OUT_OF_RANGE:
		return PCM_CALL_OUT_OF_RANGE;
	case V90_ANALOG_PATH_CLIPPING_RISK:
		return PCM_CALL_CLIPPING_RISK;
	case V90_ANALOG_PATH_INVALID_ARGUMENT:
	case V90_ANALOG_PATH_INVALID_STATE:
	case V90_ANALOG_PATH_NUMERIC_FAILURE:
	case V90_ANALOG_PATH_INTERNAL_ERROR:
		break;
	}
	return PCM_CALL_NUMERIC_FAILURE;
}

static enum pcm_call_result digital_path_result(
	enum v90_digital_path_result result)
{
	switch (result) {
	case V90_DIGITAL_PATH_OK:
	case V90_DIGITAL_PATH_UPSTREAM_FRAME:
		return PCM_CALL_OK;
	case V90_DIGITAL_PATH_SOURCE_FAILURE:
		return PCM_CALL_SOURCE_FAILURE;
	case V90_DIGITAL_PATH_FAILED:
		return PCM_CALL_FAILED;
	case V90_DIGITAL_PATH_SEQUENCE_MISMATCH:
		return PCM_CALL_SEQUENCE_MISMATCH;
	case V90_DIGITAL_PATH_INVALID_PARAMETERS:
	case V90_DIGITAL_PATH_NO_COMMON_RATE:
		return PCM_CALL_UNUSABLE_SELECTION;
	case V90_DIGITAL_PATH_OUT_OF_RANGE:
		return PCM_CALL_OUT_OF_RANGE;
	case V90_DIGITAL_PATH_INVALID_ARGUMENT:
	case V90_DIGITAL_PATH_NUMERIC_FAILURE:
	case V90_DIGITAL_PATH_INTERNAL_ERROR:
		break;
	}
	return PCM_CALL_NUMERIC_FAILURE;
}

enum pcm_call_result pcm_call_start_data_path(struct pcm_call *call)
{
	struct pcm_call next;
	enum pcm_call_result result;

	if (!pcm_call_is_valid(call))
		return PCM_CALL_INVALID_ARGUMENT;
	if (call->failed)
		return PCM_CALL_FAILED;
	if (!pcm_call_phase4_is_ready(call))
		return PCM_CALL_INVALID_STATE;
	next = *call;
	next.state = (union pcm_call_state){ 0 };
	if (call->local_role == V90_ANALOG_MODEM) {
		struct v90_phase4_analog_handoff handoff;
		struct v90_analog_path path;

		if (v90_phase4_analog_startup_handoff(
			    &call->state.phase4.analog, &handoff) !=
		    V90_PHASE4_STARTUP_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		result = analog_path_result(v90_analog_path_init(
			&path, &handoff,
			&call->analog_phase4_configuration.data_parameters));
		if (result != PCM_CALL_OK)
			return result;
		next.state.data_path.analog = path;
	} else {
		struct v90_phase4_digital_handoff handoff;
		struct v90_digital_path path;

		if (v90_phase4_digital_startup_handoff(
			    &call->state.phase4.digital, &handoff) !=
		    V90_PHASE4_STARTUP_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		result = digital_path_result(
			v90_digital_path_init(
				&path, &handoff,
				call->enabled_downstream_rate_mask));
		if (result != PCM_CALL_OK)
			return result;
		next.state.data_path.digital = path;
	}
	next.stage = PCM_CALL_DATA_PATH;
	if (!pcm_call_is_valid(&next))
		return PCM_CALL_NUMERIC_FAILURE;
	*call = next;
	return PCM_CALL_OK;
}

static bool outputs_overlap(
	const struct pcm_call *call, const uint8_t *transmit_octet,
	const struct pcm_call_observation *observation)
{
	return memory_regions_overlap(
		       call, sizeof(*call),
		       transmit_octet, sizeof(*transmit_octet)) ||
	       memory_regions_overlap(
		       call, sizeof(*call),
		       observation, sizeof(*observation)) ||
	       memory_regions_overlap(
		       transmit_octet, sizeof(*transmit_octet),
		       observation, sizeof(*observation));
}

static bool data_outputs_overlap(
	const struct pcm_call *call, const uint8_t *transmit_octet,
	const struct pcm_call_data_event *event)
{
	return memory_regions_overlap(
		       call, sizeof(*call),
		       transmit_octet, sizeof(*transmit_octet)) ||
	       memory_regions_overlap(
		       call, sizeof(*call), event, sizeof(*event)) ||
	       memory_regions_overlap(
		       transmit_octet, sizeof(*transmit_octet),
		       event, sizeof(*event));
}

static const struct v90_pcm_mapping *online_downstream_mapping(
	const struct pcm_call *call)
{
	return call->local_role == V90_ANALOG_MODEM ?
		       &call->state.data_path.analog.data_mapping :
		       &call->state.data_path.digital.data_mapping;
}

static uint8_t online_upstream_rate_units(
	const struct pcm_call *call)
{
	if (call->local_role == V90_ANALOG_MODEM)
		return call->state.data_path.analog.upstream.data.transmitter
			.points.schedule.geometry
			.primary_data_rate_2400_bps_units;
	return call->state.data_path.digital.upstream.data.geometry
		.primary_data_rate_2400_bps_units;
}

bool pcm_call_get_data_rates(
	const struct pcm_call *call,
	uint32_t *transmit_bits_per_second,
	uint32_t *receive_bits_per_second)
{
	const struct v90_pcm_mapping *downstream;
	uint8_t upstream_units;
	uint32_t downstream_rate;
	uint32_t upstream_rate;

	if (!pcm_call_is_online(call) || transmit_bits_per_second == NULL ||
	    receive_bits_per_second == NULL ||
	    memory_regions_overlap(
		    transmit_bits_per_second,
		    sizeof(*transmit_bits_per_second),
		    receive_bits_per_second,
		    sizeof(*receive_bits_per_second)) ||
	    memory_regions_overlap(
		    call, sizeof(*call),
		    transmit_bits_per_second,
		    sizeof(*transmit_bits_per_second)) ||
	    memory_regions_overlap(
		    call, sizeof(*call),
		    receive_bits_per_second,
		    sizeof(*receive_bits_per_second)))
		return false;
	downstream = online_downstream_mapping(call);
	upstream_units = online_upstream_rate_units(call);
	if (upstream_units == 0 ||
	    !v90_pcm_mapping_is_valid(downstream) ||
	    downstream->kind != V90_CP_DATA)
		return false;
	upstream_rate =
		(uint32_t)upstream_units * V90_UPSTREAM_RATE_UNIT;
	downstream_rate =
		((uint32_t)downstream->frame_bits *
		 V90_NETWORK_CHARACTERS_PER_SECOND +
		 V90_CP_FRAME_INTERVALS / 2u) /
		V90_CP_FRAME_INTERVALS;
	if (call->local_role == V90_ANALOG_MODEM) {
		*transmit_bits_per_second = upstream_rate;
		*receive_bits_per_second = downstream_rate;
	} else {
		*transmit_bits_per_second = downstream_rate;
		*receive_bits_per_second = upstream_rate;
	}
	return true;
}

enum pcm_call_result pcm_call_data_octet(
	struct pcm_call *call, uint8_t received_octet,
	const struct pcm_call_data_sources *sources,
	uint8_t *transmit_octet,
	struct pcm_call_data_event *event)
{
	const struct pcm_call_data_sources idle = { 0 };
	struct pcm_call_data_event emitted = { 0 };
	uint8_t generated;
	enum pcm_call_result result;

	if (!pcm_call_is_valid(call) || transmit_octet == NULL ||
	    event == NULL || data_outputs_overlap(call, transmit_octet, event))
		return PCM_CALL_INVALID_ARGUMENT;
	if (call->failed)
		return PCM_CALL_FAILED;
	if (call->stage != PCM_CALL_DATA_PATH)
		return PCM_CALL_INVALID_STATE;
	if (sources == NULL)
		sources = &idle;
	generated = encode_sample(call->law, 0);
	if (call->local_role == V90_ANALOG_MODEM) {
		struct v90_pcm_stream_frame frame = { 0 };
		enum v90_analog_path_result path_result =
			v90_analog_path_transmit(
				&call->state.data_path.analog,
				sources->upstream, sources->upstream_context,
				&generated);

		result = analog_path_result(path_result);
		if (result == PCM_CALL_SOURCE_FAILURE)
			return result;
		if (result != PCM_CALL_OK) {
			call->failed = true;
			return pcm_call_is_valid(call) ?
				       result : PCM_CALL_NUMERIC_FAILURE;
		}
		path_result = v90_analog_path_receive(
			&call->state.data_path.analog, received_octet, &frame);
		result = analog_path_result(path_result);
		if (path_result == V90_ANALOG_PATH_DOWNSTREAM_FRAME) {
			emitted.frame.downstream = frame;
			emitted.kind = PCM_CALL_DOWNSTREAM_FRAME;
		}
	} else {
		struct v34_received_mapping_frame frame = { 0 };
		enum v90_digital_path_result path_result =
			v90_digital_path_octet(
				&call->state.data_path.digital, received_octet,
				sources->downstream,
				sources->downstream_context,
				&generated, &frame);

		result = digital_path_result(path_result);
		if (result == PCM_CALL_SOURCE_FAILURE)
			return result;
		if (path_result == V90_DIGITAL_PATH_INVALID_ARGUMENT) {
			call->failed = true;
			return PCM_CALL_NUMERIC_FAILURE;
		}
		if (path_result == V90_DIGITAL_PATH_UPSTREAM_FRAME) {
			emitted.frame.upstream = frame;
			emitted.kind = PCM_CALL_UPSTREAM_FRAME;
		}
	}
	if (result != PCM_CALL_OK)
		call->failed = true;
	if (!pcm_call_is_valid(call)) {
		call->failed = true;
		return PCM_CALL_NUMERIC_FAILURE;
	}
	*transmit_octet = generated;
	*event = emitted;
	return result;
}

enum pcm_call_result pcm_call_octet(
	struct pcm_call *call, uint8_t received_octet,
	uint8_t *transmit_octet,
	struct pcm_call_observation *observation)
{
	struct pcm_call_observation emitted = { 0 };
	uint8_t generated;
	enum pcm_call_result result;

	if (!pcm_call_is_valid(call) || transmit_octet == NULL ||
	    observation == NULL ||
	    outputs_overlap(call, transmit_octet, observation))
		return PCM_CALL_INVALID_ARGUMENT;
	if (call->failed)
		return PCM_CALL_FAILED;
	if (call->stage == PCM_CALL_DATA_PATH)
		return PCM_CALL_INVALID_STATE;
	if (pcm_call_phase4_is_ready(call))
		return PCM_CALL_PHASE4_READY;
	generated = encode_sample(call->law, 0);
	emitted.processed = call->stage;
	switch (call->stage) {
	case PCM_CALL_PHASE2:
		result = sample_phase2(
			call, received_octet, &generated, &emitted);
		break;
	case PCM_CALL_PHASE3:
		result = sample_phase3(
			call, received_octet, &generated);
		break;
	case PCM_CALL_PHASE3_MIXED:
		result = sample_mixed_phase3(
			call, received_octet, &generated);
		break;
	case PCM_CALL_PHASE4:
		result = sample_phase4(
			call, received_octet, &generated);
		break;
	default:
		return PCM_CALL_NUMERIC_FAILURE;
	}
	if (result != PCM_CALL_OK)
		call->failed = true;
	emitted.next = call->stage;
	if (!pcm_call_is_valid(call)) {
		call->failed = true;
		return PCM_CALL_NUMERIC_FAILURE;
	}
	*transmit_octet = generated;
	*observation = emitted;
	return result;
}

const char *pcm_call_result_name(enum pcm_call_result result)
{
	switch (result) {
	case PCM_CALL_OK:
		return "ok";
	case PCM_CALL_PHASE4_READY:
		return "phase 4 ready";
	case PCM_CALL_SOURCE_FAILURE:
		return "frame source failure";
	case PCM_CALL_FAILED:
		return "failed";
	case PCM_CALL_INVALID_ARGUMENT:
		return "invalid argument";
	case PCM_CALL_INVALID_STATE:
		return "invalid state";
	case PCM_CALL_INVALID_SELECTION:
		return "invalid selection";
	case PCM_CALL_UNUSABLE_SELECTION:
		return "unusable selection";
	case PCM_CALL_MEDIA_MISMATCH:
		return "media mismatch";
	case PCM_CALL_OUT_OF_RANGE:
		return "out of range";
	case PCM_CALL_CLIPPING_RISK:
		return "clipping risk";
	case PCM_CALL_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case PCM_CALL_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
