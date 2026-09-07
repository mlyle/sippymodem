// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_call.h"

#include "memory_region.h"
#include "pcm_phase3_entry.h"
#include "pcm_phase3_handoff.h"
#include "v34_phase2_selection.h"
#include "v92_pcm_dil.h"

#include <stddef.h>
#include <string.h>

enum {
	V90_NETWORK_CHARACTERS_PER_SECOND = 8000,
	V90_UPSTREAM_RATE_UNIT = 2400,
	V90_PHASE3_RETURN_BASE_DEADLINE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 51 / 10,
	V90_PHASE3_SD_DEADLINE_SAMPLES = V34_PHASE2_SAMPLE_RATE * 3 / 2,
	V90_B1_BASE_DEADLINE_SAMPLES = V34_PHASE2_SAMPLE_RATE * 15,
	V90_B1_ROUND_TRIP_ALLOWANCES = 5,
	V90_RATE_RENEGOTIATION_BASE_DEADLINE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 5,
	V90_RATE_RENEGOTIATION_ROUND_TRIP_ALLOWANCES = 2,
	V92_CPU_REPEAT_BASE_SAMPLES = 800,
};

_Static_assert(PCM_PROTOCOL_V90 == 0,
	"zero-initialized PCM call configurations must select V.90");

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

static enum v34_station_role phase2_station_role(
	enum v90_modem_role role)
{
	return role == V90_ANALOG_MODEM ?
		       V34_ANSWERING_STATION : V34_CALLING_STATION;
}

static bool stored_local_info0_is_valid(const struct pcm_call *call)
{
	if (call->protocol == PCM_PROTOCOL_V92) {
		if (call->local_role == V90_ANALOG_MODEM)
			return v92_info0a_is_valid(
				       &call->local_info0.v92_analog) &&
			       call->local_info0.v92_analog.v92_capable &&
			       !call->local_info0.v92_analog.v90
					.acknowledges_peer_info0;
		return v92_info0d_is_valid(&call->local_info0.v92_digital) &&
		       call->local_info0.v92_digital.v92_capable &&
		       !call->local_info0.v92_digital.v90.common
				.acknowledges_peer_info0;
	}
	if (call->local_role == V90_ANALOG_MODEM)
		return v90_info0_is_valid(&call->local_info0.analog) &&
		       !call->local_info0.analog.acknowledges_peer_info0;
	return v90_info0d_is_valid(&call->local_info0.digital) &&
	       !call->local_info0.digital.common.acknowledges_peer_info0;
}

static bool stored_peer_info0_is_valid(const struct pcm_call *call)
{
	if (!call->peer_info0_stored)
		return true;
	if (call->protocol == PCM_PROTOCOL_V92)
		return call->local_role == V90_ANALOG_MODEM ?
		       v92_info0d_is_valid(
			       &call->peer_info0.v92_digital) &&
			       call->peer_info0.v92_digital.v92_capable :
		       v92_info0a_is_valid(
			       &call->peer_info0.v92_analog) &&
			       call->peer_info0.v92_analog.v92_capable;
	return call->local_role == V90_ANALOG_MODEM ?
		       v90_info0d_is_valid(&call->peer_info0.digital) :
		       v90_info0_is_valid(&call->peer_info0.analog);
}

static bool analog_configuration_is_valid(
	const union pcm_call_local_info1 *info1,
	const union pcm_call_phase3_configuration *phase3,
	const struct pcm_call_analog_phase4_configuration *phase4,
	enum g711_law law)
{
	return v90_info1a_is_valid(&info1->analog) &&
	       phase3->analog.nominal_peak_amplitude > 0 &&
	       v90_dil_profile_is_valid(phase3->analog.dil_profile) &&
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
	       v90_jd_parameters_are_valid(&phase3->digital.jd) &&
	       !v90_mp_record_acknowledges_cp(mp);
}

static bool v92_analog_configuration_is_valid(
	const union pcm_call_local_info1 *info1,
	const union pcm_call_phase3_configuration *phase3,
	enum g711_law law)
{
	const struct v92_phase3_analog_station_configuration *selected =
		&phase3->v92_analog;

	return selected->law == law &&
	       selected->local_info1a.local_md_length_276_symbol_units == 0u &&
	       v92_pcm_info1a_equal(
		       &info1->v92_analog, &selected->local_info1a) &&
	       v92_phase3_analog_station_configuration_is_valid(selected);
}

static bool v92_digital_configuration_is_valid(
	const union pcm_call_local_info1 *info1,
	const union pcm_call_phase3_configuration *phase3,
	uint32_t enabled_downstream_rate_mask, enum g711_law law)
{
	const struct pcm_call_v92_digital_phase3_configuration *selected =
		&phase3->v92_digital;

	return v92_info1d_is_valid(&info1->v92_digital) &&
	       info1->v92_digital.pcm_upstream_supported &&
	       selected->jd.data_rate_capability_mask ==
		       enabled_downstream_rate_mask &&
	       v92_phase3_digital_station_parameters_are_valid(
		       law, V90_INFO1A_MIN_TRAINING_UCODE,
		       selected->equalizer_adaptation,
		       selected->su_minimum_magnitude,
		       &selected->jd, &selected->jp);
}

static bool v92_analog_phase4_configuration_is_valid(
	const struct pcm_call_v92_analog_phase4_configuration *configuration,
	uint32_t enabled_downstream_rate_mask, enum g711_law law)
{
	return v92_suvu_parameters_are_valid(&configuration->suvu) &&
	       !configuration->suvu.acknowledges_cpd &&
	       v92_cpu_parameters_are_valid(&configuration->cpu) &&
	       configuration->cpu.law == law &&
	       !configuration->cpu.acknowledges_cpd &&
	       downstream_rate_is_enabled(
		       enabled_downstream_rate_mask,
		       configuration->cpu.data_rate_number);
}

static bool v92_digital_phase4_configuration_is_valid(
	const struct pcm_call_v92_digital_phase4_configuration *configuration)
{
	return v92_suvd_parameters_are_valid(&configuration->suvd) &&
	       !configuration->suvd.acknowledges_cpu &&
	       v92_cpd_base_parameters_are_valid(&configuration->cpd) &&
	       !configuration->cpd.acknowledges_cpu &&
	       v92_pcm_dil_rate_is_enabled(
		       V92_PCM_DIL_RATE_MASK_ALL,
		       configuration->cpd.data_rate_number);
}

static bool configuration_is_valid(
	const struct pcm_call_configuration *configuration)
{
	if (configuration == NULL ||
	    !pcm_protocol_is_valid(configuration->protocol) ||
	    !pcm_modem_role_is_valid(configuration->local_role) ||
	    !g711_law_is_valid(configuration->law) ||
	    !downstream_rate_mask_is_valid(
		    configuration->enabled_downstream_rate_mask) ||
	    configuration->phase2_nominal_amplitude <= 0)
		return false;
	if (configuration->protocol == PCM_PROTOCOL_V92) {
		if (configuration->local_role == V90_ANALOG_MODEM)
			return v92_info0a_is_valid(
				       &configuration->local_info0.v92_analog) &&
			       configuration->local_info0.v92_analog.v92_capable &&
			       !configuration->local_info0.v92_analog
				       .short_phase2_requested &&
			       v92_analog_configuration_is_valid(
				       &configuration->local_info1,
				       &configuration->phase3,
				       configuration->law) &&
			       v92_analog_phase4_configuration_is_valid(
				       &configuration->v92_analog_phase4,
				       configuration->enabled_downstream_rate_mask,
				       configuration->law);
		return v92_info0d_is_valid(
			       &configuration->local_info0.v92_digital) &&
		       configuration->local_info0.v92_digital.v92_capable &&
		       !configuration->local_info0.v92_digital
			       .short_phase2_requested &&
		       info0_law(
			       &configuration->local_info0.v92_digital.v90) ==
			       configuration->law &&
		       v92_digital_configuration_is_valid(
			       &configuration->local_info1,
			       &configuration->phase3,
			       configuration->enabled_downstream_rate_mask,
			       configuration->law) &&
		       v92_digital_phase4_configuration_is_valid(
			       &configuration->v92_digital_phase4);
	}
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
	if (!pcm_protocol_is_valid(call->protocol) ||
	    !pcm_modem_role_is_valid(call->local_role) ||
	    !g711_law_is_valid(call->law) ||
	    call->phase2_nominal_amplitude <= 0 ||
	    !stored_local_info0_is_valid(call) ||
	    !stored_peer_info0_is_valid(call) ||
	    !v34_full_retrain_is_valid(&call->retrain) ||
	    call->retrain.detector.tone.local_role !=
		    phase2_station_role(call->local_role) ||
	    !downstream_rate_mask_is_valid(
		    call->enabled_downstream_rate_mask))
		return false;
	if (call->protocol == PCM_PROTOCOL_V92)
		return call->local_role == V90_ANALOG_MODEM ?
		       v92_analog_configuration_is_valid(
			       &call->local_info1,
			       &call->phase3_configuration,
			       call->law) &&
		       v92_analog_phase4_configuration_is_valid(
			       &call->v92_analog_phase4_configuration,
			       call->enabled_downstream_rate_mask,
			       call->law) :
		       v92_digital_configuration_is_valid(
			       &call->local_info1,
			       &call->phase3_configuration,
			       call->enabled_downstream_rate_mask,
			       call->law) &&
		       v92_digital_phase4_configuration_is_valid(
			       &call->v92_digital_phase4_configuration);
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
		v90_phase3_analog_station_transmit_complete(&mixed->phase3);

	return v90_phase3_analog_station_is_valid(&mixed->phase3) &&
	       mixed->phase3.receive_stage ==
		       V90_PHASE3_ANALOG_RECEIVE_SIGNS &&
	       v90_phase3_sign_receiver_is_complete(signs) &&
	       (call->failed || !transmit_complete);
}

