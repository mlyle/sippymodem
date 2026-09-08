// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase4_e2u.h"

#include "g711.h"
#include "memory_region.h"

#include <stddef.h>

static bool downstream_stage_is_valid(
	enum v92_phase4_e2u_downstream_stage stage)
{
	return stage == V92_PHASE4_E2U_DOWNSTREAM_TRAINING ||
	       stage == V92_PHASE4_E2U_DOWNSTREAM_DATA;
}

static bool downstream_mapping(
	const struct v92_cpu_parameters *cpu,
	struct v90_pcm_mapping *mapping)
{
	return v90_pcm_mapping_init_from_v92_cpu(mapping, cpu) ==
	       V90_PCM_MAPPING_OK;
}

static bool analog_state_is_valid(
	const struct v92_phase4_e2u_analog_state *state)
{
	struct v90_pcm_mapping data_mapping;
	unsigned int bits;
	size_t expected_symbols;

	if (!v92_phase4_record_tx_is_valid(&state->transmit) ||
	    !downstream_stage_is_valid(state->downstream_stage) ||
	    !v92_cpu_parameters_are_valid(&state->cpu) ||
	    !v92_pcm_mapping_is_valid(&state->mapping) ||
	    state->mapping.law != state->cpu.law ||
	    !downstream_mapping(&state->cpu, &data_mapping))
		return false;
	if (state->downstream_stage == V92_PHASE4_E2U_DOWNSTREAM_TRAINING) {
		const struct pcm_downstream_rx *training =
			&state->downstream.training;

		if (!pcm_downstream_rx_is_valid(training) ||
		    pcm_downstream_rx_has_failed(training) ||
		    training->protocol != PCM_PROTOCOL_V92 ||
		    training->stream.decoder.mapping.law != state->cpu.law)
			return false;
	} else if (!v90_downstream_data_rx_is_valid(
			   &state->downstream.data) ||
		   !v90_pcm_mapping_equal(
			   &state->downstream.data.stream.decoder.mapping,
			   &data_mapping)) {
		return false;
	}
	bits = v92_phase4_modulator_bits_per_symbol(
		state->transmit.modulator.constellation);
	expected_symbols = state->mapping.cpd.extend_e2u ?
		V92_PHASE4_E2U_EXTENDED_SYMBOLS : V92_PHASE4_E2U_SYMBOLS;
	return bits != 0u &&
	       state->transmit.frame_bit_count == expected_symbols * bits;
}

static bool digital_state_is_valid(
	const struct v92_phase4_e2u_digital_state *state)
{
	struct v90_pcm_mapping data_mapping;
	bool downstream_valid;

	if (!downstream_stage_is_valid(state->downstream_stage) ||
	    !v92_cpu_parameters_are_valid(&state->cpu) ||
	    !downstream_mapping(&state->cpu, &data_mapping))
		return false;
	if (state->downstream_stage == V92_PHASE4_E2U_DOWNSTREAM_TRAINING) {
		const struct pcm_downstream_tx *training =
			&state->downstream.training;

		downstream_valid = pcm_downstream_tx_is_valid(training) &&
			training->protocol == PCM_PROTOCOL_V92 &&
			training->ed_requested &&
			state->cpu.law == training->encoder.mapping.law;
	} else {
		downstream_valid = v90_downstream_data_stream_is_valid(
			&state->downstream.data) &&
			v90_pcm_mapping_equal(
				&state->downstream.data.transmitter.encoder.mapping,
				&data_mapping);
	}
	return v92_e2u_rx_is_valid(&state->receive) &&
	       v92_upstream_control_line_rx_is_valid(&state->control) &&
	       !v92_upstream_control_line_rx_has_failed(&state->control) &&
	       downstream_valid &&
	       v92_pcm_mapping_is_valid(&state->mapping) &&
	       state->mapping.law == state->cpu.law &&
	       state->receive.signal.line.law == state->mapping.law &&
	       state->control.signal.line.law == state->mapping.law &&
	       state->receive.expected_symbols ==
		       (state->mapping.cpd.extend_e2u ?
			V92_PHASE4_E2U_EXTENDED_SYMBOLS :
			V92_PHASE4_E2U_SYMBOLS);
}

