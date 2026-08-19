// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase4_analog_station.h"

#include "memory_region.h"

#include <stddef.h>
#include <string.h>

static bool cpd_is_empty(const struct v92_cpd_base_parameters *parameters)
{
	static const struct v92_cpd_base_parameters empty;

	return memcmp(parameters, &empty, sizeof(empty)) == 0;
}

static bool suvd_is_empty(const struct v92_suvd_parameters *parameters)
{
	static const struct v92_suvd_parameters empty;

	return memcmp(parameters, &empty, sizeof(empty)) == 0;
}

static bool cpd_selection_equal(
	const struct v92_cpd_base_parameters *left,
	const struct v92_cpd_base_parameters *right)
{
	return left->four_times_gain_q0_16 == right->four_times_gain_q0_16 &&
	       left->data_rate_number == right->data_rate_number &&
	       left->trellis == right->trellis &&
	       left->extend_e2u == right->extend_e2u;
}

static struct v92_cpd_base_parameters cpd_base(
	const struct v92_cpd_parameters *parameters)
{
	return (struct v92_cpd_base_parameters){
		.four_times_gain_q0_16 = parameters->four_times_gain_q0_16,
		.data_rate_number = parameters->data_rate_number,
		.trellis = parameters->trellis,
		.extend_e2u = parameters->extend_e2u,
		.acknowledges_cpu = parameters->acknowledges_cpu,
	};
}

static bool mapping_matches_base(
	const struct v92_pcm_mapping *mapping,
	const struct v92_cpd_base_parameters *base)
{
	struct v92_cpd_base_parameters mapped = cpd_base(&mapping->cpd);

	return cpd_selection_equal(&mapped, base) &&
	       mapped.acknowledges_cpu == base->acknowledges_cpu;
}

/* Complete mappings enter only through v92_pcm_mapping_init() and remain
 * immutable except for the acknowledgement bit. Keep the per-symbol owner
 * check bounded; event acceptance and handoff perform the deep table check. */
static bool owned_mapping_is_valid(
	const struct v92_phase4_analog_station *station)
{
	if (!station->cpd_received)
		return cpd_is_empty(&station->cpd) &&
		       station->mapping.data_bits_per_frame == 0u;
	return g711_law_is_valid(station->mapping.law) &&
	       station->mapping.law == station->transmit.cpu.law &&
	       station->mapping.data_bits_per_frame ==
		       v92_pcm_mapping_data_bits(
			       station->mapping.cpd.data_rate_number) &&
	       station->mapping.cpd.modulus_present &&
	       station->mapping.cpd.filters_present &&
	       station->mapping.cpd.constellations_present &&
	       v92_cpd_base_parameters_are_valid(&station->cpd) &&
	       mapping_matches_base(&station->mapping, &station->cpd);
}

static bool owned_state_is_valid(
	const struct v92_phase4_analog_station *station)
{
	return (station->suvd_received ?
		v92_suvd_parameters_are_valid(&station->suvd) :
		suvd_is_empty(&station->suvd)) &&
	       owned_mapping_is_valid(station) &&
	       station->transmit.received_suvd == station->suvd_received &&
	       station->transmit.received_cpd == station->cpd_received;
}

bool v92_phase4_analog_station_is_valid(
	const struct v92_phase4_analog_station *station)
{
	return station != NULL &&
	       v92_phase4_analog_tx_is_valid(&station->transmit) &&
	       pcm_downstream_rx_is_valid(&station->receive) &&
	       station->receive.protocol == PCM_PROTOCOL_V92 &&
	       station->receive.stream.decoder.decision_plane ==
		       V90_PCM_MAPPING_TRANSMIT &&
	       station->receive.stream.decoder.mapping.law ==
		       station->transmit.cpu.law &&
	       owned_state_is_valid(station);
}

bool v92_phase4_analog_station_has_failed(
	const struct v92_phase4_analog_station *station)
{
	return v92_phase4_analog_station_is_valid(station) && station->failed;
}

bool v92_phase4_analog_station_is_e2u_ready(
	const struct v92_phase4_analog_station *station)
{
	return v92_phase4_analog_station_is_valid(station) &&
	       !station->failed && station->cpd_received &&
	       v92_phase4_analog_tx_is_e2u_ready(&station->transmit);
}