static bool digital_mixed_is_valid(const struct pcm_call *call)
{
	const struct pcm_call_digital_mixed_state *mixed =
		&call->state.mixed.digital;
	bool receive_complete =
		mixed->phase3.receive_stage ==
			V90_PHASE3_DIGITAL_RECEIVE_RETURN &&
		v90_phase3_return_marker_is_complete(
			&mixed->phase3.receive.returning.marker);

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

static bool phase3_entry_matches_call(const struct pcm_call *call)
{
	return pcm_phase3_entry_is_valid(&call->phase3_entry) &&
	       call->phase3_entry.local_role == call->local_role &&
	       call->phase3_entry.law == call->law &&
	       call->phase3_entry.selection.startup.protocol == call->protocol;
}

static bool suvd_is_empty(const struct v92_suvd_parameters *parameters)
{
	static const struct v92_suvd_parameters empty;

	return memcmp(parameters, &empty, sizeof(empty)) == 0;
}

static bool v92_analog_mixed_is_valid(
	const struct pcm_call *call, bool trailing_complete)
{
	const struct pcm_call_v92_analog_mixed_state *mixed =
		&call->state.mixed.v92_analog;
	struct v92_cpt_parameters parameters;
	struct v90_pcm_mapping mapping;

	if (!v92_phase3_analog_station_is_valid(&mixed->phase3) ||
	    mixed->phase3.law != call->law ||
	    !pcm_downstream_rx_is_valid(&mixed->receive) ||
	    mixed->receive.protocol != PCM_PROTOCOL_V92 ||
	    (mixed->receive.stage != PCM_DOWNSTREAM_RX_TRN2D &&
	     mixed->receive.stage != PCM_DOWNSTREAM_RX_V92_CONTROL) ||
	    mixed->receive.stream.decoder.decision_plane !=
		    V90_PCM_MAPPING_TRANSMIT ||
	    (mixed->early_suvd_received ?
		     !v92_suvd_parameters_are_valid(&mixed->early_suvd) :
		     !suvd_is_empty(&mixed->early_suvd)))
		return false;
	if (call->failed)
		return true;
	if (v92_phase3_analog_station_downstream_handoff(
		    &mixed->phase3, &parameters) !=
		    V92_PHASE3_ANALOG_STATION_OK ||
	    v90_pcm_mapping_init_from_v92_cpt(&mapping, &parameters) !=
		    V90_PCM_MAPPING_OK)
		return false;
	return v92_phase3_analog_station_receive_complete(&mixed->phase3) &&
	       v92_phase3_analog_station_transmit_complete(&mixed->phase3) ==
		       trailing_complete &&
	       v90_pcm_mapping_equal(
		       &mixed->receive.stream.decoder.mapping, &mapping) &&
	       !pcm_downstream_rx_has_failed(&mixed->receive);
}

static bool v92_digital_mixed_is_valid(
	const struct pcm_call *call, bool trailing_complete)
{
	const struct pcm_call_v92_digital_mixed_state *mixed =
		&call->state.mixed.v92_digital;
	struct v92_cpt_parameters parameters;
	struct v90_pcm_mapping mapping;

	if (!v92_phase3_digital_station_is_valid(&mixed->phase3) ||
	    mixed->phase3.law != call->law ||
	    !pcm_downstream_tx_is_valid(&mixed->transmit) ||
	    mixed->transmit.protocol != PCM_PROTOCOL_V92 ||
	    (mixed->transmit.stage != PCM_DOWNSTREAM_TX_TRN2D &&
	     mixed->transmit.stage != PCM_DOWNSTREAM_TX_V92_CONTROL) ||
	    !mixed->transmit.control_configured ||
	    mixed->transmit.v92_control_kind !=
		    PCM_DOWNSTREAM_V92_CONTROL_SUVD ||
	    mixed->transmit.control.suvd.request_silence !=
		    call->v92_digital_phase4_configuration.suvd.request_silence ||
	    mixed->transmit.control.suvd.acknowledges_cpu ||
	    mixed->transmit.finish_requested || mixed->transmit.complete)
		return false;
	if (call->failed)
		return true;
	if (v92_phase3_digital_station_downstream_handoff(
		    &mixed->phase3, &parameters) !=
		    V92_PHASE3_DIGITAL_STATION_OK ||
	    v90_pcm_mapping_init_from_v92_cpt(&mapping, &parameters) !=
		    V90_PCM_MAPPING_OK)
		return false;
	return v92_phase3_digital_station_transmit_complete(&mixed->phase3) &&
	       v92_phase3_digital_station_receive_complete(&mixed->phase3) ==
		       trailing_complete &&
	       v90_pcm_mapping_equal(&mixed->transmit.encoder.mapping, &mapping);
}

static bool v92_mixed_is_valid(
	const struct pcm_call *call, bool phase3_complete)
{
	return call->local_role == V90_ANALOG_MODEM ?
		       v92_analog_mixed_is_valid(call, phase3_complete) :
		       v92_digital_mixed_is_valid(call, phase3_complete);
}

static bool v92_analog_phase4_state_is_valid(const struct pcm_call *call)
{
	const struct pcm_call_v92_analog_phase4_control *state =
		&call->state.phase4.v92.owner.analog;
	uint32_t expected_interval = V92_CPU_REPEAT_BASE_SAMPLES +
		call->round_trip_delay_samples;
	bool confirmed = state->station.transmit.peer_confirmation_received;
	bool timer_valid;

	if (state->observed_cpu_records !=
	    state->station.transmit.completed_cpu_records)
		return false;
	if (confirmed)
		timer_valid = !state->awaiting_cpu_completion &&
			state->cpu_repeat_remaining_samples == 0u;
	else if (state->observed_cpu_records == 0u)
		timer_valid = !state->awaiting_cpu_completion &&
			state->cpu_repeat_remaining_samples == 0u;
	else if (state->awaiting_cpu_completion)
		timer_valid = state->cpu_repeat_remaining_samples == 0u;
	else
		timer_valid = state->cpu_repeat_remaining_samples != 0u &&
			state->cpu_repeat_remaining_samples <= expected_interval;

	return v92_phase4_analog_station_is_valid(&state->station) &&
	       state->cpu_repeat_interval_samples == expected_interval &&
	       timer_valid;
}

static bool cleardown_is_active_owned(const struct pcm_call *call)
{
	return call->protocol == PCM_PROTOCOL_V90 &&
	       call->stage == PCM_CALL_DATA_PATH &&
	       (call->local_role == V90_ANALOG_MODEM ?
			call->state.data_path.analog.cleardown_requested :
			call->state.data_path.digital.cleardown_requested);
}

static bool rate_renegotiation_is_active_owned(
	const struct pcm_call *call)
{
	if (call->protocol != PCM_PROTOCOL_V90 ||
	    call->stage != PCM_CALL_DATA_PATH ||
	    cleardown_is_active_owned(call))
		return false;
	return call->local_role == V90_ANALOG_MODEM ?
		       call->state.data_path.analog.rate_renegotiation_active :
		       call->state.data_path.digital.rate_renegotiation_active;
}

static bool rate_renegotiation_response_is_pending_owned(
	const struct pcm_call *call)
{
	if (!rate_renegotiation_is_active_owned(call))
		return false;
	/* V.90 9.6.1/9.6.2 time E/Ed reception, not the following B1/B1d.
	 * The path remains in its control epoch until both directions are ready
	 * for host data, but that must not extend the response deadline. */
	if (call->local_role == V90_ANALOG_MODEM) {
		const struct v90_analog_path *path = &call->state.data_path.analog;

		/* The old data receiver also remains present before Rd-bar ends;
		 * ed_armed distinguishes it from the new post-Ed B1d receiver. */
		return !path->ed_armed || path->downstream_stage !=
					V90_ANALOG_PATH_DOWNSTREAM_DATA;
	}
	{
		const struct v90_digital_path *path = &call->state.data_path.digital;

		return path->upstream_stage == V90_DIGITAL_PATH_UPSTREAM_STARTUP &&
		       (path->upstream.startup.stage == V90_UPSTREAM_RX_STAGE_CP ||
			path->upstream.startup.stage == V90_UPSTREAM_RX_STAGE_E);
	}
}

static bool rate_renegotiation_deadline_has_started(
	const struct pcm_call *call)
{
	if (!rate_renegotiation_response_is_pending_owned(call))
		return false;
	if (call->local_role == V90_ANALOG_MODEM) {
		const struct v90_analog_path *path = &call->state.data_path.analog;

		/* V.90 9.6.2 starts at our S-to-S-bar transition, not incoming
		 * Rd-bar. The last S point advances the generator's stage before
		 * the continuous line accepts the first S-bar point. */
		return path->upstream_stage != V90_ANALOG_PATH_UPSTREAM_RRN_RETURN ||
		       path->upstream.rrn_return.points.s_bar_symbols != 0u;
	}
	{
		const struct v90_digital_path *path =
			&call->state.data_path.digital;

		/* V.90 9.6.1 starts at our Rd-to-Rd-bar transition. */
		return path->downstream_stage !=
			       V90_DIGITAL_PATH_DOWNSTREAM_DATA_DRAIN &&
		       (path->downstream_stage !=
				V90_DIGITAL_PATH_DOWNSTREAM_RD ||
			path->downstream.rd.transmitted_symbols >= V90_RD_SYMBOLS);
	}
}

static bool call_is_valid(
	const struct pcm_call *call, bool owned_phase3)
{
	bool child_valid;
	bool child_failed;

	if (call == NULL || !stored_configuration_is_valid(call) ||
	    (call->stage == PCM_CALL_CLOSED &&
	     (call->failed || call->retrain_active || call->b1_deadline_armed ||
	      call->rate_renegotiation_deadline_armed)) ||
	    (!call->v92_peer_downstream_rate_stored &&
	     call->v92_peer_downstream_rate_mask != 0u) ||
	    (call->v92_peer_downstream_rate_stored &&
	     (call->protocol != PCM_PROTOCOL_V92 ||
	      call->local_role != V90_ANALOG_MODEM ||
	      (call->v92_peer_downstream_rate_mask &
	       ~(uint32_t)V92_JD_RATE_CAPABILITY_MASK) != 0u)) ||
	    (call->protocol == PCM_PROTOCOL_V92 &&
	     call->local_role == V90_ANALOG_MODEM &&
	     call->stage != PCM_CALL_PHASE2 &&
	     call->stage != PCM_CALL_PHASE3 &&
	     call->stage != PCM_CALL_RETRAIN_SILENCE &&
	     !call->v92_peer_downstream_rate_stored) ||
	    (!call->round_trip_delay_stored &&
	     call->round_trip_delay_samples != 0u) ||
	    (!call->b1_deadline_armed &&
	     call->b1_deadline_remaining_samples != 0u) ||
	    (call->b1_deadline_armed &&
	     (call->protocol != PCM_PROTOCOL_V90 ||
	      call->stage == PCM_CALL_RETRAIN_SILENCE ||
	      !call->peer_info0_stored || !call->round_trip_delay_stored ||
	      call->b1_deadline_remaining_samples >
		      V90_B1_BASE_DEADLINE_SAMPLES +
		      V90_B1_ROUND_TRIP_ALLOWANCES *
			      (uint32_t)call->round_trip_delay_samples)) ||
	    (!call->rate_renegotiation_deadline_armed &&
	     call->rate_renegotiation_deadline_remaining_samples != 0u) ||
	    (call->rate_renegotiation_deadline_armed &&
	     (call->protocol != PCM_PROTOCOL_V90 ||
	      call->stage != PCM_CALL_DATA_PATH ||
	      !call->round_trip_delay_stored ||
	      !rate_renegotiation_is_active_owned(call) ||
	      call->rate_renegotiation_deadline_remaining_samples == 0u ||
	      call->rate_renegotiation_deadline_remaining_samples >
		      V90_RATE_RENEGOTIATION_BASE_DEADLINE_SAMPLES +
		      V90_RATE_RENEGOTIATION_ROUND_TRIP_ALLOWANCES *
			      (uint32_t)call->round_trip_delay_samples ||
	      !rate_renegotiation_deadline_has_started(call))) ||
	    (!call->rate_renegotiation_deadline_armed &&
	     rate_renegotiation_deadline_has_started(call)) ||
	    (call->stage != PCM_CALL_PHASE2 &&
	     call->stage != PCM_CALL_RETRAIN_SILENCE &&
	     !call->peer_info0_stored) ||
	    (call->retrain_active && !call->peer_info0_stored) ||
	    (call->stage == PCM_CALL_RETRAIN_SILENCE &&
	     !call->retrain_active) ||
	    (v34_full_retrain_response_is_active(&call->retrain) !=
	     (call->stage == PCM_CALL_RETRAIN_SILENCE)) ||
	    ((call->stage == PCM_CALL_PHASE2 ||
	      call->stage == PCM_CALL_RETRAIN_SILENCE) ?
		     call->phase3_entry_stored : !call->phase3_entry_stored) ||
	    (call->phase3_entry_stored && !phase3_entry_matches_call(call)))
		return false;
	switch (call->stage) {
	case PCM_CALL_PHASE2:
		child_valid = pcm_phase2_is_valid(&call->state.phase2) &&
			      call->state.phase2.local_role == call->local_role &&
			      call->state.phase2.info0_protocol == call->protocol;
		child_failed = pcm_phase2_has_failed(&call->state.phase2);
		break;
	case PCM_CALL_PHASE3:
		if (call->protocol == PCM_PROTOCOL_V92 &&
		    call->local_role == V90_ANALOG_MODEM) {
			child_valid = v92_phase3_analog_station_is_valid(
				&call->state.phase3.v92_analog);
			child_failed = v92_phase3_analog_station_has_failed(
				&call->state.phase3.v92_analog);
		} else if (call->protocol == PCM_PROTOCOL_V92) {
			child_valid = v92_phase3_digital_station_is_valid(
				&call->state.phase3.v92_digital);
			child_failed = v92_phase3_digital_station_has_failed(
				&call->state.phase3.v92_digital);
		} else if (call->local_role == V90_ANALOG_MODEM) {
			child_valid = owned_phase3 ?
				v90_phase3_analog_station_owned_state_is_valid(
					&call->state.phase3.analog) :
				v90_phase3_analog_station_is_valid(
					&call->state.phase3.analog);
			child_failed = call->state.phase3.analog.failed;
		} else {
			child_valid = v90_phase3_digital_station_is_valid(
				&call->state.phase3.digital);
			child_failed = v90_phase3_digital_station_has_failed(
				&call->state.phase3.digital);
		}
		break;
	case PCM_CALL_PHASE3_MIXED:
		if (call->protocol == PCM_PROTOCOL_V92) {
			bool trailing_complete =
				call->local_role == V90_ANALOG_MODEM ?
					v92_phase3_analog_station_transmit_complete(
						&call->state.mixed.v92_analog.phase3) :
					v92_phase3_digital_station_receive_complete(
						&call->state.mixed.v92_digital.phase3);

			child_valid = v92_mixed_is_valid(call, trailing_complete);
			child_failed = call->local_role == V90_ANALOG_MODEM ?
				v92_phase3_analog_station_has_failed(
					&call->state.mixed.v92_analog.phase3) :
				v92_phase3_digital_station_has_failed(
					&call->state.mixed.v92_digital.phase3);
		} else {
			child_valid = call->local_role == V90_ANALOG_MODEM ?
				      analog_mixed_is_valid(call) :
				      digital_mixed_is_valid(call);
			child_failed = call->local_role == V90_ANALOG_MODEM ?
				       v90_phase3_analog_station_has_failed(
					       &call->state.mixed.analog.phase3) :
				       v90_phase3_digital_station_has_failed(
					       &call->state.mixed.digital.phase3);
		}
		break;
	case PCM_CALL_PHASE4:
		if (call->protocol == PCM_PROTOCOL_V92 &&
		    call->state.phase4.v92.child ==
			    PCM_CALL_V92_PHASE4_E2U) {
			child_valid = v92_phase4_e2u_is_valid(
				&call->state.phase4.v92.owner.e2u) &&
				call->state.phase4.v92.owner.e2u.local_role ==
					call->local_role;
			child_failed = v92_phase4_e2u_has_failed(
				&call->state.phase4.v92.owner.e2u);
		} else if (call->protocol == PCM_PROTOCOL_V92 &&
			   call->state.phase4.v92.child ==
				   PCM_CALL_V92_PHASE4_CONTROL &&
			   call->local_role == V90_ANALOG_MODEM) {
			child_valid = v92_analog_phase4_state_is_valid(call);
			child_failed = v92_phase4_analog_station_has_failed(
				&call->state.phase4.v92.owner.analog.station);
		} else if (call->protocol == PCM_PROTOCOL_V92 &&
			   call->state.phase4.v92.child ==
				   PCM_CALL_V92_PHASE4_CONTROL) {
			child_valid = v92_phase4_digital_station_is_valid(
				&call->state.phase4.v92.owner.digital);
			child_failed = v92_phase4_digital_station_has_failed(
				&call->state.phase4.v92.owner.digital);
		} else if (call->protocol == PCM_PROTOCOL_V92) {
			return false;
		} else if (call->local_role == V90_ANALOG_MODEM) {
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
	case PCM_CALL_CLOSED:
		if (call->protocol != PCM_PROTOCOL_V90)
			return false;
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
	case PCM_CALL_RETRAIN_SILENCE:
		child_valid = call->peer_info0_stored &&
			      v34_full_retrain_response_is_active(
				      &call->retrain);
		child_failed = false;
		break;
	default:
		return false;
	}
	if (call->stage == PCM_CALL_PHASE2 &&
	    call->peer_info0_stored != call->state.phase2.peer_info0_stored)
		return false;
	return child_valid && (call->failed || !child_failed);
}

bool pcm_call_is_valid(const struct pcm_call *call)
{
	return call_is_valid(call, false);
}

static bool pcm_call_owned_state_is_valid(const struct pcm_call *call)
{
	return call_is_valid(call, true);
}

enum pcm_call_result pcm_call_init(
	struct pcm_call *call,
	const struct pcm_call_configuration *configuration)
{
	struct pcm_call initialized = { 0 };
	enum pcm_phase2_result result;

	if (call == NULL || !configuration_is_valid(configuration))
		return PCM_CALL_INVALID_ARGUMENT;
	if (configuration->protocol == PCM_PROTOCOL_V92 &&
	    configuration->local_role == V90_ANALOG_MODEM)
		result = pcm_phase2_init_v92_analog(
			&initialized.state.phase2,
			configuration->phase2_nominal_amplitude,
			&configuration->local_info0.v92_analog);
	else if (configuration->protocol == PCM_PROTOCOL_V92)
		result = pcm_phase2_init_v92_digital(
			&initialized.state.phase2,
			configuration->phase2_nominal_amplitude,
			&configuration->local_info0.v92_digital);
	else if (configuration->local_role == V90_ANALOG_MODEM)
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
	if (configuration->protocol == PCM_PROTOCOL_V92 &&
	    configuration->local_role == V90_ANALOG_MODEM) {
		initialized.local_info1.v92_analog =
			configuration->local_info1.v92_analog;
		initialized.phase3_configuration.v92_analog =
			configuration->phase3.v92_analog;
		initialized.v92_analog_phase4_configuration =
			configuration->v92_analog_phase4;
	} else if (configuration->protocol == PCM_PROTOCOL_V92) {
		initialized.local_info1.v92_digital =
			configuration->local_info1.v92_digital;
		initialized.phase3_configuration.v92_digital =
			configuration->phase3.v92_digital;
		initialized.v92_digital_phase4_configuration =
			configuration->v92_digital_phase4;
	} else if (configuration->local_role == V90_ANALOG_MODEM) {
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
	initialized.protocol = configuration->protocol;
	initialized.local_role = configuration->local_role;
	initialized.law = configuration->law;
	initialized.local_info0 = initialized.state.phase2.local_info0;
	initialized.phase2_nominal_amplitude =
		configuration->phase2_nominal_amplitude;
	initialized.enabled_downstream_rate_mask =
		configuration->enabled_downstream_rate_mask;
	initialized.stage = PCM_CALL_PHASE2;
	if (v34_full_retrain_init(
		    &initialized.retrain,
		    phase2_station_role(configuration->local_role)) !=
	    V34_PHASE2_TONE_OK)
		return PCM_CALL_INVALID_ARGUMENT;
	if (!pcm_call_is_valid(&initialized))
		return PCM_CALL_NUMERIC_FAILURE;
	*call = initialized;
	return PCM_CALL_OK;
}

static enum pcm_phase2_result initialize_retrain_phase2(
	const struct pcm_call *call, struct pcm_phase2 *phase2)
{
	enum pcm_phase2_result result;

	if (call->protocol == PCM_PROTOCOL_V92 &&
	    call->local_role == V90_ANALOG_MODEM)
		result = pcm_phase2_init_retrain_v92_analog(
			phase2, call->phase2_nominal_amplitude,
			&call->local_info0.v92_analog,
			&call->peer_info0.v92_digital,
			call->round_trip_delay_samples);
	else if (call->protocol == PCM_PROTOCOL_V92)
		result = pcm_phase2_init_retrain_v92_digital(
			phase2, call->phase2_nominal_amplitude,
			&call->local_info0.v92_digital,
			&call->peer_info0.v92_analog,
			call->round_trip_delay_samples);
	else if (call->local_role == V90_ANALOG_MODEM)
		result = pcm_phase2_init_retrain_v90_analog(
			phase2, call->phase2_nominal_amplitude,
			&call->local_info0.analog,
			&call->peer_info0.digital,
			call->round_trip_delay_samples);
	else
		result = pcm_phase2_init_retrain_v90_digital(
			phase2, call->phase2_nominal_amplitude,
			&call->local_info0.digital,
			&call->peer_info0.analog,
			call->round_trip_delay_samples);
	return result;
}

static enum pcm_call_result begin_retrain(
	struct pcm_call *call, bool retain_peer_tone)
{
	struct pcm_call next = *call;

	if (!next.peer_info0_stored)
		return PCM_CALL_INVALID_STATE;
	if (v34_full_retrain_begin_response(
		    &next.retrain, retain_peer_tone) !=
	    V34_PHASE2_TONE_OK)
		return PCM_CALL_NUMERIC_FAILURE;
	next.state = (union pcm_call_state){ 0 };
	next.phase3_entry = (struct pcm_phase3_entry){ 0 };
	next.v92_peer_downstream_rate_mask = 0u;
	next.b1_deadline_remaining_samples = 0u;
	next.rate_renegotiation_deadline_remaining_samples = 0u;
	next.stage = PCM_CALL_RETRAIN_SILENCE;
	next.phase3_entry_stored = false;
	next.v92_peer_downstream_rate_stored = false;
	next.b1_deadline_armed = false;
	next.rate_renegotiation_deadline_armed = false;
	next.retrain_active = true;
	if (!pcm_call_is_valid(&next))
		return PCM_CALL_NUMERIC_FAILURE;
	*call = next;
	return PCM_CALL_OK;
}

static enum pcm_call_result recover_unusable_retrain_selection(
	struct pcm_call *call, enum pcm_call_result result)
{
	if (result != PCM_CALL_UNUSABLE_SELECTION ||
	    call->protocol != PCM_PROTOCOL_V90 || !call->retrain_active ||
	    call->stage != PCM_CALL_PHASE2)
		return result;
	/* A peer can answer INFO1 with a symbol rate for which its INFO1d offered
	 * no usable data rate.  Refuse that profile, but keep the established
	 * call agreement and request another standards-defined full retrain. */
	return begin_retrain(call, false);
}

static bool is_v90_info1a_boundary(
	const struct pcm_call *call, const struct pcm_phase2_event *event)
{
	if (call->protocol != PCM_PROTOCOL_V90)
		return false;
	return call->local_role == V90_ANALOG_MODEM ?
		event->kind == PCM_PHASE2_EVENT_LOCAL_INFO1A_SENT :
		event->kind == PCM_PHASE2_EVENT_PEER_INFO1A_RECEIVED;
}

static enum pcm_call_result arm_b1_deadline(
	struct pcm_call *call, const struct pcm_phase2_event *event)
{
	if (!is_v90_info1a_boundary(call, event))
		return PCM_CALL_OK;
	if (!call->round_trip_delay_stored)
		return PCM_CALL_SEQUENCE_MISMATCH;
	call->b1_deadline_remaining_samples =
		V90_B1_BASE_DEADLINE_SAMPLES +
		V90_B1_ROUND_TRIP_ALLOWANCES *
		(uint32_t)call->round_trip_delay_samples;
	call->b1_deadline_armed = true;
	return PCM_CALL_OK;
}

/* These predicates are only used while the call owns a data path already
 * covered by aggregate validation. Avoid recursively walking that graph for
 * every 8 kHz character. */
static bool analog_upstream_is_online_owned(
	const struct v90_analog_path *path)
{
	return !path->failed &&
	       (path->rate_renegotiation_active ||
		(path->upstream_stage == V90_ANALOG_PATH_UPSTREAM_DATA &&
		 !path->upstream.data.b1_active));
}

static bool analog_downstream_is_online_owned(
	const struct v90_analog_path *path)
{
	return !path->failed &&
	       (path->rate_renegotiation_active ||
		(path->downstream_stage == V90_ANALOG_PATH_DOWNSTREAM_DATA &&
		 path->downstream.data.stage ==
			 V90_DOWNSTREAM_DATA_RX_STAGE_DATA));
}

static bool digital_downstream_is_online_owned(
	const struct v90_digital_path *path)
{
	return !path->failed &&
	       (path->rate_renegotiation_active ||
		(path->downstream_stage ==
			 V90_DIGITAL_PATH_DOWNSTREAM_DATA &&
		 path->downstream.data.transmitted_symbols >=
			 V90_PHASE4_B1D_FRAMES * V90_CP_FRAME_INTERVALS));
}

static bool digital_upstream_is_online_owned(
	const struct v90_digital_path *path)
{
	return !path->failed &&
	       (path->rate_renegotiation_active ||
		path->upstream_stage == V90_DIGITAL_PATH_UPSTREAM_DATA);
}

static bool data_path_is_online_owned(const struct pcm_call *call)
{
	if (call->failed || call->stage != PCM_CALL_DATA_PATH)
		return false;
	if (call->local_role == V90_ANALOG_MODEM)
		return analog_upstream_is_online_owned(
			       &call->state.data_path.analog) &&
		       analog_downstream_is_online_owned(
			       &call->state.data_path.analog);
	return digital_downstream_is_online_owned(
		       &call->state.data_path.digital) &&
	       digital_upstream_is_online_owned(
		       &call->state.data_path.digital);
}

static bool peer_b1_received(const struct pcm_call *call)
{
	if (call->stage != PCM_CALL_DATA_PATH)
		return false;
	return call->local_role == V90_ANALOG_MODEM ?
		analog_downstream_is_online_owned(
			&call->state.data_path.analog) :
		digital_upstream_is_online_owned(
			&call->state.data_path.digital);
}

static enum pcm_call_result advance_b1_deadline(struct pcm_call *call)
{
	if (!call->b1_deadline_armed)
		return PCM_CALL_OK;
	if (peer_b1_received(call)) {
		call->b1_deadline_remaining_samples = 0u;
		call->b1_deadline_armed = false;
		return PCM_CALL_OK;
	}
	if (call->b1_deadline_remaining_samples != 0u)
		call->b1_deadline_remaining_samples--;
	if (call->b1_deadline_remaining_samples != 0u ||
	    (call->stage != PCM_CALL_PHASE4 &&
	     call->stage != PCM_CALL_DATA_PATH))
		return PCM_CALL_OK;
	return begin_retrain(call, false);
}

enum pcm_call_result pcm_call_start_retrain(struct pcm_call *call)
{
	if (!pcm_call_is_valid(call))
		return PCM_CALL_INVALID_ARGUMENT;
	if (call->failed || !call->peer_info0_stored ||
	    cleardown_is_active_owned(call) ||
	    call->stage == PCM_CALL_PHASE2 ||
	    call->stage == PCM_CALL_CLOSED ||
	    call->stage == PCM_CALL_RETRAIN_SILENCE)
		return PCM_CALL_INVALID_STATE;
	return begin_retrain(call, false);
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
	if (call->protocol == PCM_PROTOCOL_V92)
		return call->state.phase4.v92.child ==
			       PCM_CALL_V92_PHASE4_E2U &&
		       v92_phase4_e2u_is_complete(
			       &call->state.phase4.v92.owner.e2u);
	return call->local_role == V90_ANALOG_MODEM ?
		       v90_phase4_analog_startup_is_complete(
			       &call->state.phase4.analog) :
		       v90_phase4_digital_startup_is_complete(
			       &call->state.phase4.digital);
}

bool pcm_call_is_online(const struct pcm_call *call)
{
	return pcm_call_is_valid(call) && data_path_is_online_owned(call);
}

enum pcm_call_stage pcm_call_current_stage(const struct pcm_call *call)
{
	return pcm_call_is_valid(call) ? call->stage :
				       (enum pcm_call_stage)-1;
}

static struct v34_info0 v34_info0_from_v90(
	const struct v90_info0 *info0)
{
	return (struct v34_info0){
		.symbol_rate_2743_supported = info0->symbol_rate_2743_supported,
		.symbol_rate_2800_supported = info0->symbol_rate_2800_supported,
		.symbol_rate_3429_supported = info0->symbol_rate_3429_supported,
		.low_carrier_3000_supported = info0->low_carrier_3000_supported,
		.high_carrier_3000_supported = info0->high_carrier_3000_supported,
		.low_carrier_3200_supported = info0->low_carrier_3200_supported,
		.high_carrier_3200_supported = info0->high_carrier_3200_supported,
		.symbol_rate_3429_transmit_permitted =
			info0->symbol_rate_3429_transmit_permitted,
		.transmit_power_reduction_supported =
			info0->transmit_power_reduction_supported,
		.maximum_symbol_rate_difference_steps =
			info0->maximum_symbol_rate_difference_steps,
		.cme_modem = info0->cme_modem,
		.constellation_1664_supported =
			info0->constellation_1664_supported,
		.transmit_clock_source = V34_INFO0_CLOCK_INTERNAL,
		.acknowledges_peer_info0 = info0->acknowledges_peer_info0,
	};
}

enum pcm_call_result pcm_call_v34_fallback_handoff(
	const struct pcm_call *call,
	struct v34_phase2_handoff *handoff,
	struct v90_fallback_restart *restart)
{
	struct v34_phase2_handoff selected;
	int16_t receive_gain;

	if (!pcm_call_is_valid(call) || handoff == NULL || restart == NULL ||
	    memory_regions_overlap(call, sizeof(*call), handoff, sizeof(*handoff)) ||
	    memory_regions_overlap(call, sizeof(*call), restart, sizeof(*restart)) ||
	    memory_regions_overlap(
		    handoff, sizeof(*handoff), restart, sizeof(*restart)))
		return PCM_CALL_INVALID_ARGUMENT;
	if (call->failed || call->protocol != PCM_PROTOCOL_V90 ||
	    call->local_role != V90_DIGITAL_MODEM ||
	    !call->peer_info0_stored || !call->round_trip_delay_stored ||
	    !pcm_phase2_is_ready_for_v34_fallback(&call->state.phase2) ||
	    pcm_phase2_receive_gain(&call->state.phase2, &receive_gain) !=
		    PCM_PHASE2_OK)
		return PCM_CALL_INVALID_STATE;
	selected = (struct v34_phase2_handoff){
		.local_info0 = v34_info0_from_v90(
			&call->local_info0.digital.common),
		.peer_info0 = v34_info0_from_v90(&call->peer_info0.analog),
		.caller_info1c = call->state.phase2.state.info1.digital.record.v90,
		.answerer_info1a =
			call->state.phase2.state.info1.analog.record.v34_fallback,
		.round_trip_delay_samples = call->round_trip_delay_samples,
		.receive_gain = receive_gain,
		.local_role = V34_CALLING_STATION,
	};
	if (!v34_phase2_handoff_is_valid(&selected))
		return PCM_CALL_UNUSABLE_SELECTION;
	*handoff = selected;
	*restart = (struct v90_fallback_restart){
		.local_info0 = call->local_info0.digital,
		.peer_info0 = call->peer_info0.analog,
		.local_info1 = call->local_info1.digital,
		.phase3 = call->phase3_configuration.digital,
		.phase4 = call->digital_phase4_configuration,
		.round_trip_delay_samples = call->round_trip_delay_samples,
		.phase2_nominal_amplitude = call->phase2_nominal_amplitude,
		.law = call->law,
	};
	return PCM_CALL_OK;
}

enum pcm_call_result pcm_call_init_from_v34(
	struct pcm_call *call, const struct v90_fallback_restart *restart,
	const struct v34_full_retrain *response)
{
	struct pcm_call initialized;
	struct pcm_call_configuration configuration;
	struct pcm_phase2 phase2;

	if (call == NULL || restart == NULL ||
	    !v34_full_retrain_is_valid(response) ||
	    !v34_full_retrain_response_is_active(response) ||
	    response->detector.tone.local_role != V34_CALLING_STATION ||
	    memory_regions_overlap(call, sizeof(*call), restart, sizeof(*restart)) ||
	    memory_regions_overlap(call, sizeof(*call), response, sizeof(*response)))
		return PCM_CALL_INVALID_ARGUMENT;
	configuration = (struct pcm_call_configuration){
		.protocol = PCM_PROTOCOL_V90,
		.local_role = V90_DIGITAL_MODEM,
		.law = restart->law,
		.enabled_downstream_rate_mask =
			restart->phase3.jd.data_rate_capability_mask,
		.phase2_nominal_amplitude = restart->phase2_nominal_amplitude,
		.local_info0.digital = restart->local_info0,
		.local_info1.digital = restart->local_info1,
		.phase3.digital = restart->phase3,
		.digital_phase4 = restart->phase4,
	};
	if (pcm_call_init(&initialized, &configuration) != PCM_CALL_OK)
		return PCM_CALL_INVALID_ARGUMENT;
	initialized.peer_info0.analog = restart->peer_info0;
	initialized.peer_info0_stored = true;
	initialized.round_trip_delay_samples = restart->round_trip_delay_samples;
	initialized.round_trip_delay_stored = true;
	/* Validate the retained INFO0 pair and ranging interval before accepting
	 * ownership, not later at the end of the response silence. */
	if (initialize_retrain_phase2(&initialized, &phase2) != PCM_PHASE2_OK)
		return PCM_CALL_INVALID_ARGUMENT;
	initialized.state = (union pcm_call_state){ 0 };
	initialized.retrain = *response;
	initialized.retrain_active = true;
	initialized.stage = PCM_CALL_RETRAIN_SILENCE;
	if (!pcm_call_is_valid(&initialized))
		return PCM_CALL_INVALID_ARGUMENT;
	*call = initialized;
	return PCM_CALL_OK;
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
	if (entry.selection.startup.protocol != call->protocol)
		return PCM_CALL_UNUSABLE_SELECTION;
	next = *call;
	next.state.phase3 = (union pcm_call_phase3_state){ 0 };
	if (call->protocol == PCM_PROTOCOL_V92 &&
	    entry.selection.upstream != PCM_UPSTREAM_PCM)
		return PCM_CALL_UNUSABLE_SELECTION;
	if (call->protocol == PCM_PROTOCOL_V92 &&
	    call->local_role == V90_ANALOG_MODEM) {
		struct v92_phase3_analog_station station;

		result = pcm_phase3_entry_start_v92_analog(
			&entry, &call->phase3_configuration.v92_analog,
			&station);
		if (result != PCM_PHASE3_ENTRY_OK)
			return entry_result(result);
		next.state.phase3.v92_analog = station;
	} else if (call->protocol == PCM_PROTOCOL_V92) {
		const struct pcm_call_v92_digital_phase3_configuration *config =
			&call->phase3_configuration.v92_digital;
		struct v92_phase3_digital_station station;

		result = pcm_phase3_entry_start_v92_digital(
			&entry, config->equalizer_adaptation,
			config->su_minimum_magnitude, &config->jd, &config->jp,
			&station);
		if (result != PCM_PHASE3_ENTRY_OK)
			return entry_result(result);
		next.state.phase3.v92_digital = station;
	} else if (call->local_role == V90_ANALOG_MODEM) {
		struct v90_phase3_analog_station station;

		result = pcm_phase3_entry_start_v90_analog(
			&entry,
			call->phase3_configuration.analog
				.nominal_peak_amplitude,
			call->phase3_configuration.analog.dil_profile,
			&station);
		if (result != PCM_PHASE3_ENTRY_OK)
			return entry_result(result);
		next.state.phase3.analog = station;
	} else {
		struct v90_phase3_digital_station station;
		int16_t receive_gain;

		if (pcm_phase2_receive_gain(
			    &call->state.phase2, &receive_gain) != PCM_PHASE2_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		result = pcm_phase3_entry_start_v90_digital(
			&entry,
			receive_gain, &call->phase3_configuration.digital.jd,
			&station);
		if (result != PCM_PHASE3_ENTRY_OK)
			return entry_result(result);
		next.state.phase3.digital = station;
	}
	next.phase3_entry = entry;
	next.phase3_entry_stored = true;
	next.stage = PCM_CALL_PHASE3;
	if (!pcm_call_is_valid(&next))
		return PCM_CALL_NUMERIC_FAILURE;
	*call = next;
	return PCM_CALL_OK;
}

static enum pcm_call_result start_digital_info1(struct pcm_call *call)
{
	struct v34_phase2_probe_receiver_measurement measurement;
	struct v34_info1c selected;
	enum pcm_phase2_result result;

	if (pcm_phase2_measure_remote_probe(
		    &call->state.phase2, &measurement) != PCM_PHASE2_OK)
		return PCM_CALL_SEQUENCE_MISMATCH;
	if (v34_phase2_select_v90_info1d(
		    &measurement, &call->local_info1.digital,
		    call->peer_info0.analog.transmit_power_reduction_supported,
		    &selected) != V34_PHASE2_SELECTION_OK)
		return PCM_CALL_NUMERIC_FAILURE;
	result = pcm_phase2_start_v90_digital_info1(
		&call->state.phase2, &selected);
	if (result != PCM_PHASE2_OK)
		return PCM_CALL_SEQUENCE_MISMATCH;
	return PCM_CALL_OK;
}

static enum pcm_call_result advance_phase2(
	struct pcm_call *call, const struct pcm_phase2_event *event)
{
	enum pcm_call_result call_result;
	enum pcm_phase2_result result;

	if (event->kind == PCM_PHASE2_EVENT_FAILED) {
		/* Missing L1 after a detected reversal can leave the peers in
		 * different ranging epochs. Reuse full retrain to synchronize
		 * them; the datapump's training deadline still bounds recovery. */
		if (event->failure == PCM_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND)
			return begin_retrain(call, false);
		/* V.90 9.2.1.2.5 restarts training when final Tone A misses
		 * the reciprocal-probe deadline. Keep the INFO0/RTD agreement
		 * and use the shared full-retrain path, not terminal hangup. */
		if (call->local_role == V90_DIGITAL_MODEM &&
		    event->failure == PCM_PHASE2_FAILURE_LOCAL_PROBE_LIMIT)
			return begin_retrain(call, false);
		return PCM_CALL_FAILED;
	}
	if (event->kind == PCM_PHASE2_EVENT_RETRAIN_REQUESTED)
		return begin_retrain(call, false);
	/* V.90 9.2.2.2.4, also used by V.92 full Phase 2: choose the
	 * full-retrain recovery branch when the analogue modem misses INFO1d. */
	if (event->kind == PCM_PHASE2_EVENT_INFO1_RECOVERY_STARTED &&
	    call->local_role == V90_ANALOG_MODEM)
		return begin_retrain(call, false);
	if (pcm_phase2_is_ready_for_info1(&call->state.phase2)) {
		struct pcm_startup_selection startup;

		if (pcm_phase2_startup_selection(
			    &call->state.phase2, &startup) != PCM_PHASE2_OK)
			return PCM_CALL_SEQUENCE_MISMATCH;
		if (startup.protocol != call->protocol)
			return PCM_CALL_UNUSABLE_SELECTION;
		if (call->local_role == V90_ANALOG_MODEM)
			result = pcm_phase2_start_analog_info1(
				&call->state.phase2);
		else if (call->protocol == PCM_PROTOCOL_V92)
			result = pcm_phase2_start_v92_digital_info1(
				&call->state.phase2,
				&call->local_info1.v92_digital);
		else {
			call_result = start_digital_info1(call);
			if (call_result != PCM_CALL_OK)
				return call_result;
			result = PCM_PHASE2_OK;
		}
		if (result != PCM_PHASE2_OK)
			return PCM_CALL_SEQUENCE_MISMATCH;
	}
	if (call->local_role == V90_ANALOG_MODEM &&
	    event->kind == PCM_PHASE2_EVENT_PEER_INFO1D_RECEIVED) {
		result = call->protocol == PCM_PROTOCOL_V92 ?
			pcm_phase2_reply_pcm(
				&call->state.phase2,
				&call->local_info1.v92_analog) :
			pcm_phase2_reply_conventional(
				&call->state.phase2,
				&call->local_info1.analog);
		if (result != PCM_PHASE2_OK)
			return PCM_CALL_SEQUENCE_MISMATCH;
	}
	if (pcm_phase2_is_ready_for_v34_fallback(&call->state.phase2))
		return PCM_CALL_V34_FALLBACK_READY;
	return pcm_phase2_is_ready_for_phase3(&call->state.phase2) ?
		       enter_phase3(call) : PCM_CALL_OK;
}

static enum pcm_call_result sample_phase2(
	struct pcm_call *call, int16_t received_sample,
	uint8_t *transmit_octet, struct pcm_call_observation *observation)
{
	enum pcm_call_result call_result;
	int16_t transmitted_sample;
	enum pcm_phase2_result result = pcm_phase2_sample(
		&call->state.phase2, received_sample,
		&transmitted_sample, &observation->phase2);

	if (result != PCM_PHASE2_OK)
		return PCM_CALL_NUMERIC_FAILURE;
	*transmit_octet = encode_sample(call->law, transmitted_sample);
	if (call->state.phase2.peer_info0_stored) {
		call->peer_info0 = call->state.phase2.peer_info0;
		call->peer_info0_stored = true;
	}
	if (observation->phase2.kind == PCM_PHASE2_EVENT_ROUND_TRIP_DELAY ||
	    observation->phase2.kind ==
		    PCM_PHASE2_EVENT_INITIAL_PROBE_STARTED) {
		/* First-ranging recovery can refine the measured delay without a
		 * full retrain. The shared ranging owner validates each new epoch. */
		call->round_trip_delay_samples =
			observation->phase2.round_trip_delay_samples;
		call->round_trip_delay_stored = true;
	}
	call_result = advance_phase2(call, &observation->phase2);
	return call_result == PCM_CALL_OK ?
		arm_b1_deadline(call, &observation->phase2) : call_result;
}

static enum pcm_call_result sample_retrain_silence(
	struct pcm_call *call, int16_t received_sample,
	uint8_t *transmit_octet)
{
	struct pcm_phase2 restarted;
	struct v34_full_retrain next_retrain = call->retrain;
	enum v34_full_retrain_response_event event;

	*transmit_octet = encode_sample(call->law, 0);
	if (v34_full_retrain_sample_response(
		    &next_retrain, received_sample, &event) !=
	    V34_PHASE2_TONE_OK)
		return PCM_CALL_NUMERIC_FAILURE;
	if (event == V34_FULL_RETRAIN_RESPONSE_CONTINUES) {
		call->retrain = next_retrain;
		return PCM_CALL_OK;
	}
	if (initialize_retrain_phase2(call, &restarted) != PCM_PHASE2_OK)
		return PCM_CALL_NUMERIC_FAILURE;
	if (event ==
	    V34_FULL_RETRAIN_RESPONSE_COMPLETES_WITH_PEER_TONE) {
		restarted.state.ranging.info0_exchange.transport.tone_detector =
			next_retrain.detector.tone;
	} else {
		/* A local request has no peer phase to retain, but still listens
		 * at the successfully trained line level for the weak response. */
		restarted.state.ranging.info0_exchange.transport.tone_detector
			.carrier.profile.minimum_line_power = next_retrain.detector
				.tone.carrier.profile.minimum_line_power;
	}
	if (!pcm_phase2_is_valid(&restarted))
		return PCM_CALL_NUMERIC_FAILURE;
	call->retrain = next_retrain;
	call->state.phase2 = restarted;
	call->stage = PCM_CALL_PHASE2;
	return PCM_CALL_OK;
}

static bool retrain_detection_is_active(const struct pcm_call *call)
{
	/* V.90 requires a modem to honor the peer's Tone A/B full-retrain
	 * request during INFO1 recovery, final training, and online operation.
	 * The detector's silence preamble distinguishes that request from
	 * ordinary training. */
	if (call->stage == PCM_CALL_PHASE3 ||
	    call->stage == PCM_CALL_PHASE3_MIXED)
		return true;
	if (call->stage == PCM_CALL_PHASE4)
		return true;
	/* The data-path owner also clocks CP/MP, E and B1 before carrier opens.
	 * Waiting for online data would ignore a peer's Phase 4 retrain request. */
	if (call->stage == PCM_CALL_DATA_PATH)
		return true;
	return call->protocol == PCM_PROTOCOL_V90 &&
	       call->stage == PCM_CALL_PHASE2 &&
	       call->state.phase2.stage == PCM_PHASE2_INFO1 &&
	       phase2_info1_is_waiting_for_peer(
		       &call->state.phase2.state.info1.control);
}

static enum pcm_call_result sample_retrain_detector(
	const struct pcm_call *call, int16_t received_sample,
	struct v34_full_retrain *observed, bool *detected)
{
	*observed = call->retrain;
	return v34_retrain_detector_sample(
		       &observed->detector, received_sample, detected) ==
		       V34_PHASE2_TONE_OK ?
		       PCM_CALL_OK : PCM_CALL_NUMERIC_FAILURE;
}

static uint32_t retrain_silence_ceiling(
	const struct pcm_call *call, uint32_t nominal_window_energy)
{
	const struct v90_digital_path *path;
	const struct v34_line_receiver *line;
	uint64_t gain;
	const uint64_t unity = UINT64_C(1) << V34_GAIN_FRACTION_BITS;

	if (call->local_role != V90_DIGITAL_MODEM ||
	    call->stage != PCM_CALL_DATA_PATH)
		return nominal_window_energy;
	path = &call->state.data_path.digital;
	if (path->upstream_stage == V90_DIGITAL_PATH_UPSTREAM_DATA) {
		line = &path->upstream.data.line;
	} else {
		const struct v90_upstream_rx *startup = &path->upstream.startup;

		/* CP, E and B1 retain the same trained physical receive level.
		 * A fixed raw quiet ceiling can stop decodable weak training. */
		if (startup->stage == V90_UPSTREAM_RX_STAGE_CP)
			line = &startup->state.cp.line;
		else if (startup->stage == V90_UPSTREAM_RX_STAGE_E)
			line = &startup->state.e.line;
		else
			line = &startup->state.data.line;
	}
	gain = (uint16_t)line->timing.gain.gain;
	if (gain <= unity)
		return nominal_window_energy;
	/* The trained Q5.10 multiplier compensates conventional upstream loss.
	 * Scale guard, pause, and raw tone power by its square; otherwise decodable
	 * weak QAM can retain a silence-only pause and weak Tone A is never heard.
	 * The largest numerator is 9600000 * 1024^2 (<2^44); positive int16 gain
	 * leaves even the smaller pause ceiling >=4096. No rounding can yield 0. */
	return (uint32_t)(nominal_window_energy * unity * unity / (gain * gain));
}

static bool retrain_candidate_is_active(
	const struct v34_retrain_detector *detector,
	uint32_t maximum_window_energy)
{
	/* The retrain detector deliberately uses a generous silence ceiling so
	 * that bounded line noise cannot break the 65 ms guard. Received QAM can
	 * remain decodable below that ceiling, however, so pause the data decoder
	 * only after either unambiguous quiet or the role-specific tone itself.
	 * A complete quiet window is enough to pause; waiting for the whole
	 * retrain guard would decode its silence into spurious upstream bytes.
	 * The detector still owns qualification of the full silence/tone request. */
	return detector->tone_samples != 0u ||
	       (detector->silence_history_count ==
			V34_RETRAIN_SILENCE_WINDOW_SAMPLES &&
		detector->silence_energy <= maximum_window_energy);
}

static enum pcm_call_result observe_retrain_tone(
	struct pcm_call *call, int16_t received_sample)
{
	struct v34_full_retrain observed;
	bool detected;

	if (sample_retrain_detector(
		    call, received_sample, &observed, &detected) !=
	    PCM_CALL_OK)
		return PCM_CALL_NUMERIC_FAILURE;
	call->retrain = observed;
	return detected ? begin_retrain(call, true) : PCM_CALL_OK;
}

static enum pcm_call_result reset_retrain_detector(struct pcm_call *call)
{
	return v34_retrain_detector_reset(&call->retrain.detector) ==
		       V34_PHASE2_TONE_OK ?
		       PCM_CALL_OK : PCM_CALL_NUMERIC_FAILURE;
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
	case V90_PHASE3_DIGITAL_STATION_COMPLETE:
	case V90_PHASE3_DIGITAL_STATION_INVALID_STATE:
		return PCM_CALL_INVALID_STATE;
	case V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT:
	case V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE:
		break;
	}
	return PCM_CALL_NUMERIC_FAILURE;
}

static enum pcm_call_result v92_analog_phase3_result(
	enum v92_phase3_analog_station_result result)
{
	switch (result) {
	case V92_PHASE3_ANALOG_STATION_OK:
	case V92_PHASE3_ANALOG_STATION_COMPLETE:
		return PCM_CALL_OK;
	case V92_PHASE3_ANALOG_STATION_FAILED:
		return PCM_CALL_FAILED;
	case V92_PHASE3_ANALOG_STATION_NEED_MD_SYMBOL:
		return PCM_CALL_UNUSABLE_SELECTION;
	case V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH:
		return PCM_CALL_SEQUENCE_MISMATCH;
	case V92_PHASE3_ANALOG_STATION_INVALID_STATE:
		return PCM_CALL_INVALID_STATE;
	case V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT:
	case V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE:
		break;
	}
	return PCM_CALL_NUMERIC_FAILURE;
}

static enum pcm_call_result v92_digital_phase3_result(
	enum v92_phase3_digital_station_result result)
{
	switch (result) {
	case V92_PHASE3_DIGITAL_STATION_OK:
	case V92_PHASE3_DIGITAL_STATION_COMPLETE:
		return PCM_CALL_OK;
	case V92_PHASE3_DIGITAL_STATION_FAILED:
		return PCM_CALL_FAILED;
	case V92_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH:
		return PCM_CALL_SEQUENCE_MISMATCH;
	case V92_PHASE3_DIGITAL_STATION_UNSUPPORTED:
		return PCM_CALL_UNUSABLE_SELECTION;
	case V92_PHASE3_DIGITAL_STATION_INVALID_STATE:
		return PCM_CALL_INVALID_STATE;
	case V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT:
	case V92_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE:
		break;
	}
	return PCM_CALL_NUMERIC_FAILURE;
}

static enum pcm_call_result enter_mixed_phase3(struct pcm_call *call)
{
	struct pcm_call next = *call;

	next.state.mixed = (union pcm_call_mixed_state){ 0 };
	if (call->local_role == V90_ANALOG_MODEM) {
		struct pcm_call_analog_mixed_state mixed = {
			.phase3 = call->state.phase3.analog,
		};

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

static bool v92_trailing_phase3_is_complete(const struct pcm_call *call)
{
	return call->local_role == V90_ANALOG_MODEM ?
		       v92_phase3_analog_station_transmit_complete(
			       &call->state.mixed.v92_analog.phase3) :
		       v92_phase3_digital_station_receive_complete(
			       &call->state.mixed.v92_digital.phase3);
}

static enum pcm_call_result enter_v92_mixed_phase3(struct pcm_call *call)
{
	struct pcm_call next = *call;
	struct v92_cpt_parameters parameters;
	struct v90_pcm_mapping mapping;

	next.state.mixed = (union pcm_call_mixed_state){ 0 };
	if (call->local_role == V90_ANALOG_MODEM) {
		struct pcm_call_v92_analog_mixed_state mixed = {
			.phase3 = call->state.phase3.v92_analog,
		};

		if (v92_phase3_analog_station_downstream_handoff(
			    &mixed.phase3, &parameters) !=
			    V92_PHASE3_ANALOG_STATION_OK)
			return PCM_CALL_INVALID_STATE;
		if (v90_pcm_mapping_init_from_v92_cpt(
			    &mapping, &parameters) != V90_PCM_MAPPING_OK)
			return PCM_CALL_UNUSABLE_SELECTION;
		if (pcm_downstream_rx_init(
			    &mixed.receive, PCM_PROTOCOL_V92, &mapping,
			    V90_PCM_MAPPING_TRANSMIT) != PCM_DOWNSTREAM_RX_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		next.state.mixed.v92_analog = mixed;
	} else {
		struct pcm_call_v92_digital_mixed_state mixed = {
			.phase3 = call->state.phase3.v92_digital,
		};

		if (v92_phase3_digital_station_downstream_handoff(
			    &mixed.phase3, &parameters) !=
			    V92_PHASE3_DIGITAL_STATION_OK)
			return PCM_CALL_INVALID_STATE;
		if (v90_pcm_mapping_init_from_v92_cpt(
			    &mapping, &parameters) != V90_PCM_MAPPING_OK)
			return PCM_CALL_UNUSABLE_SELECTION;
		if (pcm_downstream_tx_init(
			    &mixed.transmit, PCM_PROTOCOL_V92, &mapping) !=
			    PCM_DOWNSTREAM_TX_OK ||
		    pcm_downstream_tx_start_suvd(
			    &mixed.transmit,
			    &call->v92_digital_phase4_configuration.suvd) !=
			    PCM_DOWNSTREAM_TX_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		next.state.mixed.v92_digital = mixed;
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

static void limit_analog_phase4_lookahead(
	struct pcm_call_analog_phase4_configuration *configuration,
	uint8_t maximum_lookahead)
{
	if (configuration->training_parameters.spectral_lookahead >
	    maximum_lookahead)
		configuration->training_parameters.spectral_lookahead =
			maximum_lookahead;
	if (configuration->data_parameters.spectral_lookahead >
	    maximum_lookahead)
		configuration->data_parameters.spectral_lookahead =
			maximum_lookahead;
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
		limit_analog_phase4_lookahead(
			&next.analog_phase4_configuration,
			receive_handoff.jd
				.maximum_spectral_shaping_lookahead);
		next.analog_phase4_configuration.data_parameters
			.selected_rate_index = selected_rate_index;
		if (!call->round_trip_delay_stored)
			return PCM_CALL_INVALID_STATE;
		result = analog_phase4_result(v90_phase4_analog_station_init(
			&station, &transmit_handoff, &receive_handoff.jd,
			receive_handoff.law, receive_handoff.training_ucode,
			call->round_trip_delay_samples,
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
			    &mixed->transmit) !=
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

static enum pcm_call_result enter_v92_phase4(struct pcm_call *call)
{
	struct pcm_call next = *call;

	next.state.phase4 = (union pcm_call_phase4_state){ 0 };
	if (call->local_role == V90_ANALOG_MODEM) {
		const struct pcm_call_v92_analog_mixed_state *mixed =
			&call->state.mixed.v92_analog;
		const struct v92_suvd_parameters *early_suvd =
			mixed->early_suvd_received ? &mixed->early_suvd : NULL;
		struct v92_phase3_analog_transmit_handoff handoff;
		struct v92_phase4_analog_station station;

		if (!call->v92_peer_downstream_rate_stored ||
		    !call->round_trip_delay_stored)
			return PCM_CALL_INVALID_STATE;
		if (!downstream_rate_is_enabled(
			    call->enabled_downstream_rate_mask &
				    call->v92_peer_downstream_rate_mask,
			    call->v92_analog_phase4_configuration.cpu
				    .data_rate_number))
			return PCM_CALL_UNUSABLE_SELECTION;
		if (v92_phase3_analog_station_transmit_handoff(
			    &mixed->phase3, &handoff) !=
		    V92_PHASE3_ANALOG_STATION_OK)
			return PCM_CALL_INVALID_STATE;
		if (v92_phase4_analog_station_init(
			    &station, &handoff, &mixed->receive,
			    &call->v92_analog_phase4_configuration.suvu,
			    &call->v92_analog_phase4_configuration.cpu,
			    early_suvd, NULL) != V92_PHASE4_ANALOG_STATION_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		next.state.phase4.v92 = (struct pcm_call_v92_phase4_state){
			.owner.analog = {
				.station = station,
				.cpu_repeat_interval_samples =
					V92_CPU_REPEAT_BASE_SAMPLES +
					call->round_trip_delay_samples,
			},
			.child = PCM_CALL_V92_PHASE4_CONTROL,
		};
	} else {
		const struct pcm_call_v92_digital_mixed_state *mixed =
			&call->state.mixed.v92_digital;
		struct v92_pcm_phase3_cpt_line_handoff handoff;
		struct v92_phase4_digital_station station;
		uint32_t upstream_rate_mask;

		if (!v92_phase3_digital_station_upstream_rate_mask(
			    &mixed->phase3, &upstream_rate_mask))
			return PCM_CALL_INVALID_STATE;
		if (!v92_pcm_dil_rate_is_enabled(
			    upstream_rate_mask,
			    call->v92_digital_phase4_configuration.cpd
				    .data_rate_number))
			return PCM_CALL_UNUSABLE_SELECTION;
		if (v92_phase3_digital_station_receive_handoff(
			    &mixed->phase3, &handoff) !=
		    V92_PHASE3_DIGITAL_STATION_OK)
			return PCM_CALL_INVALID_STATE;
		if (v92_phase4_digital_station_init(
			    &station, &handoff,
			    call->phase3_configuration.v92_digital.jp
				    .training_constellation,
			    &mixed->transmit,
			    &call->v92_digital_phase4_configuration.cpd) !=
		    V92_PHASE4_DIGITAL_STATION_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		next.state.phase4.v92 = (struct pcm_call_v92_phase4_state){
			.owner.digital = station,
			.child = PCM_CALL_V92_PHASE4_CONTROL,
		};
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

	if (call->protocol == PCM_PROTOCOL_V92) {
		if (call->stage == PCM_CALL_PHASE3) {
			bool leading_direction_complete =
				call->local_role == V90_ANALOG_MODEM ?
					v92_phase3_analog_station_receive_complete(
						&call->state.phase3.v92_analog) :
					v92_phase3_digital_station_transmit_complete(
						&call->state.phase3.v92_digital);

			if (!leading_direction_complete)
				return PCM_CALL_OK;
			result = enter_v92_mixed_phase3(call);
			if (result != PCM_CALL_OK)
				return result;
		}
		if (!v92_trailing_phase3_is_complete(call))
			return PCM_CALL_OK;
		return enter_v92_phase4(call);
	}
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

static enum pcm_call_result retain_v92_peer_downstream_rates(
	struct pcm_call *call)
{
	const struct v92_phase3_analog_station *station =
		&call->state.phase3.v92_analog;
	struct v92_jd jd;

	if (station->receive_stage != V92_PHASE3_ANALOG_RECEIVE_J ||
	    !v92_phase3_j_receiver_jd(&station->receive.j, &jd))
		return PCM_CALL_OK;
	if (call->v92_peer_downstream_rate_stored) {
		return call->v92_peer_downstream_rate_mask ==
			       jd.data_rate_capability_mask ?
			       PCM_CALL_OK : PCM_CALL_SEQUENCE_MISMATCH;
	}
	call->v92_peer_downstream_rate_mask =
		jd.data_rate_capability_mask;
	call->v92_peer_downstream_rate_stored = true;
	return jd.data_rate_capability_mask != 0u ?
		       PCM_CALL_OK : PCM_CALL_UNUSABLE_SELECTION;
}

static bool phase3_response_timed_out(const struct pcm_call *call)
{
	const struct pcm_phase3_digital_tx *transmit;
	uint64_t elapsed;

	if (call->protocol != PCM_PROTOCOL_V90)
		return false;
	if (call->local_role == V90_ANALOG_MODEM) {
		const struct v90_phase3_analog_station *station =
			&call->state.phase3.analog;
		const struct v90_phase3_analog_line_tx *initial;
		const struct v34_pulse_modulator *carrier;

		if (station->transmit_stage != V90_PHASE3_ANALOG_TRANSMIT_INITIAL ||
		    station->receive_stage != V90_PHASE3_ANALOG_RECEIVE_SD ||
		    station->receive.sd.stage >= V90_PHASE3_SD_RECEIVER_SD_BAR)
			return false;
		initial = &station->transmit.initial;
		if (initial->symbols.stage != V90_PHASE3_ANALOG_TX_JA ||
		    initial->symbols.ja_symbols == 0u)
			return false;
		/* V.90 9.3.2.4 allows 1500 ms from Ja to the Sd/Sd-bar
		 * transition. Use the accepted Ja points and shared rational
		 * line clock, including a crossing reached by this sample whose
		 * next point has not yet been accepted. No parallel timer or
		 * rounding of fractional baud is needed. */
		carrier = &initial->line.carrier;
		elapsed = (uint64_t)(initial->symbols.ja_symbols - 1u) *
			  carrier->symbol_period_units + carrier->symbol_phase_units;
		if (carrier->symbol_phase_units < carrier->channel.symbol_rate_numerator)
			elapsed += carrier->symbol_period_units;
		return elapsed >= (uint64_t)V90_PHASE3_SD_DEADLINE_SAMPLES *
				  carrier->channel.symbol_rate_numerator;
	}
	transmit = &call->state.phase3.digital.transmit;
	if (!call->state.phase3.digital.transmit_started ||
	    transmit->stage != PCM_PHASE3_DIGITAL_TX_JD ||
	    transmit->finish_jd_requested)
		return false;
	/* V.90 9.3.1.5 allows 5100 ms plus RTD from the start of TRN1d
	 * to detection of S. The shared transmitter already owns the complete
	 * elapsed-symbol count, so no parallel timer or reset state is needed.
	 * Receive runs first: S on the final sample cancels this deadline even
	 * while the remainder of the current Jd record is still being sent. */
	elapsed = (uint64_t)transmit->sequence.signs.trn1d_symbols +
		  (uint64_t)transmit->jd_records * PCM_J_FRAME_BITS +
		  transmit->bit_index;
	return elapsed >= V90_PHASE3_RETURN_BASE_DEADLINE_SAMPLES +
			  (uint32_t)call->round_trip_delay_samples;
}

static enum pcm_call_result sample_phase3(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	uint8_t *transmit_octet)
{
	enum pcm_call_result result;

	if (call->protocol == PCM_PROTOCOL_V92 &&
	    call->local_role == V90_ANALOG_MODEM) {
		int16_t sample;

		result = v92_analog_phase3_result(
			v92_phase3_analog_station_transmit(
				&call->state.phase3.v92_analog, NULL, &sample));
		if (result != PCM_CALL_OK)
			return result;
		*transmit_octet = encode_sample(call->law, sample);
		result = v92_analog_phase3_result(
			v92_phase3_analog_station_receive(
				&call->state.phase3.v92_analog,
				received_octet));
		if (result == PCM_CALL_OK)
			result = retain_v92_peer_downstream_rates(call);
	} else if (call->protocol == PCM_PROTOCOL_V92) {
		result = v92_digital_phase3_result(
			v92_phase3_digital_station_transmit(
				&call->state.phase3.v92_digital,
				transmit_octet));
		if (result != PCM_CALL_OK)
			return result;
		result = v92_digital_phase3_result(
			v92_phase3_digital_station_receive(
				&call->state.phase3.v92_digital,
				decode_octet(call->law, received_octet)));
	} else if (call->local_role == V90_ANALOG_MODEM) {
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
		bool receive_complete =
			v90_phase3_digital_station_receive_complete(
				&call->state.phase3.digital);

		/* The second return can finish while the independently owned DIL
		 * transmitter completes its current segment. Keep advancing the same
		 * conventional-sample receiver while the completed marker stays frozen. */
		if (receive_complete) {
			result = digital_phase3_result(
				v90_phase3_digital_station_advance_completed_receive(
					&call->state.phase3.digital,
					received_sample));
			if (result != PCM_CALL_OK)
				return result;
		}
		result = digital_phase3_result(
			v90_phase3_digital_station_transmit(
				&call->state.phase3.digital, transmit_octet));
		if (result != PCM_CALL_OK)
			return result;
		if (!receive_complete)
			result = digital_phase3_result(
				v90_phase3_digital_station_receive(
					&call->state.phase3.digital,
					received_sample));
	}
	if (result == PCM_CALL_OK && phase3_response_timed_out(call))
		return begin_retrain(call, false);
	return result == PCM_CALL_OK ? advance_phase3(call) : result;
}

static bool retain_early_suvd(
	struct pcm_call_v92_analog_mixed_state *mixed,
	const struct v92_suvd_parameters *parameters)
{
	if (!v92_suvd_parameters_are_valid(parameters))
		return false;
	if (!mixed->early_suvd_received) {
		mixed->early_suvd = *parameters;
		mixed->early_suvd_received = true;
		return true;
	}
	if (mixed->early_suvd.request_silence != parameters->request_silence ||
	    (mixed->early_suvd.acknowledges_cpu &&
	     !parameters->acknowledges_cpu))
		return false;
	mixed->early_suvd.acknowledges_cpu |= parameters->acknowledges_cpu;
	return true;
}

static enum pcm_call_result receive_v92_mixed_downstream(
	struct pcm_call_v92_analog_mixed_state *mixed, uint8_t received_octet)
{
	struct pcm_downstream_event event = { 0 };
	enum pcm_downstream_rx_result result = pcm_downstream_rx_receive(
		&mixed->receive, received_octet, &event);

	switch (result) {
	case PCM_DOWNSTREAM_RX_OK:
	case PCM_DOWNSTREAM_RX_BUFFERING:
	case PCM_DOWNSTREAM_RX_TRN2D_FRAME:
		return PCM_CALL_OK;
	case PCM_DOWNSTREAM_RX_SUVD:
		return event.control.type == V92_CONTROL_EVENT_SUVD &&
		       event.control.record.suvd.frame_result == V92_SUVD_OK &&
		       retain_early_suvd(
			       mixed, &event.control.record.suvd.parameters) ?
		       PCM_CALL_OK : PCM_CALL_SEQUENCE_MISMATCH;
	case PCM_DOWNSTREAM_RX_SUVD_REJECTED:
		return PCM_CALL_OK;
	case PCM_DOWNSTREAM_RX_MP:
	case PCM_DOWNSTREAM_RX_MP_PRIME:
	case PCM_DOWNSTREAM_RX_MP_REJECTED:
	case PCM_DOWNSTREAM_RX_CPD:
	case PCM_DOWNSTREAM_RX_CPD_REJECTED:
	case PCM_DOWNSTREAM_RX_ED:
	case PCM_DOWNSTREAM_RX_ED_REJECTED:
	case PCM_DOWNSTREAM_RX_INVALID_SYMBOL:
		return PCM_CALL_SEQUENCE_MISMATCH;
	case PCM_DOWNSTREAM_RX_INVALID_PARAMETERS:
		return PCM_CALL_UNUSABLE_SELECTION;
	case PCM_DOWNSTREAM_RX_INVALID_STATE:
		return PCM_CALL_INVALID_STATE;
	case PCM_DOWNSTREAM_RX_OUT_OF_RANGE:
		return PCM_CALL_OUT_OF_RANGE;
	case PCM_DOWNSTREAM_RX_INVALID_ARGUMENT:
	case PCM_DOWNSTREAM_RX_INTERNAL_ERROR:
		break;
	}
	return PCM_CALL_NUMERIC_FAILURE;
}

static enum pcm_call_result v92_downstream_transmit_result(
	enum pcm_downstream_tx_result result)
{
	switch (result) {
	case PCM_DOWNSTREAM_TX_OK:
		return PCM_CALL_OK;
	case PCM_DOWNSTREAM_TX_COMPLETE:
	case PCM_DOWNSTREAM_TX_INVALID_STATE:
		return PCM_CALL_INVALID_STATE;
	case PCM_DOWNSTREAM_TX_INVALID_PARAMETERS:
		return PCM_CALL_UNUSABLE_SELECTION;
	case PCM_DOWNSTREAM_TX_OUT_OF_RANGE:
		return PCM_CALL_OUT_OF_RANGE;
	case PCM_DOWNSTREAM_TX_INVALID_ARGUMENT:
	case PCM_DOWNSTREAM_TX_NUMERIC_FAILURE:
	case PCM_DOWNSTREAM_TX_INTERNAL_ERROR:
		break;
	}
	return PCM_CALL_NUMERIC_FAILURE;
}

static enum pcm_call_result sample_v92_mixed_phase3(
	struct pcm_call *call, uint8_t received_octet,
	uint8_t *transmit_octet)
{
	enum pcm_call_result result;

	if (call->local_role == V90_ANALOG_MODEM) {
		struct pcm_call_v92_analog_mixed_state *mixed =
			&call->state.mixed.v92_analog;
		int16_t sample;

		result = v92_analog_phase3_result(
			v92_phase3_analog_station_transmit(
				&mixed->phase3, NULL, &sample));
		if (result != PCM_CALL_OK)
			return result;
		*transmit_octet = encode_sample(call->law, sample);
		result = receive_v92_mixed_downstream(mixed, received_octet);
	} else {
		struct pcm_call_v92_digital_mixed_state *mixed =
			&call->state.mixed.v92_digital;
		struct pcm_downstream_symbol symbol;

		result = v92_downstream_transmit_result(
			pcm_downstream_tx_next(&mixed->transmit, &symbol));
		if (result != PCM_CALL_OK)
			return result;
		*transmit_octet = symbol.pcm.octet;
		result = v92_digital_phase3_result(
			v92_phase3_digital_station_receive(
				&mixed->phase3,
				decode_octet(call->law, received_octet)));
	}
	return result == PCM_CALL_OK ? advance_phase3(call) : result;
}

static enum pcm_call_result sample_mixed_phase3(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	uint8_t *transmit_octet)
{
	enum pcm_call_result result;

	if (call->protocol == PCM_PROTOCOL_V92)
		return sample_v92_mixed_phase3(
			call, received_octet, transmit_octet);

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
		return advance_phase3(call);
	} else {
		struct pcm_call_digital_mixed_state *mixed =
			&call->state.mixed.digital;

		if (v90_phase4_ri_tx_next(
			    &mixed->transmit, transmit_octet) != V90_PHASE4_RI_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		result = digital_phase3_result(
			v90_phase3_digital_station_receive(
				&mixed->phase3, received_sample));
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

static enum pcm_call_result v92_analog_phase4_result(
	enum v92_phase4_analog_station_result result)
{
	switch (result) {
	case V92_PHASE4_ANALOG_STATION_OK:
	case V92_PHASE4_ANALOG_STATION_EVENT:
		return PCM_CALL_OK;
	case V92_PHASE4_ANALOG_STATION_FAILED:
		return PCM_CALL_FAILED;
	case V92_PHASE4_ANALOG_STATION_INVALID_PARAMETERS:
		return PCM_CALL_UNUSABLE_SELECTION;
	case V92_PHASE4_ANALOG_STATION_INVALID_STATE:
	case V92_PHASE4_ANALOG_STATION_E2U_READY:
		return PCM_CALL_INVALID_STATE;
	case V92_PHASE4_ANALOG_STATION_INVALID_ARGUMENT:
	case V92_PHASE4_ANALOG_STATION_INTERNAL_ERROR:
		break;
	}
	return PCM_CALL_NUMERIC_FAILURE;
}

static enum pcm_call_result v92_digital_phase4_result(
	enum v92_phase4_digital_station_result result)
{
	switch (result) {
	case V92_PHASE4_DIGITAL_STATION_OK:
	case V92_PHASE4_DIGITAL_STATION_EVENT:
		return PCM_CALL_OK;
	case V92_PHASE4_DIGITAL_STATION_FAILED:
		return PCM_CALL_FAILED;
	case V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS:
		return PCM_CALL_UNUSABLE_SELECTION;
	case V92_PHASE4_DIGITAL_STATION_INVALID_STATE:
		return PCM_CALL_INVALID_STATE;
	case V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT:
	case V92_PHASE4_DIGITAL_STATION_INTERNAL_ERROR:
		break;
	}
	return PCM_CALL_NUMERIC_FAILURE;
}

static enum pcm_call_result observe_v92_cpu_completion(
	struct pcm_call_v92_analog_phase4_control *state,
	bool *completed_this_sample)
{
	uint64_t completed =
		state->station.transmit.completed_cpu_records;

	*completed_this_sample = false;
	if (completed == state->observed_cpu_records)
		return PCM_CALL_OK;
	if (state->observed_cpu_records == UINT64_MAX ||
	    completed != state->observed_cpu_records + 1u)
		return PCM_CALL_NUMERIC_FAILURE;
	state->observed_cpu_records = completed;
	state->awaiting_cpu_completion = false;
	state->cpu_repeat_remaining_samples =
		state->station.transmit.peer_confirmation_received ?
			0u : state->cpu_repeat_interval_samples;
	*completed_this_sample = true;
	return PCM_CALL_OK;
}

static enum pcm_call_result advance_v92_cpu_repeat(
	struct pcm_call_v92_analog_phase4_control *state,
	bool completed_this_sample)
{
	if (state->station.transmit.peer_confirmation_received) {
		state->cpu_repeat_remaining_samples = 0u;
		state->awaiting_cpu_completion = false;
		return PCM_CALL_OK;
	}
	if (completed_this_sample || state->observed_cpu_records == 0u ||
	    state->awaiting_cpu_completion)
		return PCM_CALL_OK;
	if (state->cpu_repeat_remaining_samples == 0u)
		return PCM_CALL_NUMERIC_FAILURE;
	if (--state->cpu_repeat_remaining_samples != 0u)
		return PCM_CALL_OK;
	if (v92_phase4_analog_station_repeat_cpu(&state->station) !=
	    V92_PHASE4_ANALOG_STATION_OK)
		return PCM_CALL_NUMERIC_FAILURE;
	state->awaiting_cpu_completion = true;
	return PCM_CALL_OK;
}

static void cancel_v92_cpu_repeat_if_confirmed(
	struct pcm_call_v92_analog_phase4_control *state)
{
	if (!state->station.transmit.peer_confirmation_received)
		return;
	state->cpu_repeat_remaining_samples = 0u;
	state->awaiting_cpu_completion = false;
}

static bool v92_phase4_control_is_ready(const struct pcm_call *call)
{
	if (call->state.phase4.v92.child != PCM_CALL_V92_PHASE4_CONTROL)
		return false;
	return call->local_role == V90_ANALOG_MODEM ?
		v92_phase4_analog_station_is_e2u_ready(
			&call->state.phase4.v92.owner.analog.station) :
		v92_phase4_digital_station_is_ed_ready(
			&call->state.phase4.v92.owner.digital);
}

static enum pcm_call_result enter_v92_e2u(struct pcm_call *call)
{
	struct pcm_call next = *call;
	struct v92_phase4_e2u e2u;

	if (!v92_phase4_control_is_ready(call))
		return PCM_CALL_INVALID_STATE;
	if (call->local_role == V90_ANALOG_MODEM) {
		struct v92_phase4_analog_handoff handoff;

		if (v92_phase4_analog_station_handoff(
			    &call->state.phase4.v92.owner.analog.station,
			    &handoff) != V92_PHASE4_ANALOG_STATION_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		if (v92_phase4_e2u_init_analog(&e2u, &handoff) !=
		    V92_PHASE4_E2U_OK)
			return PCM_CALL_NUMERIC_FAILURE;
	} else {
		struct v92_phase4_digital_handoff handoff;

		if (v92_phase4_digital_station_handoff(
			    &call->state.phase4.v92.owner.digital,
			    &handoff) != V92_PHASE4_DIGITAL_STATION_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		if (v92_phase4_e2u_init_digital(&e2u, &handoff) !=
		    V92_PHASE4_E2U_OK)
			return PCM_CALL_NUMERIC_FAILURE;
	}
	next.state.phase4.v92 = (struct pcm_call_v92_phase4_state){
		.owner.e2u = e2u,
		.child = PCM_CALL_V92_PHASE4_E2U,
	};
	if (!pcm_call_is_valid(&next))
		return PCM_CALL_NUMERIC_FAILURE;
	*call = next;
	return PCM_CALL_OK;
}

static enum pcm_call_result v92_e2u_result(
	enum v92_phase4_e2u_result result)
{
	switch (result) {
	case V92_PHASE4_E2U_OK:
	case V92_PHASE4_E2U_COMPLETE:
		return PCM_CALL_OK;
	case V92_PHASE4_E2U_FAILED:
		return PCM_CALL_FAILED;
	case V92_PHASE4_E2U_INVALID_PARAMETERS:
		return PCM_CALL_UNUSABLE_SELECTION;
	case V92_PHASE4_E2U_INVALID_STATE:
		return PCM_CALL_INVALID_STATE;
	case V92_PHASE4_E2U_INVALID_ARGUMENT:
	case V92_PHASE4_E2U_INTERNAL_ERROR:
		break;
	}
	return PCM_CALL_NUMERIC_FAILURE;
}

static enum pcm_call_result sample_v92_phase4(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	uint8_t *transmit_octet)
{
	if (call->local_role == V90_ANALOG_MODEM &&
	    call->state.phase4.v92.child == PCM_CALL_V92_PHASE4_CONTROL &&
	    v92_phase4_control_is_ready(call)) {
		enum pcm_call_result result = enter_v92_e2u(call);

		if (result != PCM_CALL_OK)
			return result;
	}
	if (call->state.phase4.v92.child == PCM_CALL_V92_PHASE4_E2U)
		return v92_e2u_result(v92_phase4_e2u_next(
			&call->state.phase4.v92.owner.e2u,
			received_octet, received_sample, transmit_octet));
	if (call->local_role == V90_ANALOG_MODEM) {
		struct pcm_call_v92_analog_phase4_control *state =
			&call->state.phase4.v92.owner.analog;
		bool completed_this_sample;
		int16_t sample;
		enum pcm_call_result result = v92_analog_phase4_result(
			v92_phase4_analog_station_transmit(
				&state->station, &sample));

		if (result != PCM_CALL_OK)
			return result;
		*transmit_octet = encode_sample(call->law, sample);
		result = observe_v92_cpu_completion(
			state, &completed_this_sample);
		if (result != PCM_CALL_OK)
			return result;
		result = v92_analog_phase4_result(
			v92_phase4_analog_station_receive(
				&state->station, received_octet));
		cancel_v92_cpu_repeat_if_confirmed(state);
		if (result != PCM_CALL_OK)
			return result;
		if (state->station.cpd_received &&
		    !v92_pcm_dil_rate_is_enabled(
			    call->phase3_configuration.v92_analog
				    .upstream_rate_mask,
			    state->station.cpd.data_rate_number))
			return PCM_CALL_UNUSABLE_SELECTION;
		return advance_v92_cpu_repeat(
			state, completed_this_sample);
	}
	{
		enum v92_phase4_digital_station_result station_result =
			v92_phase4_digital_station_transmit(
				&call->state.phase4.v92.owner.digital,
				transmit_octet);
		enum pcm_call_result result =
			v92_digital_phase4_result(station_result);

		if (result != PCM_CALL_OK)
			return result;
		station_result = v92_phase4_digital_station_receive(
			&call->state.phase4.v92.owner.digital, received_sample);
		result = v92_digital_phase4_result(station_result);
		if (result != PCM_CALL_OK)
			return result;
		if (call->state.phase4.v92.owner.digital.cpu_received &&
		    !downstream_rate_is_enabled(
			    call->phase3_configuration.v92_digital.jd
				    .data_rate_capability_mask,
			    call->state.phase4.v92.owner.digital.cpu
				    .data_rate_number))
			return PCM_CALL_UNUSABLE_SELECTION;
		if (station_result == V92_PHASE4_DIGITAL_STATION_EVENT &&
		    v92_phase4_control_is_ready(call))
			return enter_v92_e2u(call);
		return PCM_CALL_OK;
	}
}

static enum pcm_call_result sample_phase4(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	uint8_t *transmit_octet)
{
	if (call->protocol == PCM_PROTOCOL_V92)
		return sample_v92_phase4(
			call, received_octet, received_sample, transmit_octet);
	enum v90_phase4_startup_result result =
		call->local_role == V90_ANALOG_MODEM ?
			v90_phase4_analog_startup_octet(
				&call->state.phase4.analog,
				received_octet, transmit_octet) :
			v90_phase4_digital_startup_sample(
				&call->state.phase4.digital,
				received_sample, transmit_octet);

	return phase4_startup_result(result);
}

static enum pcm_call_result analog_path_result(
	enum v90_analog_path_result result)
{
	switch (result) {
	case V90_ANALOG_PATH_OK:
	case V90_ANALOG_PATH_DOWNSTREAM_FRAME:
	case V90_ANALOG_PATH_RATE_RENEGOTIATION:
		return PCM_CALL_OK;
	case V90_ANALOG_PATH_CLEARDOWN_COMPLETE:
		return PCM_CALL_CLEARDOWN_COMPLETE;
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
	case V90_DIGITAL_PATH_RATE_RENEGOTIATION:
		return PCM_CALL_OK;
	case V90_DIGITAL_PATH_CLEARDOWN_COMPLETE:
		return PCM_CALL_CLEARDOWN_COMPLETE;
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

enum pcm_call_result pcm_call_start_cleardown(struct pcm_call *call)
{
	enum pcm_call_result result;

	if (!pcm_call_is_valid(call))
		return PCM_CALL_INVALID_ARGUMENT;
	if (call->stage == PCM_CALL_CLOSED || cleardown_is_active_owned(call))
		return PCM_CALL_OK;
	if (!pcm_call_is_online(call) || call->protocol != PCM_PROTOCOL_V90 ||
	    rate_renegotiation_is_active_owned(call))
		return PCM_CALL_INVALID_STATE;
	result = call->local_role == V90_ANALOG_MODEM ?
		analog_path_result(v90_analog_path_start_cleardown(
			&call->state.data_path.analog)) :
		digital_path_result(v90_digital_path_start_cleardown(
			&call->state.data_path.digital));
	if (result == PCM_CALL_OK && !pcm_call_is_valid(call))
		return PCM_CALL_NUMERIC_FAILURE;
	return result;
}

enum pcm_call_result pcm_call_start_data_path(struct pcm_call *call)
{
	struct pcm_call next;
	enum pcm_call_result result;

	if (!pcm_call_is_valid(call))
		return PCM_CALL_INVALID_ARGUMENT;
	if (call->failed)
		return PCM_CALL_FAILED;
	if (call->protocol != PCM_PROTOCOL_V90)
		return PCM_CALL_INVALID_STATE;
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
				call->enabled_downstream_rate_mask,
				call->phase3_configuration.digital.jd
					.rate_renegotiation_constellation));
		if (result != PCM_CALL_OK)
			return result;
		next.state.data_path.digital = path;
	}
	next.stage = PCM_CALL_DATA_PATH;
	if (v34_retrain_detector_reset(&next.retrain.detector) !=
	    V34_PHASE2_TONE_OK)
		return PCM_CALL_NUMERIC_FAILURE;
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
		return call->state.data_path.analog
			.upstream_rate_2400_bps_units;
	return call->state.data_path.digital
		.upstream_rate_2400_bps_units;
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

static enum pcm_call_result begin_rate_renegotiation_control(
	struct pcm_call *call)
{
	struct pcm_call next = *call;

	if (call->protocol != PCM_PROTOCOL_V90 ||
	    call->stage != PCM_CALL_DATA_PATH ||
	    !call->round_trip_delay_stored ||
	    !rate_renegotiation_is_active_owned(call) ||
	    call->rate_renegotiation_deadline_armed)
		return PCM_CALL_SEQUENCE_MISMATCH;
	if (reset_retrain_detector(&next) != PCM_CALL_OK)
		return PCM_CALL_NUMERIC_FAILURE;
	if (!pcm_call_owned_state_is_valid(&next))
		return PCM_CALL_NUMERIC_FAILURE;
	*call = next;
	return PCM_CALL_OK;
}

static enum pcm_call_result update_rate_renegotiation_deadline(
	struct pcm_call *call)
{
	if (!rate_renegotiation_response_is_pending_owned(call)) {
		call->rate_renegotiation_deadline_remaining_samples = 0u;
		call->rate_renegotiation_deadline_armed = false;
		return PCM_CALL_OK;
	}
	if (!call->rate_renegotiation_deadline_armed) {
		if (!rate_renegotiation_deadline_has_started(call))
			return PCM_CALL_OK;
		call->rate_renegotiation_deadline_remaining_samples =
			V90_RATE_RENEGOTIATION_BASE_DEADLINE_SAMPLES +
			V90_RATE_RENEGOTIATION_ROUND_TRIP_ALLOWANCES *
				(uint32_t)call->round_trip_delay_samples;
		call->rate_renegotiation_deadline_armed = true;
		return PCM_CALL_OK;
	}
	call->rate_renegotiation_deadline_remaining_samples--;
	if (call->rate_renegotiation_deadline_remaining_samples != 0u)
		return PCM_CALL_OK;
	/* V.90 9.6 bounds an incomplete rate-renegotiation exchange to 5000 ms
	 * plus two round-trip delays. */
	return begin_retrain(call, false);
}

static enum pcm_call_result recover_rate_renegotiation_failure(
	struct pcm_call *call, enum pcm_call_result result)
{
	if ((result != PCM_CALL_UNUSABLE_SELECTION &&
	     result != PCM_CALL_SEQUENCE_MISMATCH) ||
	    !rate_renegotiation_is_active_owned(call))
		return result;
	/* CPs/Rt echo reconditioning and a broken committed Rd/Rd-bar exchange
	 * are not completed in place. Leave either bounded control epoch through
	 * the standards-defined full-retrain path. */
	return begin_retrain(call, false);
}

static void complete_cleardown(struct pcm_call *call)
{
	call->stage = PCM_CALL_CLOSED;
	call->retrain_active = false;
	call->b1_deadline_armed = false;
	call->b1_deadline_remaining_samples = 0u;
	call->rate_renegotiation_deadline_armed = false;
	call->rate_renegotiation_deadline_remaining_samples = 0u;
}

static enum pcm_call_result data_octet_owned(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	const struct pcm_call_data_sources *sources,
	uint8_t *transmit_octet,
	struct pcm_call_data_event *event)
{
	const struct pcm_call_data_sources idle = { 0 };
	struct pcm_call_data_event emitted = { 0 };
	struct v90_pcm_stream_frame downstream_frame = { 0 };
	struct v34_received_mapping_frame upstream_frame = { 0 };
	enum v90_analog_path_result analog_result = V90_ANALOG_PATH_OK;
	enum v90_digital_path_result digital_result = V90_DIGITAL_PATH_OK;
	bool retrain_detection_active;
	bool retrain_candidate = false;
	bool retrain_detected = false;
	uint32_t silence_ceiling;
	uint8_t generated;
	enum pcm_call_result result;

	if (call->stage == PCM_CALL_CLOSED) {
		*transmit_octet = encode_sample(call->law, 0);
		*event = emitted;
		return PCM_CALL_CLEARDOWN_COMPLETE;
	}
	if (cleardown_is_active_owned(call)) {
		generated = encode_sample(call->law, 0);
		if (call->local_role == V90_ANALOG_MODEM) {
			result = analog_path_result(v90_analog_path_transmit(
				&call->state.data_path.analog,
				NULL, NULL, &generated));
			if (result == PCM_CALL_OK)
				result = analog_path_result(
					v90_analog_path_discard_receive(
						&call->state.data_path.analog));
		} else {
			result = digital_path_result(v90_digital_path_transmit(
				&call->state.data_path.digital,
				NULL, NULL, &generated));
		}
		*transmit_octet = generated;
		*event = emitted;
		if (result == PCM_CALL_CLEARDOWN_COMPLETE)
			complete_cleardown(call);
		else if (result != PCM_CALL_OK)
			call->failed = true;
		return result;
	}
	if (sources == NULL)
		sources = &idle;
	retrain_detection_active = retrain_detection_is_active(call);
	silence_ceiling = retrain_silence_ceiling(
		call, V34_RETRAIN_SILENCE_MAX_WINDOW_ENERGY);
	retrain_candidate = retrain_detection_active &&
		(retrain_candidate_is_active(
			 &call->retrain.detector,
			 retrain_silence_ceiling(
				 call, 256u * 256u *
				       V34_RETRAIN_SILENCE_WINDOW_SAMPLES)) ||
		(call->local_role == V90_ANALOG_MODEM &&
		 received_octet == encode_sample(call->law, 0)));
	generated = encode_sample(call->law, 0);
	if (call->local_role == V90_ANALOG_MODEM) {
		analog_result = v90_analog_path_transmit(
			&call->state.data_path.analog,
			sources->upstream, sources->upstream_context,
			&generated);
		result = analog_path_result(analog_result);
	} else {
		/* A quiet candidate clamps host output below, not the upstream
		 * sample clock. A short interruption may end without a retrain;
		 * dropping its samples loses carrier and mapping-frame phase. */
		digital_result = v90_digital_path_sample_owned(
			&call->state.data_path.digital, received_sample,
			sources->downstream, sources->downstream_context,
			&generated, &upstream_frame);
		result = digital_path_result(digital_result);
	}
	if (result == PCM_CALL_SOURCE_FAILURE)
		return result;
	result = recover_rate_renegotiation_failure(call, result);
	if (result == PCM_CALL_OK && call->stage == PCM_CALL_DATA_PATH &&
	    call->local_role == V90_ANALOG_MODEM) {
		/* Data decoding must retain the six-sample mapper phase through a
		 * short gap. Only the host output is clamped; a qualified retrain
		 * establishes a fresh epoch. Control acquisition still pauses. */
		analog_result = retrain_candidate &&
			call->state.data_path.analog.downstream_stage ==
				V90_ANALOG_PATH_DOWNSTREAM_TRAINING ?
			v90_analog_path_discard_receive(
				&call->state.data_path.analog) :
			v90_analog_path_receive(
				&call->state.data_path.analog, received_octet,
				&downstream_frame);
		result = analog_path_result(analog_result);
		result = recover_rate_renegotiation_failure(call, result);
		if (!retrain_candidate &&
		    result == PCM_CALL_OK &&
		    analog_result == V90_ANALOG_PATH_DOWNSTREAM_FRAME) {
			emitted.frame.downstream = downstream_frame;
			emitted.kind = PCM_CALL_DOWNSTREAM_FRAME;
		}
	} else if (result == PCM_CALL_OK &&
		   call->stage == PCM_CALL_DATA_PATH && !retrain_candidate) {
		if (digital_result == V90_DIGITAL_PATH_UPSTREAM_FRAME) {
			emitted.frame.upstream = upstream_frame;
			emitted.kind = PCM_CALL_UPSTREAM_FRAME;
		}
	}
	if (result == PCM_CALL_OK && call->stage == PCM_CALL_DATA_PATH &&
	    (digital_result == V90_DIGITAL_PATH_RATE_RENEGOTIATION ||
	     analog_result == V90_ANALOG_PATH_RATE_RENEGOTIATION)) {
		if (rate_renegotiation_is_active_owned(call)) {
			result = begin_rate_renegotiation_control(call);
		} else {
			result = begin_retrain(call, false);
		}
	} else if (result == PCM_CALL_OK &&
		   call->stage == PCM_CALL_DATA_PATH &&
		   retrain_detection_active) {
		if (v34_retrain_detector_sample_with_power_limit(
			    &call->retrain.detector, received_sample,
			    silence_ceiling, &retrain_detected) != V34_PHASE2_TONE_OK)
			result = PCM_CALL_NUMERIC_FAILURE;
	}
	if (result == PCM_CALL_OK && retrain_detected)
		result = begin_retrain(call, true);
	else if (result == PCM_CALL_OK && call->retrain_active &&
		 data_path_is_online_owned(call))
		call->retrain_active = false;
	if (result == PCM_CALL_OK)
		result = advance_b1_deadline(call);
	if (result == PCM_CALL_OK && call->protocol == PCM_PROTOCOL_V90 &&
	    call->stage == PCM_CALL_DATA_PATH)
		result = update_rate_renegotiation_deadline(call);
	if (result == PCM_CALL_CLEARDOWN_COMPLETE) {
		complete_cleardown(call);
		generated = encode_sample(call->law, 0);
		emitted = (struct pcm_call_data_event){ 0 };
	} else if (result != PCM_CALL_OK)
		call->failed = true;
	*transmit_octet = generated;
	*event = emitted;
	return result;
}

enum pcm_call_result pcm_call_data_octet(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	const struct pcm_call_data_sources *sources,
	uint8_t *transmit_octet,
	struct pcm_call_data_event *event)
{
	struct pcm_call_data_event emitted;
	uint8_t generated;
	enum pcm_call_result result;

	if (!pcm_call_owned_state_is_valid(call) || transmit_octet == NULL ||
	    event == NULL || data_outputs_overlap(call, transmit_octet, event))
		return PCM_CALL_INVALID_ARGUMENT;
	if (call->failed)
		return PCM_CALL_FAILED;
	if (call->stage != PCM_CALL_DATA_PATH && call->stage != PCM_CALL_CLOSED)
		return PCM_CALL_INVALID_STATE;
	result = data_octet_owned(
		call, received_octet, received_sample, sources,
		&generated, &emitted);
	if (result == PCM_CALL_SOURCE_FAILURE)
		return result;
	if (!pcm_call_owned_state_is_valid(call)) {
		call->failed = true;
		return PCM_CALL_NUMERIC_FAILURE;
	}
	*transmit_octet = generated;
	*event = emitted;
	return result;
}

enum pcm_call_result pcm_call_data_octet_owned(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	const struct pcm_call_data_sources *sources,
	uint8_t *transmit_octet,
	struct pcm_call_data_event *event)
{
	if (call == NULL || transmit_octet == NULL || event == NULL ||
	    data_outputs_overlap(call, transmit_octet, event))
		return PCM_CALL_INVALID_ARGUMENT;
	if (call->failed)
		return PCM_CALL_FAILED;
	if (call->stage != PCM_CALL_DATA_PATH && call->stage != PCM_CALL_CLOSED)
		return PCM_CALL_INVALID_STATE;
	return data_octet_owned(
		call, received_octet, received_sample, sources,
		transmit_octet, event);
}

enum pcm_call_result pcm_call_octet(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	uint8_t *transmit_octet,
	struct pcm_call_observation *observation)
{
	struct pcm_call_observation emitted = { 0 };
	bool deadline_started;
	uint8_t generated;
	enum pcm_call_result result;

	if (!pcm_call_owned_state_is_valid(call) || transmit_octet == NULL ||
	    observation == NULL ||
	    outputs_overlap(call, transmit_octet, observation))
		return PCM_CALL_INVALID_ARGUMENT;
	if (call->failed)
		return PCM_CALL_FAILED;
	if (call->stage == PCM_CALL_DATA_PATH)
		return PCM_CALL_INVALID_STATE;
	if (call->stage == PCM_CALL_PHASE2 &&
	    pcm_phase2_is_ready_for_v34_fallback(&call->state.phase2))
		return PCM_CALL_V34_FALLBACK_READY;
	if (call->stage == PCM_CALL_PHASE4 && pcm_call_phase4_is_ready(call))
		return PCM_CALL_PHASE4_READY;
	generated = encode_sample(call->law, 0);
	emitted.processed = call->stage;
	switch (call->stage) {
	case PCM_CALL_CLOSED:
		result = PCM_CALL_CLEARDOWN_COMPLETE;
		break;
	case PCM_CALL_PHASE2:
		result = sample_phase2(
			call,
			call->protocol == PCM_PROTOCOL_V90 &&
				call->local_role == V90_DIGITAL_MODEM ?
				received_sample :
				decode_octet(call->law, received_octet),
			&generated, &emitted);
		break;
	case PCM_CALL_PHASE3:
		result = sample_phase3(
			call, received_octet, received_sample, &generated);
		break;
	case PCM_CALL_PHASE3_MIXED:
		result = sample_mixed_phase3(
			call, received_octet, received_sample, &generated);
		break;
	case PCM_CALL_PHASE4:
		result = sample_phase4(
			call, received_octet, received_sample, &generated);
		break;
	case PCM_CALL_RETRAIN_SILENCE:
		result = sample_retrain_silence(
			call, received_sample, &generated);
		break;
	default:
		return PCM_CALL_NUMERIC_FAILURE;
	}
	result = recover_unusable_retrain_selection(call, result);
	if (result == PCM_CALL_OK && emitted.processed != call->stage &&
	    call->stage != PCM_CALL_RETRAIN_SILENCE)
		result = reset_retrain_detector(call);
	else if (result == PCM_CALL_OK && emitted.processed == call->stage &&
		 retrain_detection_is_active(call))
		result = observe_retrain_tone(call, received_sample);
	deadline_started = emitted.processed == PCM_CALL_PHASE2 &&
		is_v90_info1a_boundary(call, &emitted.phase2);
	if (result == PCM_CALL_OK && !deadline_started)
		result = advance_b1_deadline(call);
	if (result != PCM_CALL_OK &&
	    result != PCM_CALL_CLEARDOWN_COMPLETE &&
	    result != PCM_CALL_V34_FALLBACK_READY)
		call->failed = true;
	emitted.next = call->stage;
	if (!pcm_call_owned_state_is_valid(call)) {
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
	case PCM_CALL_CLEARDOWN_COMPLETE:
		return "cleardown complete";
	case PCM_CALL_V34_FALLBACK_READY:
		return "V.34 fallback ready";
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