bool v92_phase4_e2u_is_valid(const struct v92_phase4_e2u *owner)
{
	bool child_valid;
	bool child_failed;

	if (owner == NULL || !pcm_modem_role_is_valid(owner->local_role))
		return false;
	if (owner->local_role == V90_ANALOG_MODEM) {
		child_valid = analog_state_is_valid(&owner->state.analog);
		child_failed = false;
	} else {
		child_valid = digital_state_is_valid(&owner->state.digital);
		child_failed = v92_e2u_rx_has_failed(
			&owner->state.digital.receive);
	}
	return child_valid && (owner->failed || !child_failed);
}

bool v92_phase4_e2u_has_failed(const struct v92_phase4_e2u *owner)
{
	return v92_phase4_e2u_is_valid(owner) && owner->failed;
}

bool v92_phase4_e2u_is_complete(const struct v92_phase4_e2u *owner)
{
	if (!v92_phase4_e2u_is_valid(owner) || owner->failed)
		return false;
	return owner->local_role == V90_ANALOG_MODEM ?
		v92_phase4_record_tx_is_complete(
			&owner->state.analog.transmit) :
		v92_e2u_rx_is_complete(&owner->state.digital.receive);
}

static bool arm_analog_ed(struct pcm_downstream_rx *receiver)
{
	enum pcm_downstream_rx_result result;

	if (receiver->ed_requested)
		return true;
	result = pcm_downstream_rx_expect_ed(receiver);
	return result == PCM_DOWNSTREAM_RX_OK;
}

enum v92_phase4_e2u_result v92_phase4_e2u_init_analog(
	struct v92_phase4_e2u *owner,
	const struct v92_phase4_analog_handoff *handoff)
{
	struct v92_phase4_e2u initialized = { 0 };
	enum pcm_downstream_rx_result ed_result;

	if (owner == NULL || handoff == NULL ||
	    memory_regions_overlap(owner, sizeof(*owner),
				   handoff, sizeof(*handoff)))
		return V92_PHASE4_E2U_INVALID_ARGUMENT;
	if (!v92_phase4_analog_handoff_is_valid(handoff))
		return V92_PHASE4_E2U_INVALID_PARAMETERS;
	initialized.local_role = V90_ANALOG_MODEM;
	initialized.state.analog.downstream.training = handoff->receive;
	initialized.state.analog.cpu = handoff->cpu;
	initialized.state.analog.mapping = handoff->mapping;
	initialized.state.analog.downstream_stage =
		V92_PHASE4_E2U_DOWNSTREAM_TRAINING;
	if (!pcm_downstream_rx_is_complete(
		    &initialized.state.analog.downstream.training)) {
		ed_result = pcm_downstream_rx_expect_ed(
			&initialized.state.analog.downstream.training);
		/* A fresh handoff is the complete control boundary Ed needs. A
		 * validated mid-record handoff is armed when that record finishes. */
		if (ed_result != PCM_DOWNSTREAM_RX_OK &&
		    (ed_result != PCM_DOWNSTREAM_RX_INVALID_STATE ||
		     initialized.state.analog.downstream.training.stage !=
			     PCM_DOWNSTREAM_RX_V92_CONTROL ||
		     initialized.state.analog.downstream.training.ed_requested))
			return V92_PHASE4_E2U_INVALID_PARAMETERS;
	}
	if (v92_phase4_record_tx_init_e2u(
		    &initialized.state.analog.transmit, &handoff->transmit,
		    handoff->mapping.cpd.extend_e2u) !=
		    V92_PHASE4_RECORD_TX_OK ||
	    !v92_phase4_e2u_is_valid(&initialized))
		return V92_PHASE4_E2U_INVALID_PARAMETERS;
	*owner = initialized;
	return V92_PHASE4_E2U_OK;
}

