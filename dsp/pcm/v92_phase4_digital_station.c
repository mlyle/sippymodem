// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase4_digital_station.h"

#include "memory_region.h"

#include <stddef.h>
#include <string.h>

static enum g711_law receive_law(
	const struct v92_phase4_digital_rx *receiver)
{
	return receiver->stage == V92_PHASE4_DIGITAL_RX_TRN2U ?
		       receiver->signal.trn2u.law :
		       receiver->signal.control.line.law;
}

static bool cpu_storage_is_empty(
	const struct v92_phase4_digital_station *station)
{
	static const struct v92_cpu_parameters empty;

	return memcmp(&station->cpu, &empty, sizeof(empty)) == 0;
}

static bool suvu_storage_is_empty(
	const struct v92_phase4_digital_station *station)
{
	static const struct v92_suvu_parameters empty;

	return memcmp(&station->suvu, &empty, sizeof(empty)) == 0;
}

static bool cpd_base_equal(
	const struct v92_cpd_base_parameters *left,
	const struct v92_cpd_base_parameters *right)
{
	return left->four_times_gain_q0_16 == right->four_times_gain_q0_16 &&
	       left->data_rate_number == right->data_rate_number &&
	       left->trellis == right->trellis &&
	       left->extend_e2u == right->extend_e2u &&
	       left->acknowledges_cpu == right->acknowledges_cpu;
}

static bool cpu_selection_equal(
	const struct v92_cpu_parameters *left,
	const struct v92_cpu_parameters *right)
{
	return left->law == right->law &&
	       left->trn1d_ratio_q3_13 == right->trn1d_ratio_q3_13 &&
	       left->shaping_a1_q1_6 == right->shaping_a1_q1_6 &&
	       left->shaping_a2_q1_6 == right->shaping_a2_q1_6 &&
	       left->shaping_b1_q1_6 == right->shaping_b1_q1_6 &&
	       left->shaping_b2_q1_6 == right->shaping_b2_q1_6 &&
	       left->data_rate_number == right->data_rate_number &&
	       left->sign_redundancy == right->sign_redundancy &&
	       left->spectral_lookahead_frames ==
		       right->spectral_lookahead_frames &&
	       left->constellation_count == right->constellation_count &&
	       left->codec_constellations_differ ==
		       right->codec_constellations_differ &&
	       memcmp(left->frame_constellation, right->frame_constellation,
		      sizeof(left->frame_constellation)) == 0 &&
	       memcmp(left->transmit_constellations,
		      right->transmit_constellations,
		      sizeof(left->transmit_constellations)) == 0 &&
	       memcmp(left->codec_constellations,
		      right->codec_constellations,
		      sizeof(left->codec_constellations)) == 0;
}

static bool suvu_selection_equal(
	const struct v92_suvu_parameters *left,
	const struct v92_suvu_parameters *right)
{
	return left->prefilter_level_db_q2_2 ==
		       right->prefilter_level_db_q2_2 &&
	       left->wait_for_cpu == right->wait_for_cpu &&
	       left->request_silence == right->request_silence;
}

static bool transmit_stage_is_valid(
	enum v92_phase4_digital_station_tx_stage stage)
{
	return stage == V92_PHASE4_DIGITAL_STATION_INITIAL_SUVD ||
	       stage == V92_PHASE4_DIGITAL_STATION_READY_FOR_CPD ||
	       stage == V92_PHASE4_DIGITAL_STATION_CPD ||
	       stage == V92_PHASE4_DIGITAL_STATION_STATUS_SUVD;
}

static bool downstream_state_matches(
	const struct v92_phase4_digital_station *station)
{
	const struct pcm_downstream_tx *tx = &station->transmit;

	if (tx->protocol != PCM_PROTOCOL_V92 || !tx->control_configured ||
	    tx->finish_requested || tx->complete)
		return false;
	switch (station->transmit_stage) {
	case V92_PHASE4_DIGITAL_STATION_INITIAL_SUVD:
	case V92_PHASE4_DIGITAL_STATION_READY_FOR_CPD:
		return tx->v92_control_kind ==
			       PCM_DOWNSTREAM_V92_CONTROL_SUVD &&
		       !tx->control.suvd.acknowledges_cpu &&
		       tx->control.suvd.request_silence ==
			       station->request_silence;
	case V92_PHASE4_DIGITAL_STATION_CPD: {
		struct v92_cpd_base_parameters expected = station->cpd;

		expected.acknowledges_cpu =
			station->transmit_acknowledges_cpu;
		return tx->v92_control_kind == PCM_DOWNSTREAM_V92_CONTROL_CPD &&
		       cpd_base_equal(&tx->control.cpd, &expected);
	}
	case V92_PHASE4_DIGITAL_STATION_STATUS_SUVD:
		return tx->v92_control_kind ==
			       PCM_DOWNSTREAM_V92_CONTROL_SUVD &&
		       tx->control.suvd.acknowledges_cpu ==
			       station->transmit_acknowledges_cpu &&
		       tx->control.suvd.request_silence ==
			       station->request_silence;
	}
	return false;
}

