// SPDX-License-Identifier: BSD-3-Clause

#include "v92_downstream_control.h"

#include "memory_region.h"

#include <string.h>

enum { CPD_REPEAT_BASE_SAMPLES = 800 };

static bool cpu_storage_is_empty(
	const struct v92_downstream_control *control)
{
	static const struct v92_cpu_parameters empty;

	return memcmp(&control->cpu, &empty, sizeof(empty)) == 0;
}

static bool suvu_storage_is_empty(
	const struct v92_downstream_control *control)
{
	static const struct v92_suvu_parameters empty;

	return memcmp(&control->suvu, &empty, sizeof(empty)) == 0;
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
	enum v92_downstream_control_stage stage)
{
	return stage == V92_DOWNSTREAM_CONTROL_INITIAL_SUVD ||
	       stage == V92_DOWNSTREAM_CONTROL_READY_FOR_CPD ||
	       stage == V92_DOWNSTREAM_CONTROL_CPD ||
	       stage == V92_DOWNSTREAM_CONTROL_STATUS_SUVD;
}

static bool downstream_state_matches(
	const struct v92_downstream_control *control)
{
	const struct pcm_downstream_tx *tx = &control->transmit;

	if (tx->protocol != PCM_PROTOCOL_V92 ||
	    tx->finish_requested || tx->complete)
		return false;
	if (!tx->control_configured)
		return control->transmit_stage == V92_DOWNSTREAM_CONTROL_INITIAL_SUVD &&
			tx->stage == PCM_DOWNSTREAM_TX_TRN2D;
	switch (control->transmit_stage) {
	case V92_DOWNSTREAM_CONTROL_INITIAL_SUVD:
	case V92_DOWNSTREAM_CONTROL_READY_FOR_CPD:
		return tx->v92_control_kind ==
			       PCM_DOWNSTREAM_V92_CONTROL_SUVD &&
		       !tx->control.suvd.acknowledges_cpu &&
		       tx->control.suvd.request_silence ==
			       control->request_silence;
	case V92_DOWNSTREAM_CONTROL_CPD: {
		return tx->v92_control_kind == PCM_DOWNSTREAM_V92_CONTROL_SUVD &&
		       (!tx->control.suvd.acknowledges_cpu ||
			control->cpu_received) &&
		       tx->control.suvd.request_silence ==
			       control->request_silence;
	}
	case V92_DOWNSTREAM_CONTROL_STATUS_SUVD:
		return tx->v92_control_kind ==
			       PCM_DOWNSTREAM_V92_CONTROL_SUVD &&
		       tx->control.suvd.acknowledges_cpu ==
			       control->transmit_acknowledges_cpu &&
		       tx->control.suvd.request_silence ==
			       control->request_silence;
	}
	return false;
}

static bool owned_state_is_valid(
	const struct v92_downstream_control *control)
{
	return v92_phase4_cpd_tx_is_valid(&control->initial_cpd) &&
	       transmit_stage_is_valid(control->transmit_stage) &&
	       (control->transmit_stage ==
			V92_DOWNSTREAM_CONTROL_INITIAL_SUVD ||
		control->transmit_stage ==
			V92_DOWNSTREAM_CONTROL_READY_FOR_CPD ||
		control->suvu_received) &&
	       ((control->transmit_stage ==
			 V92_DOWNSTREAM_CONTROL_INITIAL_SUVD ||
		 control->transmit_stage ==
			 V92_DOWNSTREAM_CONTROL_READY_FOR_CPD) ==
		(control->cpd_start_frame == 0u)) &&
	       control->cpd_start_frame <= control->transmit.accepted_frames &&
	       ((control->transmit_stage ==
			 V92_DOWNSTREAM_CONTROL_STATUS_SUVD) ==
		control->initial_cpd.complete) &&
	       (control->transmit_stage ==
			V92_DOWNSTREAM_CONTROL_CPD ?
		!control->initial_cpd.complete :
		true) &&
	       (control->transmit_stage ==
			V92_DOWNSTREAM_CONTROL_STATUS_SUVD ?
		control->status_start_frame ==
				control->cpd_start_frame +
				control->initial_cpd.sequence_bits /
					control->initial_cpd.frame_bits &&
			control->status_start_frame <=
				control->transmit.accepted_frames :
		control->status_start_frame == 0u) &&
	       (!control->cpu_received || control->suvu_received) &&
	       (!control->repeat_cpd || control->cpd_start_frame != 0u) &&
	       (!control->acknowledgement_end_frame || control->cpu_received) &&
	       (!control->peer_acknowledged_cpd || control->suvu_received) &&
	       (!control->transmit_acknowledges_cpu ||
		((control->transmit_stage ==
			 V92_DOWNSTREAM_CONTROL_CPD ||
		 control->transmit_stage ==
			 V92_DOWNSTREAM_CONTROL_STATUS_SUVD) &&
		 control->cpu_received)) &&
	       (control->suvu_received ?
		v92_suvu_parameters_are_valid(&control->suvu) :
		suvu_storage_is_empty(control)) &&
	       (control->cpu_received || control->previous_cpu ?
		(v92_cpu_parameters_are_valid(&control->cpu) &&
		 control->cpu.data_rate_number != 0u &&
		 control->cpu.law == control->transmit.encoder.mapping.law &&
		 (!control->cpu.acknowledges_cpd || control->cpu_received)) :
		cpu_storage_is_empty(control)) &&
	       control->peer_acknowledged_cpd ==
		((control->suvu_received &&
		  control->suvu.acknowledges_cpd) ||
		 (control->cpu_received &&
		  control->cpu.acknowledges_cpd)) &&
	       downstream_state_matches(control);
}