enum v92_phase4_e2u_result v92_phase4_e2u_init_digital(
	struct v92_phase4_e2u *owner,
	const struct v92_phase4_digital_handoff *handoff)
{
	struct v92_phase4_e2u initialized = { 0 };

	if (owner == NULL || handoff == NULL ||
	    memory_regions_overlap(owner, sizeof(*owner),
				   handoff, sizeof(*handoff)))
		return V92_PHASE4_E2U_INVALID_ARGUMENT;
	if (!v92_phase4_digital_handoff_is_valid(handoff))
		return V92_PHASE4_E2U_INVALID_PARAMETERS;
	initialized.local_role = V90_DIGITAL_MODEM;
	initialized.state.digital.control = handoff->receive;
	initialized.state.digital.downstream.training = handoff->transmit;
	initialized.state.digital.cpu = handoff->cpu;
	initialized.state.digital.mapping = handoff->mapping;
	initialized.state.digital.downstream_stage =
		V92_PHASE4_E2U_DOWNSTREAM_TRAINING;
	if (v92_e2u_rx_init(
		    &initialized.state.digital.receive,
		    &handoff->receive.signal, handoff->mapping.cpd.extend_e2u) !=
		    V92_E2U_RX_OK ||
	    pcm_downstream_tx_request_ed(
		    &initialized.state.digital.downstream.training) !=
		    PCM_DOWNSTREAM_TX_OK ||
	    !v92_phase4_e2u_is_valid(&initialized))
		return V92_PHASE4_E2U_INVALID_PARAMETERS;
	*owner = initialized;
	return V92_PHASE4_E2U_OK;
}

static bool start_analog_downstream_data(
	union v92_phase4_e2u_analog_downstream *downstream,
	enum v92_phase4_e2u_downstream_stage *stage,
	const struct v92_cpu_parameters *cpu)
{
	struct v90_downstream_data_rx data;
	struct v90_pcm_mapping mapping;

	if (!downstream_mapping(cpu, &mapping) ||
	    v90_downstream_data_rx_start_after_ed(
		    &data, &downstream->training, &mapping) !=
		    V90_DOWNSTREAM_DATA_RX_OK)
		return false;
	*downstream = (union v92_phase4_e2u_analog_downstream){
		.data = data,
	};
	*stage = V92_PHASE4_E2U_DOWNSTREAM_DATA;
	return true;
}

static bool retain_matching_cpd(
	struct v92_pcm_mapping *retained,
	const struct v92_cpd_parameters *parameters)
{
	struct v92_pcm_mapping candidate;
	bool full;
	bool acknowledged;

	if (!v92_cpd_parameters_are_valid(parameters) ||
	    retained->cpd.four_times_gain_q0_16 !=
		    parameters->four_times_gain_q0_16 ||
	    retained->cpd.data_rate_number != parameters->data_rate_number ||
	    retained->cpd.trellis != parameters->trellis ||
	    retained->cpd.extend_e2u != parameters->extend_e2u ||
	    (retained->cpd.acknowledges_cpu &&
	     !parameters->acknowledges_cpu))
		return false;
	full = parameters->modulus_present && parameters->filters_present &&
	       parameters->constellations_present;
	if (!full) {
		if (parameters->modulus_present || parameters->filters_present ||
		    parameters->constellations_present)
			return false;
		retained->cpd.acknowledges_cpu |=
			parameters->acknowledges_cpu;
		return true;
	}
	if (v92_pcm_mapping_init(
		    &candidate, retained->law, parameters) != V92_PCM_MAPPING_OK)
		return false;
	acknowledged = retained->cpd.acknowledges_cpu ||
		       candidate.cpd.acknowledges_cpu;
	retained->cpd.acknowledges_cpu = acknowledged;
	candidate.cpd.acknowledges_cpu = acknowledged;
	return v92_pcm_mapping_equal(retained, &candidate);
}

