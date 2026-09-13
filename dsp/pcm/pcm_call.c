// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_call.h"

#include "memory_region.h"
#include "packed_bits.h"
#include "pcm_phase3_entry.h"
#include "pcm_phase3_handoff.h"
#include "v90_profile.h"
#include "v92_data_quality.h"
#include "v34_phase2_selection.h"
#include "v92_cpd_direct.h"
#include "v92_cpd_channel.h"
#include "v92_pcm_dil.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

enum {
	V90_NETWORK_CHARACTERS_PER_SECOND = 8000,
	V90_UPSTREAM_RATE_UNIT = 2400,
	PCM_PHASE3_RETURN_BASE_DEADLINE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 51 / 10,
	PCM_PHASE3_SD_DEADLINE_SAMPLES = V34_PHASE2_SAMPLE_RATE * 3 / 2,
	V92_PHASE3_JA_BASE_DEADLINE_SAMPLES = V34_PHASE2_SAMPLE_RATE * 9 / 2,
	V92_PHASE3_JD_DEADLINE_SAMPLES = V34_PHASE2_SAMPLE_RATE * 9 / 2,
	V90_B1_BASE_DEADLINE_SAMPLES = V34_PHASE2_SAMPLE_RATE * 15,
	V90_B1_ROUND_TRIP_ALLOWANCES = 5,
	V92_B1_BASE_DEADLINE_SAMPLES = V34_PHASE2_SAMPLE_RATE * 20,
	V92_B1_ROUND_TRIP_ALLOWANCES = 6,
	V90_RATE_RENEGOTIATION_BASE_DEADLINE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 5,
	V90_RATE_RENEGOTIATION_ROUND_TRIP_ALLOWANCES = 2,
	V92_CPU_REPEAT_BASE_SAMPLES = 800,
	/* A peer's full retrain looks like bad PCM data before its silence and
	 * tone qualify. Allow the longest legal silence plus the detector's tone
	 * and onset guards. This also covers Ru acquisition. Freeze one failure
	 * for this bounded interval; further bad samples cannot restart it. */
	V92_CONTROL_QUALITY_GUARD_SAMPLES = V34_PHASE2_SAMPLE_RATE * 75 / 1000 +
		V34_RETRAIN_TONE_SAMPLES + V34_RETRAIN_EDGE_SAMPLES,
};

_Static_assert(V92_CONTROL_QUALITY_GUARD_SAMPLES >= V92_RU_RX_MIN_SYMBOLS +
	V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY + V92_RU_RX_FRAME_SYMBOLS,
	"quality recovery must allow a complete Ru qualification");

_Static_assert(PCM_PROTOCOL_V90 == 0,
	"zero-initialized PCM call configurations must select V.90");

static bool uses_pcm_upstream(const struct pcm_call *call)
{
	return call->phase3_entry_stored &&
	       call->phase3_entry.selection.upstream == PCM_UPSTREAM_PCM;
}

bool pcm_call_uses_conventional_upstream(const struct pcm_call *call)
{
	return call != NULL && call->phase3_entry_stored &&
	       call->phase3_entry.selection.upstream == PCM_UPSTREAM_V34;
}

static bool pcm_upstream_is_enabled(
	enum v90_modem_role role, const union pcm_call_local_info1 *info1)
{
	return role == V90_ANALOG_MODEM ? info1->pcm_upstream_enabled :
		info1->v92_digital.pcm_upstream_supported;
}

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
	       (!phase4->generate_constellations ||
		(phase4->training_parameters.constellation_count == 1u &&
		 !phase4->training_parameters.codec_constellations_differ &&
		 phase4->data_parameters.constellation_count == 1u &&
		 !phase4->data_parameters.codec_constellations_differ)) &&
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
	const struct v34_info1c *info1,
	const union pcm_call_phase3_configuration *phase3,
	const struct pcm_call_digital_phase4_configuration *phase4)
{
	const struct v90_mp_record *mp = &phase4->initial_mp;

	if (!v90_mp_record_is_valid(mp))
		return false;
	return v34_info1c_is_valid(info1) &&
	       v90_jd_parameters_are_valid(&phase3->digital.jd) &&
	       !v90_mp_record_acknowledges_cp(mp);
}

static bool v92_analog_configuration_is_valid(
	const union pcm_call_local_info1 *info1,
	const union pcm_call_phase3_configuration *phase3,
	const struct pcm_call_analog_phase4_configuration *phase4,
	uint32_t enabled_downstream_rate_mask, enum g711_law law)
{
	const struct v92_phase3_analog_station_configuration *selected =
		&phase3->v92_analog;

	if (!analog_configuration_is_valid(info1, phase3, phase4, law) ||
	    !downstream_rate_is_enabled(enabled_downstream_rate_mask,
		    phase4->data_parameters.selected_rate_index) ||
	    !cp_supports_rate_mask(&phase4->data_parameters,
		    enabled_downstream_rate_mask))
		return false;
	if (!info1->pcm_upstream_enabled)
		return true;
	return selected->law == law &&
	       (!phase4->generate_constellations ||
		(selected->cpt.constellation_count == 1u &&
		 !selected->cpt.codec_constellations_differ)) &&
	       selected->local_info1a.local_md_length_276_symbol_units == 0u &&
	       v92_pcm_info1a_equal(
		       &info1->v92_analog, &selected->local_info1a) &&
	       v92_phase3_analog_station_configuration_is_valid(selected);
}

static bool v92_digital_configuration_is_valid(
	const union pcm_call_local_info1 *info1,
	const union pcm_call_phase3_configuration *phase3,
	const struct pcm_call_digital_phase4_configuration *phase4,
	uint32_t enabled_downstream_rate_mask, enum g711_law law)
{
	const struct pcm_call_v92_digital_phase3_configuration *selected =
		&phase3->v92_digital;

	if (!v92_info1d_is_valid(&info1->v92_digital) ||
	    !digital_configuration_is_valid(
		    &info1->v92_digital.v34, phase3, phase4) ||
	    phase3->digital.jd.data_rate_capability_mask != enabled_downstream_rate_mask)
		return false;
	if (!info1->v92_digital.pcm_upstream_supported)
		return true;
	return selected->jd.data_rate_capability_mask ==
		       enabled_downstream_rate_mask &&
	       v92_phase3_digital_station_parameters_are_valid(
		       law, V90_INFO1A_MIN_TRAINING_UCODE,
		       selected->trn1d_symbols ? selected->trn1d_symbols :
			       V90_PHASE3_TRN1D_MIN_SYMBOLS,
		       selected->equalizer_adaptation,
		       selected->su_minimum_magnitude,
		       &selected->jd, &selected->jp);
}

static bool v92_analog_phase4_configuration_is_valid(
	const struct pcm_call_v92_analog_phase4_configuration *configuration,
	uint32_t enabled_downstream_rate_mask, enum g711_law law, bool generated)
{
	return v92_suvu_parameters_are_valid(&configuration->suvu) &&
	       (!generated || (configuration->cpu.constellation_count == 1u &&
			       !configuration->cpu.codec_constellations_differ)) &&
	       !configuration->suvu.acknowledges_cpd &&
	       v92_cpu_parameters_are_valid(&configuration->cpu) &&
	       configuration->cpu.law == law &&
	       !configuration->cpu.acknowledges_cpd &&
	       downstream_rate_is_enabled(
		       enabled_downstream_rate_mask,
		       configuration->cpu.data_rate_number);
}

static bool v92_digital_phase4_configuration_is_valid(
	const struct pcm_call_v92_digital_phase4_configuration *configuration,
	enum g711_law law)
{
	struct v92_pcm_mapping mapping;

	if (!v92_suvd_parameters_are_valid(&configuration->suvd) ||
	    configuration->suvd.acknowledges_cpu)
		return false;
	switch (configuration->upstream_selection) {
	case PCM_CALL_V92_UPSTREAM_CONFIGURED:
		return !configuration->cpd.acknowledges_cpu &&
		       v92_pcm_mapping_init(&mapping, law, &configuration->cpd) ==
			       V92_PCM_MAPPING_OK;
	case PCM_CALL_V92_UPSTREAM_DIRECT:
		return configuration->direct_maximum_data_rate_number >=
			       V92_PCM_DIL_MIN_DATA_RATE_NUMBER &&
		       configuration->direct_maximum_data_rate_number <=
			       V92_PCM_DIL_MAX_DATA_RATE_NUMBER;
	}
	return false;
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
				       &configuration->analog_phase4,
				       configuration->enabled_downstream_rate_mask,
				       configuration->law) &&
			       (!configuration->local_info1.pcm_upstream_enabled ||
				v92_analog_phase4_configuration_is_valid(
					&configuration->v92_analog_phase4,
					configuration->enabled_downstream_rate_mask,
					configuration->law,
					configuration->analog_phase4.generate_constellations));
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
			       &configuration->digital_phase4,
			       configuration->enabled_downstream_rate_mask,
			       configuration->law) &&
		       (!configuration->local_info1.v92_digital.pcm_upstream_supported ||
			v92_digital_phase4_configuration_is_valid(
				&configuration->v92_digital_phase4, configuration->law));
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
		       &configuration->local_info1.digital,
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
			       &call->analog_phase4_configuration,
			       call->enabled_downstream_rate_mask,
			       call->law) &&
		       (!call->local_info1.pcm_upstream_enabled ||
			v92_analog_phase4_configuration_is_valid(
				&call->v92_analog_phase4_configuration,
				call->enabled_downstream_rate_mask,
				call->law,
				call->analog_phase4_configuration.generate_constellations)) :
		       v92_digital_configuration_is_valid(
			       &call->local_info1,
			       &call->phase3_configuration,
			       &call->digital_phase4_configuration,
			       call->enabled_downstream_rate_mask,
			       call->law) &&
		       (!call->local_info1.v92_digital.pcm_upstream_supported ||
			v92_digital_phase4_configuration_is_valid(
				&call->v92_digital_phase4_configuration, call->law));
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
			       &call->local_info1.digital,
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
	    mixed->transmit.stage != PCM_DOWNSTREAM_TX_TRN2D ||
	    mixed->transmit.control_configured ||
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
	const struct v92_upstream_control *control = &state->station.transmit.control;
	bool timer_valid;

	if (state->station.cleardown.active)
		return v92_phase4_analog_station_is_valid(&state->station);
	if (state->observed_cpu_records !=
	    state->station.transmit.control.completed_cpu_records)
		return false;
	if (control->peer_confirmation_received || state->observed_cpu_records == 0u ||
	    control->retry != V92_UPSTREAM_RETRY_IDLE)
		timer_valid = state->cpu_repeat_remaining_samples == 0u;
	else
		timer_valid = state->cpu_repeat_remaining_samples != 0u &&
			state->cpu_repeat_remaining_samples <= expected_interval;

	return v92_phase4_analog_station_is_valid(&state->station) &&
	       v92_constellation_size_is_valid(call->state.phase4.v92.rate_renegotiation_constellation) &&
	       state->cpu_repeat_interval_samples == expected_interval &&
	       timer_valid;
}

static bool cleardown_is_active_owned(const struct pcm_call *call)
{
	if (uses_pcm_upstream(call) && call->stage == PCM_CALL_PHASE4 &&
	    call->state.phase4.v92.child == PCM_CALL_V92_PHASE4_CONTROL)
		return call->local_role == V90_ANALOG_MODEM ?
			call->state.phase4.v92.owner.analog.station.cleardown.active :
			call->state.phase4.v92.owner.digital.control.cleardown.active;
	return pcm_call_uses_conventional_upstream(call) &&
	       call->stage == PCM_CALL_DATA_PATH &&
	       (call->local_role == V90_ANALOG_MODEM ?
			call->state.data_path.analog.cleardown_requested :
			call->state.data_path.digital.cleardown_requested);
}

static bool v92_fast_is_active_owned(const struct pcm_call *call)
{
	return uses_pcm_upstream(call) && call->stage == PCM_CALL_DATA_PATH &&
		(call->local_role == V90_ANALOG_MODEM ?
		 call->state.data_path.v92_analog.fast.deadline_remaining != 0u :
		 call->state.data_path.v92_digital.fast.stage != PCM_CALL_V92_FAST_IDLE);
}

bool pcm_call_is_disconnecting(const struct pcm_call *call)
{
	return call != NULL && (call->stage == PCM_CALL_CLOSED ||
		(uses_pcm_upstream(call) && call->stage == PCM_CALL_PHASE4 &&
		 call->state.phase4.v92.cleardown_requested) ||
		cleardown_is_active_owned(call) || (v92_fast_is_active_owned(call) &&
		 (call->local_role == V90_ANALOG_MODEM ? call->state.data_path.v92_analog.fast.cleardown.active :
		  call->state.data_path.v92_digital.fast.control.cleardown.active)));
}

static bool rate_renegotiation_is_active_owned(
	const struct pcm_call *call)
{
	if (call->v92_rate_renegotiation_active)
		return !cleardown_is_active_owned(call);
	if (!pcm_call_uses_conventional_upstream(call) ||
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
	if (call->v92_rate_renegotiation_active)
		return true;
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
			path->upstream.startup.stage == V90_UPSTREAM_RX_STAGE_CPS ||
			path->upstream.startup.stage == V90_UPSTREAM_RX_STAGE_RECONDITION ||
			path->upstream.startup.stage == V90_UPSTREAM_RX_STAGE_E);
	}
}

static bool rate_renegotiation_deadline_has_started(
	const struct pcm_call *call)
{
	if (!rate_renegotiation_response_is_pending_owned(call))
		return false;
	if (call->v92_rate_renegotiation_active)
		return true;
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
			path->downstream.rd.transmitted_symbols >= PCM_R_SYMBOLS);
	}
}

static uint32_t b1_deadline_limit(const struct pcm_call *call)
{
	bool pcm_upstream = uses_pcm_upstream(call);

	/* PCM INFO1a finishes before its Phase 3 handoff. Start timing at the
	 * record boundary, while the INFO1 owner still holds that selection. */
	if (call->stage == PCM_CALL_PHASE2 &&
	    call->state.phase2.stage == PCM_PHASE2_INFO1 &&
	    call->state.phase2.state.info1.analog_record_stored)
		pcm_upstream = call->state.phase2.state.info1.analog.kind == PCM_INFO1A_PCM;
	/* V.90 9.2 and V.92 9.6 time peer B1 from the end of INFO1a. */
	return pcm_upstream ?
		V92_B1_BASE_DEADLINE_SAMPLES +
			V92_B1_ROUND_TRIP_ALLOWANCES * call->round_trip_delay_samples :
		V90_B1_BASE_DEADLINE_SAMPLES +
			V90_B1_ROUND_TRIP_ALLOWANCES * call->round_trip_delay_samples;
}

static bool v92_fast_is_valid(const struct pcm_call *call)
{
	const struct pcm_call_v92_digital_data *path = &call->state.data_path.v92_digital;
	const struct pcm_call_v92_fast_exchange *fast = &path->fast;
	if (fast->stage == PCM_CALL_V92_FAST_IDLE)
		return !fast->deadline_remaining && !fast->b1_frame && !fast->rm_complete && !fast->b1_started;
	return fast->stage >= PCM_CALL_V92_FAST_WAIT_PRIME && fast->stage <= PCM_CALL_V92_FAST_FINISH &&
		call->retrain_active && !call->v92_rate_renegotiation_active &&
		fast->deadline_remaining && fast->deadline_remaining <= b1_deadline_limit(call) &&
		v92_downstream_control_is_valid(&fast->control) &&
		(!fast->control.failed || call->failed) &&
		v92_control_rx_is_valid(&fast->receive) && fast->receive.direction == V92_CONTROL_UPSTREAM &&
		fast->receive.alignment_bits == path->upstream.decoder.mapping.data_bits_per_frame &&
		pcm_r_tx_is_valid(&fast->rf) && fast->rf.signal == PCM_R_RF &&
		fast->control.transmit.encoder.mapping.law == call->law &&
		v90_pcm_mapping_equal(&fast->rf.mapping, &fast->control.transmit.encoder.mapping) &&
		(fast->stage >= PCM_CALL_V92_FAST_CONTROL ? pcm_r_tx_is_complete(&fast->rf) :
		 fast->stage == PCM_CALL_V92_FAST_RF ? !pcm_r_tx_is_complete(&fast->rf) : !fast->rf.transmitted_symbols) &&
		(fast->stage != PCM_CALL_V92_FAST_WAIT_PRIME || !fast->rm_complete) &&
		(!fast->b1_started || fast->b1_frame) && (!fast->b1_frame || fast->rm_complete) &&
		(!fast->b1_frame || fast->b1_started || fast->b1_frame >= path->upstream.line.received_frames) &&
		(fast->stage == PCM_CALL_V92_FAST_FINISH || path->downstream_stage == V92_PHASE4_E2U_DOWNSTREAM_DATA);
}