bool v92_downstream_control_is_valid(
	const struct v92_downstream_control *control)
{
	if (control == NULL ||
	    !pcm_downstream_tx_is_valid(&control->transmit) ||
	    !v92_cleardown_is_valid(&control->cleardown))
		return false;
	if (control->cleardown.active)
		return control->transmit.protocol == PCM_PROTOCOL_V92 &&
			!control->transmit.ed_requested &&
			transmit_stage_is_valid(control->transmit_stage) &&
			(!control->transmit.finish_requested ||
			 v92_cleardown_is_ready(&control->cleardown, control->transmit.transmitted_symbols)) &&
			control->cleardown.cp_end <= control->transmit.accepted_frames * V90_CP_FRAME_INTERVALS &&
			control->cleardown.acknowledgement_end <= control->transmit.accepted_frames * V90_CP_FRAME_INTERVALS &&
			v92_phase4_cpd_tx_is_valid(&control->initial_cpd);
	return owned_state_is_valid(control);
}

bool v92_downstream_control_start_cleardown(
	struct v92_downstream_control *control)
{
	if (!v92_downstream_control_is_valid(control) || control->failed)
		return false;
	if (control->cleardown.active)
		return true;
	if (control->transmit.stage != PCM_DOWNSTREAM_TX_V92_CONTROL)
		return false;
	v92_cleardown_start(&control->cleardown, control->round_trip_delay_samples, true);
	return true;
}

bool v92_downstream_control_init(struct v92_downstream_control *control,
	const struct pcm_downstream_tx *downstream,
	const struct v92_suvd_parameters *suvd, const struct v92_cpd_parameters *cpd,
	const struct v92_cpu_parameters *previous_cpu, uint16_t round_trip_delay_samples)
{
	if (control == NULL || downstream == NULL || cpd == NULL || suvd == NULL ||
	    !pcm_downstream_tx_is_valid(downstream) || downstream->protocol != PCM_PROTOCOL_V92 ||
	    !v92_suvd_parameters_are_valid(suvd) || suvd->acknowledges_cpu ||
	    (downstream->encoder.mapping.kind == V90_CP_DATA &&
	     (suvd->request_silence || previous_cpu == NULL)) ||
	    (downstream->control_configured &&
	     (downstream->v92_control_kind != PCM_DOWNSTREAM_V92_CONTROL_SUVD ||
	      downstream->control.suvd.acknowledges_cpu ||
	      downstream->control.suvd.request_silence != suvd->request_silence)) ||
	    downstream->finish_requested || downstream->complete ||
	    !v92_cpd_parameters_are_valid(cpd) || cpd->acknowledges_cpu ||
	    (previous_cpu != NULL && (!v92_cpu_parameters_are_valid(previous_cpu) ||
	     previous_cpu->data_rate_number == 0u)))
		return false;
	struct v92_downstream_control initialized = {
		.transmit = *downstream,
		.round_trip_delay_samples = round_trip_delay_samples,
		.request_silence = suvd->request_silence,
		.previous_cpu = previous_cpu != NULL,
	};
	if (previous_cpu != NULL) {
		initialized.cpu = *previous_cpu;
		initialized.cpu.acknowledges_cpd = false;
	}
	if (downstream->encoder.mapping.kind == V90_CP_DATA) {
		struct v90_pcm_mapping previous;
		if (v90_pcm_mapping_init_from_v92_cpu(&previous, previous_cpu) != V90_PCM_MAPPING_OK ||
		    !v90_pcm_mapping_equal(&previous, &downstream->encoder.mapping))
			return false;
	}
	if (!v92_phase4_cpd_tx_init(&initialized.initial_cpd, downstream, cpd))
		return false;
	if (downstream->stage == PCM_DOWNSTREAM_TX_V92_CONTROL && downstream->control_bit_index == 0u &&
	    downstream->v92_control_record_accepted)
		initialized.transmit_stage = V92_DOWNSTREAM_CONTROL_READY_FOR_CPD;
	if (!v92_downstream_control_is_valid(&initialized))
		return false;
	*control = initialized;
	return true;
}

