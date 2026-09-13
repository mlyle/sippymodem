// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase4_digital_station.h"

#include "memory_region.h"
#include "v92_phase4_record_tx.h"

#include <stddef.h>
#include <string.h>

static enum g711_law receive_law(
	const struct v92_phase4_digital_rx *receiver)
{
	return receiver->stage == V92_PHASE4_DIGITAL_RX_TRN2U ?
		       receiver->signal.trn2u.law :
		       receiver->signal.control.signal.line.law;
}

static bool pause_candidate_is_valid(const struct v92_e2u_rx *e2u,
	const struct v92_upstream_control_line_rx *control)
{
	return v92_e2u_rx_is_valid(e2u) &&
		e2u->expected_symbols == V92_PHASE4_E2U_SYMBOLS &&
		e2u->signal.line.renegotiation && control->signal.line.renegotiation &&
		e2u->signal.line.law == control->signal.line.law &&
		e2u->signal.line.demodulator.constellation == control->signal.line.demodulator.constellation;
}

bool v92_phase4_digital_station_is_valid(const struct v92_phase4_digital_station *station)
{
	return station != NULL && v92_phase4_digital_rx_is_valid(&station->receive) &&
		v92_downstream_control_is_valid(&station->control) &&
		receive_law(&station->receive) == station->control.transmit.encoder.mapping.law &&
		(!station->searching_e2u ||
		 (v92_phase4_digital_rx_is_receiving_control(&station->receive) &&
		  pause_candidate_is_valid(&station->e2u, &station->receive.signal.control) &&
		  (station->control.failed || (!station->e2u.failed &&
		   v92_downstream_control_silence_requested(&station->control))))) &&
		(station->control.cleardown.active ||
		 (v92_upstream_plan_is_valid(&station->upstream_plan) &&
		  station->control.previous_cpu == v92_phase4_digital_rx_is_renegotiating(&station->receive) &&
		  (!v92_phase4_digital_rx_has_failed(&station->receive) || station->control.failed)));
}

bool v92_phase4_digital_station_has_failed(const struct v92_phase4_digital_station *station)
{
	return v92_phase4_digital_station_is_valid(station) && station->control.failed;
}

bool v92_phase4_digital_station_start_cleardown(struct v92_phase4_digital_station *station)
{
	if (!v92_phase4_digital_station_is_valid(station) ||
	    (station->searching_e2u && v92_e2u_rx_is_complete(&station->e2u)) ||
	    !v92_downstream_control_start_cleardown(&station->control))
		return false;
	station->searching_e2u = false;
	return true;
}

static enum v92_phase4_digital_station_result initialize_downstream(
	struct v92_phase4_digital_station *initialized,
	const struct pcm_downstream_tx *downstream,
	const struct v92_suvd_parameters *suvd,
	const struct v92_cpd_parameters *cpd,
	uint16_t round_trip_delay_samples)
{
	if (downstream == NULL || cpd == NULL || suvd == NULL)
		return V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if (!v92_downstream_control_init(&initialized->control, downstream, suvd, cpd,
		    initialized->control.previous_cpu ? &initialized->control.cpu : NULL,
		    round_trip_delay_samples))
		return V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS;
	if (!v92_phase4_digital_station_is_valid(initialized))
		return V92_PHASE4_DIGITAL_STATION_INTERNAL_ERROR;
	return V92_PHASE4_DIGITAL_STATION_OK;
}

enum v92_phase4_digital_station_result v92_phase4_digital_station_init(
	struct v92_phase4_digital_station *station,
	const struct v92_pcm_phase3_cpt_line_handoff *receive_handoff,
	enum v92_constellation_size constellation,
	const struct pcm_downstream_tx *downstream,
	const struct v92_suvd_parameters *suvd,
	const struct v92_cpd_parameters *cpd, uint16_t round_trip_delay_samples)
{
	struct v92_phase4_digital_station initialized = { 0 };

	if (station == NULL || receive_handoff == NULL)
		return V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if (v92_phase4_digital_rx_init(&initialized.receive, receive_handoff, constellation) !=
	    V92_PHASE4_DIGITAL_RX_OK)
		return V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS;
	enum v92_phase4_digital_station_result result = initialize_downstream(
		&initialized, downstream, suvd, cpd, round_trip_delay_samples);
	if (result != V92_PHASE4_DIGITAL_STATION_OK)
		return result;
	*station = initialized;
	return V92_PHASE4_DIGITAL_STATION_OK;
}