enum v90_downstream_data_rx_result v92_downstream_receive_owned(
	union v92_phase4_e2u_analog_downstream *downstream,
	enum v92_phase4_e2u_downstream_stage *stage,
	const struct v92_cpu_parameters *cpu, struct v92_pcm_mapping *mapping,
	uint8_t octet, struct v90_pcm_stream_frame *frame)
{
	struct pcm_downstream_event event = { 0 };
	enum pcm_downstream_rx_result result;
	struct pcm_downstream_rx *receive;

	if (*stage == V92_PHASE4_E2U_DOWNSTREAM_DATA)
		return v90_downstream_data_rx_receive_octet(
			&downstream->data, octet, frame);
	receive = &downstream->training;
	if (pcm_downstream_rx_is_complete(receive)) {
		if (!start_analog_downstream_data(downstream, stage, cpu))
			return V90_DOWNSTREAM_DATA_RX_INTERNAL_ERROR;
		return v90_downstream_data_rx_receive_octet(
			&downstream->data, octet, frame);
	}
	result = pcm_downstream_rx_receive(receive, octet, &event);
	if (result == PCM_DOWNSTREAM_RX_ED_REJECTED ||
	    result == PCM_DOWNSTREAM_RX_INVALID_ARGUMENT ||
	    result == PCM_DOWNSTREAM_RX_INVALID_PARAMETERS ||
	    result == PCM_DOWNSTREAM_RX_INVALID_STATE ||
	    result == PCM_DOWNSTREAM_RX_INVALID_SYMBOL ||
	    result == PCM_DOWNSTREAM_RX_OUT_OF_RANGE ||
	    result == PCM_DOWNSTREAM_RX_INTERNAL_ERROR ||
	    result == PCM_DOWNSTREAM_RX_MP ||
	    result == PCM_DOWNSTREAM_RX_MP_PRIME ||
	    result == PCM_DOWNSTREAM_RX_MP_REJECTED)
		return V90_DOWNSTREAM_DATA_RX_INVALID_SYMBOL;
	if (result == PCM_DOWNSTREAM_RX_CPD) {
		if (event.control.type != V92_CONTROL_EVENT_CPD ||
		    event.control.record.cpd.frame_result != V92_CPD_OK ||
		    !retain_matching_cpd(
			    mapping,
			    &event.control.record.cpd.parameters))
			return V90_DOWNSTREAM_DATA_RX_INVALID_SYMBOL;
		return arm_analog_ed(receive) ? V90_DOWNSTREAM_DATA_RX_BUFFERING :
			V90_DOWNSTREAM_DATA_RX_INTERNAL_ERROR;
	}
	if (result == PCM_DOWNSTREAM_RX_SUVD) {
		/* SUVd carries status only; CPd owns the retained selection. */
		if (event.control.type != V92_CONTROL_EVENT_SUVD ||
		    event.control.record.suvd.frame_result != V92_SUVD_OK ||
		    !event.control.record.suvd.parameters.acknowledges_cpu)
			return V90_DOWNSTREAM_DATA_RX_INVALID_SYMBOL;
		return arm_analog_ed(receive) ? V90_DOWNSTREAM_DATA_RX_BUFFERING :
			V90_DOWNSTREAM_DATA_RX_INTERNAL_ERROR;
	}
	if (result == PCM_DOWNSTREAM_RX_CPD_REJECTED ||
	    result == PCM_DOWNSTREAM_RX_SUVD_REJECTED)
		return V90_DOWNSTREAM_DATA_RX_BUFFERING;
	if (result == PCM_DOWNSTREAM_RX_ED)
		return start_analog_downstream_data(downstream, stage, cpu) ?
			V90_DOWNSTREAM_DATA_RX_BUFFERING :
			V90_DOWNSTREAM_DATA_RX_INTERNAL_ERROR;
	return result == PCM_DOWNSTREAM_RX_BUFFERING ?
		V90_DOWNSTREAM_DATA_RX_BUFFERING :
		V90_DOWNSTREAM_DATA_RX_INVALID_SYMBOL;
}

static bool start_digital_downstream_data(
	union v92_phase4_e2u_digital_downstream *downstream,
	enum v92_phase4_e2u_downstream_stage *stage,
	const struct v92_cpu_parameters *cpu)
{
	struct v90_downstream_data_stream data;
	struct v90_pcm_mapping mapping;

	if (!downstream_mapping(cpu, &mapping) ||
	    v90_downstream_data_stream_start_after_ed(
		    &data, &downstream->training, &mapping) !=
		    V90_DOWNSTREAM_DATA_STREAM_OK)
		return false;
	*downstream = (union v92_phase4_e2u_digital_downstream){
		.data = data,
	};
	*stage = V92_PHASE4_E2U_DOWNSTREAM_DATA;
	return true;
}