bool v92_downstream_control_select(struct v92_downstream_control *control,
	const struct v92_cpd_parameters *cpd)
{
	struct v92_phase4_cpd_tx transmit;
	if (!v92_downstream_control_is_valid(control) || control->failed || cpd == NULL ||
	    control->cleardown.active || cpd->acknowledges_cpu ||
	    (control->transmit_stage != V92_DOWNSTREAM_CONTROL_INITIAL_SUVD &&
	     control->transmit_stage != V92_DOWNSTREAM_CONTROL_READY_FOR_CPD) ||
	    !v92_phase4_cpd_tx_init(&transmit, &control->transmit, cpd))
		return false;
	control->initial_cpd = transmit;
	return true;
}

bool v92_downstream_control_ready(struct v92_downstream_control *control)
{
	if (!v92_downstream_control_is_valid(control) || control->failed || control->cleardown.active)
		return false;
	if (control->transmit.control_configured)
		return true;
	const struct v92_suvd_parameters suvd = { .request_silence = control->request_silence };
	return pcm_downstream_tx_start_suvd(&control->transmit, &suvd) == PCM_DOWNSTREAM_TX_OK;
}

static bool fail(struct v92_downstream_control *control)
{
	control->failed = true;
	return false;
}

static bool accept_cpu(struct v92_downstream_control *control,
		       const struct v92_cpu_parameters *cpu)
{
	if (!v92_cpu_parameters_are_valid(cpu) || cpu->law != control->transmit.encoder.mapping.law)
		return false;
	if (!control->suvu_received)
		return true;
	if (!control->cpu_received) {
		control->cpu = *cpu;
		control->cpu_received = true;
	} else {
		if (!cpu_selection_equal(&control->cpu, cpu) ||
		    (control->cpu.acknowledges_cpd && !cpu->acknowledges_cpd))
			return false;
		control->cpu.acknowledges_cpd |= cpu->acknowledges_cpd;
	}
	control->peer_acknowledged_cpd = control->suvu.acknowledges_cpd || control->cpu.acknowledges_cpd;
	return true;
}