static bool note_suvd(
	struct v92_phase4_analog_station *station,
	const struct v92_suvd_parameters *parameters)
{
	struct v92_phase4_analog_tx transmit;
	struct v92_suvd_parameters retained;

	if (!v92_suvd_parameters_are_valid(parameters))
		return false;
	if (station->suvd_received) {
		if (station->suvd.request_silence !=
			    parameters->request_silence ||
		    (station->suvd.acknowledges_cpu &&
		     !parameters->acknowledges_cpu))
			return false;
		retained = station->suvd;
		retained.acknowledges_cpu |= parameters->acknowledges_cpu;
	} else {
		retained = *parameters;
	}
	transmit = station->transmit;
	if (transmit.stage != V92_PHASE4_ANALOG_TX_E2U_READY &&
	    v92_phase4_analog_tx_note_suvd(&transmit, &retained) !=
		    V92_PHASE4_ANALOG_TX_OK)
		return false;
	station->transmit = transmit;
	station->suvd = retained;
	station->suvd_received = true;
	return true;
}

static bool note_cpd(
	struct v92_phase4_analog_station *station,
	const struct v92_cpd_parameters *parameters)
{
	struct v92_pcm_mapping mapping;
	struct v92_phase4_analog_tx transmit = station->transmit;
	struct v92_cpd_base_parameters base;
	bool full;

	if (!v92_cpd_parameters_are_valid(parameters))
		return false;
	base = cpd_base(parameters);
	full = parameters->modulus_present && parameters->filters_present &&
	       parameters->constellations_present;
	if (station->cpd_received) {
		if (!cpd_selection_equal(&station->cpd, &base) ||
		    (station->cpd.acknowledges_cpu &&
		     !base.acknowledges_cpu))
			return false;
		if (full) {
			if (v92_pcm_mapping_init(
				    &mapping, station->transmit.cpu.law,
				    parameters) != V92_PCM_MAPPING_OK)
				return false;
			mapping.cpd.acknowledges_cpu =
				station->mapping.cpd.acknowledges_cpu;
			if (!v92_pcm_mapping_equal(&mapping, &station->mapping))
				return false;
		} else {
			if (parameters->modulus_present ||
			    parameters->filters_present ||
			    parameters->constellations_present)
				return false;
			mapping = station->mapping;
		}
	} else {
		if (!full || v92_pcm_mapping_init(
				     &mapping, station->transmit.cpu.law,
				     parameters) != V92_PCM_MAPPING_OK)
			return false;
	}
	base.acknowledges_cpu |= station->cpd.acknowledges_cpu;
	mapping.cpd.acknowledges_cpu = base.acknowledges_cpu;
	if (transmit.stage != V92_PHASE4_ANALOG_TX_E2U_READY &&
	    v92_phase4_analog_tx_note_cpd(
		    &transmit, base.acknowledges_cpu) !=
		    V92_PHASE4_ANALOG_TX_OK)
		return false;
	station->transmit = transmit;
	station->mapping = mapping;
	station->cpd = base;
	station->cpd_received = true;
	return true;
}

enum v92_phase4_analog_station_result v92_phase4_analog_station_init(
	struct v92_phase4_analog_station *station,
	const struct v92_phase3_analog_transmit_handoff *transmit_handoff,
	const struct pcm_downstream_rx *downstream,
	const struct v92_suvu_parameters *suvu,
	const struct v92_cpu_parameters *cpu,
	const struct v92_suvd_parameters *early_suvd,
	const struct v92_cpd_parameters *early_cpd)
{
	struct v92_phase4_analog_station initialized = { 0 };
	struct v92_pcm_mapping early_mapping;

	if (station == NULL || transmit_handoff == NULL || downstream == NULL ||
	    suvu == NULL || cpu == NULL)
		return V92_PHASE4_ANALOG_STATION_INVALID_ARGUMENT;
	if (!pcm_downstream_rx_is_valid(downstream) ||
	    downstream->protocol != PCM_PROTOCOL_V92 ||
	    downstream->stream.decoder.decision_plane !=
		    V90_PCM_MAPPING_TRANSMIT ||
	    downstream->stream.decoder.mapping.law != cpu->law ||
	    (early_suvd != NULL &&
	     !v92_suvd_parameters_are_valid(early_suvd)) ||
	    (early_cpd != NULL &&
	     v92_pcm_mapping_init(&early_mapping, cpu->law, early_cpd) !=
		     V92_PCM_MAPPING_OK))
		return V92_PHASE4_ANALOG_STATION_INVALID_PARAMETERS;
	if (v92_phase4_analog_tx_init(
		    &initialized.transmit,
		    transmit_handoff->training_constellation,
		    transmit_handoff->previous_encoded_sign,
		    transmit_handoff->lu, suvu, cpu) !=
	    V92_PHASE4_ANALOG_TX_OK)
		return V92_PHASE4_ANALOG_STATION_INVALID_PARAMETERS;
	initialized.receive = *downstream;
	if ((early_suvd != NULL && !note_suvd(&initialized, early_suvd)) ||
	    (early_cpd != NULL && !note_cpd(&initialized, early_cpd)) ||
	    !v92_phase4_analog_station_is_valid(&initialized))
		return V92_PHASE4_ANALOG_STATION_INTERNAL_ERROR;
	*station = initialized;
	return V92_PHASE4_ANALOG_STATION_OK;
}