enum v92_phase4_digital_station_result v92_phase4_digital_station_init_after_ru(
	struct v92_phase4_digital_station *station, const struct v92_ru_line_rx *ru,
	const struct pcm_downstream_tx *downstream,
	const struct v92_cpd_parameters *cpd, const struct v92_upstream_plan *plan,
	const struct v92_cpu_parameters *cpu, uint16_t round_trip_delay_samples)
{
	struct v92_phase4_digital_station initialized = { 0 };
	struct v92_cpd_parameters selected;

	if (station == NULL || ru == NULL || downstream == NULL || cpd == NULL ||
	    plan == NULL || cpu == NULL)
		return V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if (!v92_upstream_plan_is_valid(plan) || !v92_cpu_parameters_are_valid(cpu) ||
	    !pcm_downstream_tx_is_valid(downstream) || downstream->protocol != PCM_PROTOCOL_V92 ||
	    (downstream->control_configured && downstream->v92_control_kind != PCM_DOWNSTREAM_V92_CONTROL_SUVD) ||
	    !v92_cpd_parameters_are_valid(cpd) || !cpu->data_rate_number || !cpd->data_rate_number ||
	    v92_phase4_digital_rx_init_after_ru(&initialized.receive, ru) != V92_PHASE4_DIGITAL_RX_OK)
		return V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS;
	initialized.control.cpu = *cpu;
	initialized.control.previous_cpu = true;
	initialized.control.cpu.acknowledges_cpd = false;
	initialized.upstream_plan = *plan;
	selected = *cpd;
	selected.acknowledges_cpu = false;
	/* V.92 Table 30: do not carry initial E2u extension into RRN. */
	selected.extend_e2u = false;
	const struct v92_suvd_parameters suvd = {
		.request_silence = downstream->control_configured && downstream->control.suvd.request_silence,
	};
	enum v92_phase4_digital_station_result result = initialize_downstream(
		&initialized, downstream, &suvd, &selected, round_trip_delay_samples);
	if (result != V92_PHASE4_DIGITAL_STATION_OK)
		return result;
	/* Reentry reuses the previous channel selection. Do not wait for a new
	 * 12000-symbol measurement that the peer need not send during RRN. */
	if (!v92_downstream_control_ready(&initialized.control))
		return V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS;
	if (!v92_phase4_digital_station_is_valid(&initialized))
		return V92_PHASE4_DIGITAL_STATION_INTERNAL_ERROR;
	*station = initialized;
	return V92_PHASE4_DIGITAL_STATION_OK;
}

static enum v92_phase4_digital_station_result fail(
	struct v92_phase4_digital_station *station)
{
	station->control.failed = true;
	return V92_PHASE4_DIGITAL_STATION_INTERNAL_ERROR;
}

enum v92_phase4_digital_station_result v92_phase4_digital_station_select_upstream(
	struct v92_phase4_digital_station *station,
	const struct v92_cpd_parameters *cpd, const struct v92_upstream_plan *plan)
{
	if (station == NULL || cpd == NULL || plan == NULL ||
	    memory_regions_overlap(station, sizeof(*station), cpd, sizeof(*cpd)) ||
	    memory_regions_overlap(station, sizeof(*station), plan, sizeof(*plan)))
		return V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if (!v92_phase4_digital_station_is_valid(station) || station->control.failed ||
	    (station->control.transmit_stage != V92_DOWNSTREAM_CONTROL_INITIAL_SUVD &&
	     station->control.transmit_stage != V92_DOWNSTREAM_CONTROL_READY_FOR_CPD) ||
	    station->receive.channel.symbols != V92_UPSTREAM_CHANNEL_SYMBOLS)
		return V92_PHASE4_DIGITAL_STATION_INVALID_STATE;
	if (!v92_upstream_plan_is_valid(plan) || cpd->acknowledges_cpu ||
	    !v92_downstream_control_select(&station->control, cpd))
		return V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS;
	station->upstream_plan = *plan;
	return V92_PHASE4_DIGITAL_STATION_OK;
}