static bool accept_event(
	struct v92_downstream_control *control,
	const struct v92_control_event *event)
{
	switch (event->type) {
	case V92_CONTROL_EVENT_NONE:
		return true;
	case V92_CONTROL_EVENT_SUVU:
		if (event->record.suvu.frame_result != V92_SUVU_OK ||
		    (control->transmit.encoder.mapping.kind == V90_CP_DATA &&
		     event->record.suvu.parameters.request_silence) ||
		    !v92_suvu_parameters_are_valid(
			    &event->record.suvu.parameters))
			return false;
		if (!control->suvu_received) {
			control->suvu = event->record.suvu.parameters;
			control->suvu_received = true;
		} else {
			if (!suvu_selection_equal(
				    &control->suvu,
				    &event->record.suvu.parameters) ||
			    (control->suvu.acknowledges_cpd &&
			     !event->record.suvu.parameters.acknowledges_cpd))
				return false;
			control->suvu.acknowledges_cpd |=
				event->record.suvu.parameters.acknowledges_cpd;
		}
		control->peer_acknowledged_cpd =
			control->suvu.acknowledges_cpd ||
			(control->cpu_received &&
			 control->cpu.acknowledges_cpd);
		return true;
	case V92_CONTROL_EVENT_CPU:
		return event->record.cpu.frame_result == V92_CPU_OK &&
			accept_cpu(control, &event->record.cpu.parameters);
	case V92_CONTROL_EVENT_CPUS: {
		/* Table 24 changes only the rate and acknowledgement. Initial
		 * training has no preceding mapping, so still requires long CPu. */
		if (!control->previous_cpu ||
		    event->record.cpus.frame_result != V92_CPU_OK)
			return false;
		struct v92_cpu_parameters cpu = control->cpu;
		cpu.data_rate_number = event->record.cpus.parameters.data_rate_number;
		cpu.acknowledges_cpd = event->record.cpus.parameters.acknowledges_cpd;
		return accept_cpu(control, &cpu);
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

bool v92_downstream_control_receive(struct v92_downstream_control *control,
	const struct v92_control_event *received)
{
	if (!v92_downstream_control_is_valid(control) || control->failed || received == NULL)
		return false;
	struct v92_control_event event = *received;
	/* Decoded events are a public boundary: a malformed zero-rate record
	 * must not bypass selection validation and trigger cleardown. */
	if ((event.type == V92_CONTROL_EVENT_CPU &&
	     (event.record.cpu.frame_result != V92_CPU_OK ||
	      !v92_cpu_parameters_are_valid(&event.record.cpu.parameters) ||
	      event.record.cpu.parameters.law != control->transmit.encoder.mapping.law)) ||
	    (event.type == V92_CONTROL_EVENT_CPUS &&
	     (event.record.cpus.frame_result != V92_CPU_OK ||
	      event.record.cpus.parameters.data_rate_number > V92_CPU_MAX_DATA_RATE_NUMBER)))
		return fail(control);
	if ((event.type == V92_CONTROL_EVENT_CPU && event.record.cpu.frame_result == V92_CPU_OK) ||
	    (event.type == V92_CONTROL_EVENT_CPUS && event.record.cpus.frame_result == V92_CPU_OK)) {
		bool zero = event.type == V92_CONTROL_EVENT_CPU ?
			event.record.cpu.parameters.data_rate_number == 0u :
			event.record.cpus.parameters.data_rate_number == 0u;
		bool ack = event.type == V92_CONTROL_EVENT_CPU ?
			event.record.cpu.parameters.acknowledges_cpd :
			event.record.cpus.parameters.acknowledges_cpd;

		if (zero)
			v92_cleardown_start(&control->cleardown, control->round_trip_delay_samples, false);
		if (control->cleardown.active)
			v92_cleardown_receive(&control->cleardown, zero, ack,
				control->transmit.transmitted_symbols);
	}
	if (control->cleardown.active)
		return true;
	if (!accept_event(control, &event))
		return fail(control);
	/* V.92 9.6.1.1.3: expiry alone is insufficient. Wait through a whole
	 * valid CPu/SUVu at or after the deadline; damaged records do not count.
	 * The output clock includes queued coder frames, so use the CPd's wire
	 * end rather than the earlier instant its last source frame was accepted. */
	if (!control->peer_acknowledged_cpd &&
	    control->transmit_stage == V92_DOWNSTREAM_CONTROL_STATUS_SUVD &&
	    (event.type == V92_CONTROL_EVENT_CPU ||
	     event.type == V92_CONTROL_EVENT_CPUS ||
	     event.type == V92_CONTROL_EVENT_SUVU) &&
	    control->transmit.transmitted_symbols / V90_CP_FRAME_INTERVALS >=
		    control->status_start_frame &&
	    control->transmit.transmitted_symbols -
		    control->status_start_frame * V90_CP_FRAME_INTERVALS >=
			    (unsigned int)CPD_REPEAT_BASE_SAMPLES + control->round_trip_delay_samples)
		control->repeat_cpd = true;
	if (!owned_state_is_valid(control))
		return fail(control);
	return true;
}

static bool apply_pending_downstream(
	struct v92_downstream_control *control)
{
	if (!pcm_downstream_tx_is_v92_control_boundary(&control->transmit))
		return true;
	if (control->transmit_stage ==
	    V92_DOWNSTREAM_CONTROL_INITIAL_SUVD) {
		/* An early SUVu may arrive while Rf-bar is still being sent. The
		 * freshly initialized coder must send its first SUVd before CPd;
		 * a source boundary alone does not prove that SUVd was sent. */
		if (!control->transmit.v92_control_record_accepted)
			return true;
		control->transmit_stage =
			V92_DOWNSTREAM_CONTROL_READY_FOR_CPD;
	}
	if (control->suvu_received &&
	    (control->transmit_stage == V92_DOWNSTREAM_CONTROL_READY_FOR_CPD ||
	     (control->transmit_stage == V92_DOWNSTREAM_CONTROL_STATUS_SUVD &&
	      control->repeat_cpd && !v92_downstream_control_is_ed_ready(control)))) {
		struct v92_cpd_parameters cpd;
		const struct v92_suvd_parameters suvd = {
			.request_silence = control->request_silence,
			.acknowledges_cpu = control->cpu_received,
		};

		if (v92_cpd_decode(control->initial_cpd.sequence,
				   sizeof(control->initial_cpd.sequence),
				   control->initial_cpd.sequence_bits,
				   control->initial_cpd.frame_bits, &cpd) != V92_CPD_OK)
			return false;
		cpd.acknowledges_cpu = control->cpu_received;
		if (pcm_downstream_tx_start_suvd(
			    &control->transmit, &suvd) !=
			    PCM_DOWNSTREAM_TX_OK ||
		    !v92_phase4_cpd_tx_init(
			    &control->initial_cpd, &control->transmit, &cpd))
			return false;
		control->transmit_stage = V92_DOWNSTREAM_CONTROL_CPD;
		control->transmit_acknowledges_cpu = cpd.acknowledges_cpu;
		control->cpd_start_frame = control->transmit.accepted_frames;
		control->status_start_frame = 0u;
		if (cpd.acknowledges_cpu && !control->acknowledgement_end_frame)
			control->acknowledgement_end_frame = control->cpd_start_frame +
				control->initial_cpd.sequence_bits / control->initial_cpd.frame_bits;
		return true;
	}
	if (control->transmit_stage ==
		    V92_DOWNSTREAM_CONTROL_CPD &&
	    control->cpu_received &&
	    !control->transmit.control.suvd.acknowledges_cpu) {
		const struct v92_suvd_parameters suvd = {
			.request_silence = control->request_silence,
			.acknowledges_cpu = true,
		};

		if (pcm_downstream_tx_start_suvd(
			    &control->transmit, &suvd) !=
		    PCM_DOWNSTREAM_TX_OK)
			return false;
	}
	if (control->transmit_stage ==
		    V92_DOWNSTREAM_CONTROL_STATUS_SUVD &&
	    control->cpu_received &&
	    !control->transmit_acknowledges_cpu) {
		const struct v92_suvd_parameters suvd = {
			.request_silence = control->request_silence,
			.acknowledges_cpu = control->cpu_received,
		};

		if (pcm_downstream_tx_start_suvd(
			    &control->transmit, &suvd) !=
		    PCM_DOWNSTREAM_TX_OK)
			return false;
		control->transmit_stage =
			V92_DOWNSTREAM_CONTROL_STATUS_SUVD;
		control->transmit_acknowledges_cpu =
			suvd.acknowledges_cpu;
	}
	return true;
}

static bool transmit_cleardown(
	struct v92_downstream_control *control, uint8_t *network_octet)
{
	struct pcm_downstream_tx *tx = &control->transmit;
	struct pcm_downstream_symbol symbol;
	enum pcm_downstream_tx_result result;

	if (!tx->control_configured) {
		const struct v92_suvd_parameters status = { 0 };
		if (pcm_downstream_tx_start_suvd(tx, &status) != PCM_DOWNSTREAM_TX_OK)
			return fail(control);
	}
	/* An externally sourced long CPd also owns its unsent tail. */
	if (control->transmit_stage == V92_DOWNSTREAM_CONTROL_CPD &&
	    !control->initial_cpd.complete) {
		if (!v92_phase4_cpd_tx_next(&control->initial_cpd, tx, &symbol))
			return fail(control);
		*network_octet = symbol.pcm.octet;
		return true;
	}
	if (pcm_downstream_tx_is_v92_control_boundary(tx)) {
		bool zero = tx->v92_control_kind == PCM_DOWNSTREAM_V92_CONTROL_CPD &&
			tx->control.cpd.data_rate_number == 0u;

		if (zero && tx->v92_control_record_accepted)
			v92_cleardown_sent(&control->cleardown,
				tx->accepted_frames * V90_CP_FRAME_INTERVALS,
				tx->control.cpd.acknowledges_cpu);
		if (v92_cleardown_is_ready(&control->cleardown, tx->transmitted_symbols)) {
			if (pcm_downstream_tx_request_finish(tx) != PCM_DOWNSTREAM_TX_OK)
				return fail(control);
		} else if (!zero || tx->control.cpd.acknowledges_cpu != control->cleardown.peer_requested) {
			struct v92_cpd_parameters previous;

			if (v92_cpd_decode(control->initial_cpd.sequence, sizeof(control->initial_cpd.sequence),
				    control->initial_cpd.sequence_bits, control->initial_cpd.frame_bits,
				    &previous) != V92_CPD_OK)
				return fail(control);
			const struct v92_cpd_base_parameters cpd = {
				.four_times_gain_q0_16 = previous.four_times_gain_q0_16,
				.trellis = previous.trellis,
				.acknowledges_cpu = control->cleardown.peer_requested,
			};
			if (pcm_downstream_tx_start_cpd(tx, &cpd) != PCM_DOWNSTREAM_TX_OK)
				return fail(control);
		}
	}
	result = pcm_downstream_tx_next(tx, &symbol);
	if (result == PCM_DOWNSTREAM_TX_COMPLETE)
		return true;
	if (result != PCM_DOWNSTREAM_TX_OK)
		return fail(control);
	*network_octet = symbol.pcm.octet;
	return true;
}

bool v92_downstream_control_transmit(
	struct v92_downstream_control *control, uint8_t *network_octet)
{
	struct pcm_downstream_symbol symbol;

	if (control == NULL || network_octet == NULL ||
	    memory_regions_overlap(
		    control, sizeof(*control), network_octet,
		    sizeof(*network_octet)))
		return false;
	if (!v92_downstream_control_is_valid(control))
		return false;
	if (control->failed)
		return false;
	if (control->cleardown.active)
		return transmit_cleardown(control, network_octet);
	if (!apply_pending_downstream(control))
		return fail(control);
	if (control->transmit_stage == V92_DOWNSTREAM_CONTROL_CPD) {
		if (!v92_phase4_cpd_tx_next(
			    &control->initial_cpd, &control->transmit, &symbol))
			return fail(control);
		if (control->initial_cpd.complete) {
			control->transmit_stage =
				V92_DOWNSTREAM_CONTROL_STATUS_SUVD;
			control->status_start_frame =
				control->cpd_start_frame +
				control->initial_cpd.sequence_bits /
					control->initial_cpd.frame_bits;
			control->transmit_acknowledges_cpu =
				control->transmit.control.suvd.acknowledges_cpu;
		}
	} else {
		uint64_t before = control->transmit.accepted_frames;
		uint64_t record_end = before +
			(control->transmit.control_sequence_bits - control->transmit.control_bit_index) /
				control->transmit.encoder.mapping.frame_bits;

		if (pcm_downstream_tx_next(&control->transmit, &symbol) != PCM_DOWNSTREAM_TX_OK)
			return fail(control);
		/* Merely updating the stored SUVd while CPd is in flight does not
		 * send an acknowledgement. Count the record only once sourced. */
		if (control->transmit_acknowledges_cpu && !control->acknowledgement_end_frame &&
		    control->transmit.accepted_frames > before)
			control->acknowledgement_end_frame = record_end;
	}
	if (!owned_state_is_valid(control))
		return fail(control);
	*network_octet = symbol.pcm.octet;
	return true;
}

bool v92_downstream_control_is_ed_ready(
	const struct v92_downstream_control *control)
{
	/* The receive record boundary is authoritative for E2u. The downstream
	 * owner can finish a partial CPd/SUVd before Ed; waiting here would discard
	 * the leading E2u symbols while the independent transmitter drains. */
	return v92_downstream_control_is_valid(control) &&
	       !control->failed && !control->cleardown.active && control->cpu_received &&
	       control->peer_acknowledged_cpd &&
	       control->transmit_acknowledges_cpu &&
	       control->acknowledgement_end_frame != 0u &&
	       control->transmit.accepted_frames >= control->acknowledgement_end_frame;
}