static enum v92_phase4_analog_station_result fail(
	struct v92_phase4_analog_station *station)
{
	station->failed = true;
	return V92_PHASE4_ANALOG_STATION_FAILED;
}

static bool accept_cpd_event(
	struct v92_phase4_analog_station *station,
	const struct v92_cpd_parameters *parameters)
{
	return note_cpd(station, parameters);
}

static bool arm_ed_if_acknowledged(
	struct v92_phase4_analog_station *station, bool acknowledges_cpu)
{
	return !acknowledges_cpu ||
	       pcm_downstream_rx_expect_ed(&station->receive) ==
		       PCM_DOWNSTREAM_RX_OK;
}

enum v92_phase4_analog_station_result v92_phase4_analog_station_receive(
	struct v92_phase4_analog_station *station, uint8_t network_octet)
{
	struct pcm_downstream_event event = { 0 };
	enum pcm_downstream_rx_result result;

	if (station == NULL)
		return V92_PHASE4_ANALOG_STATION_INVALID_ARGUMENT;
	if (!v92_phase4_analog_station_is_valid(station))
		return V92_PHASE4_ANALOG_STATION_INVALID_STATE;
	if (station->failed)
		return V92_PHASE4_ANALOG_STATION_FAILED;
	result = pcm_downstream_rx_receive(
		&station->receive, network_octet, &event);
	switch (result) {
	case PCM_DOWNSTREAM_RX_BUFFERING:
	case PCM_DOWNSTREAM_RX_TRN2D_FRAME:
		return V92_PHASE4_ANALOG_STATION_OK;
	case PCM_DOWNSTREAM_RX_SUVD:
		if (event.control.type != V92_CONTROL_EVENT_SUVD ||
		    event.control.record.suvd.frame_result != V92_SUVD_OK ||
		    !note_suvd(
			    station,
			    &event.control.record.suvd.parameters) ||
		    !arm_ed_if_acknowledged(
			    station,
			    event.control.record.suvd.parameters
				    .acknowledges_cpu))
			return fail(station);
		return V92_PHASE4_ANALOG_STATION_EVENT;
	case PCM_DOWNSTREAM_RX_CPD:
		if (event.control.type != V92_CONTROL_EVENT_CPD ||
		    event.control.record.cpd.frame_result != V92_CPD_OK ||
		    !accept_cpd_event(
			    station,
			    &event.control.record.cpd.parameters) ||
		    !arm_ed_if_acknowledged(
			    station,
			    event.control.record.cpd.parameters
				    .acknowledges_cpu))
			return fail(station);
		return V92_PHASE4_ANALOG_STATION_EVENT;
	case PCM_DOWNSTREAM_RX_SUVD_REJECTED:
	case PCM_DOWNSTREAM_RX_CPD_REJECTED:
		return V92_PHASE4_ANALOG_STATION_EVENT;
	case PCM_DOWNSTREAM_RX_ED:
		if (!v92_phase4_analog_tx_is_e2u_ready(&station->transmit) &&
		    v92_phase4_analog_tx_note_ed(&station->transmit) !=
			    V92_PHASE4_ANALOG_TX_OK)
			return fail(station);
		return V92_PHASE4_ANALOG_STATION_EVENT;
	case PCM_DOWNSTREAM_RX_OK:
	case PCM_DOWNSTREAM_RX_MP:
	case PCM_DOWNSTREAM_RX_MP_PRIME:
	case PCM_DOWNSTREAM_RX_MP_REJECTED:
	case PCM_DOWNSTREAM_RX_ED_REJECTED:
	case PCM_DOWNSTREAM_RX_INVALID_ARGUMENT:
	case PCM_DOWNSTREAM_RX_INVALID_PARAMETERS:
	case PCM_DOWNSTREAM_RX_INVALID_STATE:
	case PCM_DOWNSTREAM_RX_INVALID_SYMBOL:
	case PCM_DOWNSTREAM_RX_OUT_OF_RANGE:
	case PCM_DOWNSTREAM_RX_INTERNAL_ERROR:
		return fail(station);
	}
	return fail(station);
}

enum v92_phase4_analog_station_result v92_phase4_analog_station_transmit(
	struct v92_phase4_analog_station *station, int16_t *sample)
{
	enum v92_phase4_analog_tx_result result;