enum v92_phase4_digital_station_result v92_phase4_digital_station_receive(
	struct v92_phase4_digital_station *station, int16_t sample)
{
	struct v92_control_event event = { 0 };
	if (station == NULL)
		return V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if (!v92_phase4_digital_station_is_valid(station))
		return V92_PHASE4_DIGITAL_STATION_INVALID_STATE;
	if (station->control.failed)
		return V92_PHASE4_DIGITAL_STATION_FAILED;
	if (station->searching_e2u && v92_e2u_rx_is_complete(&station->e2u))
		return V92_PHASE4_DIGITAL_STATION_INVALID_STATE;
	enum v92_phase4_digital_rx_result result;
	if (station->searching_e2u) {
		enum v92_e2u_rx_result received = v92_e2u_rx_search(
			&station->e2u, &station->receive.signal.control,
			v92_downstream_control_e2u_delay(&station->control) == 0u, sample, &event);
		if (received == V92_E2U_RX_COMPLETE)
			return v92_downstream_control_note_e2u(&station->control) ?
				V92_PHASE4_DIGITAL_STATION_OK : fail(station);
		if (received != V92_E2U_RX_OK)
			return fail(station);
		result = event.type == V92_CONTROL_EVENT_NONE ? V92_PHASE4_DIGITAL_RX_OK :
			V92_PHASE4_DIGITAL_RX_EVENT;
	} else {
		result = v92_phase4_digital_rx_sample(&station->receive, sample, &event);
	}
	if (!v92_downstream_control_receive(&station->control, &event))
		return fail(station);
	if (station->control.cleardown.active) {
		station->searching_e2u = false;
		return V92_PHASE4_DIGITAL_STATION_OK;
	}
	/* A complete SUVu (including a CRC-rejected repeat) marks an exact E2u
	 * candidate boundary. Start before confirmation; waiting for the SUVu
	 * ACK here would deadlock when that is the record lost on the line. */
	if (!station->searching_e2u && event.type != V92_CONTROL_EVENT_NONE &&
	    station->receive.signal.control.control.record_boundary &&
	    v92_downstream_control_silence_requested(&station->control)) {
		if (v92_e2u_rx_init(&station->e2u, &station->receive.signal.control.signal, false) != V92_E2U_RX_OK)
			return fail(station);
		station->searching_e2u = true;
	}
	if ((result != V92_PHASE4_DIGITAL_RX_OK && result != V92_PHASE4_DIGITAL_RX_ALIGNED &&
	     result != V92_PHASE4_DIGITAL_RX_EVENT) || !v92_phase4_digital_station_is_valid(station))
		return fail(station);
	return result == V92_PHASE4_DIGITAL_RX_EVENT ?
		V92_PHASE4_DIGITAL_STATION_EVENT : V92_PHASE4_DIGITAL_STATION_OK;
}

enum v92_phase4_digital_station_result v92_phase4_digital_station_transmit(
	struct v92_phase4_digital_station *station, uint8_t *network_octet)
{
	if (station == NULL || network_octet == NULL ||
	    memory_regions_overlap(station, sizeof(*station), network_octet, sizeof(*network_octet)))
		return V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if (!v92_phase4_digital_station_is_valid(station))
		return V92_PHASE4_DIGITAL_STATION_INVALID_STATE;
	if (station->control.failed)
		return V92_PHASE4_DIGITAL_STATION_FAILED;
	/* Initial training waits for channel measurement; rate renegotiation
	 * enables the inherited mapping at initialization instead. */
	if (!station->control.cleardown.active &&
	    !station->control.transmit.control_configured &&
	    station->receive.channel.symbols == V92_UPSTREAM_CHANNEL_SYMBOLS &&
	    !v92_downstream_control_ready(&station->control))
		return fail(station);
	if (!v92_downstream_control_transmit(&station->control, network_octet))
		return fail(station);
	return station->control.transmit.complete ? V92_PHASE4_DIGITAL_STATION_CLEARDOWN_COMPLETE :
		V92_PHASE4_DIGITAL_STATION_OK;
}

bool v92_phase4_digital_station_cpu_parameters(
	const struct v92_phase4_digital_station *station,
	struct v92_cpu_parameters *parameters)
{
	if (!v92_phase4_digital_station_is_valid(station) ||
	    station->control.failed || !station->control.cpu_received || parameters == NULL ||
	    memory_regions_overlap(
		    station, sizeof(*station), parameters,
		    sizeof(*parameters)))
		return false;
	*parameters = station->control.cpu;
	return true;
}

bool v92_phase4_digital_station_peer_acknowledged_cpd(
	const struct v92_phase4_digital_station *station)
{
	return v92_phase4_digital_station_is_valid(station) &&
	       !station->control.failed && !station->control.cleardown.active &&
	       !v92_downstream_control_silence_requested(&station->control) &&
	       station->control.peer_confirmation_received;
}

bool v92_phase4_digital_station_is_ed_ready(const struct v92_phase4_digital_station *station)
{
	return v92_phase4_digital_station_is_valid(station) &&
		v92_downstream_control_is_ed_ready(&station->control);
}

bool v92_phase4_digital_station_is_pause_ready(const struct v92_phase4_digital_station *station)
{
	return v92_phase4_digital_station_is_valid(station) &&
		v92_downstream_control_is_silence_ready(&station->control);
}