enum v90_downstream_data_stream_result v92_downstream_transmit_owned(
	union v92_phase4_e2u_digital_downstream *downstream,
	enum v92_phase4_e2u_downstream_stage *stage,
	const struct v92_cpu_parameters *cpu,
	v90_downstream_frame_source source, void *context, uint8_t *octet)
{
	struct v90_pcm_symbol symbol;
	enum v90_downstream_data_stream_result data_result;

	if (*stage == V92_PHASE4_E2U_DOWNSTREAM_TRAINING) {
		struct pcm_downstream_symbol training_symbol;
		enum pcm_downstream_tx_result result = pcm_downstream_tx_next(
			&downstream->training, &training_symbol);

		if (result == PCM_DOWNSTREAM_TX_OK) {
			*octet = training_symbol.pcm.octet;
			return V90_DOWNSTREAM_DATA_STREAM_OK;
		}
		if (result != PCM_DOWNSTREAM_TX_COMPLETE ||
		    !start_digital_downstream_data(downstream, stage, cpu))
			return V90_DOWNSTREAM_DATA_STREAM_INTERNAL_ERROR;
	}
	data_result = v90_downstream_data_stream_next(
		&downstream->data, source, context, &symbol);
	if (data_result != V90_DOWNSTREAM_DATA_STREAM_OK)
		return data_result;
	*octet = symbol.octet;
	return V90_DOWNSTREAM_DATA_STREAM_OK;
}

static bool retain_matching_cpu(
	struct v92_cpu_parameters *retained,
	const struct v92_cpu_parameters *parameters)
{
	struct v90_pcm_mapping retained_mapping;
	struct v90_pcm_mapping candidate_mapping;

	if (!v92_cpu_parameters_are_valid(parameters) ||
	    retained->trn1d_ratio_q3_13 != parameters->trn1d_ratio_q3_13 ||
	    (retained->acknowledges_cpd && !parameters->acknowledges_cpd) ||
	    !downstream_mapping(retained, &retained_mapping) ||
	    !downstream_mapping(parameters, &candidate_mapping) ||
	    !v90_pcm_mapping_equal(&retained_mapping, &candidate_mapping))
		return false;
	retained->acknowledges_cpd |= parameters->acknowledges_cpd;
	return true;
}

static bool control_event_rearms_trial(
	struct v92_phase4_e2u_digital_state *state,
	const struct v92_control_event *event)
{
	switch (event->type) {
	case V92_CONTROL_EVENT_SUVU:
		return event->record.suvu.frame_result == V92_SUVU_OK &&
		       v92_suvu_parameters_are_valid(
			       &event->record.suvu.parameters);
	case V92_CONTROL_EVENT_CPU:
		return event->record.cpu.frame_result == V92_CPU_OK &&
		       retain_matching_cpu(
			       &state->cpu, &event->record.cpu.parameters);
	case V92_CONTROL_EVENT_SUVU_REJECTED:
	case V92_CONTROL_EVENT_CPU_REJECTED:
		return true;
	case V92_CONTROL_EVENT_NONE:
	case V92_CONTROL_EVENT_SUVD:
	case V92_CONTROL_EVENT_CPD:
	case V92_CONTROL_EVENT_SUVD_REJECTED:
	case V92_CONTROL_EVENT_CPD_REJECTED:
		return false;
	}
	return false;
}

static enum v92_phase4_e2u_result digital_receive(
	struct v92_phase4_e2u_digital_state *state, int16_t sample)
{
	struct v92_upstream_control_line_rx control = state->control;
	struct v92_control_event event = { 0 };
	enum v92_upstream_control_line_rx_result control_result;
	enum v92_e2u_rx_result trial_result = V92_E2U_RX_NO_MATCH;
	bool trial_active = state->receive.received_symbols <
		state->receive.expected_symbols;

	control_result = v92_upstream_control_line_rx_sample(
		&control, sample, &event);
	if (trial_active) {
		trial_result = v92_e2u_rx_sample(&state->receive, sample);
		if (trial_result == V92_E2U_RX_COMPLETE)
			return V92_PHASE4_E2U_COMPLETE;
		if (trial_result != V92_E2U_RX_OK &&
		    trial_result != V92_E2U_RX_NO_MATCH)
			return V92_PHASE4_E2U_FAILED;
	}
	if (control_result != V92_UPSTREAM_CONTROL_LINE_RX_OK &&
	    control_result != V92_UPSTREAM_CONTROL_LINE_RX_EVENT)
		return V92_PHASE4_E2U_FAILED;
	state->control = control;
	if (control_result != V92_UPSTREAM_CONTROL_LINE_RX_EVENT)
		return V92_PHASE4_E2U_OK;
	if (!control_event_rearms_trial(state, &event) ||
	    v92_e2u_rx_init(
		    &state->receive, &state->control.signal,
		    state->mapping.cpd.extend_e2u) != V92_E2U_RX_OK)
		return V92_PHASE4_E2U_FAILED;
	return V92_PHASE4_E2U_OK;
}