static bool owned_state_is_valid(
	const struct v92_phase4_digital_station *station)
{
	return receive_law(&station->receive) ==
		       station->transmit.encoder.mapping.law &&
	       v92_cpd_base_parameters_are_valid(&station->cpd) &&
	       !station->cpd.acknowledges_cpu &&
	       transmit_stage_is_valid(station->transmit_stage) &&
	       (station->transmit_stage ==
			V92_PHASE4_DIGITAL_STATION_INITIAL_SUVD ||
		station->transmit_stage ==
			V92_PHASE4_DIGITAL_STATION_READY_FOR_CPD ||
		station->suvu_received) &&
	       ((station->transmit_stage ==
			 V92_PHASE4_DIGITAL_STATION_INITIAL_SUVD ||
		 station->transmit_stage ==
			 V92_PHASE4_DIGITAL_STATION_READY_FOR_CPD) ==
		(station->cpd_start_frame == 0u)) &&
	       station->cpd_start_frame <= station->transmit.accepted_frames &&
	       (!station->cpu_received || station->suvu_received) &&
	       (!station->peer_acknowledged_cpd || station->suvu_received) &&
	       (!station->transmit_acknowledges_cpu ||
		((station->transmit_stage ==
			 V92_PHASE4_DIGITAL_STATION_CPD ||
		 station->transmit_stage ==
			 V92_PHASE4_DIGITAL_STATION_STATUS_SUVD) &&
		 station->cpu_received)) &&
	       (station->suvu_received ?
		v92_suvu_parameters_are_valid(&station->suvu) :
		suvu_storage_is_empty(station)) &&
	       (station->cpu_received ?
		(v92_cpu_parameters_are_valid(&station->cpu) &&
		 station->cpu.law == station->transmit.encoder.mapping.law) :
		cpu_storage_is_empty(station)) &&
	       station->peer_acknowledged_cpd ==
		((station->suvu_received &&
		  station->suvu.acknowledges_cpd) ||
		 (station->cpu_received &&
		  station->cpu.acknowledges_cpd)) &&
	       (!v92_phase4_digital_rx_has_failed(&station->receive) ||
		station->failed) &&
	       downstream_state_matches(station);
}

bool v92_phase4_digital_station_is_valid(
	const struct v92_phase4_digital_station *station)
{
	return station != NULL &&
	       v92_phase4_digital_rx_is_valid(&station->receive) &&
	       pcm_downstream_tx_is_valid(&station->transmit) &&
	       owned_state_is_valid(station);
}

bool v92_phase4_digital_station_has_failed(
	const struct v92_phase4_digital_station *station)
{
	return v92_phase4_digital_station_is_valid(station) &&
	       station->failed;
}

enum v92_phase4_digital_station_result v92_phase4_digital_station_init(
	struct v92_phase4_digital_station *station,
	const struct v92_pcm_phase3_cpt_line_handoff *receive_handoff,
	enum v92_constellation_size constellation,
	const struct pcm_downstream_tx *downstream,
	const struct v92_cpd_base_parameters *cpd)
{
	struct v92_phase4_digital_station initialized = { 0 };

	if (station == NULL || receive_handoff == NULL || downstream == NULL ||
	    cpd == NULL)
		return V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if (!pcm_downstream_tx_is_valid(downstream) ||
	    downstream->protocol != PCM_PROTOCOL_V92 ||
	    !downstream->control_configured ||
	    downstream->v92_control_kind !=
		    PCM_DOWNSTREAM_V92_CONTROL_SUVD ||
	    downstream->control.suvd.acknowledges_cpu ||
	    downstream->finish_requested || downstream->complete ||
	    !v92_cpd_base_parameters_are_valid(cpd) ||
	    cpd->acknowledges_cpu)
		return V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS;
	if (v92_phase4_digital_rx_init(
		    &initialized.receive, receive_handoff, constellation) !=
	    V92_PHASE4_DIGITAL_RX_OK)
		return V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS;
	initialized.transmit = *downstream;
	initialized.cpd = *cpd;
	if (downstream->stage == PCM_DOWNSTREAM_TX_V92_CONTROL &&
	    downstream->control_bit_index == 0u)
		initialized.transmit_stage =
			V92_PHASE4_DIGITAL_STATION_READY_FOR_CPD;
	initialized.request_silence =
		downstream->control.suvd.request_silence;
	if (!v92_phase4_digital_station_is_valid(&initialized))
		return V92_PHASE4_DIGITAL_STATION_INTERNAL_ERROR;
	*station = initialized;
	return V92_PHASE4_DIGITAL_STATION_OK;
}

