// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase4_digital_station.h"

#include "memory_region.h"

#include <stddef.h>
#include <string.h>

enum { CPD_REPEAT_BASE_SAMPLES = 800 };

static enum g711_law receive_law(
	const struct v92_phase4_digital_rx *receiver)
{
	return receiver->stage == V92_PHASE4_DIGITAL_RX_TRN2U ?
		       receiver->signal.trn2u.law :
		       receiver->signal.control.signal.line.law;
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

	if (tx->protocol != PCM_PROTOCOL_V92 ||
	    tx->finish_requested || tx->complete)
		return false;
	if (!tx->control_configured)
		return station->transmit_stage == V92_PHASE4_DIGITAL_STATION_INITIAL_SUVD &&
			tx->stage == PCM_DOWNSTREAM_TX_TRN2D;
	switch (station->transmit_stage) {
	case V92_PHASE4_DIGITAL_STATION_INITIAL_SUVD:
	case V92_PHASE4_DIGITAL_STATION_READY_FOR_CPD:
		return tx->v92_control_kind ==
			       PCM_DOWNSTREAM_V92_CONTROL_SUVD &&
		       !tx->control.suvd.acknowledges_cpu &&
		       tx->control.suvd.request_silence ==
			       station->request_silence;
	case V92_PHASE4_DIGITAL_STATION_CPD: {
		return tx->v92_control_kind == PCM_DOWNSTREAM_V92_CONTROL_SUVD &&
		       (!tx->control.suvd.acknowledges_cpu ||
			station->cpu_received) &&
		       tx->control.suvd.request_silence ==
			       station->request_silence;
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
	       v92_phase4_cpd_tx_is_valid(&station->initial_cpd) &&
	       v92_upstream_plan_is_valid(&station->upstream_plan) &&
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
	       ((station->transmit_stage ==
			 V92_PHASE4_DIGITAL_STATION_STATUS_SUVD) ==
		station->initial_cpd.complete) &&
	       (station->transmit_stage ==
			V92_PHASE4_DIGITAL_STATION_CPD ?
		!station->initial_cpd.complete :
		true) &&
	       (station->transmit_stage ==
			V92_PHASE4_DIGITAL_STATION_STATUS_SUVD ?
		station->status_start_frame ==
				station->cpd_start_frame +
				station->initial_cpd.sequence_bits /
					station->initial_cpd.frame_bits &&
			station->status_start_frame <=
				station->transmit.accepted_frames :
		station->status_start_frame == 0u) &&
	       (!station->cpu_received || station->suvu_received) &&
	       (!station->repeat_cpd || station->cpd_start_frame != 0u) &&
	       (!station->acknowledgement_end_frame || station->cpu_received) &&
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
	       (station->cpu_received || v92_phase4_digital_rx_is_renegotiating(&station->receive) ?
		(v92_cpu_parameters_are_valid(&station->cpu) &&
		 station->cpu.data_rate_number != 0u &&
		 station->cpu.law == station->transmit.encoder.mapping.law &&
		 (!station->cpu.acknowledges_cpd || station->cpu_received)) :
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
	if (station == NULL ||
	    !v92_phase4_digital_rx_is_valid(&station->receive) ||
	    !pcm_downstream_tx_is_valid(&station->transmit) ||
	    !v92_cleardown_is_valid(&station->cleardown))
		return false;
	if (station->cleardown.active)
		return station->transmit.protocol == PCM_PROTOCOL_V92 &&
			!station->transmit.ed_requested &&
			transmit_stage_is_valid(station->transmit_stage) &&
			(!station->transmit.finish_requested ||
			 v92_cleardown_is_ready(&station->cleardown, station->transmit.transmitted_symbols)) &&
			station->cleardown.cp_end <= station->transmit.accepted_frames * V90_CP_FRAME_INTERVALS &&
			station->cleardown.acknowledgement_end <= station->transmit.accepted_frames * V90_CP_FRAME_INTERVALS &&
			receive_law(&station->receive) == station->transmit.encoder.mapping.law &&
			v92_phase4_cpd_tx_is_valid(&station->initial_cpd);
	return owned_state_is_valid(station);
}

bool v92_phase4_digital_station_has_failed(
	const struct v92_phase4_digital_station *station)
{
	return v92_phase4_digital_station_is_valid(station) &&
	       station->failed;
}

bool v92_phase4_digital_station_start_cleardown(
	struct v92_phase4_digital_station *station)
{
	if (!v92_phase4_digital_station_is_valid(station) || station->failed)
		return false;
	if (station->cleardown.active)
		return true;
	if (station->transmit.stage != PCM_DOWNSTREAM_TX_V92_CONTROL)
		return false;
	v92_cleardown_start(&station->cleardown, station->round_trip_delay_samples, true);
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
	if (!pcm_downstream_tx_is_valid(downstream) ||
	    downstream->protocol != PCM_PROTOCOL_V92 ||
	    !v92_suvd_parameters_are_valid(suvd) || suvd->acknowledges_cpu ||
	    (downstream->control_configured &&
	     (downstream->v92_control_kind != PCM_DOWNSTREAM_V92_CONTROL_SUVD ||
	      downstream->control.suvd.acknowledges_cpu ||
	      downstream->control.suvd.request_silence != suvd->request_silence)) ||
	    downstream->finish_requested || downstream->complete ||
	    !v92_cpd_parameters_are_valid(cpd) ||
	    cpd->acknowledges_cpu)
		return V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS;
	initialized->transmit = *downstream;
	initialized->round_trip_delay_samples = round_trip_delay_samples;
	if (!v92_phase4_cpd_tx_init(
		    &initialized->initial_cpd, downstream, cpd))
		return V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS;
	if (downstream->stage == PCM_DOWNSTREAM_TX_V92_CONTROL &&
	    downstream->control_bit_index == 0u)
		initialized->transmit_stage =
			V92_PHASE4_DIGITAL_STATION_READY_FOR_CPD;
	initialized->request_silence = suvd->request_silence;
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
	const struct v92_suvd_parameters suvd = { 0 };

	if (station == NULL || ru == NULL || downstream == NULL || cpd == NULL ||
	    plan == NULL || cpu == NULL)
		return V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if (!v92_upstream_plan_is_valid(plan) || !v92_cpu_parameters_are_valid(cpu) ||
	    !v92_cpd_parameters_are_valid(cpd) || !cpu->data_rate_number || !cpd->data_rate_number ||
	    v92_phase4_digital_rx_init_after_ru(&initialized.receive, ru) != V92_PHASE4_DIGITAL_RX_OK)
		return V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS;
	initialized.cpu = *cpu;
	initialized.cpu.acknowledges_cpd = false;
	initialized.upstream_plan = *plan;
	selected = *cpd;
	selected.acknowledges_cpu = false;
	/* V.92 Table 30: do not carry initial E2u extension into RRN. */
	selected.extend_e2u = false;
	enum v92_phase4_digital_station_result result = initialize_downstream(
		&initialized, downstream, &suvd, &selected, round_trip_delay_samples);
	if (result != V92_PHASE4_DIGITAL_STATION_OK)
		return result;
	/* Reentry reuses the previous channel selection. Do not wait for a new
	 * 12000-symbol measurement that the peer need not send during RRN. */
	if (!initialized.transmit.control_configured &&
	    pcm_downstream_tx_start_suvd(&initialized.transmit, &suvd) != PCM_DOWNSTREAM_TX_OK)
		return V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS;
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

enum v92_phase4_digital_station_result v92_phase4_digital_station_select_upstream(
	struct v92_phase4_digital_station *station,
	const struct v92_cpd_parameters *cpd, const struct v92_upstream_plan *plan)
{
	struct v92_phase4_cpd_tx transmit;

	if (station == NULL || cpd == NULL || plan == NULL ||
	    memory_regions_overlap(station, sizeof(*station), cpd, sizeof(*cpd)) ||
	    memory_regions_overlap(station, sizeof(*station), plan, sizeof(*plan)))
		return V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if (!v92_phase4_digital_station_is_valid(station) || station->failed ||
	    (station->transmit_stage != V92_PHASE4_DIGITAL_STATION_INITIAL_SUVD &&
	     station->transmit_stage != V92_PHASE4_DIGITAL_STATION_READY_FOR_CPD) ||
	    station->receive.channel.symbols != V92_UPSTREAM_CHANNEL_SYMBOLS)
		return V92_PHASE4_DIGITAL_STATION_INVALID_STATE;
	if (!v92_upstream_plan_is_valid(plan) || cpd->acknowledges_cpu ||
	    !v92_phase4_cpd_tx_init(&transmit, &station->transmit, cpd))
		return V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS;
	station->initial_cpd = transmit;
	station->upstream_plan = *plan;
	return V92_PHASE4_DIGITAL_STATION_OK;
}

static bool accept_cpu(struct v92_phase4_digital_station *station,
		       const struct v92_cpu_parameters *cpu)
{
	if (!v92_cpu_parameters_are_valid(cpu) || cpu->law != station->transmit.encoder.mapping.law)
		return false;
	if (!station->suvu_received)
		return true;
	if (!station->cpu_received) {
		station->cpu = *cpu;
		station->cpu_received = true;
	} else {
		if (!cpu_selection_equal(&station->cpu, cpu) ||
		    (station->cpu.acknowledges_cpd && !cpu->acknowledges_cpd))
			return false;
		station->cpu.acknowledges_cpd |= cpu->acknowledges_cpd;
	}
	station->peer_acknowledged_cpd = station->suvu.acknowledges_cpd || station->cpu.acknowledges_cpd;
	return true;
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
		return event->record.cpu.frame_result == V92_CPU_OK &&
			accept_cpu(station, &event->record.cpu.parameters);
	case V92_CONTROL_EVENT_CPUS: {
		/* Table 24 changes only the rate and acknowledgement. Initial
		 * training has no preceding mapping, so still requires long CPu. */
		if (!v92_phase4_digital_rx_is_renegotiating(&station->receive) ||
		    event->record.cpus.frame_result != V92_CPU_OK)
			return false;
		struct v92_cpu_parameters cpu = station->cpu;
		cpu.data_rate_number = event->record.cpus.parameters.data_rate_number;
		cpu.acknowledges_cpd = event->record.cpus.parameters.acknowledges_cpd;
		return accept_cpu(station, &cpu);
	}
	case V92_CONTROL_EVENT_SUVU_REJECTED:
	case V92_CONTROL_EVENT_CPU_REJECTED:
	case V92_CONTROL_EVENT_CPUS_REJECTED:
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
	if (result == V92_PHASE4_DIGITAL_RX_EVENT &&
	    (event.type == V92_CONTROL_EVENT_CPU || event.type == V92_CONTROL_EVENT_CPUS)) {
		bool zero = event.type == V92_CONTROL_EVENT_CPU ?
			event.record.cpu.parameters.data_rate_number == 0u :
			event.record.cpus.parameters.data_rate_number == 0u;
		bool ack = event.type == V92_CONTROL_EVENT_CPU ?
			event.record.cpu.parameters.acknowledges_cpd :
			event.record.cpus.parameters.acknowledges_cpd;

		if (zero)
			v92_cleardown_start(&station->cleardown, station->round_trip_delay_samples, false);
		if (station->cleardown.active)
			v92_cleardown_receive(&station->cleardown, zero, ack,
				station->transmit.transmitted_symbols);
	}
	if (station->cleardown.active)
		return V92_PHASE4_DIGITAL_STATION_OK;
	if (result == V92_PHASE4_DIGITAL_RX_FAILED)
		return fail(station);
	if (result != V92_PHASE4_DIGITAL_RX_OK &&
	    result != V92_PHASE4_DIGITAL_RX_ALIGNED &&
	    result != V92_PHASE4_DIGITAL_RX_EVENT)
		return fail(station);
	if (!accept_event(station, &event))
		return fail(station);
	/* V.92 9.6.1.1.3: expiry alone is insufficient. Wait through a whole
	 * valid CPu/SUVu at or after the deadline; damaged records do not count.
	 * The output clock includes queued coder frames, so use the CPd's wire
	 * end rather than the earlier instant its last source frame was accepted. */
	if (!station->peer_acknowledged_cpd &&
	    station->transmit_stage == V92_PHASE4_DIGITAL_STATION_STATUS_SUVD &&
	    (event.type == V92_CONTROL_EVENT_CPU ||
	     event.type == V92_CONTROL_EVENT_CPUS ||
	     event.type == V92_CONTROL_EVENT_SUVU) &&
	    station->transmit.transmitted_symbols / V90_CP_FRAME_INTERVALS >=
		    station->status_start_frame &&
	    station->transmit.transmitted_symbols -
		    station->status_start_frame * V90_CP_FRAME_INTERVALS >=
			    (unsigned int)CPD_REPEAT_BASE_SAMPLES + station->round_trip_delay_samples)
		station->repeat_cpd = true;
	if (!owned_state_is_valid(station))
		return fail(station);
	return result == V92_PHASE4_DIGITAL_RX_EVENT ?
		       V92_PHASE4_DIGITAL_STATION_EVENT :
		       V92_PHASE4_DIGITAL_STATION_OK;
}

static bool apply_pending_downstream(
	struct v92_phase4_digital_station *station)
{
	/* SUVd permits the analogue modem to end TRN2u early (9.6.2.1.1).
	 * An unconfigured transmitter keeps TRN2d continuous until we have
	 * the complete measurement, then advertises readiness for controls. */
	if (!station->transmit.control_configured &&
	    station->receive.channel.symbols == V92_UPSTREAM_CHANNEL_SYMBOLS) {
		const struct v92_suvd_parameters suvd = { .request_silence = station->request_silence };

		if (pcm_downstream_tx_start_suvd(&station->transmit, &suvd) != PCM_DOWNSTREAM_TX_OK)
			return false;
	}
	if (!pcm_downstream_tx_is_v92_control_boundary(&station->transmit))
		return true;
	if (station->transmit_stage ==
	    V92_PHASE4_DIGITAL_STATION_INITIAL_SUVD) {
		station->transmit_stage =
			V92_PHASE4_DIGITAL_STATION_READY_FOR_CPD;
	}
	if (station->suvu_received &&
	    (station->transmit_stage == V92_PHASE4_DIGITAL_STATION_READY_FOR_CPD ||
	     (station->transmit_stage == V92_PHASE4_DIGITAL_STATION_STATUS_SUVD &&
	      station->repeat_cpd && !v92_phase4_digital_station_is_ed_ready(station)))) {
		struct v92_cpd_parameters cpd;
		const struct v92_suvd_parameters suvd = {
			.request_silence = station->request_silence,
			.acknowledges_cpu = station->cpu_received,
		};

		if (v92_cpd_decode(station->initial_cpd.sequence,
				   sizeof(station->initial_cpd.sequence),
				   station->initial_cpd.sequence_bits,
				   station->initial_cpd.frame_bits, &cpd) != V92_CPD_OK)
			return false;
		cpd.acknowledges_cpu = station->cpu_received;
		if (pcm_downstream_tx_start_suvd(
			    &station->transmit, &suvd) !=
			    PCM_DOWNSTREAM_TX_OK ||
		    !v92_phase4_cpd_tx_init(
			    &station->initial_cpd, &station->transmit, &cpd))
			return false;
		station->transmit_stage = V92_PHASE4_DIGITAL_STATION_CPD;
		station->transmit_acknowledges_cpu = cpd.acknowledges_cpu;
		station->cpd_start_frame = station->transmit.accepted_frames;
		station->status_start_frame = 0u;
		if (cpd.acknowledges_cpu && !station->acknowledgement_end_frame)
			station->acknowledgement_end_frame = station->cpd_start_frame +
				station->initial_cpd.sequence_bits / station->initial_cpd.frame_bits;
		return true;
	}
	if (station->transmit_stage ==
		    V92_PHASE4_DIGITAL_STATION_CPD &&
	    station->cpu_received &&
	    !station->transmit.control.suvd.acknowledges_cpu) {
		const struct v92_suvd_parameters suvd = {
			.request_silence = station->request_silence,
			.acknowledges_cpu = true,
		};

		if (pcm_downstream_tx_start_suvd(
			    &station->transmit, &suvd) !=
		    PCM_DOWNSTREAM_TX_OK)
			return false;
	}
	if (station->transmit_stage ==
		    V92_PHASE4_DIGITAL_STATION_STATUS_SUVD &&
	    station->cpu_received &&
	    !station->transmit_acknowledges_cpu) {
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

static enum v92_phase4_digital_station_result transmit_cleardown(
	struct v92_phase4_digital_station *station, uint8_t *network_octet)
{
	struct pcm_downstream_tx *tx = &station->transmit;
	struct pcm_downstream_symbol symbol;
	enum pcm_downstream_tx_result result;

	if (!tx->control_configured) {
		const struct v92_suvd_parameters status = { 0 };
		if (pcm_downstream_tx_start_suvd(tx, &status) != PCM_DOWNSTREAM_TX_OK)
			return fail(station);
	}
	/* An externally sourced long CPd also owns its unsent tail. */
	if (station->transmit_stage == V92_PHASE4_DIGITAL_STATION_CPD &&
	    !station->initial_cpd.complete) {
		if (!v92_phase4_cpd_tx_next(&station->initial_cpd, tx, &symbol))
			return fail(station);
		*network_octet = symbol.pcm.octet;
		return V92_PHASE4_DIGITAL_STATION_OK;
	}
	if (pcm_downstream_tx_is_v92_control_boundary(tx)) {
		bool zero = tx->v92_control_kind == PCM_DOWNSTREAM_V92_CONTROL_CPD &&
			tx->control.cpd.data_rate_number == 0u;

		if (zero && tx->v92_control_record_accepted)
			v92_cleardown_sent(&station->cleardown,
				tx->accepted_frames * V90_CP_FRAME_INTERVALS,
				tx->control.cpd.acknowledges_cpu);
		if (v92_cleardown_is_ready(&station->cleardown, tx->transmitted_symbols)) {
			if (pcm_downstream_tx_request_finish(tx) != PCM_DOWNSTREAM_TX_OK)
				return fail(station);
		} else if (!zero || tx->control.cpd.acknowledges_cpu != station->cleardown.peer_requested) {
			struct v92_cpd_parameters previous;

			if (v92_cpd_decode(station->initial_cpd.sequence, sizeof(station->initial_cpd.sequence),
				    station->initial_cpd.sequence_bits, station->initial_cpd.frame_bits,
				    &previous) != V92_CPD_OK)
				return fail(station);
			const struct v92_cpd_base_parameters cpd = {
				.four_times_gain_q0_16 = previous.four_times_gain_q0_16,
				.trellis = previous.trellis,
				.acknowledges_cpu = station->cleardown.peer_requested,
			};
			if (pcm_downstream_tx_start_cpd(tx, &cpd) != PCM_DOWNSTREAM_TX_OK)
				return fail(station);
		}
	}
	result = pcm_downstream_tx_next(tx, &symbol);
	if (result == PCM_DOWNSTREAM_TX_COMPLETE)
		return V92_PHASE4_DIGITAL_STATION_CLEARDOWN_COMPLETE;
	if (result != PCM_DOWNSTREAM_TX_OK)
		return fail(station);
	*network_octet = symbol.pcm.octet;
	return V92_PHASE4_DIGITAL_STATION_OK;
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
	if (station->cleardown.active)
		return transmit_cleardown(station, network_octet);
	if (!apply_pending_downstream(station))
		return fail(station);
	if (station->transmit_stage == V92_PHASE4_DIGITAL_STATION_CPD) {
		if (!v92_phase4_cpd_tx_next(
			    &station->initial_cpd, &station->transmit, &symbol))
			return fail(station);
		if (station->initial_cpd.complete) {
			station->transmit_stage =
				V92_PHASE4_DIGITAL_STATION_STATUS_SUVD;
			station->status_start_frame =
				station->cpd_start_frame +
				station->initial_cpd.sequence_bits /
					station->initial_cpd.frame_bits;
			station->transmit_acknowledges_cpu =
				station->transmit.control.suvd.acknowledges_cpu;
		}
	} else {
		uint64_t before = station->transmit.accepted_frames;
		uint64_t record_end = before +
			(station->transmit.control_sequence_bits - station->transmit.control_bit_index) /
				station->transmit.encoder.mapping.frame_bits;

		if (pcm_downstream_tx_next(&station->transmit, &symbol) != PCM_DOWNSTREAM_TX_OK)
			return fail(station);
		/* Merely updating the stored SUVd while CPd is in flight does not
		 * send an acknowledgement. Count the record only once sourced. */
		if (station->transmit_acknowledges_cpu && !station->acknowledgement_end_frame &&
		    station->transmit.accepted_frames > before)
			station->acknowledgement_end_frame = record_end;
	}
	if (!owned_state_is_valid(station))
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

bool v92_phase4_digital_station_is_ed_ready(
	const struct v92_phase4_digital_station *station)
{
	/* The receive record boundary is authoritative for E2u. The downstream
	 * owner can finish a partial CPd/SUVd before Ed; waiting here would discard
	 * the leading E2u symbols while the independent transmitter drains. */
	return v92_phase4_digital_station_is_valid(station) &&
	       !station->failed && !station->cleardown.active && station->cpu_received &&
	       station->peer_acknowledged_cpd &&
	       station->transmit_acknowledges_cpu &&
	       station->acknowledgement_end_frame != 0u &&
	       station->transmit.accepted_frames >= station->acknowledgement_end_frame;
}

bool v92_phase4_digital_handoff_is_valid(
	const struct v92_phase4_digital_handoff *handoff)
{
	return handoff != NULL &&
	       v92_upstream_control_line_rx_is_valid(&handoff->receive) &&
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
	if (!v92_phase4_digital_station_is_ed_ready(station) ||
	    station->receive.stage != V92_PHASE4_DIGITAL_RX_CONTROL)
		return V92_PHASE4_DIGITAL_STATION_INVALID_STATE;
	generated = (struct v92_phase4_digital_handoff){
		.receive = station->receive.signal.control,
		.cpu = station->cpu,
		.upstream_plan = station->upstream_plan,
	};
	if (!v92_phase4_ed_tx_init(&generated.transmit, &station->transmit,
		    station->transmit_stage == V92_PHASE4_DIGITAL_STATION_CPD ?
			    &station->initial_cpd : NULL))
		return V92_PHASE4_DIGITAL_STATION_INTERNAL_ERROR;
	/* The wire owner already retains every negotiated section. Decode it
	 * once at this boundary instead of reconstructing filters and levels
	 * from a default profile. This also preserves CPd-prime. */
	if (v92_cpd_decode(station->initial_cpd.sequence,
			   sizeof(station->initial_cpd.sequence),
			   station->initial_cpd.sequence_bits,
			   station->initial_cpd.frame_bits, &parameters) != V92_CPD_OK ||
	    v92_pcm_mapping_init(&generated.mapping,
				 station->transmit.encoder.mapping.law, &parameters) !=
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