static enum v92_phase4_e2u_result fail(struct v92_phase4_e2u *owner)
{
	owner->failed = true;
	return V92_PHASE4_E2U_FAILED;
}

enum v92_phase4_e2u_result v92_phase4_e2u_next(
	struct v92_phase4_e2u *owner, uint8_t received_octet,
	int16_t received_sample, uint8_t *transmit_octet)
{
	struct v92_phase4_e2u next;
	uint8_t generated;

	if (owner == NULL || transmit_octet == NULL ||
	    memory_regions_overlap(owner, sizeof(*owner),
				   transmit_octet, sizeof(*transmit_octet)))
		return V92_PHASE4_E2U_INVALID_ARGUMENT;
	if (!v92_phase4_e2u_is_valid(owner))
		return V92_PHASE4_E2U_INVALID_STATE;
	if (owner->failed)
		return V92_PHASE4_E2U_FAILED;
	if (v92_phase4_e2u_is_complete(owner))
		return V92_PHASE4_E2U_COMPLETE;
	next = *owner;
	if (next.local_role == V90_ANALOG_MODEM) {
		int16_t sample;
		struct v90_pcm_stream_frame frame;
		struct v92_phase4_e2u_analog_state *state = &next.state.analog;
		enum v90_downstream_data_rx_result received;

		if (v92_phase4_record_tx_next(
			    &next.state.analog.transmit, &sample) !=
			    V92_PHASE4_RECORD_TX_OK)
			return fail(owner);
		received = v92_downstream_receive_owned(
			&state->downstream, &state->downstream_stage,
			&state->cpu, &state->mapping, received_octet, &frame);
		if (received != V90_DOWNSTREAM_DATA_RX_BUFFERING &&
		    received != V90_DOWNSTREAM_DATA_RX_B1D_FRAME &&
		    received != V90_DOWNSTREAM_DATA_RX_B1D_COMPLETE &&
		    received != V90_DOWNSTREAM_DATA_RX_DATA_FRAME)
			return fail(owner);
		generated = next.state.analog.cpu.law == G711_LAW_PCMA ?
			g711_encode_pcma(sample) : g711_encode_pcmu(sample);
	} else {
		enum v92_phase4_e2u_result receive_result;
		struct v92_phase4_e2u_digital_state *state = &next.state.digital;

		if (v92_downstream_transmit_owned(
			    &state->downstream, &state->downstream_stage,
			    &state->cpu, NULL, NULL, &generated) !=
		    V90_DOWNSTREAM_DATA_STREAM_OK)
			return fail(owner);
		receive_result = digital_receive(
			&next.state.digital, received_sample);
		if (receive_result == V92_PHASE4_E2U_FAILED)
			return fail(owner);
	}
	if (!v92_phase4_e2u_is_valid(&next))
		return fail(owner);
	*owner = next;
	*transmit_octet = generated;
	return v92_phase4_e2u_is_complete(owner) ?
		V92_PHASE4_E2U_COMPLETE : V92_PHASE4_E2U_OK;
}