static enum v92_phase4_digital_station_result fail(
	struct v92_phase4_digital_station *station)
{
	station->failed = true;
	return V92_PHASE4_DIGITAL_STATION_INTERNAL_ERROR;
}

static bool accept_event(
	struct v92_phase4_digital_station *station,
	const struct v92_control_event *event)
{
	switch (event->type) {
	case V92_CONTROL_EVENT_NONE:
		return true;
	case V92_CONTROL_EVENT_SUVU:
		if (event->record.suvu.frame_result != V92_SUVU_OK ||
		    !v92_suvu_parameters_are_valid(
			    &event->record.suvu.parameters))
			return false;
		if (!station->suvu_received) {
			station->suvu = event->record.suvu.parameters;
			station->suvu_received = true;
		} else {
			if (!suvu_selection_equal(
				    &station->suvu,
				    &event->record.suvu.parameters) ||
			    (station->suvu.acknowledges_cpd &&
			     !event->record.suvu.parameters.acknowledges_cpd))
				return false;
			station->suvu.acknowledges_cpd |=
				event->record.suvu.parameters.acknowledges_cpd;
		}
		station->peer_acknowledged_cpd =
			station->suvu.acknowledges_cpd ||
			(station->cpu_received &&
			 station->cpu.acknowledges_cpd);
		return true;
	case V92_CONTROL_EVENT_CPU:
		if (event->record.cpu.frame_result != V92_CPU_OK ||
		    !v92_cpu_parameters_are_valid(&event->record.cpu.parameters) ||
		    event->record.cpu.parameters.law !=
			    station->transmit.encoder.mapping.law)
			return false;
		if (!station->suvu_received)
			return true;
		if (!station->cpu_received) {
			station->cpu = event->record.cpu.parameters;
			station->cpu_received = true;
		} else {
			if (!cpu_selection_equal(
				    &station->cpu,
				    &event->record.cpu.parameters) ||
			    (station->cpu.acknowledges_cpd &&
			     !event->record.cpu.parameters.acknowledges_cpd))
				return false;
			station->cpu.acknowledges_cpd |=
				event->record.cpu.parameters.acknowledges_cpd;
		}
		station->peer_acknowledged_cpd =
			station->suvu.acknowledges_cpd ||
			station->cpu.acknowledges_cpd;
		return true;
	case V92_CONTROL_EVENT_SUVU_REJECTED:
	case V92_CONTROL_EVENT_CPU_REJECTED:
		return true;
	case V92_CONTROL_EVENT_SUVD:
	case V92_CONTROL_EVENT_CPD:
	case V92_CONTROL_EVENT_SUVD_REJECTED:
	case V92_CONTROL_EVENT_CPD_REJECTED:
		return false;
	}
	return false;
}

enum v92_phase4_digital_station_result v92_phase4_digital_station_receive(
	struct v92_phase4_digital_station *station, int16_t sample)
{
	struct v92_control_event event = { 0 };
	enum v92_phase4_digital_rx_result result;

	if (station == NULL)
		return V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if (!v92_phase4_digital_station_is_valid(station))
		return V92_PHASE4_DIGITAL_STATION_INVALID_STATE;
	if (station->failed)
		return V92_PHASE4_DIGITAL_STATION_FAILED;
	result = v92_phase4_digital_rx_sample(
		&station->receive, sample, &event);
	if (result == V92_PHASE4_DIGITAL_RX_FAILED)
		return fail(station);
	if (result != V92_PHASE4_DIGITAL_RX_OK &&
	    result != V92_PHASE4_DIGITAL_RX_ALIGNED &&
	    result != V92_PHASE4_DIGITAL_RX_EVENT)
		return fail(station);
	if (!accept_event(station, &event) || !owned_state_is_valid(station))
		return fail(station);
	return result == V92_PHASE4_DIGITAL_RX_EVENT ?
		       V92_PHASE4_DIGITAL_STATION_EVENT :
		       V92_PHASE4_DIGITAL_STATION_OK;
}