static bool v92_data_path_is_valid(const struct pcm_call *call)
{
	struct v90_pcm_mapping expected;
	const struct v92_cpu_parameters *cpu;
	const struct v92_pcm_mapping *upstream;
	enum v92_phase4_e2u_downstream_stage stage;
	bool child_valid;

	if (call->local_role == V90_ANALOG_MODEM) {
		const struct pcm_call_v92_analog_data *path =
			&call->state.data_path.v92_analog;
		const struct pcm_call_v92_analog_fast *fast = &path->fast;
		if (!fast->deadline_remaining && (fast->record_bits || fast->record_index || fast->repeat_remaining ||
		    fast->transmitted_symbols || fast->rm_complete || fast->finish_started || fast->b1_started ||
		    fast->zero_record || fast->zero_ack || fast->cleardown.active || fast->mapping.data_bits_per_frame ||
		    path->upstream.rm_symbols_remaining || path->upstream.finish_symbols_remaining))
			return false;
		if (fast->deadline_remaining &&
		    (!call->retrain_active || call->v92_rate_renegotiation_active ||
		     fast->deadline_remaining > b1_deadline_limit(call) ||
		     !v92_upstream_control_is_valid(&fast->control) || fast->control.suvu.request_silence ||
		     !v92_pcm_mapping_is_valid(&fast->mapping) ||
		     fast->mapping.law != call->law || !v92_cleardown_is_valid(&fast->cleardown) ||
		     (fast->rm_complete != (path->upstream.rm_symbols_remaining == 0u)) ||
		     (fast->control.received_cpd && fast->mapping.cpd.extend_e2u) ||
		     (fast->zero_record && !fast->cleardown.active) ||
		     fast->record_bits > sizeof(fast->record) * 8u || fast->record_index > fast->record_bits ||
		     fast->repeat_remaining > (uint32_t)V92_CPU_REPEAT_BASE_SAMPLES + call->round_trip_delay_samples ||
		     (fast->finish_started && !fast->rm_complete) || (fast->b1_started && !fast->finish_started)))
			return false;

		cpu = &path->cpu;
		upstream = &path->upstream.encoder.mapping;
		stage = path->downstream_stage;
		if ((stage != V92_PHASE4_E2U_DOWNSTREAM_TRAINING &&
		     stage != V92_PHASE4_E2U_DOWNSTREAM_DATA) ||
		    !v92_upstream_data_tx_is_valid(&path->upstream) ||
		    !pcm_r_rx_is_valid(&path->rd) || path->rd.signal != PCM_R_RD ||
		    !pcm_r_rx_is_valid(&path->rf) || path->rf.signal != PCM_R_RF ||
		    path->rf.decision_plane != V90_PCM_MAPPING_TRANSMIT ||
		    !v90_pcm_mapping_equal(&path->rd.mapping, &path->rf.mapping) ||
		    path->rd.decision_plane != V90_PCM_MAPPING_TRANSMIT ||
		    !v90_pcm_mapping_is_valid(&path->training_mapping) ||
		    path->training_mapping.law != call->law ||
		    !v92_constellation_size_is_valid(path->rate_renegotiation_constellation) ||
		    v90_pcm_mapping_init_from_v92_cpu(&expected, cpu) !=
			    V90_PCM_MAPPING_OK || !v90_pcm_mapping_equal(&expected, &path->rd.mapping))
			return false;
		if (stage == V92_PHASE4_E2U_DOWNSTREAM_TRAINING)
			child_valid = pcm_downstream_rx_is_valid(
					&path->downstream.training) &&
				(call->failed || !pcm_downstream_rx_has_failed(
					&path->downstream.training)) &&
				path->downstream.training.protocol == PCM_PROTOCOL_V92 &&
				(path->downstream.training.ed_requested || fast->deadline_remaining) &&
				path->downstream.training.stream.decoder.mapping.law == call->law;
		else
			child_valid = v90_downstream_data_rx_is_valid(
					&path->downstream.data) &&
				v90_pcm_mapping_equal(&expected,
					&path->downstream.data.stream.decoder.mapping);
	} else {
		const struct pcm_call_v92_digital_data *path =
			&call->state.data_path.v92_digital;

		cpu = &path->cpu;
		upstream = &path->upstream.decoder.mapping;
		stage = path->downstream_stage;
		if ((stage != V92_PHASE4_E2U_DOWNSTREAM_TRAINING &&
		     stage != V92_PHASE4_E2U_DOWNSTREAM_DATA) ||
		    !v92_upstream_data_rx_is_valid(&path->upstream) || !v92_fast_is_valid(call) ||
		    !v92_ru_line_rx_is_valid(&path->ru) ||
		    path->ru.constellation != call->phase3_configuration.v92_digital.jp.rate_renegotiation_constellation ||
		    !v90_pcm_mapping_is_valid(&path->training_mapping) ||
		    path->training_mapping.law != call->law ||
		    !isfinite(path->pending_squared_error) ||
		    path->pending_squared_error < 0.0 ||
		    path->quality_guard_samples > V92_CONTROL_QUALITY_GUARD_SAMPLES ||
		    (!path->quality_guard_samples &&
		     (path->pending_squared_error != 0.0 || path->pending_ambiguous ||
		      path->pending_uncertain)) ||
		    (path->upstream.failed && !call->failed &&
		     (!path->quality_guard_samples || !path->pending_ambiguous ||
		      call->b1_deadline_armed || path->fast.stage != PCM_CALL_V92_FAST_IDLE)) ||
		    !isfinite(path->b1u_squared_error) ||
		    path->b1u_squared_error < 0.0 ||
		    (path->upstream.line.received_frames <= V92_B1U_ACQUISITION_FRAMES &&
		     path->b1u_squared_error != 0.0) ||
		    v90_pcm_mapping_init_from_v92_cpu(&expected, cpu) !=
			    V90_PCM_MAPPING_OK)
			return false;
		if (stage == V92_PHASE4_E2U_DOWNSTREAM_TRAINING)
			child_valid = v92_phase4_ed_tx_is_valid(
					&path->downstream.training) &&
				path->downstream.training.transmit.encoder.mapping.law == call->law;
		else
			child_valid = v90_downstream_data_stream_is_valid(
					&path->downstream.data) &&
				v90_pcm_mapping_equal(&expected,
					&path->downstream.data.transmitter.encoder.mapping);
	}
	return cpu->law == call->law && upstream->law == call->law && child_valid;
}