bool v92_phase4_e2u_handoff_is_valid(
	const struct v92_phase4_e2u_handoff *handoff)
{
	if (handoff == NULL || !pcm_modem_role_is_valid(handoff->local_role))
		return false;
	if (handoff->local_role == V90_ANALOG_MODEM) {
		const struct v92_phase4_e2u_analog_handoff *analog =
			&handoff->state.analog;
		struct v90_pcm_mapping mapping;
		bool downstream_valid;

		if (!downstream_stage_is_valid(analog->downstream_stage) ||
		    !v92_cpu_parameters_are_valid(&analog->cpu) ||
		    !downstream_mapping(&analog->cpu, &mapping))
			return false;
		if (analog->downstream_stage ==
		    V92_PHASE4_E2U_DOWNSTREAM_TRAINING) {
			downstream_valid = pcm_downstream_rx_is_valid(
				&analog->downstream.training) &&
				!pcm_downstream_rx_has_failed(
					&analog->downstream.training) &&
				analog->downstream.training.protocol ==
					PCM_PROTOCOL_V92 &&
				analog->downstream.training.stream.decoder.mapping.law ==
					analog->cpu.law;
		} else {
			downstream_valid = v90_downstream_data_rx_is_valid(
				&analog->downstream.data) &&
				v90_pcm_mapping_equal(
					&analog->downstream.data.stream.decoder.mapping,
					&mapping);
		}

		return v92_phase4_modulator_is_valid(&analog->transmit) &&
		       downstream_valid &&
		       v92_pcm_mapping_is_valid(&analog->mapping) &&
		       analog->cpu.law == analog->mapping.law;
	}
	{
		const struct v92_phase4_e2u_digital_handoff *digital =
			&handoff->state.digital;
		struct v90_pcm_mapping mapping;
		bool downstream_valid;

		if (!downstream_stage_is_valid(digital->downstream_stage) ||
		    !v92_cpu_parameters_are_valid(&digital->cpu) ||
		    !downstream_mapping(&digital->cpu, &mapping))
			return false;
		if (digital->downstream_stage ==
		    V92_PHASE4_E2U_DOWNSTREAM_TRAINING) {
			downstream_valid = pcm_downstream_tx_is_valid(
				&digital->downstream.training) &&
				digital->downstream.training.protocol ==
					PCM_PROTOCOL_V92 &&
				digital->downstream.training.ed_requested &&
				digital->downstream.training.encoder.mapping.law ==
					digital->cpu.law;
		} else {
			downstream_valid = v90_downstream_data_stream_is_valid(
				&digital->downstream.data) &&
				v90_pcm_mapping_equal(
					&digital->downstream.data.transmitter.encoder.mapping,
					&mapping);
		}

		return v92_upstream_rx_is_valid(&digital->receive) &&
		       !v92_upstream_rx_has_failed(&digital->receive) &&
		       downstream_valid &&
		       v92_pcm_mapping_is_valid(&digital->mapping) &&
		       digital->mapping.law == digital->cpu.law &&
		       digital->receive.line.law == digital->mapping.law;
	}
}

enum v92_phase4_e2u_result v92_phase4_e2u_handoff(
	const struct v92_phase4_e2u *owner,
	struct v92_phase4_e2u_handoff *handoff)
{
	struct v92_phase4_e2u_handoff generated = { 0 };

	if (owner == NULL || handoff == NULL ||
	    memory_regions_overlap(owner, sizeof(*owner),
				   handoff, sizeof(*handoff)))
		return V92_PHASE4_E2U_INVALID_ARGUMENT;
	if (!v92_phase4_e2u_is_complete(owner))
		return V92_PHASE4_E2U_INVALID_STATE;
	generated.local_role = owner->local_role;
	if (owner->local_role == V90_ANALOG_MODEM) {
		const struct v92_phase4_e2u_analog_state *analog =
			&owner->state.analog;

		if (v92_phase4_record_tx_handoff(
			    &analog->transmit,
			    &generated.state.analog.transmit) !=
			    V92_PHASE4_RECORD_TX_OK)
			return V92_PHASE4_E2U_INTERNAL_ERROR;
		generated.state.analog.downstream = analog->downstream;
		generated.state.analog.cpu = analog->cpu;
		generated.state.analog.mapping = analog->mapping;
		generated.state.analog.downstream_stage =
			analog->downstream_stage;
	} else {
		const struct v92_phase4_e2u_digital_state *digital =
			&owner->state.digital;

		if (v92_e2u_rx_handoff(
			    &digital->receive,
			    &generated.state.digital.receive) != V92_E2U_RX_OK)
			return V92_PHASE4_E2U_INTERNAL_ERROR;
		generated.state.digital.downstream = digital->downstream;
		generated.state.digital.cpu = digital->cpu;
		generated.state.digital.mapping = digital->mapping;
		generated.state.digital.downstream_stage =
			digital->downstream_stage;
	}
	if (!v92_phase4_e2u_handoff_is_valid(&generated))
		return V92_PHASE4_E2U_INTERNAL_ERROR;
	*handoff = generated;
	return V92_PHASE4_E2U_OK;
}

const char *v92_phase4_e2u_result_name(enum v92_phase4_e2u_result result)
{
	switch (result) {
	case V92_PHASE4_E2U_OK:
		return "ok";
	case V92_PHASE4_E2U_COMPLETE:
		return "complete";
	case V92_PHASE4_E2U_FAILED:
		return "failed";
	case V92_PHASE4_E2U_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PHASE4_E2U_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_PHASE4_E2U_INVALID_STATE:
		return "invalid state";
	case V92_PHASE4_E2U_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