bool v92_phase4_digital_handoff_is_valid(
	const struct v92_phase4_digital_handoff *handoff)
{
	return handoff != NULL &&
	       handoff->e2u_delay_samples != UINT64_MAX &&
	       (!handoff->searching_e2u || !handoff->e2u.received_symbols || !handoff->e2u_delay_samples) &&
	       v92_upstream_control_line_rx_is_valid(&handoff->receive) &&
	       (!handoff->searching_e2u || (handoff->pause && !handoff->e2u.failed &&
		pause_candidate_is_valid(&handoff->e2u, &handoff->receive))) &&
	       (!handoff->pause || (handoff->receive.signal.line.renegotiation &&
		!handoff->transmit.cpd_pending &&
		handoff->transmit.transmit.encoder.mapping.kind == V90_CP_TRAINING &&
		handoff->transmit.transmit.v92_control_kind == PCM_DOWNSTREAM_V92_CONTROL_SUVD &&
		handoff->transmit.transmit.control.suvd.acknowledges_cpu)) &&
	       !v92_upstream_control_line_rx_has_failed(&handoff->receive) &&
	       handoff->receive.signal.training_tail_remaining == 0u &&
	       v92_phase4_ed_tx_is_valid(&handoff->transmit) &&
	       v92_cpu_parameters_are_valid(&handoff->cpu) &&
	       handoff->cpu.law == handoff->transmit.transmit.encoder.mapping.law &&
	       v92_pcm_mapping_is_valid(&handoff->mapping) &&
	       v92_upstream_plan_is_valid(&handoff->upstream_plan) &&
	       handoff->mapping.law == handoff->cpu.law &&
	       handoff->receive.signal.line.law == handoff->mapping.law;
}

enum v92_phase4_digital_station_result v92_phase4_digital_station_handoff(
	const struct v92_phase4_digital_station *station,
	struct v92_phase4_digital_handoff *handoff)
{
	struct v92_phase4_digital_handoff generated;
	struct v92_cpd_parameters parameters;

	if (station == NULL || handoff == NULL ||
	    memory_regions_overlap(
		    station, sizeof(*station), handoff, sizeof(*handoff)))
		return V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if ((!v92_phase4_digital_station_is_ed_ready(station) &&
	     !v92_phase4_digital_station_is_pause_ready(station)) ||
	    !v92_phase4_digital_rx_is_receiving_control(&station->receive))
		return V92_PHASE4_DIGITAL_STATION_INVALID_STATE;
	generated = (struct v92_phase4_digital_handoff){
		.receive = station->receive.signal.control,
		.cpu = station->control.cpu,
		.upstream_plan = station->upstream_plan,
		.e2u_delay_samples = v92_downstream_control_e2u_delay(&station->control),
		.pause = v92_phase4_digital_station_is_pause_ready(station),
		.searching_e2u = station->searching_e2u,
	};
	if (station->searching_e2u)
		generated.e2u = station->e2u;
	if (!v92_phase4_ed_tx_init(&generated.transmit, &station->control.transmit,
		    station->control.transmit_stage == V92_DOWNSTREAM_CONTROL_CPD ?
			    &station->control.initial_cpd : NULL))
		return V92_PHASE4_DIGITAL_STATION_INTERNAL_ERROR;
	/* The wire owner already retains every negotiated section. Decode it
	 * once at this boundary instead of reconstructing filters and levels
	 * from a default profile. This also preserves CPd-prime. */
	if (v92_cpd_decode(station->control.initial_cpd.sequence,
			   sizeof(station->control.initial_cpd.sequence),
			   station->control.initial_cpd.sequence_bits,
			   station->control.initial_cpd.frame_bits, &parameters) != V92_CPD_OK ||
	    v92_pcm_mapping_init(&generated.mapping,
				 station->control.transmit.encoder.mapping.law, &parameters) !=
		    V92_PCM_MAPPING_OK)
		return V92_PHASE4_DIGITAL_STATION_INTERNAL_ERROR;
	if (!v92_phase4_digital_handoff_is_valid(&generated))
		return V92_PHASE4_DIGITAL_STATION_INTERNAL_ERROR;
	*handoff = generated;
	return V92_PHASE4_DIGITAL_STATION_OK;
}

const char *v92_phase4_digital_station_result_name(
	enum v92_phase4_digital_station_result result)
{
	switch (result) {
	case V92_PHASE4_DIGITAL_STATION_OK:
		return "ok";
	case V92_PHASE4_DIGITAL_STATION_EVENT:
		return "event";
	case V92_PHASE4_DIGITAL_STATION_CLEARDOWN_COMPLETE:
		return "cleardown complete";
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