static bool v92_rate_prefix_is_valid(const struct pcm_call *call)
{
	const struct pcm_call_v92_phase4_state *state = &call->state.phase4.v92;

	if (state->prefix_queue_count >= V90_CP_FRAME_INTERVALS ||
	    (state->prefix_queue_count && (state->prefix_stage != PCM_CALL_V92_PREFIX_RD ||
		state->prefix.rd.transmitted_symbols)))
		return false;
	for (unsigned i = state->prefix_queue_count; i < V90_CP_FRAME_INTERVALS; ++i)
		if (state->prefix_queue[i])
			return false;
	if (state->cleardown_requested && !call->v92_rate_renegotiation_active)
		return false;
	if (state->peer_requested_silence && (!call->v92_rate_renegotiation_active ||
	    (state->child != PCM_CALL_V92_PHASE4_E2U && state->child != PCM_CALL_V92_PHASE4_PAUSE)))
		return false;
	if (call->v92_rate_renegotiation_active &&
	    (!v90_pcm_mapping_is_valid(&state->training_mapping) || state->training_mapping.law != call->law))
		return false;
	if (state->prefix_stage == PCM_CALL_V92_PREFIX_NONE)
		return true;
	if (state->prefix_stage == PCM_CALL_V92_PREFIX_RU) {
		const struct pcm_call_v92_analog_rate_prefix *prefix = &state->prefix.analog;
		if (!call->v92_rate_renegotiation_active || call->local_role != V90_ANALOG_MODEM ||
		    state->child != PCM_CALL_V92_PHASE4_CONTROL ||
		    !state->owner.analog.station.transmit.control.renegotiation ||
		    !v92_pcm_phase3_ru_is_valid(&prefix->transmit) ||
		    prefix->transmit.law != call->law ||
		    !pcm_r_rx_is_valid(&prefix->receive) || prefix->receive.mapping.law != call->law ||
		    prefix->receive.signal != PCM_R_RD ||
		    prefix->receive.decision_plane != V90_PCM_MAPPING_TRANSMIT ||
		    prefix->queue_count >= V92_PCM_MAPPING_FRAME_SYMBOLS ||
		    (prefix->queue_count && (prefix->transmit.emitted_symbols ||
			prefix->transmit.kind != V92_PCM_PHASE3_RU)))
			return false;
		for (unsigned i = prefix->queue_count; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
			if (prefix->queue[i])
				return false;
		return true;
	}
	if (!call->v92_rate_renegotiation_active || call->local_role != V90_DIGITAL_MODEM ||
	    (state->child != PCM_CALL_V92_PHASE4_RU && state->child != PCM_CALL_V92_PHASE4_CONTROL))
		return false;
	switch (state->prefix_stage) {
	case PCM_CALL_V92_PREFIX_DATA:
		if (state->child != PCM_CALL_V92_PHASE4_RU)
			return false;
		/* fall through */
	case PCM_CALL_V92_PREFIX_DRAIN:
		return v90_downstream_data_stream_is_valid(&state->prefix.data) &&
			state->prefix.data.transmitter.encoder.mapping.law == call->law;
	case PCM_CALL_V92_PREFIX_RD:
		return pcm_r_tx_is_valid(&state->prefix.rd) &&
			state->prefix.rd.mapping.law == call->law && state->prefix.rd.signal == PCM_R_RD;
	default:
		return false;
	}
}

static bool call_is_valid(
	const struct pcm_call *call, bool owned_phase3)
{
	bool child_valid;
	bool child_failed;

	if (call == NULL || !stored_configuration_is_valid(call) ||
	    !pcm_downstream_equalizer_is_valid(&call->downstream_equalizer) ||
	    (call->downstream_equalizer.training_level &&
	     (call->local_role != V90_ANALOG_MODEM || call->downstream_equalizer.law != call->law ||
	      !uses_pcm_upstream(call))) ||
	    !v92_data_quality_is_valid(&call->pcm_upstream_quality) ||
	    (call->v92_rate_renegotiation_active &&
	     (!uses_pcm_upstream(call) ||
	      !call->retrain_active ||
	      (call->stage != PCM_CALL_PHASE4 && call->stage != PCM_CALL_DATA_PATH))) ||
	    (call->v92_upstream_retry_ceiling != 0u &&
	     (call->protocol != PCM_PROTOCOL_V92 || call->local_role != V90_DIGITAL_MODEM ||
	      call->v92_digital_phase4_configuration.upstream_selection != PCM_CALL_V92_UPSTREAM_DIRECT ||
	      call->v92_upstream_retry_ceiling >=
		      call->v92_digital_phase4_configuration.direct_maximum_data_rate_number)) ||
	    (call->stage == PCM_CALL_CLOSED &&
	     (call->failed || call->retrain_active || call->b1_deadline_armed ||
	      call->rate_renegotiation_deadline_armed)) ||
	    (!call->v92_peer_jd_stored &&
	     (call->v92_peer_jd.data_rate_capability_mask != 0u ||
	      call->v92_peer_jd.maximum_spectral_shaping_lookahead != 0u)) ||
	    (call->v92_peer_jd_stored &&
	     (!uses_pcm_upstream(call) ||
	      call->local_role != V90_ANALOG_MODEM ||
	      !v92_jd_is_valid(&call->v92_peer_jd))) ||
	    (uses_pcm_upstream(call) &&
	     call->local_role == V90_ANALOG_MODEM &&
	     call->stage != PCM_CALL_PHASE2 &&
	     call->stage != PCM_CALL_PHASE3 &&
	     call->stage != PCM_CALL_RETRAIN_SILENCE &&
	     !call->v92_peer_jd_stored) ||
	    (!call->round_trip_delay_stored &&
	     call->round_trip_delay_samples != 0u) ||
	    (!call->b1_deadline_armed &&
	     call->b1_deadline_remaining_samples != 0u) ||
	    (call->b1_deadline_armed &&
	     (call->stage == PCM_CALL_RETRAIN_SILENCE ||
	      !call->peer_info0_stored || !call->round_trip_delay_stored ||
	      call->b1_deadline_remaining_samples > b1_deadline_limit(call))) ||
	    (!call->rate_renegotiation_deadline_armed &&
	     call->rate_renegotiation_deadline_remaining_samples != 0u) ||
	    (call->rate_renegotiation_deadline_armed &&
	     ((!call->v92_rate_renegotiation_active &&
	       (!pcm_call_uses_conventional_upstream(call) ||
	        call->stage != PCM_CALL_DATA_PATH)) ||
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
		if (uses_pcm_upstream(call) &&
		    call->local_role == V90_ANALOG_MODEM) {
			child_valid = v92_phase3_analog_station_is_valid(
				&call->state.phase3.v92_analog);
			child_failed = v92_phase3_analog_station_has_failed(
				&call->state.phase3.v92_analog);
		} else if (uses_pcm_upstream(call)) {
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
		if (uses_pcm_upstream(call)) {
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
		if (uses_pcm_upstream(call) && !v92_rate_prefix_is_valid(call))
			return false;
		if (uses_pcm_upstream(call) &&
		    call->state.phase4.v92.child == PCM_CALL_V92_PHASE4_RU) {
			const struct pcm_call_v92_rate_request *ru = &call->state.phase4.v92.owner.ru;

			child_valid = call->v92_rate_renegotiation_active &&
				call->local_role == V90_DIGITAL_MODEM &&
				v92_ru_line_rx_is_valid(&ru->receive) &&
				ru->receive.law == call->law &&
				(call->state.phase4.v92.prefix_stage != PCM_CALL_V92_PREFIX_DATA ||
				 ru->receive.signal.run >= V92_RU_RX_MIN_SYMBOLS) &&
				pcm_downstream_tx_is_valid(&ru->transmit) &&
				ru->transmit.protocol == PCM_PROTOCOL_V92 &&
				ru->transmit.encoder.mapping.law == call->law &&
				ru->transmit.v92_control_kind == PCM_DOWNSTREAM_V92_CONTROL_SUVD &&
				!ru->transmit.control.suvd.acknowledges_cpu &&
				!ru->transmit.finish_requested && !ru->transmit.ed_requested &&
				v92_upstream_plan_is_valid(&ru->plan) && ru->cpu.law == call->law &&
				v92_cpd_parameters_are_valid(&ru->cpd) &&
				v92_cpu_parameters_are_valid(&ru->cpu);
			child_failed = false;
		} else if (uses_pcm_upstream(call) &&
		    call->state.phase4.v92.child == PCM_CALL_V92_PHASE4_PAUSE) {
			const union pcm_call_v92_phase4_owner *owner = &call->state.phase4.v92.owner;
			if (!call->v92_rate_renegotiation_active ||
			    call->state.phase4.v92.prefix_stage != PCM_CALL_V92_PREFIX_NONE)
				return false;
			if (call->local_role == V90_ANALOG_MODEM) {
				child_valid = v92_rate_pause_analog_is_valid(&owner->analog_pause.signal) &&
					owner->analog_pause.signal.cpu.law == call->law &&
					owner->analog_pause.signal.cpu.data_rate_number != 0u &&
					owner->analog_pause.signal.peer_requested == call->state.phase4.v92.peer_requested_silence &&
					v92_pcm_mapping_is_valid(&owner->analog_pause.mapping) &&
					owner->analog_pause.mapping.law == call->law;
				child_failed = owner->analog_pause.signal.failed;
			} else {
				child_valid = v92_rate_pause_digital_is_valid(&owner->digital_pause.signal) &&
					owner->digital_pause.signal.upstream.signal.line.law == call->law &&
					owner->digital_pause.signal.peer_requested == call->state.phase4.v92.peer_requested_silence &&
					v92_pcm_mapping_is_valid(&owner->digital_pause.mapping) &&
					owner->digital_pause.mapping.law == call->law &&
					v92_cpu_parameters_are_valid(&owner->digital_pause.cpu) &&
					owner->digital_pause.cpu.law == call->law &&
					owner->digital_pause.cpu.data_rate_number != 0u &&
					v92_upstream_plan_is_valid(&owner->digital_pause.plan);
				child_failed = owner->digital_pause.signal.failed;
			}
		} else if (uses_pcm_upstream(call) &&
		    call->state.phase4.v92.child ==
			    PCM_CALL_V92_PHASE4_E2U) {
			child_valid = v92_phase4_e2u_is_valid(
				&call->state.phase4.v92.owner.e2u) &&
				call->state.phase4.v92.owner.e2u.local_role ==
					call->local_role;
			child_failed = v92_phase4_e2u_has_failed(
				&call->state.phase4.v92.owner.e2u);
		} else if (uses_pcm_upstream(call) &&
			   call->state.phase4.v92.child ==
				   PCM_CALL_V92_PHASE4_CONTROL &&
			   call->local_role == V90_ANALOG_MODEM) {
			child_valid = v92_analog_phase4_state_is_valid(call);
			child_failed = v92_phase4_analog_station_has_failed(
				&call->state.phase4.v92.owner.analog.station);
		} else if (uses_pcm_upstream(call) &&
			   call->state.phase4.v92.child ==
				   PCM_CALL_V92_PHASE4_CONTROL) {
			child_valid = v92_phase4_digital_station_is_valid(
				&call->state.phase4.v92.owner.digital);
			child_failed = v92_phase4_digital_station_has_failed(
				&call->state.phase4.v92.owner.digital);
		} else if (uses_pcm_upstream(call)) {
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
		if (uses_pcm_upstream(call)) {
			child_valid = call->stage == PCM_CALL_CLOSED ?
				memory_region_is_zero(&call->state, sizeof(call->state)) :
				v92_data_path_is_valid(call);
			child_failed = false;
			break;
		}
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
	/* Keep the complete role-local proposal and policy together. */
	initialized.local_info1 = configuration->local_info1;
	if (configuration->protocol == PCM_PROTOCOL_V92 &&
	    configuration->local_role == V90_ANALOG_MODEM) {
		initialized.phase3_configuration.analog = configuration->phase3.analog;
		initialized.phase3_configuration.v92_analog =
			configuration->phase3.v92_analog;
		initialized.v92_analog_phase4_configuration =
			configuration->v92_analog_phase4;
		initialized.analog_phase4_configuration = configuration->analog_phase4;
	} else if (configuration->protocol == PCM_PROTOCOL_V92) {
		initialized.phase3_configuration.digital = configuration->phase3.digital;
		initialized.phase3_configuration.v92_digital =
			configuration->phase3.v92_digital;
		initialized.v92_digital_phase4_configuration =
			configuration->v92_digital_phase4;
		initialized.digital_phase4_configuration = configuration->digital_phase4;
	} else if (configuration->local_role == V90_ANALOG_MODEM) {
		initialized.phase3_configuration.analog =
			configuration->phase3.analog;
		initialized.analog_phase4_configuration =
			configuration->analog_phase4;
	} else {
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
	next.downstream_equalizer = (struct pcm_downstream_equalizer){ 0 };
	next.phase3_entry = (struct pcm_phase3_entry){ 0 };
	next.v92_peer_jd = (struct v92_jd){ 0 };
	next.b1_deadline_remaining_samples = 0u;
	next.rate_renegotiation_deadline_remaining_samples = 0u;
	next.stage = PCM_CALL_RETRAIN_SILENCE;
	next.phase3_entry_stored = false;
	next.v92_peer_jd_stored = false;
	next.b1_deadline_armed = false;
	next.rate_renegotiation_deadline_armed = false;
	next.v92_rate_renegotiation_active = false;
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
	    !call->retrain_active || call->stage != PCM_CALL_PHASE2)
		return result;
	/* A peer can answer INFO1 with a symbol rate for which its INFO1d offered
	 * no usable data rate.  Refuse that profile, but keep the established
	 * call agreement and request another full retrain. V.92 9.3 shares
	 * V.90's full Phase 2 recovery procedures without dropping V.92. */
	return begin_retrain(call, false);
}

static bool is_info1a_boundary(
	const struct pcm_call *call, const struct pcm_phase2_event *event)
{
	return call->local_role == V90_ANALOG_MODEM ?
		event->kind == PCM_PHASE2_EVENT_LOCAL_INFO1A_SENT :
		event->kind == PCM_PHASE2_EVENT_PEER_INFO1A_RECEIVED;
}

static enum pcm_call_result arm_b1_deadline(
	struct pcm_call *call, const struct pcm_phase2_event *event)
{
	if (!is_info1a_boundary(call, event))
		return PCM_CALL_OK;
	if (!call->round_trip_delay_stored)
		return PCM_CALL_SEQUENCE_MISMATCH;
	call->b1_deadline_remaining_samples = b1_deadline_limit(call);
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

static bool v92_peer_b1_received_owned(const struct pcm_call *call)
{
	if (call->local_role == V90_ANALOG_MODEM) {
		const struct pcm_call_v92_analog_data *path =
			&call->state.data_path.v92_analog;

		return (!path->fast.deadline_remaining || pcm_r_rx_is_complete(&path->rf)) &&
		       path->downstream_stage == V92_PHASE4_E2U_DOWNSTREAM_DATA &&
		       path->downstream.data.stage == V90_DOWNSTREAM_DATA_RX_STAGE_DATA;
	}
	const struct pcm_call_v92_digital_data *path =
		&call->state.data_path.v92_digital;

	/* Wait for all delayed B1u decisions, including the unconstrained tail,
	 * before reporting online. A small residual alone does not prove that
	 * a dense constellation can carry data on the measured channel. */
	return (path->fast.stage == PCM_CALL_V92_FAST_IDLE || path->fast.b1_started) &&
	       path->upstream.line.received_frames >= V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES &&
	       !path->b1u_decode_failed &&
	       path->b1u_squared_error <= (double)V92_B1U_VALIDATION_SYMBOLS *
		       V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR;
}

static bool v92_local_b1_sent_owned(const struct pcm_call *call)
{
	if (call->local_role == V90_ANALOG_MODEM) {
		const struct pcm_call_v92_analog_data *path = &call->state.data_path.v92_analog;
		return (!path->fast.deadline_remaining || path->fast.b1_started) &&
			path->upstream.b1u_symbols_remaining == 0u;
	}
	const struct pcm_call_v92_digital_data *path = &call->state.data_path.v92_digital;

	return (path->fast.stage == PCM_CALL_V92_FAST_IDLE || path->fast.stage == PCM_CALL_V92_FAST_FINISH) &&
	       path->downstream_stage == V92_PHASE4_E2U_DOWNSTREAM_DATA &&
	       path->downstream.data.transmitted_symbols >=
		       V90_PHASE4_B1D_FRAMES * V90_CP_FRAME_INTERVALS;
}

static bool data_path_is_online_owned(const struct pcm_call *call)
{
	if (call->failed || call->stage != PCM_CALL_DATA_PATH ||
	    cleardown_is_active_owned(call))
		return false;
	if (uses_pcm_upstream(call))
		return v92_peer_b1_received_owned(call) && v92_local_b1_sent_owned(call);
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
	if (uses_pcm_upstream(call))
		return v92_peer_b1_received_owned(call);
	return call->local_role == V90_ANALOG_MODEM ?
		analog_downstream_is_online_owned(
			&call->state.data_path.analog) :
		digital_upstream_is_online_owned(
			&call->state.data_path.digital);
}

static enum pcm_call_result retry_v92_upstream(struct pcm_call *call)
{
	uint8_t previous = call->v92_upstream_retry_ceiling;
	uint8_t rate = call->state.data_path.v92_digital.upstream.decoder.mapping.cpd.data_rate_number;
	enum pcm_call_result result;

	/* Local receiver policy, not a wire-protocol restriction. A failed
	 * automatic mapping should try a lower rate from the next peer Ja.
	 * Keep fixed mappings unchanged; at the lowest rate, online recovery
	 * still has its bounded quality retry allowance. Never raise a ceiling
	 * retained from an earlier failure. */
	if (call->v92_digital_phase4_configuration.upstream_selection == PCM_CALL_V92_UPSTREAM_DIRECT) {
		if (rate <= 1u && call->b1_deadline_armed)
			return PCM_CALL_UNUSABLE_SELECTION;
		if (rate > 1u && (previous == 0u || rate <= previous))
			call->v92_upstream_retry_ceiling = rate - 1u;
	}
	result = begin_retrain(call, false);
	if (result != PCM_CALL_OK)
		call->v92_upstream_retry_ceiling = previous;
	return result;
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
	if (call->stage == PCM_CALL_DATA_PATH && uses_pcm_upstream(call) &&
	    call->local_role == V90_DIGITAL_MODEM &&
	    call->v92_digital_phase4_configuration.upstream_selection == PCM_CALL_V92_UPSTREAM_DIRECT &&
	    call->state.data_path.v92_digital.upstream.line.received_frames >=
		    V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES) {
		/* V.92 9.6.1.2 allows full retrain during Phase 4. Once every B1u
		 * decision is available, do not repeat a selection that failed it.
		 * Ja in the next epoch still decides which lower rates are usable.
		 * Strictly decreasing ceilings bound recovery, even without a host
		 * training timeout. Fixed caller-owned mappings are never replaced. */
		return retry_v92_upstream(call);
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
	    pcm_call_is_disconnecting(call) ||
	    call->stage == PCM_CALL_PHASE2 ||
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
	if (uses_pcm_upstream(call))
		return call->state.phase4.v92.child ==
			       PCM_CALL_V92_PHASE4_E2U &&
		       v92_phase4_e2u_is_data_ready(
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
	struct pcm_fallback_restart *restart)
{
	struct v34_phase2_handoff selected;
	const struct v90_info0 *local_info0;
	const struct v90_info0 *peer_info0;
	int16_t receive_gain;

	if (!pcm_call_is_valid(call) || handoff == NULL || restart == NULL ||
	    memory_regions_overlap(call, sizeof(*call), handoff, sizeof(*handoff)) ||
	    memory_regions_overlap(call, sizeof(*call), restart, sizeof(*restart)) ||
	    memory_regions_overlap(
		    handoff, sizeof(*handoff), restart, sizeof(*restart)))
		return PCM_CALL_INVALID_ARGUMENT;
	if (call->failed || call->stage != PCM_CALL_PHASE2 ||
	    call->local_role != V90_DIGITAL_MODEM ||
	    !call->peer_info0_stored || !call->round_trip_delay_stored ||
	    !pcm_phase2_is_ready_for_v34_fallback(&call->state.phase2) ||
	    pcm_phase2_receive_gain(&call->state.phase2, &receive_gain) !=
		    PCM_PHASE2_OK)
		return PCM_CALL_INVALID_STATE;
	local_info0 = call->protocol == PCM_PROTOCOL_V92 ?
		&call->local_info0.v92_digital.v90.common : &call->local_info0.digital.common;
	peer_info0 = call->protocol == PCM_PROTOCOL_V92 ?
		&call->peer_info0.v92_analog.v90 : &call->peer_info0.analog;
	selected = (struct v34_phase2_handoff){
		.local_info0 = v34_info0_from_v90(local_info0),
		.peer_info0 = v34_info0_from_v90(peer_info0),
		.caller_info1c = call->protocol == PCM_PROTOCOL_V92 ?
			call->state.phase2.state.info1.digital.record.v92.v34 :
			call->state.phase2.state.info1.digital.record.v90,
		.answerer_info1a =
			call->state.phase2.state.info1.analog.record.v34_fallback,
		.round_trip_delay_samples = call->round_trip_delay_samples,
		.receive_gain = receive_gain,
		.local_role = V34_CALLING_STATION,
	};
	if (!v34_phase2_handoff_is_valid(&selected))
		return PCM_CALL_UNUSABLE_SELECTION;
	*handoff = selected;
	*restart = (struct pcm_fallback_restart){
		.protocol = call->protocol,
		.local_info0 = call->local_info0,
		.peer_info0 = call->peer_info0,
		.local_info1 = call->local_info1,
		.phase3 = call->phase3_configuration.digital,
		.v92_phase3 = call->phase3_configuration.v92_digital,
		.phase4 = call->digital_phase4_configuration,
		.v92_phase4 = call->v92_digital_phase4_configuration,
		.enabled_downstream_rate_mask = call->enabled_downstream_rate_mask,
		.upstream_retry_ceiling = call->v92_upstream_retry_ceiling,
		.round_trip_delay_samples = call->round_trip_delay_samples,
		.phase2_nominal_amplitude = call->phase2_nominal_amplitude,
		.law = call->law,
	};
	return PCM_CALL_OK;
}

enum pcm_call_result pcm_call_init_from_v34(
	struct pcm_call *call, const struct pcm_fallback_restart *restart,
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
		.protocol = restart->protocol,
		.local_role = V90_DIGITAL_MODEM,
		.law = restart->law,
		.enabled_downstream_rate_mask = restart->enabled_downstream_rate_mask,
		.phase2_nominal_amplitude = restart->phase2_nominal_amplitude,
		.local_info0 = restart->local_info0,
		.local_info1 = restart->local_info1,
		.phase3.digital = restart->phase3,
		.phase3.v92_digital = restart->v92_phase3,
		.digital_phase4 = restart->phase4,
		.v92_digital_phase4 = restart->v92_phase4,
	};
	if (pcm_call_init(&initialized, &configuration) != PCM_CALL_OK)
		return PCM_CALL_INVALID_ARGUMENT;
	initialized.peer_info0 = restart->peer_info0;
	initialized.peer_info0_stored = true;
	initialized.v92_upstream_retry_ceiling = restart->upstream_retry_ceiling;
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

static bool condition_downstream(struct pcm_call *call, uint8_t *octet)
{
	if (call->local_role != V90_ANALOG_MODEM || !uses_pcm_upstream(call))
		return true;
	struct pcm_downstream_equalizer *equalizer = &call->downstream_equalizer;
	if (call->stage == PCM_CALL_PHASE3 &&
	    call->state.phase3.v92_analog.receive_stage == V92_PHASE3_ANALOG_RECEIVE_RI) {
		const struct v90_phase4_ri_search *ri = &call->state.phase3.v92_analog.receive.ri;
		/* Acquisition sees the original waveform, never its own decisions.
		 * Discard the fit if Ri returns to hunt. An accepted sign/magnitude
		 * window supplies the binary target without using later payload. */
		if (!v90_phase4_ri_search_is_acquired(ri) ||
		    !v90_phase4_ri_level_matches(call->law, ri->training_ucode,
			decode_octet(call->law, *octet))) {
			if (equalizer->training_level)
				*equalizer = (struct pcm_downstream_equalizer){ 0 };
			return true;
		}
		if (!equalizer->training_level) {
			int16_t level;
			if (!g711_ucode_sample(call->law, ri->training_ucode, true, &level) ||
			    !pcm_downstream_equalizer_init(equalizer, call->law, level))
				return false;
		}
		return pcm_downstream_equalizer_train(equalizer, *octet);
	}
	return pcm_downstream_equalizer_receive(equalizer, *octet, octet);
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

static uint32_t downstream_power_limit(
	const struct pcm_call *call, const struct v90_pcm_mapping *training)
{
	/* V.90 Table 15, in 16-bit linear RMS units. V.92 INFO0d retains
	 * the same power code and measurement-point fields. */
	static const uint16_t rms[] = {
		15124, 14276, 13480, 12724, 12012, 11340, 10708, 10108,
		9544, 9008, 8504, 8028, 7580, 7156, 6756, 6380,
		6020, 5684, 5368, 5068, 4784, 4516, 4264, 4024,
		3800, 3588, 3388, 3196, 3020, 2852, 2692, 2540,
	};
	const struct v90_info0d *peer = call->protocol == PCM_PROTOCOL_V92 ?
		&call->peer_info0.v92_digital.v90 : &call->peer_info0.digital;
	uint32_t level = rms[peer->maximum_transmit_power];
	uint32_t limit = level * level;

	if (training != NULL) {
		uint32_t lower, upper;
		enum v90_pcm_mapping_plane plane = peer->measure_power_at_codec_output ?
			V90_PCM_MAPPING_CODEC : V90_PCM_MAPPING_TRANSMIT;

		if (v90_pcm_mapping_power(training, plane, call->law, &lower, &upper) !=
		    V90_PCM_MAPPING_OK)
			return 0;
		/* V.90 8.5.2 limits data to +3 dB above CPt (not twice the
		 * power). Use the same conservative design bound for V.92 CPu. */
		uint32_t relative = (uint32_t)(lower * pow(10.0, 0.3));
		if (relative < limit)
			limit = relative;
	}
	return limit;
}

static bool select_training_ucode(const struct pcm_call *call, uint8_t *ucode)
{
	uint32_t limit = downstream_power_limit(call, NULL);

	/* V.90 Table 10 / V.92 Tables 18 and 19: UINFO is greater than 66
	 * and its two-point power cannot exceed the peer's INFO0d maximum.
	 * Keep a quieter local preference; only lower an over-power request. */
	for (unsigned selected = *ucode; selected >= V90_INFO1A_MIN_TRAINING_UCODE; --selected) {
		int16_t sample;

		if (!g711_ucode_sample(call->law, (uint8_t)selected, true, &sample))
			return false;
		if ((uint32_t)sample * (uint32_t)sample <= limit) {
			*ucode = (uint8_t)selected;
			return true;
		}
	}
	return false;
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
	if (entry.selection.startup.protocol != call->protocol ||
	    (entry.selection.upstream == PCM_UPSTREAM_PCM &&
	     !pcm_upstream_is_enabled(call->local_role, &call->local_info1)))
		return PCM_CALL_UNUSABLE_SELECTION;
	next = *call;
	next.state.phase3 = (union pcm_call_phase3_state){ 0 };
	if (entry.selection.upstream == PCM_UPSTREAM_PCM &&
	    call->local_role == V90_ANALOG_MODEM) {
		struct v92_phase3_analog_station station;
		struct v92_phase3_analog_station_configuration config =
			call->phase3_configuration.v92_analog;

		/* The retained proposal is a ceiling; train with the INFO1a that
		 * actually went over the wire after applying the peer power limit. */
		config.local_info1a = entry.selection.analog.pcm;
		if (call->analog_phase4_configuration.generate_constellations) {
			struct v90_pcm_mapping mapping;
			uint32_t lower, upper, limit = downstream_power_limit(call, NULL);

			if (v90_pcm_mapping_init_from_v92_cpt(&mapping, &config.cpt) != V90_PCM_MAPPING_OK ||
			    v90_pcm_mapping_power(&mapping, V90_PCM_MAPPING_CODEC, call->law,
						  &lower, &upper) != V90_PCM_MAPPING_OK ||
			    (upper > limit && !v90_profile_select_constellation(&mapping, limit,
						&config.cpt.transmit_constellations[0])))
				return PCM_CALL_UNUSABLE_SELECTION;
		}

		result = pcm_phase3_entry_start_v92_analog(
			&entry, &config,
			&station);
		if (result != PCM_PHASE3_ENTRY_OK)
			return entry_result(result);
		next.state.phase3.v92_analog = station;
	} else if (entry.selection.upstream == PCM_UPSTREAM_PCM) {
		const struct pcm_call_v92_digital_phase3_configuration *config =
			&call->phase3_configuration.v92_digital;
		struct v92_phase3_digital_station station;

		result = pcm_phase3_entry_start_v92_digital(
			&entry, config->trn1d_symbols ? config->trn1d_symbols :
				V90_PHASE3_TRN1D_MIN_SYMBOLS,
			config->equalizer_adaptation,
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
	bool v92 = call->protocol == PCM_PROTOCOL_V92;
	const struct v34_info1c *configured = v92 ?
		&call->local_info1.v92_digital.v34 : &call->local_info1.digital;
	const struct v90_info0 *peer = v92 ?
		&call->peer_info0.v92_analog.v90 : &call->peer_info0.analog;
	struct v34_info0 capabilities = v34_info0_from_v90(peer);
	struct v34_phase2_probe_receiver_measurement measurement;
	struct v34_info1c selected;
	enum pcm_phase2_result result;

	if (pcm_phase2_measure_remote_probe(
		    &call->state.phase2, &measurement) != PCM_PHASE2_OK)
		return PCM_CALL_SEQUENCE_MISMATCH;
	if (v34_phase2_select_v90_info1d(
		    &measurement, configured, &capabilities, &selected) !=
	    V34_PHASE2_SELECTION_OK)
		return PCM_CALL_NUMERIC_FAILURE;
	if (v92) {
		struct v92_info1d info1 = call->local_info1.v92_digital;

		/* Full V.92 Phase 2 shares V.90's measured upstream probe
		 * groups. Bit 70 is PCM permission, not a carrier selection. */
		selected.answer_transmit[V34_SYMBOL_RATE_3429].high_carrier = false;
		info1.v34 = selected;
		result = pcm_phase2_start_v92_digital_info1(
			&call->state.phase2, &info1);
	} else
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
		union pcm_call_local_info1 selected = call->local_info1;
		bool pcm_upstream = false;

		if (call->protocol == PCM_PROTOCOL_V92) {
			struct v92_info1d peer;

			if (pcm_phase2_peer_v92_info1d(&call->state.phase2, &peer) !=
			    PCM_PHASE2_OK)
				return PCM_CALL_SEQUENCE_MISMATCH;
			pcm_upstream = peer.pcm_upstream_supported &&
				call->local_info1.pcm_upstream_enabled;
		}
		if (!select_training_ucode(call, pcm_upstream ?
			&selected.v92_analog.training_ucode : &selected.analog.training_ucode))
			return PCM_CALL_UNUSABLE_SELECTION;
		result = pcm_upstream ?
			pcm_phase2_reply_pcm(
				&call->state.phase2,
				&selected.v92_analog) :
			pcm_phase2_reply_conventional(
				&call->state.phase2,
				&selected.analog);
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
	if (pcm_call_is_disconnecting(call))
		return false;
	/* V.90 and V.92 require a modem to honor the peer's Tone A/B full-retrain
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
	return call->stage == PCM_CALL_PHASE2 &&
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

	if (!pcm_call_uses_conventional_upstream(call) ||
	    call->local_role != V90_DIGITAL_MODEM ||
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
			    PCM_DOWNSTREAM_TX_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		/* An explicit CPd selection does not make the upstream receiver
		 * ready any earlier. The Phase 4 owner enables SUVd after training. */
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

static bool select_analog_constellations(struct pcm_call *call)
{
	struct pcm_call_analog_phase4_configuration *config = &call->analog_phase4_configuration;
	struct v90_pcm_mapping training, data;

	if (!config->generate_constellations)
		return true;
	return v90_pcm_mapping_init(&training, &config->training_parameters) == V90_PCM_MAPPING_OK &&
	       v90_profile_select_constellation(&training, downstream_power_limit(call, NULL),
			&config->training_parameters.transmit_constellations[0]) &&
	       v90_pcm_mapping_init(&training, &config->training_parameters) == V90_PCM_MAPPING_OK &&
	       v90_pcm_mapping_init(&data, &config->data_parameters) == V90_PCM_MAPPING_OK &&
	       v90_profile_select_constellation(&data, downstream_power_limit(call, &training),
			&config->data_parameters.transmit_constellations[0]);
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
		if (!select_analog_constellations(&next))
			return PCM_CALL_UNUSABLE_SELECTION;
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

static bool v92_filters_fit_capabilities(
	const struct v92_cpd_parameters *cpd, const struct v92_pcm_info1a *capabilities)
{
	unsigned int total = 0u;
	bool supports_z1 = capabilities->filter_sections == V92_PCM_FILTER_Z1_P1_Z2 ||
		capabilities->filter_sections == V92_PCM_FILTER_Z1_P1_P2_Z2;
	bool supports_p2 = capabilities->filter_sections == V92_PCM_FILTER_P1_P2_Z2 ||
		capabilities->filter_sections == V92_PCM_FILTER_Z1_P1_P2_Z2;

	if ((!supports_z1 && cpd->filter[V92_CPD_PRECODER_FEED_FORWARD].count != 0u) ||
	    (!supports_p2 && cpd->filter[V92_CPD_PREFILTER_FEEDBACK].count != 0u))
		return false;
	for (size_t section = 0u; section < V92_CPD_FILTER_SECTION_COUNT; ++section) {
		unsigned int count = cpd->filter[section].count;

		if (count > capabilities->maximum_section_coefficients)
			return false;
		total += count;
	}
	return total <= capabilities->maximum_total_coefficients;
}

static enum pcm_call_result select_v92_upstream(
	const struct pcm_call *call, uint32_t peer_rate_mask, double signal_level,
	struct v92_cpd_parameters *cpd)
{
	const struct pcm_call_v92_digital_phase4_configuration *configuration =
		&call->v92_digital_phase4_configuration;

	if (configuration->upstream_selection == PCM_CALL_V92_UPSTREAM_DIRECT) {
		uint8_t rate = configuration->direct_maximum_data_rate_number;
		long level;

		if (call->v92_upstream_retry_ceiling != 0u && call->v92_upstream_retry_ceiling < rate)
			rate = call->v92_upstream_retry_ceiling;
		if (!isfinite(signal_level) || signal_level < 1.0 || signal_level > INT16_MAX)
			return PCM_CALL_UNUSABLE_SELECTION;
		level = lround(signal_level);
		/* Rate-dependent modulus, trellis, constellation and normalized gain
		 * must be selected together at the measured upstream level. */
		for (; rate != 0u; --rate)
			if (v92_pcm_dil_rate_is_enabled(peer_rate_mask, rate) &&
			    v92_cpd_direct_select(cpd, call->law, rate, (int16_t)level, false) == V92_CPD_OK)
				break;
		if (rate == 0u)
			return PCM_CALL_UNUSABLE_SELECTION;
	} else {
		*cpd = configuration->cpd;
		if (!v92_pcm_dil_rate_is_enabled(peer_rate_mask, cpd->data_rate_number))
			return PCM_CALL_UNUSABLE_SELECTION;
	}
	return v92_filters_fit_capabilities(cpd, &call->phase3_entry.selection.analog.pcm) ?
		PCM_CALL_OK : PCM_CALL_UNUSABLE_SELECTION;
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
		struct v92_cpu_parameters cpu = call->v92_analog_phase4_configuration.cpu;

		if (!call->v92_peer_jd_stored ||
		    !call->round_trip_delay_stored)
			return PCM_CALL_INVALID_STATE;
		/* Keep the configured capacity and rate ceiling across retrains.
		 * Generated levels are reselected after K changes; explicit maps stay. */
		cpu.data_rate_number = highest_downstream_rate(
			call->enabled_downstream_rate_mask & call->v92_peer_jd.data_rate_capability_mask &
			((UINT32_C(1) << cpu.data_rate_number) - 1u));
		if (cpu.data_rate_number == 0u)
			return PCM_CALL_UNUSABLE_SELECTION;
		if (cpu.spectral_lookahead_frames >
		    call->v92_peer_jd.maximum_spectral_shaping_lookahead)
			cpu.spectral_lookahead_frames =
				call->v92_peer_jd.maximum_spectral_shaping_lookahead;
		if (call->analog_phase4_configuration.generate_constellations) {
			struct v90_pcm_mapping mapping;
			uint32_t limit = downstream_power_limit(call, &mixed->receive.stream.decoder.mapping);

			if (v90_pcm_mapping_init_from_v92_cpu(&mapping, &cpu) != V90_PCM_MAPPING_OK ||
			    !v90_profile_select_constellation(&mapping, limit, &cpu.transmit_constellations[0]))
				return PCM_CALL_UNUSABLE_SELECTION;
		}
		if (v92_phase3_analog_station_transmit_handoff(
			    &mixed->phase3, &handoff) !=
		    V92_PHASE3_ANALOG_STATION_OK)
			return PCM_CALL_INVALID_STATE;
		if (v92_phase4_analog_station_init(
			    &station, &handoff, &mixed->receive,
			    &call->v92_analog_phase4_configuration.suvu,
			    &cpu,
			    early_suvd, NULL) != V92_PHASE4_ANALOG_STATION_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		station.round_trip_delay_samples = call->round_trip_delay_samples;
		next.state.phase4.v92 = (struct pcm_call_v92_phase4_state){
			.rate_renegotiation_constellation = mixed->phase3.rate_renegotiation_constellation,
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
		struct v92_cpd_parameters cpd;
		enum pcm_call_result result;
		uint32_t upstream_rate_mask;

		if (!v92_phase3_digital_station_upstream_rate_mask(
			    &mixed->phase3, &upstream_rate_mask))
			return PCM_CALL_INVALID_STATE;
		if (v92_phase3_digital_station_receive_handoff(
			    &mixed->phase3, &handoff) !=
		    V92_PHASE3_DIGITAL_STATION_OK)
			return PCM_CALL_INVALID_STATE;
		result = select_v92_upstream(call, upstream_rate_mask, handoff.signal_level, &cpd);
		if (result != PCM_CALL_OK)
			return result;
		if (v92_phase4_digital_station_init(
			    &station, &handoff,
			    call->phase3_configuration.v92_digital.jp
				    .training_constellation,
			    &mixed->transmit,
			    &call->v92_digital_phase4_configuration.suvd,
			    &cpd, call->round_trip_delay_samples) !=
		    V92_PHASE4_DIGITAL_STATION_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		/* Nonzero DIL acknowledges CPt with Ri, then E1u with Ri-bar.
		 * Upstream TRN2u continues during that final downstream suffix. */
		for (size_t index = 0; index < mixed->phase3.early_trn2u_samples; ++index)
			if (v92_phase4_digital_station_receive(
				    &station, mixed->phase3.early_trn2u[index]) !=
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

	if (uses_pcm_upstream(call)) {
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

static enum pcm_call_result retain_v92_peer_jd(
	struct pcm_call *call)
{
	const struct v92_phase3_analog_station *station =
		&call->state.phase3.v92_analog;
	struct v92_jd jd;

	if (station->receive_stage != V92_PHASE3_ANALOG_RECEIVE_J ||
	    !v92_phase3_j_receiver_jd(&station->receive.j, &jd))
		return PCM_CALL_OK;
	if (call->v92_peer_jd_stored) {
		return call->v92_peer_jd.data_rate_capability_mask == jd.data_rate_capability_mask &&
			       call->v92_peer_jd.maximum_spectral_shaping_lookahead ==
				       jd.maximum_spectral_shaping_lookahead ?
			       PCM_CALL_OK : PCM_CALL_SEQUENCE_MISMATCH;
	}
	call->v92_peer_jd = jd;
	call->v92_peer_jd_stored = true;
	return jd.data_rate_capability_mask != 0u ?
		       PCM_CALL_OK : PCM_CALL_UNUSABLE_SELECTION;
}

static bool phase3_response_timed_out(const struct pcm_call *call)
{
	const struct pcm_phase3_digital_tx *transmit;
	uint64_t elapsed;

	if (uses_pcm_upstream(call)) {
		const struct v92_phase3_digital_station *station;

		if (call->local_role == V90_ANALOG_MODEM) {
			const struct v92_phase3_analog_station *analog = &call->state.phase3.v92_analog;
			const struct v92_pcm_phase3_initial_tx *initial = &analog->transmit.initial;

			if (analog->transmit_stage != V92_PHASE3_ANALOG_TRANSMIT_INITIAL)
				return false;
			/* V.92 9.5.2.2: Sd reversal within 1500 ms of starting Ja;
			 * Jd within 4500 ms of ending Ja. Receive runs first, so
			 * a last-sample response cancels either deadline. */
			if (initial->stage == V92_PCM_PHASE3_INITIAL_TX_COMPLETE)
				return analog->post_ja_silence_samples >= V92_PHASE3_JD_DEADLINE_SAMPLES;
			return initial->stage == V92_PCM_PHASE3_INITIAL_TX_JA &&
				analog->receive_stage == V92_PHASE3_ANALOG_RECEIVE_SD &&
				analog->receive.sd.stage < V90_PHASE3_SD_RECEIVER_SD_BAR &&
				initial->sequence.ja.emitted_symbols >= PCM_PHASE3_SD_DEADLINE_SAMPLES;
		}
		station = &call->state.phase3.v92_digital;
		if (station->receive_stage == V92_PHASE3_DIGITAL_RECEIVE_INITIAL) {
			/* V.92 9.5.1.2.1: Ja is due 4500 ms + RTD after INFO1a.
			 * Reuse that boundary's B1 clock; this sample is received
			 * before its remaining count advances. */
			return call->b1_deadline_armed &&
			       b1_deadline_limit(call) - call->b1_deadline_remaining_samples + 1u >=
				V92_PHASE3_JA_BASE_DEADLINE_SAMPLES + (uint32_t)call->round_trip_delay_samples;
		}
		if (station->receive_stage != V92_PHASE3_DIGITAL_RECEIVE_SU_PHASE &&
		    station->receive_stage != V92_PHASE3_DIGITAL_RECEIVE_SU_RESPONSE)
			return false;
		transmit = &station->downstream;
		if (transmit->stage != PCM_PHASE3_DIGITAL_TX_JD &&
		    transmit->stage != PCM_PHASE3_DIGITAL_TX_JP)
			return false;
		/* V.92 9.5.1.2.2 times the final Su reversal from TRN1d,
		 * including both Jd and Jp. Receiving it on the last allowed
		 * sample moves to second TRN1u before this check. */
		elapsed = (uint64_t)transmit->sequence.signs.trn1d_symbols +
			  ((uint64_t)transmit->jd_records + transmit->jp_records) * PCM_J_FRAME_BITS +
			  transmit->bit_index;
		return elapsed >= PCM_PHASE3_RETURN_BASE_DEADLINE_SAMPLES +
			(uint32_t)call->round_trip_delay_samples;
	}
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
		return elapsed >= (uint64_t)PCM_PHASE3_SD_DEADLINE_SAMPLES *
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
	return elapsed >= PCM_PHASE3_RETURN_BASE_DEADLINE_SAMPLES +
			  (uint32_t)call->round_trip_delay_samples;
}

static enum pcm_call_result sample_phase3(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	uint8_t *transmit_octet)
{
	enum pcm_call_result result;

	if (uses_pcm_upstream(call) &&
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
			result = retain_v92_peer_jd(call);
	} else if (uses_pcm_upstream(call)) {
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

	if (uses_pcm_upstream(call))
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
	case V92_PHASE4_ANALOG_STATION_CLEARDOWN_COMPLETE:
		return PCM_CALL_CLEARDOWN_COMPLETE;
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
	case V92_PHASE4_DIGITAL_STATION_CLEARDOWN_COMPLETE:
		return PCM_CALL_CLEARDOWN_COMPLETE;
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
		state->station.transmit.control.completed_cpu_records;

	*completed_this_sample = false;
	if (completed == state->observed_cpu_records)
		return PCM_CALL_OK;
	if (state->observed_cpu_records == UINT64_MAX ||
	    completed != state->observed_cpu_records + 1u)
		return PCM_CALL_NUMERIC_FAILURE;
	state->observed_cpu_records = completed;
	state->cpu_repeat_remaining_samples =
		state->station.transmit.control.peer_confirmation_received ||
		state->station.transmit.control.retry != V92_UPSTREAM_RETRY_IDLE ?
			0u : state->cpu_repeat_interval_samples;
	*completed_this_sample = true;
	return PCM_CALL_OK;
}

static enum pcm_call_result advance_v92_cpu_repeat(
	struct pcm_call_v92_analog_phase4_control *state,
	bool completed_this_sample)
{
	if (state->station.transmit.control.peer_confirmation_received ||
	    state->station.transmit.control.retry != V92_UPSTREAM_RETRY_IDLE) {
		state->cpu_repeat_remaining_samples = 0u;
		return PCM_CALL_OK;
	}
	if (completed_this_sample || state->observed_cpu_records == 0u)
		return PCM_CALL_OK;
	if (state->cpu_repeat_remaining_samples == 0u)
		return PCM_CALL_NUMERIC_FAILURE;
	if (--state->cpu_repeat_remaining_samples != 0u)
		return PCM_CALL_OK;
	if (v92_phase4_analog_station_expire_cpu_wait(&state->station) !=
	    V92_PHASE4_ANALOG_STATION_OK)
		return PCM_CALL_NUMERIC_FAILURE;
	return PCM_CALL_OK;
}

static void cancel_v92_cpu_repeat_if_confirmed(
	struct pcm_call_v92_analog_phase4_control *state)
{
	if (!state->station.transmit.control.peer_confirmation_received)
		return;
	state->cpu_repeat_remaining_samples = 0u;
}

static bool v92_phase4_control_is_ready(const struct pcm_call *call)
{
	if (call->state.phase4.v92.child != PCM_CALL_V92_PHASE4_CONTROL)
		return false;
	return call->local_role == V90_ANALOG_MODEM ?
		v92_phase4_analog_station_can_handoff(
			&call->state.phase4.v92.owner.analog.station) :
		v92_phase4_digital_station_is_ed_ready(
			&call->state.phase4.v92.owner.digital) ||
		v92_phase4_digital_station_is_pause_ready(
			&call->state.phase4.v92.owner.digital);
}

static bool v92_pause_is_committed(const struct pcm_call *call)
{
	const struct pcm_call_v92_phase4_state *state = &call->state.phase4.v92;
	if (state->child == PCM_CALL_V92_PHASE4_PAUSE)
		return true;
	if (state->child == PCM_CALL_V92_PHASE4_E2U)
		return (call->local_role == V90_ANALOG_MODEM ? state->owner.e2u.state.analog.downstream_stage :
			state->owner.e2u.state.digital.downstream_stage) == V92_PHASE4_E2U_DOWNSTREAM_PAUSE;
	return state->child == PCM_CALL_V92_PHASE4_CONTROL &&
		(call->local_role == V90_ANALOG_MODEM ?
		 v92_phase4_analog_station_can_handoff(&state->owner.analog.station) &&
		 v92_phase4_analog_tx_silence_requested(&state->owner.analog.station.transmit) :
		 v92_phase4_digital_station_is_pause_ready(&state->owner.digital));
}

static enum pcm_call_result enter_v92_pause(struct pcm_call *call)
{
	struct v92_phase4_e2u_handoff handoff;
	struct pcm_call next = *call;
	union pcm_call_v92_phase4_owner owner = { 0 };

	if (!call->v92_rate_renegotiation_active ||
	    v92_phase4_e2u_handoff(&call->state.phase4.v92.owner.e2u, &handoff) != V92_PHASE4_E2U_OK)
		return PCM_CALL_INVALID_STATE;
	bool peer_requested = call->state.phase4.v92.peer_requested_silence;
	if (call->local_role == V90_ANALOG_MODEM) {
		if (!v92_rate_pause_analog_init(&owner.analog_pause.signal, &handoff, peer_requested))
			return PCM_CALL_NUMERIC_FAILURE;
		owner.analog_pause.mapping = handoff.state.analog.mapping;
	} else {
		if (!v92_rate_pause_digital_init(&owner.digital_pause.signal, &handoff, peer_requested))
			return PCM_CALL_NUMERIC_FAILURE;
		owner.digital_pause.mapping = handoff.state.digital.mapping;
		owner.digital_pause.cpu = handoff.state.digital.cpu;
		owner.digital_pause.plan = handoff.state.digital.upstream_plan;
	}
	next.state.phase4.v92.owner = owner;
	next.state.phase4.v92.child = PCM_CALL_V92_PHASE4_PAUSE;
	if (!pcm_call_is_valid(&next))
		return PCM_CALL_NUMERIC_FAILURE;
	*call = next;
	return PCM_CALL_OK;
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
		next.state.phase4.v92.training_mapping = handoff.receive.stream.decoder.mapping;
		next.state.phase4.v92.peer_requested_silence = handoff.pause &&
			call->state.phase4.v92.owner.analog.station.suvd.request_silence;
	} else {
		struct v92_phase4_digital_handoff handoff;

		next.state.phase4.v92.training_mapping =
			call->state.phase4.v92.owner.digital.control.transmit.encoder.mapping;
		if (v92_phase4_digital_station_handoff(
			    &call->state.phase4.v92.owner.digital,
			    &handoff) != V92_PHASE4_DIGITAL_STATION_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		if (v92_phase4_e2u_init_digital(&e2u, &handoff) !=
		    V92_PHASE4_E2U_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		next.state.phase4.v92.peer_requested_silence = handoff.pause &&
			call->state.phase4.v92.owner.digital.control.suvu.request_silence;
	}
	next.state.phase4.v92.owner = (union pcm_call_v92_phase4_owner){ .e2u = e2u };
	next.state.phase4.v92.child = PCM_CALL_V92_PHASE4_E2U;
	if (!pcm_call_is_valid(&next))
		return PCM_CALL_NUMERIC_FAILURE;
	/* E2u may itself have confirmed the pause in the control station.
	 * Transition at that consumed boundary; do not clock E2u a second time. */
	if (v92_phase4_e2u_is_complete(&e2u) && !v92_phase4_e2u_is_data_ready(&e2u)) {
		enum pcm_call_result result = enter_v92_pause(&next);
		if (result != PCM_CALL_OK)
			return result;
	}
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
	case V92_PHASE4_E2U_SEQUENCE_MISMATCH:
		return PCM_CALL_SEQUENCE_MISMATCH;
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

/* PHASE4_READY consumes no output interval: the prefix has drained and the
 * control transmitter now owns that same interval. */
static enum pcm_call_result transmit_v92_rate_prefix(
	struct pcm_call_v92_phase4_state *state, uint8_t *octet)
{
	struct v90_pcm_symbol symbol;
	enum v90_downstream_data_stream_result result;

	if (state->prefix_queue_count) {
		*octet = state->prefix_queue[0];
		state->prefix_queue_count--;
		memmove(state->prefix_queue, state->prefix_queue + 1, state->prefix_queue_count);
		state->prefix_queue[state->prefix_queue_count] = 0;
		return PCM_CALL_OK;
	}
	if (state->prefix_stage == PCM_CALL_V92_PREFIX_RU) {
		struct pcm_call_v92_analog_rate_prefix *prefix = &state->prefix.analog;
		if (prefix->queue_count) {
			*octet = encode_sample(prefix->transmit.law, prefix->queue[0]);
			prefix->queue_count--;
			memmove(prefix->queue, prefix->queue + 1,
				prefix->queue_count * sizeof(prefix->queue[0]));
			prefix->queue[prefix->queue_count] = 0;
			return PCM_CALL_OK;
		}
		if (!v92_pcm_phase3_ru_remaining(&prefix->transmit)) {
			if (prefix->transmit.kind == V92_PCM_PHASE3_RU_BAR) {
				if (pcm_r_rx_is_complete(&prefix->receive))
					state->prefix_stage = PCM_CALL_V92_PREFIX_NONE;
				return PCM_CALL_PHASE4_READY;
			}
			if (v92_pcm_phase3_ru_init(&prefix->transmit, prefix->transmit.law,
				prefix->transmit.lu_ucode, V92_PCM_PHASE3_RU_BAR) != V92_PCM_PHASE3_RU_OK)
				return PCM_CALL_NUMERIC_FAILURE;
		}
		if (v92_pcm_phase3_ru_next(&prefix->transmit, &symbol) != V92_PCM_PHASE3_RU_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		*octet = symbol.octet;
		return PCM_CALL_OK;
	}
	if (state->prefix_stage == PCM_CALL_V92_PREFIX_DATA) {
		result = v90_downstream_data_stream_next(&state->prefix.data, NULL, NULL, &symbol);
		if (result != V90_DOWNSTREAM_DATA_STREAM_OK)
			return PCM_CALL_NUMERIC_FAILURE;
	} else {
		if (state->prefix_stage == PCM_CALL_V92_PREFIX_DRAIN) {
			result = v90_downstream_data_stream_finish(&state->prefix.data, &symbol);
			if (result == V90_DOWNSTREAM_DATA_STREAM_OK) {
				*octet = symbol.octet;
				return PCM_CALL_OK;
			}
			struct pcm_r_tx rd;
			if (result != V90_DOWNSTREAM_DATA_STREAM_COMPLETE ||
			    pcm_r_tx_init(&rd, &state->prefix.data.transmitter.encoder.mapping,
				PCM_R_RD) != PCM_R_TX_OK)
				return PCM_CALL_NUMERIC_FAILURE;
			state->prefix.rd = rd;
			state->prefix_stage = PCM_CALL_V92_PREFIX_RD;
		}
		if (state->prefix_stage == PCM_CALL_V92_PREFIX_RD) {
			enum pcm_r_tx_result transmitted = pcm_r_tx_next(&state->prefix.rd, &symbol);

			if (transmitted == PCM_R_TX_COMPLETE)
				state->prefix_stage = PCM_CALL_V92_PREFIX_NONE;
			else if (transmitted != PCM_R_TX_OK)
				return PCM_CALL_NUMERIC_FAILURE;
		}
		if (state->prefix_stage == PCM_CALL_V92_PREFIX_NONE)
			return PCM_CALL_PHASE4_READY;
	}
	*octet = symbol.octet;
	return PCM_CALL_OK;
}

static enum pcm_call_result receive_v92_rate_request(
	struct pcm_call *call, int16_t sample)
{
	struct pcm_call_v92_phase4_state *state = &call->state.phase4.v92;
	struct pcm_call_v92_rate_request *ru = &state->owner.ru;
	enum v92_ru_rx_result received = v92_ru_line_rx_sample(&ru->receive, sample);

	if (received == V92_RU_RX_BUFFERING || received == V92_RU_RX_DETECTED)
		return PCM_CALL_OK;
	if (received == V92_RU_RX_BAR_STARTED) {
		if (state->prefix_stage == PCM_CALL_V92_PREFIX_DATA)
			state->prefix_stage = PCM_CALL_V92_PREFIX_DRAIN;
		return PCM_CALL_OK;
	}
	if (received != V92_RU_RX_COMPLETE)
		return PCM_CALL_SEQUENCE_MISMATCH;
	struct v92_phase4_digital_station station;

	if (v92_phase4_digital_station_init_after_ru(&station, &ru->receive,
		&ru->transmit, &ru->cpd, &ru->plan, &ru->cpu,
		call->round_trip_delay_samples) != V92_PHASE4_DIGITAL_STATION_OK)
		return PCM_CALL_NUMERIC_FAILURE;
	state->owner = (union pcm_call_v92_phase4_owner){ .digital = station };
	state->child = PCM_CALL_V92_PHASE4_CONTROL;
	return PCM_CALL_OK;
}

static bool activate_v92_cleardown(struct pcm_call *call)
{
	struct pcm_call_v92_phase4_state *state = &call->state.phase4.v92;

	if (!state->cleardown_requested || state->child != PCM_CALL_V92_PHASE4_CONTROL ||
	    v92_pause_is_committed(call))
		return true;
	/* Keep receiving status during TRN2 so the ordinary training exit and
	 * coder continuity remain intact. No CP or E2u can be committed first. */
	if (call->local_role == V90_ANALOG_MODEM) {
		struct v92_phase4_analog_station *station = &state->owner.analog.station;
		return station->transmit.stage == V92_PHASE4_ANALOG_TX_TRN2U ||
			v92_phase4_analog_station_start_cleardown(station);
	}
	struct v92_phase4_digital_station *station = &state->owner.digital;
	return station->control.transmit.stage == PCM_DOWNSTREAM_TX_TRN2D ||
		v92_phase4_digital_station_start_cleardown(station);
}

static enum pcm_call_result sample_v92_pause(struct pcm_call *call,
	uint8_t received_octet, int16_t received_sample, uint8_t *transmit_octet)
{
	struct pcm_call_v92_phase4_state *state = &call->state.phase4.v92;
	if (call->local_role == V90_ANALOG_MODEM) {
		struct v92_rate_pause_analog *pause = &state->owner.analog_pause.signal;
		int16_t sample;
		if (!v92_rate_pause_analog_transmit(pause, &sample) ||
		    !v92_rate_pause_analog_receive(pause, received_octet))
			return PCM_CALL_SEQUENCE_MISMATCH;
		*transmit_octet = encode_sample(call->law, sample);
		if (!v92_rate_pause_analog_is_ready(pause))
			return PCM_CALL_OK;
		struct pcm_call_v92_analog_phase4_control resumed = {
			.cpu_repeat_interval_samples = V92_CPU_REPEAT_BASE_SAMPLES + call->round_trip_delay_samples,
		};
		if (!v92_rate_pause_analog_resume(&resumed.station, pause,
			&state->owner.analog_pause.mapping, call->round_trip_delay_samples))
			return PCM_CALL_NUMERIC_FAILURE;
		state->owner = (union pcm_call_v92_phase4_owner){ .analog = resumed };
	} else {
		struct v92_rate_pause_digital *pause = &state->owner.digital_pause.signal;
		if (!v92_rate_pause_digital_transmit(pause, transmit_octet) ||
		    !v92_rate_pause_digital_receive(pause, received_sample))
			return PCM_CALL_SEQUENCE_MISMATCH;
		if (!v92_rate_pause_digital_is_ready(pause))
			return PCM_CALL_OK;
		struct v92_phase4_digital_station resumed;
		if (!v92_rate_pause_digital_resume(&resumed, pause,
			&state->owner.digital_pause.mapping.cpd, &state->owner.digital_pause.plan,
			&state->owner.digital_pause.cpu, call->round_trip_delay_samples))
			return PCM_CALL_NUMERIC_FAILURE;
		state->owner = (union pcm_call_v92_phase4_owner){ .digital = resumed };
	}
	state->child = PCM_CALL_V92_PHASE4_CONTROL;
	state->peer_requested_silence = false;
	return PCM_CALL_OK;
}

static enum pcm_call_result sample_v92_phase4(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	uint8_t *transmit_octet)
{
	bool prefix_sent = false;
	if (!activate_v92_cleardown(call))
		return PCM_CALL_INVALID_STATE;
	if (call->state.phase4.v92.prefix_stage != PCM_CALL_V92_PREFIX_NONE) {
		enum pcm_call_result result = transmit_v92_rate_prefix(
			&call->state.phase4.v92, transmit_octet);

		if (result != PCM_CALL_OK && result != PCM_CALL_PHASE4_READY)
			return result;
		prefix_sent = result == PCM_CALL_OK;
	}
	if (call->state.phase4.v92.child == PCM_CALL_V92_PHASE4_RU) {
		if (!prefix_sent) {
			struct pcm_downstream_symbol symbol;
			if (pcm_downstream_tx_next(&call->state.phase4.v92.owner.ru.transmit,
				&symbol) != PCM_DOWNSTREAM_TX_OK)
				return PCM_CALL_NUMERIC_FAILURE;
			*transmit_octet = symbol.pcm.octet;
		}
		return receive_v92_rate_request(call, received_sample);
	}
	if (call->local_role == V90_ANALOG_MODEM &&
	    call->state.phase4.v92.child == PCM_CALL_V92_PHASE4_CONTROL &&
	    v92_phase4_control_is_ready(call)) {
		enum pcm_call_result result = enter_v92_e2u(call);

		if (result != PCM_CALL_OK)
			return result;
	}
	if (call->state.phase4.v92.child == PCM_CALL_V92_PHASE4_E2U) {
		struct v92_phase4_e2u *e2u = &call->state.phase4.v92.owner.e2u;
		enum pcm_call_result result = v92_e2u_result(v92_phase4_e2u_next(
			e2u, received_octet, received_sample, transmit_octet));
		if (result == PCM_CALL_OK && v92_phase4_e2u_is_complete(e2u) &&
		    !v92_phase4_e2u_is_data_ready(e2u))
			return enter_v92_pause(call);
		return result;
	}
	if (call->state.phase4.v92.child == PCM_CALL_V92_PHASE4_PAUSE)
		return sample_v92_pause(call, received_octet, received_sample, transmit_octet);
	if (call->local_role == V90_ANALOG_MODEM) {
		struct pcm_call_v92_analog_phase4_control *state =
			&call->state.phase4.v92.owner.analog;
		bool completed_this_sample;
		int16_t sample;
		enum pcm_call_result result = prefix_sent ? PCM_CALL_OK : v92_analog_phase4_result(
			v92_phase4_analog_station_transmit(
				&state->station, &sample));

		if (result != PCM_CALL_OK)
			return result;
		if (!prefix_sent)
			*transmit_octet = encode_sample(call->law, sample);
		if (call->state.phase4.v92.prefix_stage == PCM_CALL_V92_PREFIX_RU &&
		    !pcm_r_rx_is_complete(&call->state.phase4.v92.prefix.analog.receive)) {
			enum pcm_r_rx_result received = pcm_r_rx_receive(
				&call->state.phase4.v92.prefix.analog.receive, received_octet);
			return received == PCM_R_RX_BUFFERING || received == PCM_R_RX_SIGNAL_DETECTED ||
				received == PCM_R_RX_BAR_STARTED || received == PCM_R_RX_BAR_COMPLETE ?
				PCM_CALL_OK : PCM_CALL_SEQUENCE_MISMATCH;
		}
		if (state->station.cleardown.active)
			return v92_analog_phase4_result(v92_phase4_analog_station_receive(
				&state->station, received_octet));
		result = observe_v92_cpu_completion(
			state, &completed_this_sample);
		if (result != PCM_CALL_OK)
			return result;
		/* Expire before consuming this sample so a whole peer record
		 * ending exactly at the threshold can qualify or cancel retry. */
		result = advance_v92_cpu_repeat(state, completed_this_sample);
		if (result != PCM_CALL_OK)
			return result;
		result = v92_analog_phase4_result(
			v92_phase4_analog_station_receive(
				&state->station, received_octet));
		cancel_v92_cpu_repeat_if_confirmed(state);
		if (result != PCM_CALL_OK)
			return result;
		if (state->station.cleardown.active)
			return PCM_CALL_OK;
		if (state->station.cpd_received &&
		    (!v92_pcm_dil_rate_is_enabled(
			    call->phase3_configuration.v92_analog
				    .upstream_rate_mask,
			    state->station.cpd.data_rate_number) ||
		     !v92_filters_fit_capabilities(
			     &state->station.mapping.cpd,
			     &call->phase3_entry.selection.analog.pcm)))
			return PCM_CALL_UNUSABLE_SELECTION;
		return PCM_CALL_OK;
	}
	{
		const struct v92_jd *jd = &call->phase3_configuration.v92_digital.jd;
		struct v92_phase4_digital_station *station = &call->state.phase4.v92.owner.digital;
		uint16_t measured_symbols = station->receive.channel.symbols;
		const struct v92_cpu_parameters *cpu = &call->state.phase4.v92.owner.digital.control.cpu;
		enum v92_phase4_digital_station_result station_result = prefix_sent ?
			V92_PHASE4_DIGITAL_STATION_OK : v92_phase4_digital_station_transmit(
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
		if (station->control.cleardown.active)
			return PCM_CALL_OK;
		/* The final delayed TRN2u target completes the channel measurement
		 * before SUVu can start CPd. Freeze wire filters and local receive
		 * coordinates together here; repeats must not redesign either.
		 * A scalar gain change already belongs to the trained receive FIR.
		 * Redesigning CPd for it can destroy the direct constellation's
		 * quantizer spacing without providing useful channel compensation. */
		if (measured_symbols < V92_UPSTREAM_CHANNEL_SYMBOLS &&
		    station->receive.channel.symbols == V92_UPSTREAM_CHANNEL_SYMBOLS &&
		    station->receive.channel_distorted &&
		    !v92_upstream_channel_fits_gain(&station->receive.channel) &&
		    call->v92_digital_phase4_configuration.upstream_selection == PCM_CALL_V92_UPSTREAM_DIRECT) {
			struct v92_cpd_parameters cpd;
			struct v92_upstream_plan plan;
			const struct v92_phase4_cpd_tx *wire = &station->control.initial_cpd;
			int16_t level = station->receive.signal.control.signal.line.demodulator.lu;

			if (v92_cpd_decode(wire->sequence, sizeof(wire->sequence),
				    wire->sequence_bits, wire->frame_bits, &cpd) != V92_CPD_OK)
				return PCM_CALL_UNUSABLE_SELECTION;
			if (!v92_cpd_channel_design(&cpd, &station->receive.channel, call->law, level, &plan)) {
				/* A truncated or corrupted TRN2u cannot identify a usable
				 * channel. V.92 9.6.1.2 permits full retrain during Phase 4;
				 * do not accept the bad fit or renegotiate a data rate before
				 * data exists. Share the receiver's bounded recovery budget. */
				enum v92_data_quality_result quality =
					v92_data_quality_retrain(&call->pcm_upstream_quality);

				return quality == V92_DATA_QUALITY_RETRAIN ? begin_retrain(call, false) :
					quality == V92_DATA_QUALITY_EXHAUSTED ? PCM_CALL_UNUSABLE_SELECTION :
					PCM_CALL_NUMERIC_FAILURE;
			}
			if (!v92_filters_fit_capabilities(&cpd, &call->phase3_entry.selection.analog.pcm))
				return PCM_CALL_UNUSABLE_SELECTION;
			result = v92_digital_phase4_result(
				v92_phase4_digital_station_select_upstream(station, &cpd, &plan));
			if (result != PCM_CALL_OK)
				return result;
		}
		if (call->state.phase4.v92.owner.digital.control.cpu_received &&
		    (!downstream_rate_is_enabled(jd->data_rate_capability_mask, cpu->data_rate_number) ||
		     cpu->spectral_lookahead_frames > jd->maximum_spectral_shaping_lookahead))
			return PCM_CALL_UNUSABLE_SELECTION;
		if (v92_phase4_control_is_ready(call))
			return enter_v92_e2u(call);
		return PCM_CALL_OK;
	}
}

static enum pcm_call_result sample_phase4(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	uint8_t *transmit_octet)
{
	if (uses_pcm_upstream(call))
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
	if (pcm_call_is_disconnecting(call))
		return PCM_CALL_OK;
	if (v92_fast_is_active_owned(call) && call->local_role == V90_ANALOG_MODEM) {
		struct pcm_call_v92_analog_fast *fast = &call->state.data_path.v92_analog.fast;
		if (fast->finish_started)
			return PCM_CALL_INVALID_STATE;
		v92_cleardown_start(&fast->cleardown, call->round_trip_delay_samples, true);
		return PCM_CALL_OK;
	}
	if (v92_fast_is_active_owned(call) &&
	    call->state.data_path.v92_digital.fast.stage != PCM_CALL_V92_FAST_FINISH)
		return v92_downstream_control_start_cleardown(&call->state.data_path.v92_digital.fast.control) ?
			PCM_CALL_OK : PCM_CALL_INVALID_STATE;
	if (uses_pcm_upstream(call) && (pcm_call_is_online(call) ||
	    (call->v92_rate_renegotiation_active && call->stage == PCM_CALL_PHASE4 &&
	     (v92_pause_is_committed(call) ||
	      (call->state.phase4.v92.child != PCM_CALL_V92_PHASE4_E2U &&
	       !v92_phase4_control_is_ready(call)))))) {
		struct pcm_call next = *call;
		if (next.stage == PCM_CALL_DATA_PATH) {
			result = pcm_call_start_rate_renegotiation(&next);
			if (result != PCM_CALL_OK)
				return result;
		}
		next.state.phase4.v92.cleardown_requested = true;
		if (!pcm_call_is_valid(&next))
			return PCM_CALL_NUMERIC_FAILURE;
		*call = next;
		return PCM_CALL_OK;
	}
	if (uses_pcm_upstream(call) && call->stage == PCM_CALL_PHASE4 &&
	    call->state.phase4.v92.child == PCM_CALL_V92_PHASE4_CONTROL) {
		bool started = call->local_role == V90_ANALOG_MODEM ?
			v92_phase4_analog_station_start_cleardown(&call->state.phase4.v92.owner.analog.station) :
			v92_phase4_digital_station_start_cleardown(&call->state.phase4.v92.owner.digital);

		result = started ? PCM_CALL_OK : PCM_CALL_INVALID_STATE;
	} else if (call->stage != PCM_CALL_DATA_PATH ||
		   !pcm_call_uses_conventional_upstream(call)) {
		return PCM_CALL_INVALID_STATE;
	} else {
		result = call->local_role == V90_ANALOG_MODEM ?
			analog_path_result(v90_analog_path_start_cleardown(
				&call->state.data_path.analog)) :
			digital_path_result(v90_digital_path_start_cleardown(
				&call->state.data_path.digital));
	}
	if (result == PCM_CALL_OK) {
		call->b1_deadline_armed = false;
		call->b1_deadline_remaining_samples = 0u;
		call->rate_renegotiation_deadline_armed = false;
		call->rate_renegotiation_deadline_remaining_samples = 0u;
		if (!pcm_call_is_valid(call))
			return PCM_CALL_NUMERIC_FAILURE;
	}
	return result;
}

static bool start_v92_data_path(struct pcm_call *next, const struct pcm_call *call)
{
	struct v92_phase4_e2u_handoff handoff;

	if (!v92_phase4_e2u_is_data_ready(&call->state.phase4.v92.owner.e2u) ||
	    v92_phase4_e2u_handoff(&call->state.phase4.v92.owner.e2u, &handoff) !=
	    V92_PHASE4_E2U_OK)
		return false;
	if (call->local_role == V90_ANALOG_MODEM) {
		const struct v92_phase4_e2u_analog_handoff *from = &handoff.state.analog;
		struct pcm_call_v92_analog_data *path = &next->state.data_path.v92_analog;

		if (v92_upstream_data_tx_init(&path->upstream, &from->mapping,
					     from->transmit.lu) != V92_UPSTREAM_DATA_TX_OK)
			return false;
		path->downstream = from->downstream;
		path->downstream_stage = from->downstream_stage;
		path->cpu = from->cpu;
		path->training_mapping = call->state.phase4.v92.training_mapping;
		path->rate_renegotiation_constellation = call->state.phase4.v92.rate_renegotiation_constellation;
		struct v90_pcm_mapping mapping;
		if (v90_pcm_mapping_init_from_v92_cpu(&mapping, &path->cpu) != V90_PCM_MAPPING_OK ||
		    pcm_r_rx_init(&path->rd, &mapping, V90_PCM_MAPPING_TRANSMIT, PCM_R_RD) != PCM_R_RX_OK ||
		    pcm_r_rx_init(&path->rf, &mapping, V90_PCM_MAPPING_TRANSMIT, PCM_R_RF) != PCM_R_RX_OK)
			return false;
	} else {
		const struct v92_phase4_e2u_digital_handoff *from = &handoff.state.digital;
		struct pcm_call_v92_digital_data *path = &next->state.data_path.v92_digital;

		if (v92_upstream_data_rx_init(&path->upstream, &from->receive,
					     &from->mapping, &from->upstream_plan) != V92_UPSTREAM_DATA_RX_OK)
			return false;
		if (!v92_ru_line_rx_init_after_e2u(&path->ru, &from->receive) ||
		    !v92_data_quality_restart(&next->pcm_upstream_quality))
			return false;
		/* Keep the trained FIR for Ru, but select post-Ru modulation with
		 * Jp bit 49 rather than the initial-training bit 48. */
		path->ru.constellation = call->phase3_configuration.v92_digital.jp.rate_renegotiation_constellation;
		path->training_mapping = call->state.phase4.v92.training_mapping;
		path->downstream = from->downstream;
		path->downstream_stage = from->downstream_stage;
		path->cpu = from->cpu;
	}
	return true;
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
	if (uses_pcm_upstream(call)) {
		if (!start_v92_data_path(&next, call))
			return PCM_CALL_NUMERIC_FAILURE;
	} else if (call->local_role == V90_ANALOG_MODEM) {
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
	if (uses_pcm_upstream(call))
		return call->local_role == V90_ANALOG_MODEM ?
			&call->state.data_path.v92_analog.downstream.data.stream.decoder.mapping :
			&call->state.data_path.v92_digital.downstream.data.transmitter.encoder.mapping;
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
	if (!v90_pcm_mapping_is_valid(downstream) || downstream->kind != V90_CP_DATA)
		return false;
	if (uses_pcm_upstream(call)) {
		const struct v92_pcm_mapping *mapping = call->local_role == V90_ANALOG_MODEM ?
			&call->state.data_path.v92_analog.upstream.encoder.mapping :
			&call->state.data_path.v92_digital.upstream.decoder.mapping;

		upstream_rate = ((uint32_t)mapping->data_bits_per_frame *
				V90_NETWORK_CHARACTERS_PER_SECOND +
				V92_PCM_MAPPING_FRAME_SYMBOLS / 2u) / V92_PCM_MAPPING_FRAME_SYMBOLS;
	} else {
		upstream_units = online_upstream_rate_units(call);
		if (upstream_units == 0)
			return false;
		upstream_rate = (uint32_t)upstream_units * V90_UPSTREAM_RATE_UNIT;
	}
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

	if (!pcm_call_uses_conventional_upstream(call) ||
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
	if (pcm_call_is_disconnecting(call))
		return PCM_CALL_CLEARDOWN_COMPLETE;
	/* V.90 9.6 bounds the response to 5000 ms plus two round-trip delays.
	 * Use the same bounded recovery policy for PCM Ru/control/B1, not as
	 * a purported V.92 normative timer. */
	return begin_retrain(call, false);
}

static enum pcm_call_result recover_rate_renegotiation_failure(
	struct pcm_call *call, enum pcm_call_result result)
{
	if ((result != PCM_CALL_UNUSABLE_SELECTION &&
	     result != PCM_CALL_SEQUENCE_MISMATCH) ||
	    !rate_renegotiation_is_active_owned(call))
		return result;
	if (pcm_call_is_disconnecting(call))
		return PCM_CALL_CLEARDOWN_COMPLETE;
	/* Leave a broken committed rate exchange through the standards-defined
	 * full-retrain path. */
	return begin_retrain(call, false);
}

static void complete_cleardown(struct pcm_call *call)
{
	call->downstream_equalizer = (struct pcm_downstream_equalizer){ 0 };
	if (uses_pcm_upstream(call))
		memset(&call->state, 0, sizeof(call->state));
	call->stage = PCM_CALL_CLOSED;
	call->retrain_active = false;
	call->v92_rate_renegotiation_active = false;
	call->b1_deadline_armed = false;
	call->b1_deadline_remaining_samples = 0u;
	call->rate_renegotiation_deadline_armed = false;
	call->rate_renegotiation_deadline_remaining_samples = 0u;
}

static enum pcm_call_result begin_v92_rate_request(struct pcm_call *call, bool local)
{
	const struct pcm_call_v92_digital_data *path = &call->state.data_path.v92_digital;
	struct pcm_call_v92_phase4_state state = {
		.owner.ru = {
			.receive = path->ru,
			.cpd = path->upstream.decoder.mapping.cpd,
			.plan = path->upstream.line.plan,
			.cpu = path->cpu,
		},
		.prefix.data = path->downstream.data,
		.training_mapping = path->training_mapping,
		.prefix_stage = local ? PCM_CALL_V92_PREFIX_DRAIN : PCM_CALL_V92_PREFIX_DATA,
		.child = PCM_CALL_V92_PHASE4_RU,
	};
	struct v90_pcm_mapping mapping;
	const struct v92_suvd_parameters suvd = {
		.request_silence = call->v92_digital_phase4_configuration.suvd.request_silence,
	};

	if (path->downstream_stage != V92_PHASE4_E2U_DOWNSTREAM_DATA)
		return PCM_CALL_SEQUENCE_MISMATCH;
	if (path->fast.stage != PCM_CALL_V92_FAST_IDLE) {
		/* V.92 9.9.1.1.2 gives Ru priority. The caller has observed its
		 * phase reversal: retain the pending output frame and start Rd at
		 * the next six-symbol boundary, with the preceding data mapping. */
		state.prefix_stage = PCM_CALL_V92_PREFIX_DRAIN;
		if (path->fast.stage == PCM_CALL_V92_FAST_RF) {
			struct pcm_r_tx rf = path->fast.rf;
			while (rf.transmitted_symbols % V90_CP_FRAME_INTERVALS) {
				struct v90_pcm_symbol symbol;
				if (pcm_r_tx_next(&rf, &symbol) != PCM_R_TX_OK)
					return PCM_CALL_NUMERIC_FAILURE;
				state.prefix_queue[state.prefix_queue_count++] = symbol.octet;
			}
		} else if (path->fast.stage == PCM_CALL_V92_FAST_CONTROL) {
			const struct pcm_downstream_tx *transmit = &path->fast.control.transmit;
			state.prefix_queue_count = transmit->queue_count;
			for (unsigned i = 0; i < transmit->queue_count; ++i)
				state.prefix_queue[i] = transmit->queue[i].pcm.octet;
		}
		if (path->fast.stage == PCM_CALL_V92_FAST_RF || path->fast.stage == PCM_CALL_V92_FAST_CONTROL) {
			struct pcm_r_tx rd;
			if (pcm_r_tx_init(&rd, &path->fast.rf.mapping, PCM_R_RD) != PCM_R_TX_OK)
				return PCM_CALL_NUMERIC_FAILURE;
			state.prefix.rd = rd;
			state.prefix_stage = PCM_CALL_V92_PREFIX_RD;
		}
	}
	/* Both directions reuse the old channel selection. Configure SUVd now
	 * so a delayed Ru cannot leave TRN2d running beyond V.92 9.8.1.1.2.
	 * The continuous coder sends 2040T before starting the status records. */
	if (v90_pcm_mapping_init_rate_renegotiation(&mapping, &path->training_mapping,
		&path->downstream.data.transmitter.encoder.mapping) != V90_PCM_MAPPING_OK ||
	    pcm_downstream_tx_init(&state.owner.ru.transmit, PCM_PROTOCOL_V92, &mapping) != PCM_DOWNSTREAM_TX_OK ||
	    pcm_downstream_tx_start_suvd(&state.owner.ru.transmit, &suvd) != PCM_DOWNSTREAM_TX_OK)
		return PCM_CALL_NUMERIC_FAILURE;
	call->state = (union pcm_call_state){ .phase4.v92 = state };
	call->stage = PCM_CALL_PHASE4;
	call->retrain_active = true;
	call->v92_rate_renegotiation_active = true;
	return reset_retrain_detector(call);
}

static enum pcm_call_result begin_v92_analog_rate_request(struct pcm_call *call)
{
	const struct pcm_call_v92_analog_data *path = &call->state.data_path.v92_analog;
	struct pcm_call_v92_phase4_state state = {
		.training_mapping = path->training_mapping,
		.rate_renegotiation_constellation = path->rate_renegotiation_constellation,
		.prefix_stage = PCM_CALL_V92_PREFIX_RU,
		.child = PCM_CALL_V92_PHASE4_CONTROL,
		.owner.analog.cpu_repeat_interval_samples =
			V92_CPU_REPEAT_BASE_SAMPLES + call->round_trip_delay_samples,
		.prefix.analog.receive = path->rd,
		.prefix.analog.queue_count = path->upstream.queue_count,
	};
	struct v90_pcm_mapping mapping;
	struct v90_pcm_symbol level;
	struct pcm_downstream_rx downstream;
	struct pcm_call_v92_analog_rate_prefix *prefix = &state.prefix.analog;

	/* Ru belongs to the current data epoch. Do not restore the nominal
	 * Phase 3 preference and undo the peer's negotiated power reduction. */
	if (!v90_pcm_symbol_parse(&level, call->law,
		encode_sample(call->law, path->upstream.upstream_level)) ||
	    v90_pcm_mapping_init_rate_renegotiation(&mapping, &path->training_mapping,
		&path->rd.mapping) != V90_PCM_MAPPING_OK ||
	    pcm_downstream_rx_init(&downstream, PCM_PROTOCOL_V92, &mapping,
		V90_PCM_MAPPING_TRANSMIT) != PCM_DOWNSTREAM_RX_OK ||
	    v92_phase4_analog_station_init_after_ru(&state.owner.analog.station,
		path->rate_renegotiation_constellation, path->upstream.upstream_level, &downstream,
		&path->cpu, &path->upstream.encoder.mapping,
		call->v92_analog_phase4_configuration.suvu.request_silence,
		call->round_trip_delay_samples) != V92_PHASE4_ANALOG_STATION_OK ||
	    v92_pcm_phase3_ru_init(&prefix->transmit, call->law, level.ucode,
		V92_PCM_PHASE3_RU) != V92_PCM_PHASE3_RU_OK)
		return PCM_CALL_NUMERIC_FAILURE;
	memcpy(prefix->queue, path->upstream.queue, sizeof(prefix->queue));
	call->state = (union pcm_call_state){ .phase4.v92 = state };
	call->stage = PCM_CALL_PHASE4;
	call->retrain_active = true;
	call->v92_rate_renegotiation_active = true;
	return reset_retrain_detector(call);
}

enum pcm_call_result pcm_call_start_rate_renegotiation(struct pcm_call *call)
{
	if (!pcm_call_is_valid(call))
		return PCM_CALL_INVALID_ARGUMENT;
	if (!uses_pcm_upstream(call) || !pcm_call_is_online(call) || call->retrain_active)
		return PCM_CALL_INVALID_STATE;
	struct pcm_call next = *call;
	enum pcm_call_result result = call->local_role == V90_ANALOG_MODEM ?
		begin_v92_analog_rate_request(&next) : begin_v92_rate_request(&next, true);
	if (result == PCM_CALL_OK)
		result = update_rate_renegotiation_deadline(&next);
	if (result != PCM_CALL_OK)
		return result;
	if (!pcm_call_is_valid(&next))
		return PCM_CALL_NUMERIC_FAILURE;
	*call = next;
	return PCM_CALL_OK;
}

static bool begin_v92_analog_fast(struct pcm_call *call)
{
	struct pcm_call_v92_analog_data *path = &call->state.data_path.v92_analog;
	struct pcm_call_v92_analog_fast fast = {
		.mapping = path->upstream.encoder.mapping,
		.deadline_remaining = b1_deadline_limit(call),
	};
	struct v92_cpu_parameters cpu = path->cpu;
	struct v92_suvu_parameters suvu = call->v92_analog_phase4_configuration.suvu;
	cpu.acknowledges_cpd = false;
	/* V.92 9.9: fast exchange keeps the trained line; SUVu bit 32 is clear. */
	suvu.request_silence = false;
	if (!v92_upstream_control_init(&fast.control, &suvu, &cpu) ||
	    v92_upstream_data_tx_start_rm(&path->upstream) != V92_UPSTREAM_DATA_TX_OK)
		return false;
	path->fast = fast;
	call->retrain_active = true;
	return true;
}

static bool v92_fast_upstream_bits(void *context, uint8_t *bits, size_t count)
{
	struct pcm_call_v92_analog_fast *fast = context;
	if (fast->record_index == fast->record_bits) {
		if (fast->cleardown.active) {
			const struct v92_cpus_parameters zero = { .acknowledges_cpd = fast->cleardown.peer_requested };
			if (v92_cpus_encode(&zero, count, fast->record, sizeof(fast->record), &fast->record_bits) != V92_CPU_OK)
				return false;
			fast->zero_record = true;
			fast->zero_ack = zero.acknowledges_cpd;
		} else if (!v92_upstream_control_start(&fast->control, count, fast->record,
			sizeof(fast->record), &fast->record_bits))
			return false;
		fast->record_index = 0u;
	}
	if (count > fast->record_bits - fast->record_index)
		return false;
	for (size_t bit = 0; bit < count; ++bit)
		bits[bit] = packed_msb_bit(fast->record, fast->record_index++);
	return true;
}

static enum pcm_call_result v92_fast_analog_transmit(struct pcm_call *call,
	const struct pcm_call_data_sources *sources, int16_t *sample)
{
	struct pcm_call_v92_analog_data *path = &call->state.data_path.v92_analog;
	struct pcm_call_v92_analog_fast *fast = &path->fast;
	bool boundary = fast->rm_complete && !path->upstream.queue_count &&
		fast->record_index == fast->record_bits;
	if (boundary && fast->cleardown.active && v92_cleardown_is_ready(&fast->cleardown, fast->transmitted_symbols))
		return PCM_CALL_CLEARDOWN_COMPLETE;
	if (boundary && !fast->finish_started && !fast->cleardown.active &&
	    v92_upstream_control_can_finish(&fast->control)) {
		if (v92_upstream_data_tx_start_e2u(&path->upstream) != V92_UPSTREAM_DATA_TX_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		fast->finish_started = true;
	}
	enum v92_upstream_data_tx_result result = v92_upstream_data_tx_next(&path->upstream,
		fast->b1_started ? sources->pcm_upstream : v92_fast_upstream_bits,
		fast->b1_started ? sources->pcm_upstream_context : fast, sample);
	if (result == V92_UPSTREAM_DATA_TX_SOURCE_FAILURE)
		return fast->b1_started ? PCM_CALL_SOURCE_FAILURE : PCM_CALL_NUMERIC_FAILURE;
	if (result != V92_UPSTREAM_DATA_TX_OK && result != V92_UPSTREAM_DATA_TX_RM_COMPLETE &&
	    result != V92_UPSTREAM_DATA_TX_FB1U_COMPLETE)
		return PCM_CALL_NUMERIC_FAILURE;
	fast->transmitted_symbols++;
	if (result == V92_UPSTREAM_DATA_TX_RM_COMPLETE)
		fast->rm_complete = true;
	if (result == V92_UPSTREAM_DATA_TX_FB1U_COMPLETE) {
		if (v92_upstream_data_tx_init(&path->upstream, &fast->mapping, path->upstream.upstream_level) != V92_UPSTREAM_DATA_TX_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		fast->b1_started = true;
	}
	bool record_complete = fast->rm_complete && !fast->finish_started && fast->record_bits &&
		fast->record_index == fast->record_bits && !path->upstream.queue_count;
	if (record_complete) {
		if (fast->zero_record)
			v92_cleardown_sent(&fast->cleardown, fast->transmitted_symbols, fast->zero_ack);
		else {
			if (!v92_upstream_control_complete(&fast->control))
				return PCM_CALL_NUMERIC_FAILURE;
			if (fast->control.record == V92_UPSTREAM_CONTROL_CPU && !fast->control.peer_confirmation_received &&
			    fast->control.retry == V92_UPSTREAM_RETRY_IDLE) {
				fast->repeat_remaining = V92_CPU_REPEAT_BASE_SAMPLES + call->round_trip_delay_samples;
				return PCM_CALL_OK;
			}
		}
	}
	if (fast->control.peer_confirmation_received || fast->control.retry != V92_UPSTREAM_RETRY_IDLE || fast->cleardown.active)
		fast->repeat_remaining = 0u;
	if (fast->repeat_remaining && --fast->repeat_remaining == 0u &&
	    !v92_upstream_control_expire_cpu_wait(&fast->control))
		return PCM_CALL_NUMERIC_FAILURE;
	return PCM_CALL_OK;
}

static bool v92_fast_analog_receive(struct pcm_call *call, uint8_t octet)
{
	struct pcm_call_v92_analog_data *path = &call->state.data_path.v92_analog;
	struct pcm_call_v92_analog_fast *fast = &path->fast;
	struct pcm_downstream_event event;
	struct pcm_downstream_rx *receive = &path->downstream.training;
	/* Ed may supply the peer's acknowledgment. Arm its aligned search after
	 * our acknowledged record has drained, even if all peer ack bits were lost.
	 * Repeated whole control records remain legal while this search is armed. */
	if (!fast->cleardown.active && !receive->ed_requested && fast->control.received_cpd &&
	    fast->control.acknowledgment_sent && receive->stream.pending_count == 0u &&
	    receive->control.v92.stage == V92_CONTROL_RX_SEARCH_SYNC &&
	    !receive->control.v92.bit_index && !receive->control.v92.sync_one_count &&
	    pcm_downstream_rx_expect_ed(receive) != PCM_DOWNSTREAM_RX_OK)
		return false;
	enum pcm_downstream_rx_result result = pcm_downstream_rx_receive(&path->downstream.training, octet, &event);
	bool ready = v92_upstream_control_can_finish(&fast->control);
	bool ack = false;
	/* An unresponsive peer cannot stop an accepted zero-rate tail. Only a
	 * decoded CPd affects its acknowledgement; the transmitter owns the bound. */
	if (fast->cleardown.active && result != PCM_DOWNSTREAM_RX_CPD)
		return true;
	if (result == PCM_DOWNSTREAM_RX_CPD) {
		struct v92_cpd_parameters selected = event.control.record.cpd.parameters;
		if (!selected.data_rate_number)
			v92_cleardown_start(&fast->cleardown, call->round_trip_delay_samples, false);
		if (fast->cleardown.active) {
			v92_cleardown_receive(&fast->cleardown, !selected.data_rate_number,
				selected.acknowledges_cpu, fast->transmitted_symbols);
			return true;
		}
		if (selected.extend_e2u || !v92_pcm_dil_rate_is_enabled(
			call->phase3_configuration.v92_analog.upstream_rate_mask, selected.data_rate_number))
			return false;
		/* Parameter-mode CPd may inherit omitted sections from the preceding
		 * selection, but cannot change a selection already acknowledged here. */
		const struct v92_cpd_parameters *old = &fast->mapping.cpd;
		if (!selected.modulus_present)
			memcpy(selected.modulus, old->modulus, sizeof(selected.modulus));
		if (!selected.filters_present)
			memcpy(selected.filter, old->filter, sizeof(selected.filter));
		if (!selected.constellations_present) {
			memcpy(selected.constellation, old->constellation, sizeof(selected.constellation));
			memcpy(selected.frame_constellation, old->frame_constellation, sizeof(selected.frame_constellation));
		}
		selected.modulus_present = selected.filters_present = selected.constellations_present = true;
		struct v92_pcm_mapping mapping;
		ack = selected.acknowledges_cpu;
		selected.acknowledges_cpu = old->acknowledges_cpu;
		if (v92_pcm_mapping_init(&mapping, call->law, &selected) != V92_PCM_MAPPING_OK ||
		    (fast->control.received_cpd && !v92_pcm_mapping_equal(&mapping, &fast->mapping)) ||
		    (!ready && !v92_upstream_control_note_cpd(&fast->control, ack)))
			return false;
		fast->mapping = mapping;
	} else if (result == PCM_DOWNSTREAM_RX_SUVD) {
		const struct v92_suvd_parameters *suvd = &event.control.record.suvd.parameters;
		if (suvd->request_silence || (!ready && !v92_upstream_control_note_suvd(&fast->control, suvd)))
			return false;
		ack = suvd->acknowledges_cpu;
	} else if (result == PCM_DOWNSTREAM_RX_ED) {
		return ready || v92_upstream_control_note_ed(&fast->control);
	} else if (result != PCM_DOWNSTREAM_RX_BUFFERING && result != PCM_DOWNSTREAM_RX_SUVD_REJECTED &&
		result != PCM_DOWNSTREAM_RX_CPD_REJECTED)
		return false;
	return !ack || path->downstream.training.ed_requested ||
		pcm_downstream_rx_expect_ed(&path->downstream.training) == PCM_DOWNSTREAM_RX_OK;
}

static bool begin_v92_fast_response(struct pcm_call *call)
{
	struct pcm_call_v92_digital_data *path = &call->state.data_path.v92_digital;
	struct pcm_call_v92_fast_exchange fast = {
		.stage = PCM_CALL_V92_FAST_WAIT_PRIME,
		/* Local bounded-recovery policy, not an FPE-specific normative
		 * timer: allow the ordinary V.92 B1 budget from qualified RM. */
		.deadline_remaining = b1_deadline_limit(call),
	};
	struct pcm_downstream_tx transmit;
	struct v92_cpd_parameters cpd = path->upstream.decoder.mapping.cpd;
	const struct v92_suvd_parameters suvd = { 0 };
	const struct v90_pcm_mapping *mapping = &path->downstream.data.transmitter.encoder.mapping;
	cpd.acknowledges_cpu = false;
	cpd.extend_e2u = false;
	if (pcm_downstream_tx_init(&transmit, PCM_PROTOCOL_V92, mapping) != PCM_DOWNSTREAM_TX_OK ||
	    !v92_downstream_control_init(&fast.control, &transmit, &suvd, &cpd, &path->cpu,
		call->round_trip_delay_samples) || !v92_downstream_control_ready(&fast.control) ||
	    v92_control_rx_init_upstream(&fast.receive, path->upstream.decoder.mapping.data_bits_per_frame) != V92_CONTROL_RX_OK ||
	    pcm_r_tx_init(&fast.rf, mapping, PCM_R_RF) != PCM_R_TX_OK)
		return false;
	path->fast = fast;
	path->quality_guard_samples = 0u;
	path->pending_squared_error = 0.0;
	path->pending_ambiguous = false;
	path->pending_uncertain = false;
	call->retrain_active = true;
	return true;
}

enum pcm_call_result pcm_call_start_fast_exchange(struct pcm_call *call)
{
	if (!pcm_call_is_valid(call))
		return PCM_CALL_INVALID_ARGUMENT;
	if (!uses_pcm_upstream(call) || !pcm_call_is_online(call) || call->retrain_active)
		return PCM_CALL_INVALID_STATE;
	struct pcm_call next = *call;
	if (call->local_role == V90_ANALOG_MODEM) {
		if (!begin_v92_analog_fast(&next))
			return PCM_CALL_NUMERIC_FAILURE;
	} else {
		if (!begin_v92_fast_response(&next))
			return PCM_CALL_NUMERIC_FAILURE;
		next.state.data_path.v92_digital.fast.stage = PCM_CALL_V92_FAST_DRAIN;
	}
	if (!pcm_call_is_valid(&next))
		return PCM_CALL_NUMERIC_FAILURE;
	*call = next;
	return PCM_CALL_OK;
}

static enum pcm_call_result v92_fast_transmit(struct pcm_call_v92_digital_data *path,
	const struct pcm_call_data_sources *sources, uint8_t *generated)
{
	struct pcm_call_v92_fast_exchange *fast = &path->fast;
	struct v90_pcm_symbol symbol;
	if (fast->stage == PCM_CALL_V92_FAST_DRAIN) {
		enum v90_downstream_data_stream_result r = v90_downstream_data_stream_finish(&path->downstream.data, &symbol);
		if (r == V90_DOWNSTREAM_DATA_STREAM_OK) {
			*generated = symbol.octet;
			return PCM_CALL_OK;
		}
		if (r != V90_DOWNSTREAM_DATA_STREAM_COMPLETE)
			return PCM_CALL_NUMERIC_FAILURE;
		fast->stage = PCM_CALL_V92_FAST_RF;
	}
	if (fast->stage == PCM_CALL_V92_FAST_RF) {
		if (pcm_r_tx_next(&fast->rf, &symbol) != PCM_R_TX_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		*generated = symbol.octet;
		if (pcm_r_tx_is_complete(&fast->rf))
			fast->stage = PCM_CALL_V92_FAST_CONTROL;
		return PCM_CALL_OK;
	}
	if (fast->stage == PCM_CALL_V92_FAST_CONTROL) {
		if (!v92_downstream_control_transmit(&fast->control, generated))
			return PCM_CALL_SEQUENCE_MISMATCH;
		return fast->control.transmit.complete ? PCM_CALL_CLEARDOWN_COMPLETE : PCM_CALL_OK;
	}
	enum v90_downstream_data_stream_result result = v92_downstream_transmit_owned(
		&path->downstream, &path->downstream_stage, &path->cpu,
		fast->stage == PCM_CALL_V92_FAST_FINISH ? sources->downstream : NULL,
		sources->downstream_context, generated);
	return result == V90_DOWNSTREAM_DATA_STREAM_OK ? PCM_CALL_OK :
		result == V90_DOWNSTREAM_DATA_STREAM_SOURCE_FAILURE ? PCM_CALL_SOURCE_FAILURE : PCM_CALL_NUMERIC_FAILURE;
}

static bool v92_fast_receive(struct pcm_call *call, const struct v92_upstream_data_frame *frame)
{
	struct pcm_call_v92_digital_data *path = &call->state.data_path.v92_digital;
	struct pcm_call_v92_fast_exchange *fast = &path->fast;
	if (frame->rm == V92_PCM_FRAME_RM_PRIME && fast->stage == PCM_CALL_V92_FAST_WAIT_PRIME)
		fast->stage = PCM_CALL_V92_FAST_DRAIN;
	if (!fast->rm_complete && frame->rm == V92_PCM_FRAME_RM_COMPLETE) {
		if (fast->stage == PCM_CALL_V92_FAST_WAIT_PRIME ||
		    v92_upstream_data_rx_reset_control(&path->upstream) != V92_UPSTREAM_DATA_RX_OK)
			return false;
		fast->rm_complete = true;
	}
	if (!fast->rm_complete || !frame->bit_count || fast->b1_started)
		return true;
	bool zeros = true, ones = true;
	for (size_t bit = 0; bit < frame->bit_count; ++bit) {
		zeros &= frame->bits[bit] == 0u;
		ones &= frame->bits[bit] == 1u;
	}
	if (fast->b1_frame)
		return !frame->ambiguous && ones;
	/* E2u is one aligned zero frame after the current control record. It can
	 * supply the final acknowledgment, but never an initial CPu selection. */
	if (!frame->ambiguous && fast->control.cpu_received &&
	    (v92_downstream_control_is_ed_ready(&fast->control) ||
	     (fast->control.acknowledgement_end_frame && fast->control.transmit.transmitted_symbols >=
		fast->control.acknowledgement_end_frame * V90_CP_FRAME_INTERVALS)) &&
	    fast->receive.stage == V92_CONTROL_RX_SEARCH_SYNC && zeros) {
		struct v92_control_event acknowledged = { .type = V92_CONTROL_EVENT_SUVU };
		acknowledged.record.suvu.parameters = fast->control.suvu;
		acknowledged.record.suvu.parameters.acknowledges_cpd = true;
		if (!v92_downstream_control_receive(&fast->control, &acknowledged))
			return false;
		if (path->upstream.line.received_frames > UINT64_MAX - (V92_B1U_FRAMES - V92_PCM_RX_DELAY_FRAMES))
			return false;
		fast->b1_frame = path->upstream.line.received_frames + V92_B1U_FRAMES - V92_PCM_RX_DELAY_FRAMES;
		return true;
	}
	if (frame->ambiguous)
		return v92_control_rx_init_upstream(&fast->receive, path->upstream.decoder.mapping.data_bits_per_frame) == V92_CONTROL_RX_OK;
	for (size_t bit = 0; bit < frame->bit_count; ++bit) {
		struct v92_control_event event;
		if (v92_control_rx_receive_bit(&fast->receive, frame->bits[bit], &event) != V92_CONTROL_RX_OK ||
		    !v92_downstream_control_receive(&fast->control, &event))
			return false;
		if (fast->control.cpu_received &&
		    (!downstream_rate_is_enabled(call->enabled_downstream_rate_mask, fast->control.cpu.data_rate_number) ||
		     fast->control.cpu.spectral_lookahead_frames > call->phase3_configuration.v92_digital.jd.maximum_spectral_shaping_lookahead))
			return false;
	}
	return true;
}

static double v92_quality_error(const struct pcm_call_v92_digital_data *path,
	double squared_error)
{
	/* Remove only measured post-codec host gain, never mapping or analogue
	 * channel gain. Keep the frame's original residual as diagnostic evidence. */
	double gain = path->upstream.line.host.gain;
	return gain > 0.0 ? squared_error / gain / gain : squared_error;
}

static enum pcm_call_result observe_v92_data_quality(
	struct pcm_call *call, const struct v92_upstream_data_frame *frame,
	struct pcm_call_data_event *event)
{
	struct pcm_call_v92_digital_data *path = &call->state.data_path.v92_digital;
	struct v92_data_quality next = call->pcm_upstream_quality;
	enum v92_data_quality_result quality;

	if (path->quality_guard_samples)
		return PCM_CALL_OK;
	double error = v92_quality_error(path, frame->squared_error);
	quality = v92_data_quality_observe(&next, error,
		frame->ambiguous, frame->uncertain);
	if (quality == V92_DATA_QUALITY_OK) {
		call->pcm_upstream_quality = next;
		event->frame.pcm_upstream = *frame;
		event->kind = PCM_CALL_PCM_UPSTREAM_FRAME;
	} else if (quality == V92_DATA_QUALITY_RETRAIN || quality == V92_DATA_QUALITY_EXHAUSTED) {
		path->pending_squared_error = error;
		path->pending_ambiguous = frame->ambiguous;
		path->pending_uncertain = frame->uncertain;
		path->quality_guard_samples = V92_CONTROL_QUALITY_GUARD_SAMPLES;
	} else {
		return PCM_CALL_NUMERIC_FAILURE;
	}
	return PCM_CALL_OK;
}

static enum pcm_call_result v92_data_octet_owned(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	int16_t host_sample, bool received_available,
	const struct pcm_call_data_sources *sources, uint8_t *generated,
	struct pcm_call_data_event *emitted)
{
	if (call->local_role == V90_ANALOG_MODEM) {
		struct pcm_call_v92_analog_data *path = &call->state.data_path.v92_analog;
		struct v90_pcm_stream_frame frame;
		enum v90_downstream_data_rx_result received;
		enum v92_upstream_data_tx_result transmitted;
		int16_t sample;

		if (path->fast.deadline_remaining) {
			enum pcm_call_result result = v92_fast_analog_transmit(call, sources, &sample);
			if (result != PCM_CALL_OK)
				return result;
			if (--path->fast.deadline_remaining == 0u)
				return path->fast.cleardown.active ? PCM_CALL_CLEARDOWN_COMPLETE : begin_retrain(call, false);
		} else {
			transmitted = v92_upstream_data_tx_next(&path->upstream,
				sources->pcm_upstream, sources->pcm_upstream_context, &sample);
			if (transmitted == V92_UPSTREAM_DATA_TX_SOURCE_FAILURE)
				return PCM_CALL_SOURCE_FAILURE;
			if (transmitted != V92_UPSTREAM_DATA_TX_OK)
				return PCM_CALL_NUMERIC_FAILURE;
		}
		*generated = encode_sample(call->law, sample);
		if (!condition_downstream(call, &received_octet))
			return PCM_CALL_NUMERIC_FAILURE;
		bool suppress_frame = false;
		if (data_path_is_online_owned(call) || path->fast.deadline_remaining) {
			enum pcm_r_rx_result rd = pcm_r_rx_receive(&path->rd, received_octet);
			if (rd == PCM_R_RX_BAR_STARTED)
				return begin_v92_analog_rate_request(call);
			if (rd != PCM_R_RX_BUFFERING && rd != PCM_R_RX_SIGNAL_DETECTED &&
			    rd != PCM_R_RX_SEQUENCE_MISMATCH)
				return PCM_CALL_SEQUENCE_MISMATCH;
			suppress_frame = pcm_r_rx_signal_detected(&path->rd);
			if (!pcm_r_rx_is_complete(&path->rf)) {
				enum pcm_r_rx_result rf = pcm_r_rx_receive(&path->rf, received_octet);
				if (rf != PCM_R_RX_BUFFERING && rf != PCM_R_RX_SIGNAL_DETECTED &&
				    rf != PCM_R_RX_BAR_STARTED && rf != PCM_R_RX_BAR_COMPLETE && rf != PCM_R_RX_SEQUENCE_MISMATCH)
					return PCM_CALL_SEQUENCE_MISMATCH;
				suppress_frame |= pcm_r_rx_signal_detected(&path->rf);
				if (rf == PCM_R_RX_BAR_STARTED && !path->fast.deadline_remaining && !begin_v92_analog_fast(call))
					return PCM_CALL_NUMERIC_FAILURE;
				if (rf == PCM_R_RX_BAR_COMPLETE) {
					struct pcm_downstream_rx control;
					if (!path->fast.deadline_remaining || pcm_downstream_rx_init(&control, PCM_PROTOCOL_V92,
						&path->rf.mapping, V90_PCM_MAPPING_TRANSMIT) != PCM_DOWNSTREAM_RX_OK)
						return PCM_CALL_NUMERIC_FAILURE;
					path->downstream = (union v92_phase4_e2u_analog_downstream){ .training = control };
					path->downstream_stage = V92_PHASE4_E2U_DOWNSTREAM_TRAINING;
					return PCM_CALL_OK;
				}
			}
		}
		if (path->fast.deadline_remaining) {
			if (!pcm_r_rx_is_complete(&path->rf))
				return PCM_CALL_OK;
			if (path->downstream_stage == V92_PHASE4_E2U_DOWNSTREAM_TRAINING &&
			    !pcm_downstream_rx_is_complete(&path->downstream.training))
				return v92_fast_analog_receive(call, received_octet) ? PCM_CALL_OK : begin_retrain(call, false);
		}
		received = v92_downstream_receive_owned(&path->downstream,
			&path->downstream_stage, &path->cpu, &path->upstream.encoder.mapping,
			received_octet, &frame);
		if (received == V90_DOWNSTREAM_DATA_RX_DATA_FRAME) {
			if (!suppress_frame) {
				emitted->frame.downstream = frame;
				emitted->kind = PCM_CALL_DOWNSTREAM_FRAME;
			}
		} else if (received != V90_DOWNSTREAM_DATA_RX_BUFFERING &&
			   received != V90_DOWNSTREAM_DATA_RX_B1D_FRAME &&
			   received != V90_DOWNSTREAM_DATA_RX_B1D_COMPLETE) {
			return PCM_CALL_SEQUENCE_MISMATCH;
		}
		if (path->fast.deadline_remaining && data_path_is_online_owned(call)) {
			if (pcm_r_rx_init(&path->rf, &path->rd.mapping, V90_PCM_MAPPING_TRANSMIT, PCM_R_RF) != PCM_R_RX_OK)
				return PCM_CALL_NUMERIC_FAILURE;
			path->fast = (struct pcm_call_v92_analog_fast){ 0 };
		}
	} else {
		struct pcm_call_v92_digital_data *path = &call->state.data_path.v92_digital;
		struct v92_upstream_data_frame frame;
		enum v92_upstream_data_rx_result received;
		enum v92_ru_rx_result ru;
		enum v90_downstream_data_stream_result transmitted;

		if (path->fast.stage != PCM_CALL_V92_FAST_IDLE) {
			enum pcm_call_result result = v92_fast_transmit(path, sources, generated);
			if (result != PCM_CALL_OK)
				return result;
			if (--path->fast.deadline_remaining == 0u) {
				*generated = encode_sample(call->law, 0);
				if (path->fast.control.cleardown.active)
					return PCM_CALL_CLEARDOWN_COMPLETE;
				return begin_retrain(call, false);
			}
			if (path->fast.b1_frame && !path->fast.b1_started &&
			    path->upstream.line.received_frames == path->fast.b1_frame && !path->upstream.line.symbol_count) {
				struct v92_pcm_mapping mapping = path->upstream.decoder.mapping;
				struct v92_upstream_plan plan = path->upstream.line.plan;
				mapping.cpd.extend_e2u = false;
				if (v92_upstream_data_rx_start_b1u(&path->upstream, &mapping, &plan) != V92_UPSTREAM_DATA_RX_OK ||
				    !v92_data_quality_restart(&call->pcm_upstream_quality))
					return PCM_CALL_NUMERIC_FAILURE;
				path->b1u_squared_error = 0.0;
				path->b1u_decode_failed = false;
				path->fast.b1_started = true;
			}
		} else {
			transmitted = v92_downstream_transmit_owned(&path->downstream,
				&path->downstream_stage, &path->cpu, sources->downstream,
				sources->downstream_context, generated);
			if (transmitted == V90_DOWNSTREAM_DATA_STREAM_SOURCE_FAILURE)
				return PCM_CALL_SOURCE_FAILURE;
			if (transmitted != V90_DOWNSTREAM_DATA_STREAM_OK)
				return PCM_CALL_NUMERIC_FAILURE;
		}
		/* Concealed media is not a received PCM zero. An erased symbol can
		 * eliminate the true survivor, including its delayed payload history,
		 * before a later score tie exposes the uncertainty. Clamp delivery
		 * immediately and let the existing control guard and retry budget
		 * own recovery. Repeated loss must not extend that bounded guard.
		 * Apply this only after transmit succeeds: SOURCE_FAILURE consumes
		 * neither this sample nor its loss notification. */
		if (!received_available && !path->upstream.line.delay_remaining) {
			if (!v92_peer_b1_received_owned(call))
				path->b1u_decode_failed = true;
			else if (!path->quality_guard_samples) {
				path->pending_ambiguous = true;
				path->quality_guard_samples = V92_CONTROL_QUALITY_GUARD_SAMPLES;
			}
		}
		/* A lost decoder cannot resume on its old coding history. Keep
		 * transmitting and clocking the independent control detectors while
		 * the bounded online guard owns that failure; never emit its data. */
		received = path->upstream.failed ? V92_UPSTREAM_DATA_RX_OK :
			v92_upstream_data_rx_octet_owned(&path->upstream,
				received_octet, received_sample, host_sample, received_available, &frame);
		if (received == V92_UPSTREAM_DATA_RX_NO_PATH) {
			/* A noise burst can eliminate every valid coded word. The
			 * failed decoder cannot continue, including during B1u or a
			 * fast exchange. Use the strictly decreasing initial-training
			 * ceiling, or the quality retry budget for established calls
			 * and fixed mappings. Never invent the discarded payload. */
			if (path->fast.control.cleardown.active)
				return PCM_CALL_CLEARDOWN_COMPLETE;
			if (call->b1_deadline_armed &&
			    call->v92_digital_phase4_configuration.upstream_selection == PCM_CALL_V92_UPSTREAM_DIRECT)
				return retry_v92_upstream(call);
			if (call->b1_deadline_armed || path->fast.stage != PCM_CALL_V92_FAST_IDLE) {
				enum v92_data_quality_result quality = v92_data_quality_observe(
					&call->pcm_upstream_quality, 0.0, true, false);
				return quality == V92_DATA_QUALITY_RETRAIN ? retry_v92_upstream(call) :
					quality == V92_DATA_QUALITY_EXHAUSTED ? PCM_CALL_FAILED : PCM_CALL_NUMERIC_FAILURE;
			}
			/* A full-retrain onset can also eliminate every path. Give it
			 * the same control priority as ambiguous data without extending
			 * an already running guard or spending the quality retry budget. */
			path->pending_ambiguous = true;
			if (!path->quality_guard_samples)
				path->quality_guard_samples = V92_CONTROL_QUALITY_GUARD_SAMPLES;
			received = V92_UPSTREAM_DATA_RX_OK;
		}
		if (received != V92_UPSTREAM_DATA_RX_FRAME && received != V92_UPSTREAM_DATA_RX_OK)
			return PCM_CALL_NUMERIC_FAILURE;
		ru = v92_ru_line_rx_sample(&path->ru, received_sample);
		if (ru == V92_RU_RX_DETECTED && data_path_is_online_owned(call))
			return begin_v92_rate_request(call, false);
		if (path->fast.stage != PCM_CALL_V92_FAST_IDLE && path->fast.stage != PCM_CALL_V92_FAST_FINISH &&
		    !path->fast.control.cleardown.active && path->ru.signal.run == V92_RU_RX_MIN_SYMBOLS) {
			/* Stop interpreting Ru as RM/control, but keep the independent
			 * transmitter running until the qualified Ru-to-Ru-bar edge. */
			return ru == V92_RU_RX_BAR_STARTED ? begin_v92_rate_request(call, false) : PCM_CALL_OK;
		}
		if (ru != V92_RU_RX_BUFFERING && ru != V92_RU_RX_MISMATCH && ru != V92_RU_RX_DETECTED)
			return PCM_CALL_SEQUENCE_MISMATCH;
		if (received == V92_UPSTREAM_DATA_RX_FRAME && path->fast.stage == PCM_CALL_V92_FAST_IDLE &&
		    frame.rm == V92_PCM_FRAME_RM_SIGNAL && data_path_is_online_owned(call) &&
		    !begin_v92_fast_response(call))
			return PCM_CALL_NUMERIC_FAILURE;
		if (path->fast.stage != PCM_CALL_V92_FAST_IDLE) {
			if ((received == V92_UPSTREAM_DATA_RX_FRAME && !v92_fast_receive(call, &frame)) ||
			    ru == V92_RU_RX_DETECTED) {
				*generated = encode_sample(call->law, 0);
				return begin_retrain(call, false);
			}
			if (path->fast.stage == PCM_CALL_V92_FAST_CONTROL &&
			    v92_downstream_control_is_ed_ready(&path->fast.control)) {
				struct v92_downstream_control *control = &path->fast.control;
				if (!v92_phase4_ed_tx_init(&path->downstream.training, &control->transmit,
					control->transmit_stage == V92_DOWNSTREAM_CONTROL_CPD ? &control->initial_cpd : NULL))
					return PCM_CALL_NUMERIC_FAILURE;
				path->cpu = control->cpu;
				path->downstream_stage = V92_PHASE4_E2U_DOWNSTREAM_TRAINING;
				path->fast.stage = PCM_CALL_V92_FAST_FINISH;
			}
			if (!path->fast.b1_started)
				return PCM_CALL_OK;
			if (data_path_is_online_owned(call))
				path->fast = (struct pcm_call_v92_fast_exchange){ 0 };
		}
		if (path->quality_guard_samples && --path->quality_guard_samples == 0u) {
			enum v92_data_quality_result quality = v92_data_quality_observe(
				&call->pcm_upstream_quality, path->pending_squared_error,
				path->pending_ambiguous, path->pending_uncertain);

			path->pending_squared_error = 0.0;
			path->pending_ambiguous = false;
			path->pending_uncertain = false;
			if (quality == V92_DATA_QUALITY_RETRAIN)
				return retry_v92_upstream(call);
			return quality == V92_DATA_QUALITY_EXHAUSTED ? PCM_CALL_FAILED : PCM_CALL_NUMERIC_FAILURE;
		}
		if (received == V92_UPSTREAM_DATA_RX_FRAME) {
			/* The first half fits the receive model. Score only the
			 * following validation frames: a corrected acquisition error
			 * must not force another retrain on a now-usable channel. */
			if (path->upstream.line.received_frames > V92_B1U_ACQUISITION_FRAMES &&
			    path->upstream.line.received_frames <= V92_B1U_FRAMES) {
				double error = path->b1u_squared_error + v92_quality_error(path, frame.squared_error);

				if (!isfinite(error))
					return PCM_CALL_NUMERIC_FAILURE;
				path->b1u_squared_error = error;
			}
			if (frame.b1u && frame.bit_count != 0u) {
				path->b1u_decode_failed |= frame.ambiguous;
				for (size_t bit = 0u; bit < frame.bit_count; ++bit)
					path->b1u_decode_failed |= frame.bits[bit] != 1u;
			}
			/* The decoder's lookahead can still deliver B1u bits after
			 * the physical B1 boundary. Never expose them as host data. */
			if (v92_peer_b1_received_owned(call) &&
			    frame.bit_count != 0u && !frame.b1u) {
				return observe_v92_data_quality(call, &frame, emitted);
			}
		}
	}
	return PCM_CALL_OK;
}

static enum pcm_call_result data_octet_owned(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	int16_t host_sample, bool received_available,
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
	/* One G.711 zero can be a negotiated PCM symbol. Qualify quiet from
	 * the shared signal window, not from an individual network octet. */
	retrain_candidate = retrain_detection_active &&
		retrain_candidate_is_active(
			&call->retrain.detector,
			retrain_silence_ceiling(
				call, 256u * 256u *
				      V34_RETRAIN_SILENCE_WINDOW_SAMPLES));
	generated = encode_sample(call->law, 0);
	if (uses_pcm_upstream(call)) {
		result = v92_data_octet_owned(call, received_octet, received_sample, host_sample, received_available,
			sources, &generated, &emitted);
		if (retrain_candidate)
			emitted = (struct pcm_call_data_event){ 0 };
	} else if (call->local_role == V90_ANALOG_MODEM) {
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
	if (result == PCM_CALL_OK && pcm_call_uses_conventional_upstream(call) &&
	    call->stage == PCM_CALL_DATA_PATH &&
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
		 data_path_is_online_owned(call)) {
		call->retrain_active = false;
		call->v92_rate_renegotiation_active = false;
	}
	if (result == PCM_CALL_OK)
		result = advance_b1_deadline(call);
	if (result == PCM_CALL_OK &&
	    (call->stage == PCM_CALL_DATA_PATH || call->v92_rate_renegotiation_active))
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
	bool received_available,
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
		call, received_octet, received_sample, received_sample, received_available, sources,
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
	int16_t host_sample, bool received_available,
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
		call, received_octet, received_sample, host_sample, received_available, sources,
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
	if (!condition_downstream(call, &received_octet)) {
		call->failed = true;
		return PCM_CALL_NUMERIC_FAILURE;
	}
	generated = encode_sample(call->law, 0);
	emitted.processed = call->stage;
	switch (call->stage) {
	case PCM_CALL_CLOSED:
		result = PCM_CALL_CLEARDOWN_COMPLETE;
		break;
	case PCM_CALL_PHASE2:
		result = sample_phase2(
			call,
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
	result = recover_rate_renegotiation_failure(call, result);
	if (cleardown_is_active_owned(call)) {
		call->b1_deadline_armed = false;
		call->b1_deadline_remaining_samples = 0u;
	}
	if (result == PCM_CALL_OK && emitted.processed != call->stage &&
	    call->stage != PCM_CALL_RETRAIN_SILENCE)
		result = reset_retrain_detector(call);
	else if (result == PCM_CALL_OK && emitted.processed == call->stage &&
		 retrain_detection_is_active(call))
		result = observe_retrain_tone(call, received_sample);
	deadline_started = emitted.processed == PCM_CALL_PHASE2 &&
		is_info1a_boundary(call, &emitted.phase2);
	if (result == PCM_CALL_OK && !deadline_started)
		result = advance_b1_deadline(call);
	if (result == PCM_CALL_OK && call->v92_rate_renegotiation_active)
		result = update_rate_renegotiation_deadline(call);
	if (result == PCM_CALL_CLEARDOWN_COMPLETE)
		complete_cleardown(call);
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