static bool apply_pending_downstream(
	struct v92_phase4_digital_station *station)
{
	if (!pcm_downstream_tx_is_v92_control_boundary(&station->transmit))
		return true;
	if (station->transmit_stage ==
	    V92_PHASE4_DIGITAL_STATION_INITIAL_SUVD) {
		station->transmit_stage =
			V92_PHASE4_DIGITAL_STATION_READY_FOR_CPD;
	}
	if (station->suvu_received &&
	    station->transmit_stage ==
		    V92_PHASE4_DIGITAL_STATION_READY_FOR_CPD) {
		struct v92_cpd_base_parameters cpd = station->cpd;

		cpd.acknowledges_cpu = station->cpu_received;
		if (pcm_downstream_tx_start_cpd(&station->transmit, &cpd) !=
		    PCM_DOWNSTREAM_TX_OK)
			return false;
		station->transmit_stage = V92_PHASE4_DIGITAL_STATION_CPD;
		station->transmit_acknowledges_cpu = cpd.acknowledges_cpu;
		station->cpd_start_frame = station->transmit.accepted_frames;
		return true;
	}
	if ((station->transmit_stage == V92_PHASE4_DIGITAL_STATION_CPD &&
	     station->transmit.accepted_frames > station->cpd_start_frame) ||
	    (station->transmit_stage ==
		     V92_PHASE4_DIGITAL_STATION_STATUS_SUVD &&
	     station->cpu_received &&
	     !station->transmit_acknowledges_cpu)) {
		const struct v92_suvd_parameters suvd = {
			.request_silence = station->request_silence,
			.acknowledges_cpu = station->cpu_received,
		};

		if (pcm_downstream_tx_start_suvd(
			    &station->transmit, &suvd) !=
		    PCM_DOWNSTREAM_TX_OK)
			return false;
		station->transmit_stage =
			V92_PHASE4_DIGITAL_STATION_STATUS_SUVD;
		station->transmit_acknowledges_cpu =
			suvd.acknowledges_cpu;
	}
	return true;
}

enum v92_phase4_digital_station_result v92_phase4_digital_station_transmit(
	struct v92_phase4_digital_station *station, uint8_t *network_octet)
{
	struct pcm_downstream_symbol symbol;

	if (station == NULL || network_octet == NULL ||
	    memory_regions_overlap(
		    station, sizeof(*station), network_octet,
		    sizeof(*network_octet)))
		return V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if (!v92_phase4_digital_station_is_valid(station))
		return V92_PHASE4_DIGITAL_STATION_INVALID_STATE;
	if (station->failed)
		return V92_PHASE4_DIGITAL_STATION_FAILED;
	if (!apply_pending_downstream(station) ||
	    pcm_downstream_tx_next(&station->transmit, &symbol) !=
		    PCM_DOWNSTREAM_TX_OK ||
	    !owned_state_is_valid(station))
		return fail(station);
	*network_octet = symbol.pcm.octet;
	return V92_PHASE4_DIGITAL_STATION_OK;
}

bool v92_phase4_digital_station_cpu_parameters(
	const struct v92_phase4_digital_station *station,
	struct v92_cpu_parameters *parameters)
{
	if (!v92_phase4_digital_station_is_valid(station) ||
	    station->failed || !station->cpu_received || parameters == NULL ||
	    memory_regions_overlap(
		    station, sizeof(*station), parameters,
		    sizeof(*parameters)))
		return false;
	*parameters = station->cpu;
	return true;
}

bool v92_phase4_digital_station_peer_acknowledged_cpd(
	const struct v92_phase4_digital_station *station)
{
	return v92_phase4_digital_station_is_valid(station) &&
	       !station->failed && station->peer_acknowledged_cpd;
}

const char *v92_phase4_digital_station_result_name(
	enum v92_phase4_digital_station_result result)
{
	switch (result) {
	case V92_PHASE4_DIGITAL_STATION_OK:
		return "ok";
	case V92_PHASE4_DIGITAL_STATION_EVENT:
		return "event";
	case V92_PHASE4_DIGITAL_STATION_FAILED:
		return "failed";
	case V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PHASE4_DIGITAL_STATION_INVALID_STATE:
		return "invalid state";
	case V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_PHASE4_DIGITAL_STATION_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