	if (station == NULL || sample == NULL ||
	    memory_regions_overlap(
		    station, sizeof(*station), sample, sizeof(*sample)))
		return V92_PHASE4_ANALOG_STATION_INVALID_ARGUMENT;
	if (!v92_phase4_analog_station_is_valid(station))
		return V92_PHASE4_ANALOG_STATION_INVALID_STATE;
	if (station->failed)
		return V92_PHASE4_ANALOG_STATION_FAILED;
	result = v92_phase4_analog_tx_next(&station->transmit, sample);
	if (result == V92_PHASE4_ANALOG_TX_OK)
		return V92_PHASE4_ANALOG_STATION_OK;
	if (result == V92_PHASE4_ANALOG_TX_E2U_READY_RESULT)
		return V92_PHASE4_ANALOG_STATION_E2U_READY;
	return fail(station);
}

enum v92_phase4_analog_station_result
v92_phase4_analog_station_repeat_cpu(
	struct v92_phase4_analog_station *station)
{
	if (station == NULL)
		return V92_PHASE4_ANALOG_STATION_INVALID_ARGUMENT;
	if (!v92_phase4_analog_station_is_valid(station) || station->failed)
		return V92_PHASE4_ANALOG_STATION_INVALID_STATE;
	return v92_phase4_analog_tx_repeat_cpu(&station->transmit) ==
		       V92_PHASE4_ANALOG_TX_OK ?
		       V92_PHASE4_ANALOG_STATION_OK :
		       V92_PHASE4_ANALOG_STATION_INVALID_STATE;
}

bool v92_phase4_analog_station_cpd_parameters(
	const struct v92_phase4_analog_station *station,
	struct v92_cpd_base_parameters *parameters)
{
	if (!v92_phase4_analog_station_is_valid(station) || station->failed ||
	    !station->cpd_received || parameters == NULL ||
	    memory_regions_overlap(
		    station, sizeof(*station), parameters,
		    sizeof(*parameters)))
		return false;
	*parameters = station->cpd;
	return true;
}

bool v92_phase4_analog_handoff_is_valid(
	const struct v92_phase4_analog_handoff *handoff)
{
	return handoff != NULL &&
	       v92_phase4_modulator_is_valid(&handoff->transmit) &&
	       pcm_downstream_rx_is_valid(&handoff->receive) &&
	       !pcm_downstream_rx_has_failed(&handoff->receive) &&
	       handoff->receive.protocol == PCM_PROTOCOL_V92 &&
	       handoff->receive.stage != PCM_DOWNSTREAM_RX_TRN2D &&
	       handoff->receive.stream.decoder.decision_plane ==
		       V90_PCM_MAPPING_TRANSMIT &&
	       v92_cpu_parameters_are_valid(&handoff->cpu) &&
	       handoff->cpu.law ==
		       handoff->receive.stream.decoder.mapping.law &&
	       v92_pcm_mapping_is_valid(&handoff->mapping) &&
	       handoff->mapping.law == handoff->cpu.law;
}

enum v92_phase4_analog_station_result v92_phase4_analog_station_handoff(
	const struct v92_phase4_analog_station *station,
	struct v92_phase4_analog_handoff *handoff)
{
	struct v92_phase4_analog_handoff generated;

	if (station == NULL || handoff == NULL ||
	    memory_regions_overlap(
		    station, sizeof(*station), handoff, sizeof(*handoff)))
		return V92_PHASE4_ANALOG_STATION_INVALID_ARGUMENT;
	if (!v92_phase4_analog_station_is_e2u_ready(station))
		return V92_PHASE4_ANALOG_STATION_INVALID_STATE;
	generated = (struct v92_phase4_analog_handoff){
		.receive = station->receive,
		.cpu = station->transmit.cpu,
		.mapping = station->mapping,
	};
	if (v92_phase4_analog_tx_handoff(
		    &station->transmit, &generated.transmit) !=
		    V92_PHASE4_ANALOG_TX_OK ||
	    !v92_phase4_analog_handoff_is_valid(&generated))
		return V92_PHASE4_ANALOG_STATION_INTERNAL_ERROR;
	*handoff = generated;
	return V92_PHASE4_ANALOG_STATION_OK;
}

const char *v92_phase4_analog_station_result_name(
	enum v92_phase4_analog_station_result result)
{
	switch (result) {
	case V92_PHASE4_ANALOG_STATION_OK:
		return "ok";
	case V92_PHASE4_ANALOG_STATION_EVENT:
		return "event";
	case V92_PHASE4_ANALOG_STATION_E2U_READY:
		return "E2u ready";
	case V92_PHASE4_ANALOG_STATION_FAILED:
		return "failed";
	case V92_PHASE4_ANALOG_STATION_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PHASE4_ANALOG_STATION_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_PHASE4_ANALOG_STATION_INVALID_STATE:
		return "invalid state";
	case V92_PHASE4_ANALOG_STATION_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
