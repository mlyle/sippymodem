// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase3_analog_station.h"
#include "v90_profile.h"
#include "v90_phase3_sd.h"

#include <stddef.h>

enum {
	/* V.92 9.5.2.1.11: CPt within 5000 ms of the post-Jp Su reversal. */
	CPT_DEADLINE_SYMBOLS = 40000,
	CPT_MAX_TRN1U_SYMBOLS = (CPT_DEADLINE_SYMBOLS -
		V92_SU_JP_BAR_INPUT_SYMBOLS - V92_PCM_PHASE3_CPT_PREAMBLE_SYMBOLS - 1u) /
		V92_PCM_PHASE3_TRN1U_ALIGNMENT * V92_PCM_PHASE3_TRN1U_ALIGNMENT,
};

static bool receive_stage_is_valid(
	enum v92_phase3_analog_receive_stage stage)
{
	return stage >= V92_PHASE3_ANALOG_RECEIVE_SD &&
	       stage <= V92_PHASE3_ANALOG_RECEIVE_COMPLETE;
}

static bool transmit_stage_is_valid(
	enum v92_phase3_analog_transmit_stage stage)
{
	return stage >= V92_PHASE3_ANALOG_TRANSMIT_INITIAL &&
	       stage <= V92_PHASE3_ANALOG_TRANSMIT_TRN2U;
}

static bool receive_is_valid(
	const struct v92_phase3_analog_station *station)
{
	switch (station->receive_stage) {
	case V92_PHASE3_ANALOG_RECEIVE_SD:
		return pcm_s_pattern_rx_is_valid(&station->receive.sd) &&
		       station->receive.sd.signal_ucode ==
			       ((station->downstream_training_ucode +
			         V90_PHASE3_SD_TRAINING_OFFSET) & 0x7fu) &&
		       !pcm_s_pattern_rx_is_complete(&station->receive.sd);
	case V92_PHASE3_ANALOG_RECEIVE_J:
		return station->receive.j.protocol == PCM_PROTOCOL_V92 &&
		       pcm_phase3_j_receiver_is_valid(&station->receive.j) &&
		       station->receive.j.stage != PCM_PHASE3_J_RECEIVER_DONE;
	case V92_PHASE3_ANALOG_RECEIVE_RI:
		return v90_phase4_ri_search_is_valid(&station->receive.ri) &&
		       !v90_phase4_ri_search_is_complete(&station->receive.ri);
	case V92_PHASE3_ANALOG_RECEIVE_COMPLETE:
		return v90_phase4_ri_search_is_complete(&station->receive.ri);
	}
	return false;
}

static bool transmit_is_valid(
	const struct v92_phase3_analog_station *station)
{
	switch (station->transmit_stage) {
	case V92_PHASE3_ANALOG_TRANSMIT_INITIAL:
		return v92_pcm_phase3_initial_tx_is_valid(
			&station->transmit.initial) &&
		       (station->post_ja_silence_samples == 0u ||
			station->transmit.initial.stage == V92_PCM_PHASE3_INITIAL_TX_COMPLETE) &&
		       station->transmit.initial.law == station->law &&
		       station->transmit.initial.lu_ucode ==
			       station->upstream_lu_ucode &&
		       station->transmit.initial.upstream_rate_mask ==
			       station->upstream_rate_mask &&
		       station->transmit.initial.dil_profile == station->dil_profile;
	case V92_PHASE3_ANALOG_TRANSMIT_SU:
		return v92_su_is_valid(&station->transmit.su) &&
		       !v92_su_is_complete(&station->transmit.su) &&
		       station->transmit.su.stage != V92_SU_TRN1U_READY &&
		       station->transmit_delay.phase_q16 ==
			       station->transmit.su.phase_q16;
	case V92_PHASE3_ANALOG_TRANSMIT_CPT:
		return v92_pcm_phase3_cpt_tx_is_valid(&station->transmit.cpt) &&
		       !v92_pcm_phase3_cpt_tx_is_complete(
			       &station->transmit.cpt);
	case V92_PHASE3_ANALOG_TRANSMIT_TRN2U:
		return v92_pcm_phase4_trn2u_is_valid(&station->transmit.trn2u);
	}
	return false;
}

static bool coordination_is_valid(
	const struct v92_phase3_analog_station *station)
{
	switch (station->receive_stage) {
	case V92_PHASE3_ANALOG_RECEIVE_SD:
		return station->transmit_stage ==
		       V92_PHASE3_ANALOG_TRANSMIT_INITIAL;
	case V92_PHASE3_ANALOG_RECEIVE_J:
		switch (station->receive.j.stage) {
		case PCM_PHASE3_J_RECEIVER_TRN1D:
		case PCM_PHASE3_J_RECEIVER_JD_SEARCH:
			return station->transmit_stage ==
			       V92_PHASE3_ANALOG_TRANSMIT_INITIAL;
		case PCM_PHASE3_J_RECEIVER_JD:
			return station->transmit_stage ==
			       V92_PHASE3_ANALOG_TRANSMIT_SU;
		case PCM_PHASE3_J_RECEIVER_JP:
			return (station->transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_SU &&
				station->transmit.su.stage == V92_SU_JP_BAR_RESPONSE) ||
			       (station->transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_CPT &&
				station->transmit.cpt.stage == V92_PCM_PHASE3_CPT_TX_TRN1U);
		case PCM_PHASE3_J_RECEIVER_DONE:
			return false;
		}
		return false;
	case V92_PHASE3_ANALOG_RECEIVE_RI:
		return (station->transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_SU &&
			station->transmit.su.stage == V92_SU_JP_BAR_RESPONSE) ||
		       station->transmit_stage ==
			       V92_PHASE3_ANALOG_TRANSMIT_CPT ||
		       station->transmit_stage ==
			       V92_PHASE3_ANALOG_TRANSMIT_TRN2U;
	case V92_PHASE3_ANALOG_RECEIVE_COMPLETE:
		return station->transmit_stage ==
			       V92_PHASE3_ANALOG_TRANSMIT_CPT ||
		       station->transmit_stage ==
			       V92_PHASE3_ANALOG_TRANSMIT_TRN2U;
	}
	return false;
}

static bool phase4_constellation_is_valid(
	const struct v92_phase3_analog_station *station)
{
	switch (station->receive_stage) {
	case V92_PHASE3_ANALOG_RECEIVE_SD:
		return station->phase4_training_constellation == 0 &&
		       station->rate_renegotiation_constellation == 0;
	case V92_PHASE3_ANALOG_RECEIVE_J:
		switch (station->receive.j.stage) {
		case PCM_PHASE3_J_RECEIVER_TRN1D:
		case PCM_PHASE3_J_RECEIVER_JD_SEARCH:
		case PCM_PHASE3_J_RECEIVER_JD:
			return station->phase4_training_constellation == 0 &&
			       station->rate_renegotiation_constellation == 0;
		case PCM_PHASE3_J_RECEIVER_JP:
			return v92_constellation_size_is_valid(
				       station->phase4_training_constellation) &&
			       station->phase4_training_constellation ==
				       station->receive.j.parameters.v92.jp.training_constellation &&
			       station->rate_renegotiation_constellation ==
				       station->receive.j.parameters.v92.jp.rate_renegotiation_constellation;
		case PCM_PHASE3_J_RECEIVER_DONE:
			return false;
		}
		return false;
	case V92_PHASE3_ANALOG_RECEIVE_RI:
	case V92_PHASE3_ANALOG_RECEIVE_COMPLETE:
		return v92_constellation_size_is_valid(
			station->phase4_training_constellation) &&
		       v92_constellation_size_is_valid(station->rate_renegotiation_constellation);
	}
	return false;
}

bool v92_phase3_analog_station_is_valid(
	const struct v92_phase3_analog_station *station)
{
	return station != NULL &&
	       receive_stage_is_valid(station->receive_stage) &&
	       transmit_stage_is_valid(station->transmit_stage) &&
	       g711_law_is_valid(station->law) &&
	       station->upstream_lu_ucode != 0u &&
	       station->upstream_lu_ucode <= 127u &&
	       station->downstream_training_ucode >=
		       V90_INFO1A_MIN_TRAINING_UCODE &&
	       station->downstream_training_ucode <=
		       V90_INFO1A_MAX_TRAINING_UCODE &&
	       station->upstream_lu > 0 &&
	       station->upstream_rate_mask != 0u &&
	       station->second_trn1u_symbols >= V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS &&
	       station->second_trn1u_symbols <= CPT_DEADLINE_SYMBOLS &&
	       station->second_trn1u_symbols % V92_PCM_PHASE3_TRN1U_ALIGNMENT == 0u &&
	       v92_pcm_dil_rate_mask_is_valid(station->upstream_rate_mask) &&
	       v90_dil_profile_is_valid(station->dil_profile) &&
	       v92_cpt_parameters_are_valid(&station->cpt_parameters) &&
	       station->cpt_parameters.law == station->law &&
	       fractional_delay_is_valid(&station->transmit_delay) &&
	       phase4_constellation_is_valid(station) &&
	       receive_is_valid(station) && transmit_is_valid(station) &&
	       coordination_is_valid(station);
}

bool v92_phase3_analog_station_has_failed(
	const struct v92_phase3_analog_station *station)
{
	return v92_phase3_analog_station_is_valid(station) && station->failed;
}

bool v92_phase3_analog_station_configuration_is_valid(
	const struct v92_phase3_analog_station_configuration *configuration)
{
	return configuration != NULL &&
	       g711_law_is_valid(configuration->law) &&
	       configuration->upstream_lu_ucode != 0u &&
	       configuration->upstream_lu_ucode <= 127u &&
	       configuration->upstream_rate_mask != 0u &&
	       v90_dil_profile_is_valid(configuration->dil_profile) &&
	       v92_pcm_dil_rate_mask_is_valid(
		       configuration->upstream_rate_mask) &&
	       v92_pcm_info1a_is_valid(&configuration->local_info1a) &&
	       v92_cpt_parameters_are_valid(&configuration->cpt) &&
	       configuration->cpt.law == configuration->law &&
	       (!configuration->learn_dil ||
		(configuration->cpt.constellation_count == 1u &&
		 !configuration->cpt.codec_constellations_differ));
}

enum v92_phase3_analog_station_result v92_phase3_analog_station_init(
	struct v92_phase3_analog_station *station,
	const struct v92_phase3_analog_station_configuration *configuration)
{
	struct v92_phase3_analog_station initialized = { 0 };
	struct v90_dil_parameters dil;
	struct v90_pcm_symbol lu;

	if (station == NULL ||
	    !v92_phase3_analog_station_configuration_is_valid(configuration) ||
	    !v90_pcm_symbol_make(
		    &lu, configuration->law,
		    configuration->upstream_lu_ucode, true))
		return V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	if (v90_dil_profile_init(&dil, configuration->dil_profile) != V90_DIL_OK ||
	    !v90_dil_training_symbol_count(&dil, &initialized.second_trn1u_symbols))
		return V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	if (initialized.second_trn1u_symbols < V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS)
		initialized.second_trn1u_symbols = V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS;
	initialized.second_trn1u_symbols =
		(initialized.second_trn1u_symbols + V92_PCM_PHASE3_TRN1U_ALIGNMENT - 1u) /
		V92_PCM_PHASE3_TRN1U_ALIGNMENT * V92_PCM_PHASE3_TRN1U_ALIGNMENT;
	if (v92_pcm_phase3_initial_tx_init(
		    &initialized.transmit.initial, configuration->law,
		    configuration->upstream_lu_ucode,
		    &configuration->local_info1a,
		    configuration->upstream_rate_mask,
		    configuration->initial_trn1u_symbols,
		    configuration->dil_profile) !=
		    V92_PCM_PHASE3_INITIAL_TX_OK ||
	    pcm_s_pattern_rx_init(
		    &initialized.receive.sd, configuration->law,
		    (configuration->local_info1a.training_ucode +
		     V90_PHASE3_SD_TRAINING_OFFSET) & 0x7fu) !=
		    PCM_S_PATTERN_RX_OK)
		return V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	fractional_delay_init(&initialized.transmit_delay);
	initialized.learn_dil = configuration->learn_dil && dil.segment_count != 0;
	if (initialized.learn_dil &&
	    !pcm_dil_receiver_init(&initialized.dil_receiver, &dil, configuration->law,
		    configuration->local_info1a.training_ucode))
		return V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	initialized.restore_codec_grid = configuration->restore_codec_grid;
	initialized.cpt_parameters = configuration->cpt;
	initialized.upstream_rate_mask = configuration->upstream_rate_mask;
	initialized.dil_profile = configuration->dil_profile;
	initialized.receive_stage = V92_PHASE3_ANALOG_RECEIVE_SD;
	initialized.transmit_stage = V92_PHASE3_ANALOG_TRANSMIT_INITIAL;
	initialized.law = configuration->law;
	initialized.upstream_lu = lu.sample;
	initialized.upstream_lu_ucode = configuration->upstream_lu_ucode;
	initialized.downstream_training_ucode =
		configuration->local_info1a.training_ucode;
	if (!v92_phase3_analog_station_is_valid(&initialized))
		return V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	*station = initialized;
	return V92_PHASE3_ANALOG_STATION_OK;
}

bool v92_phase3_analog_station_transmit_complete(
	const struct v92_phase3_analog_station *station)
{
	return v92_phase3_analog_station_is_valid(station) &&
	       !station->failed &&
	       station->transmit_stage ==
		       V92_PHASE3_ANALOG_TRANSMIT_TRN2U &&
	       station->receive_stage == V92_PHASE3_ANALOG_RECEIVE_COMPLETE;
}

bool v92_phase3_analog_station_receive_complete(
	const struct v92_phase3_analog_station *station)
{
	return v92_phase3_analog_station_is_valid(station) &&
	       !station->failed &&
	       station->receive_stage == V92_PHASE3_ANALOG_RECEIVE_COMPLETE;
}

static enum v92_phase3_analog_station_result transmit_initial(
	struct v92_phase3_analog_station *station,
	const struct v90_pcm_symbol *md_symbol, int16_t *sample)
{
	struct v90_pcm_symbol symbol;
	enum v92_pcm_phase3_initial_tx_result result =
		v92_pcm_phase3_initial_tx_next(
			&station->transmit.initial, md_symbol, &symbol);

	if (result == V92_PCM_PHASE3_INITIAL_TX_NEED_MD_SYMBOL)
		return V92_PHASE3_ANALOG_STATION_NEED_MD_SYMBOL;
	if (result == V92_PCM_PHASE3_INITIAL_TX_DONE) {
		if (station->post_ja_silence_samples < UINT32_MAX)
			++station->post_ja_silence_samples;
		*sample = 0;
		return V92_PHASE3_ANALOG_STATION_OK;
	}
	if (result == V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT)
		return V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	if (result != V92_PCM_PHASE3_INITIAL_TX_OK)
		return V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	*sample = symbol.sample;
	return V92_PHASE3_ANALOG_STATION_OK;
}

static enum v92_phase3_analog_station_result transmit_su(
	struct v92_phase3_analog_station *station, int16_t *sample)
{
	enum v92_su_result result = v92_su_next(
		&station->transmit.su, &station->transmit_delay, sample);

	if (result == V92_SU_REACHED_TRN1U_READY) {
		struct v92_pcm_phase3_cpt_tx cpt;

		/* Figures 10 and 11/V.92: TRN1u follows the (24 + epsilon)T
		 * reversal immediately. Jp-prime controls downstream reception;
		 * waiting for it here would insert another, unrequested Su segment. */
		if (v92_su_finish(&station->transmit.su, &station->transmit_delay,
			    station->restore_codec_grid) != V92_SU_COMPLETE ||
		    v92_pcm_phase3_cpt_tx_init(&cpt, station->law,
			    station->upstream_lu_ucode, &station->cpt_parameters) !=
			    V92_PCM_PHASE3_CPT_TX_OK)
			return V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
		station->transmit = (union v92_phase3_analog_transmit){ .cpt = cpt };
		station->transmit_stage = V92_PHASE3_ANALOG_TRANSMIT_CPT;
		return V92_PHASE3_ANALOG_STATION_OK;
	}
	return result == V92_SU_OK || result == V92_SU_REACHED_JP_WAIT ?
		       V92_PHASE3_ANALOG_STATION_OK :
		       V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
}

static enum v92_phase3_analog_station_result transmit_cpt(
	struct v92_phase3_analog_station *station, int16_t *sample)
{
	struct v90_pcm_symbol symbol;

	if (station->dil_profile != V90_DIL_PROFILE_NONE &&
	    station->transmit.cpt.stage == V92_PCM_PHASE3_CPT_TX_TRN1U &&
	    station->transmit.cpt.trn1u.emitted_symbols >= CPT_MAX_TRN1U_SYMBOLS)
		return V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH;
	if (v92_pcm_phase3_cpt_tx_next(
		    &station->transmit.cpt, &symbol) !=
	    V92_PCM_PHASE3_CPT_TX_OK)
		return V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	*sample = symbol.sample;
	/* N=0 waits for Ri; nonzero DIL allows a training traversal, bounded
	 * at Jp-prime by the time left since the post-Jp Su reversal. */
	if (station->receive_stage == V92_PHASE3_ANALOG_RECEIVE_RI &&
	    station->transmit.cpt.stage == V92_PCM_PHASE3_CPT_TX_TRN1U &&
	    station->transmit.cpt.trn1u.emitted_symbols >= station->second_trn1u_symbols &&
	    (station->dil_profile != V90_DIL_PROFILE_NONE ||
	     v90_phase4_ri_search_is_acquired(&station->receive.ri)) &&
	    v92_pcm_phase3_trn1u_can_handoff(&station->transmit.cpt.trn1u)) {
		if (station->learn_dil) {
			struct v90_pcm_mapping mapping;
			uint32_t lower, limit;

			/* The call owner has already bounded this proposal by INFO0d.
			 * Fit measured levels within the same exact power budget. */
			if (v90_pcm_mapping_init_from_v92_cpt(&mapping,
				    &station->cpt_parameters, station->law) != V90_PCM_MAPPING_OK ||
			    v90_pcm_mapping_power(&mapping, V90_PCM_MAPPING_CODEC,
				    station->law, &lower, &limit) != V90_PCM_MAPPING_OK)
				return V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
			if ((!pcm_dil_receiver_finish_unconditioned(&station->dil_receiver, &station->dil_result) &&
			     !pcm_dil_receiver_finish(&station->dil_receiver, &station->dil_result)) ||
			    !v90_profile_select_measured_constellations(&mapping, limit,
				    station->dil_result.usable,
				    station->cpt_parameters.transmit_constellations,
				    station->cpt_parameters.frame_constellation,
				    &station->cpt_parameters.constellation_count))
				return V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH;
			station->cpt_parameters.trn1d_ratio_q3_13 =
				station->dil_result.trn1d_ratio_q3_13;
			if (v92_cpt_encode(&station->cpt_parameters, station->transmit.cpt.frame,
				    sizeof(station->transmit.cpt.frame),
				    &station->transmit.cpt.frame_bit_count) != V92_CPT_OK)
				return V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH;
		}
		if (v92_pcm_phase3_cpt_tx_start(&station->transmit.cpt) != V92_PCM_PHASE3_CPT_TX_OK)
			return V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH;
	}
	if (v92_pcm_phase3_cpt_tx_is_complete(&station->transmit.cpt)) {
		struct v92_pcm_phase4_trn2u trn2u;

		if (v92_pcm_phase4_trn2u_init(&trn2u,
			    station->phase4_training_constellation,
			    station->transmit.cpt.coding.previous_sign,
			    station->upstream_lu) != V92_PCM_PHASE4_TRN2U_OK)
			return V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
		station->transmit =
			(union v92_phase3_analog_transmit){ .trn2u = trn2u };
		station->transmit_stage = V92_PHASE3_ANALOG_TRANSMIT_TRN2U;
	}
	return V92_PHASE3_ANALOG_STATION_OK;
}

static bool result_is_terminal(
	enum v92_phase3_analog_station_result result)
{
	return result == V92_PHASE3_ANALOG_STATION_FAILED ||
	       result == V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH ||
	       result == V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
}

enum v92_phase3_analog_station_result v92_phase3_analog_station_transmit(
	struct v92_phase3_analog_station *station,
	const struct v90_pcm_symbol *md_symbol, int16_t *sample)
{
	struct v92_phase3_analog_station next;
	enum v92_phase3_analog_station_result result;
	int16_t generated;

	if (!v92_phase3_analog_station_is_valid(station) || sample == NULL)
		return V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V92_PHASE3_ANALOG_STATION_FAILED;
	if (station->transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_TRN2U &&
	    station->receive_stage == V92_PHASE3_ANALOG_RECEIVE_COMPLETE)
		return V92_PHASE3_ANALOG_STATION_COMPLETE;
	if (md_symbol != NULL &&
	    (station->transmit_stage !=
		     V92_PHASE3_ANALOG_TRANSMIT_INITIAL ||
	     station->transmit.initial.stage !=
		     V92_PCM_PHASE3_INITIAL_TX_OPENING ||
	     station->transmit.initial.sequence.opening.stage !=
		     V92_PCM_PHASE3_OPENING_MD))
		return V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	next = *station;
	switch (next.transmit_stage) {
	case V92_PHASE3_ANALOG_TRANSMIT_INITIAL:
		result = transmit_initial(&next, md_symbol, &generated);
		break;
	case V92_PHASE3_ANALOG_TRANSMIT_SU:
		result = transmit_su(&next, &generated);
		break;
	case V92_PHASE3_ANALOG_TRANSMIT_CPT:
		result = transmit_cpt(&next, &generated);
		break;
	case V92_PHASE3_ANALOG_TRANSMIT_TRN2U:
		result = v92_pcm_phase4_trn2u_next(&next.transmit.trn2u,
			&generated) == V92_PCM_PHASE4_TRN2U_OK ?
			V92_PHASE3_ANALOG_STATION_OK :
			V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
		break;
	default:
		return V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	}
	if (result == V92_PHASE3_ANALOG_STATION_NEED_MD_SYMBOL ||
	    result == V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT)
		return result;
	if (result_is_terminal(result)) {
		next.failed = true;
		if (v92_phase3_analog_station_is_valid(&next))
			*station = next;
		else
			station->failed = true;
		return result;
	}
	/* Su's reconstruction delay is physical channel memory, not a training
	 * coder. Use it before Su too, and retain pending samples when
	 * TRN1u/CPt and TRN2u take over. Su already advances it internally. */
	if (result == V92_PHASE3_ANALOG_STATION_OK &&
	    station->transmit_stage != V92_PHASE3_ANALOG_TRANSMIT_SU &&
	    fractional_delay_process(&next.transmit_delay, generated, &generated) !=
		    FRACTIONAL_DELAY_OK)
		result = V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	if (result != V92_PHASE3_ANALOG_STATION_OK ||
	    !v92_phase3_analog_station_is_valid(&next)) {
		station->failed = true;
		return V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	}
	*station = next;
	*sample = generated;
	return V92_PHASE3_ANALOG_STATION_OK;
}

static enum v92_phase3_analog_station_result receive_sd(
	struct v92_phase3_analog_station *station, uint8_t network_octet)
{
	enum pcm_s_pattern_rx_result result =
		pcm_s_pattern_rx_receive(
			&station->receive.sd, network_octet);

	if (result == PCM_S_PATTERN_RX_OK ||
	    result == PCM_S_PATTERN_RX_ACQUIRED)
		return V92_PHASE3_ANALOG_STATION_OK;
	/* V.92 9.5.2.1.3: keep Ja through Sd, then stop at the next
	 * twelve-symbol boundary after the reversal is detected. */
	if (result == PCM_S_PATTERN_RX_REVERSAL_DETECTED)
		return v92_pcm_phase3_initial_tx_request_finish(
			       &station->transmit.initial) ==
			       V92_PCM_PHASE3_INITIAL_TX_OK ?
			       V92_PHASE3_ANALOG_STATION_OK :
			       V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH;
	if (result == PCM_S_PATTERN_RX_SEQUENCE_COMPLETE) {
		struct pcm_phase3_j_receiver j;

		if (pcm_phase3_j_receiver_init(
			    &j, PCM_PROTOCOL_V92, station->law,
			    station->downstream_training_ucode) !=
		    PCM_PHASE3_J_RECEIVER_OK)
			return V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
		station->receive =
			(union v92_phase3_analog_receive){ .j = j };
		station->receive_stage = V92_PHASE3_ANALOG_RECEIVE_J;
		return V92_PHASE3_ANALOG_STATION_OK;
	}
	return result == PCM_S_PATTERN_RX_INVALID_SEQUENCE ?
		       V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH :
		       V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
}

static enum v92_phase3_analog_station_result receive_j(
	struct v92_phase3_analog_station *station, uint8_t network_octet)
{
	enum pcm_phase3_j_receiver_result result =
		pcm_phase3_j_receiver_receive(
			&station->receive.j, network_octet);

	if (result == PCM_PHASE3_J_RECEIVER_OK ||
	    result == PCM_PHASE3_J_RECEIVER_JD_REPEATED ||
	    result == PCM_PHASE3_J_RECEIVER_JP_REPEATED)
		return V92_PHASE3_ANALOG_STATION_OK;
	if (result == PCM_PHASE3_J_RECEIVER_TRN1D_READY)
		return pcm_phase3_j_receiver_begin_jd_search(
			       &station->receive.j) ==
			       PCM_PHASE3_J_RECEIVER_OK ?
			       V92_PHASE3_ANALOG_STATION_OK :
			       V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH;
	if (result == PCM_PHASE3_J_RECEIVER_JD_DETECTED) {
		struct v92_su su;
		struct v92_jd jd;

		if (!v92_pcm_phase3_initial_tx_is_complete(
			    &station->transmit.initial) ||
		    !pcm_phase3_j_receiver_v92_jd(&station->receive.j, &jd))
			return V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH;
		/* CPt selects a training map the peer must be able to transmit.
		 * Clamp the station-owned copy; a retrain starts from configuration. */
		if (station->cpt_parameters.spectral_lookahead >
		    jd.maximum_spectral_shaping_lookahead)
			station->cpt_parameters.spectral_lookahead =
				jd.maximum_spectral_shaping_lookahead;
		if (v92_su_init(&su, station->upstream_lu) != V92_SU_OK)
			return V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
		station->transmit =
			(union v92_phase3_analog_transmit){ .su = su };
		station->transmit_stage = V92_PHASE3_ANALOG_TRANSMIT_SU;
		return V92_PHASE3_ANALOG_STATION_OK;
	}
	if (result == PCM_PHASE3_J_RECEIVER_JP_DETECTED) {
		struct v92_jp jp;

		if (!pcm_phase3_j_receiver_v92_jp(&station->receive.j, &jp) ||
		    v92_su_accept_jp(&station->transmit.su, &jp) != V92_SU_OK)
			return V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH;
		station->phase4_training_constellation =
			jp.training_constellation;
		station->rate_renegotiation_constellation = jp.rate_renegotiation_constellation;
		return V92_PHASE3_ANALOG_STATION_OK;
	}
	if (result == PCM_PHASE3_J_RECEIVER_COMPLETE) {
		struct v90_phase4_ri_search ri;

		if (station->dil_profile != V90_DIL_PROFILE_NONE) {
			/* Training is already in progress while Jp finishes. Allow
			 * a DIL traversal from this receive boundary, still bounded
			 * by the five-second deadline measured from the Su reversal. */
			uint32_t emitted = station->transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_CPT ?
				station->transmit.cpt.trn1u.emitted_symbols : 0u;
			if (emitted >= CPT_MAX_TRN1U_SYMBOLS)
				return V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH;
			uint32_t target = (emitted + station->second_trn1u_symbols +
				V92_PCM_PHASE3_TRN1U_ALIGNMENT - 1u) /
				V92_PCM_PHASE3_TRN1U_ALIGNMENT * V92_PCM_PHASE3_TRN1U_ALIGNMENT;
			station->second_trn1u_symbols = target < CPT_MAX_TRN1U_SYMBOLS ?
				target : CPT_MAX_TRN1U_SYMBOLS;
		}
		if (v90_phase4_ri_search_init(
			    &ri, station->law,
			    station->downstream_training_ucode) !=
			    V90_PHASE4_RI_SEARCH_OK)
			return V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH;
		station->receive =
			(union v92_phase3_analog_receive){ .ri = ri };
		station->receive_stage = V92_PHASE3_ANALOG_RECEIVE_RI;
		return V92_PHASE3_ANALOG_STATION_OK;
	}
	return result == PCM_PHASE3_J_RECEIVER_INVALID_SEQUENCE ?
		       V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH :
		       V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
}

static enum v92_phase3_analog_station_result receive_ri(
	struct v92_phase3_analog_station *station, uint8_t network_octet,
	uint8_t unconditioned_octet)
{
	if (station->learn_dil &&
	    (station->transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_SU ||
	     (station->transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_CPT &&
	      station->transmit.cpt.stage == V92_PCM_PHASE3_CPT_TX_TRN1U)) &&
	    station->dil_receiver.symbols < station->dil_receiver.limit &&
	    !pcm_dil_receiver_receive(&station->dil_receiver, network_octet, unconditioned_octet))
		return V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	/* A nonzero-DIL peer cannot acknowledge CPt before receiving it.
	 * Do not hunt for Ri in the preceding training symbols. */
	if (station->dil_profile != V90_DIL_PROFILE_NONE &&
	    (station->transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_SU ||
	     (station->transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_CPT &&
	      station->transmit.cpt.completed_records == 0u)))
		return V92_PHASE3_ANALOG_STATION_OK;
	enum v90_phase4_ri_search_result result =
		v90_phase4_ri_search_receive(
			&station->receive.ri, network_octet);

	/* The acknowledgement ending CPt is Ri-bar for N=0, plain Ri for DIL. */
	if (result == (station->dil_profile == V90_DIL_PROFILE_NONE ?
		V90_PHASE4_RI_SEARCH_TRANSITION_DETECTED :
		V90_PHASE4_RI_SEARCH_ACQUIRED))
		return station->transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_CPT &&
		       v92_pcm_phase3_cpt_tx_request_finish(
			       &station->transmit.cpt) ==
			       V92_PCM_PHASE3_CPT_TX_OK ?
			       V92_PHASE3_ANALOG_STATION_OK :
			       V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH;
	if (result == V90_PHASE4_RI_SEARCH_OK ||
	    result == V90_PHASE4_RI_SEARCH_ACQUIRED ||
	    result == V90_PHASE4_RI_SEARCH_TRANSITION_DETECTED)
		return V92_PHASE3_ANALOG_STATION_OK;
	if (result == V90_PHASE4_RI_SEARCH_DONE) {
		station->receive_stage = V92_PHASE3_ANALOG_RECEIVE_COMPLETE;
		return V92_PHASE3_ANALOG_STATION_COMPLETE;
	}
	return result == V90_PHASE4_RI_SEARCH_INVALID_SEQUENCE ?
		       V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH :
		       V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
}

enum v92_phase3_analog_station_result v92_phase3_analog_station_receive(
	struct v92_phase3_analog_station *station, uint8_t network_octet,
	uint8_t unconditioned_octet)
{
	struct v92_phase3_analog_station next;
	enum v92_phase3_analog_station_result result;

	if (!v92_phase3_analog_station_is_valid(station))
		return V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V92_PHASE3_ANALOG_STATION_FAILED;
	if (station->receive_stage == V92_PHASE3_ANALOG_RECEIVE_COMPLETE)
		return V92_PHASE3_ANALOG_STATION_COMPLETE;
	next = *station;
	switch (next.receive_stage) {
	case V92_PHASE3_ANALOG_RECEIVE_SD:
		result = receive_sd(&next, network_octet);
		break;
	case V92_PHASE3_ANALOG_RECEIVE_J:
		result = receive_j(&next, network_octet);
		break;
	case V92_PHASE3_ANALOG_RECEIVE_RI:
		result = receive_ri(&next, network_octet, unconditioned_octet);
		break;
	case V92_PHASE3_ANALOG_RECEIVE_COMPLETE:
		return V92_PHASE3_ANALOG_STATION_COMPLETE;
	default:
		return V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	}
	if (result_is_terminal(result)) {
		next.failed = true;
		if (v92_phase3_analog_station_is_valid(&next))
			*station = next;
		else
			station->failed = true;
		return result;
	}
	if ((result != V92_PHASE3_ANALOG_STATION_OK &&
	     result != V92_PHASE3_ANALOG_STATION_COMPLETE) ||
	    !v92_phase3_analog_station_is_valid(&next)) {
		station->failed = true;
		return V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	}
	*station = next;
	return result;
}

enum v92_phase3_analog_station_result
v92_phase3_analog_station_downstream_handoff(
	const struct v92_phase3_analog_station *station,
	struct v92_cpt_parameters *parameters)
{
	if (!v92_phase3_analog_station_is_valid(station) || parameters == NULL)
		return V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V92_PHASE3_ANALOG_STATION_FAILED;
	if (!v92_phase3_analog_station_receive_complete(station))
		return V92_PHASE3_ANALOG_STATION_INVALID_STATE;
	*parameters = station->cpt_parameters;
	return V92_PHASE3_ANALOG_STATION_OK;
}

enum v92_phase3_analog_station_result
v92_phase3_analog_station_transmit_handoff(
	const struct v92_phase3_analog_station *station,
	struct v92_pcm_phase4_trn2u *handoff)
{
	if (!v92_phase3_analog_station_is_valid(station) || handoff == NULL)
		return V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V92_PHASE3_ANALOG_STATION_FAILED;
	if (!v92_phase3_analog_station_transmit_complete(station))
		return V92_PHASE3_ANALOG_STATION_INVALID_STATE;
	*handoff = station->transmit.trn2u;
	return V92_PHASE3_ANALOG_STATION_OK;
}

const char *v92_phase3_analog_station_result_name(
	enum v92_phase3_analog_station_result result)
{
	switch (result) {
	case V92_PHASE3_ANALOG_STATION_OK:
		return "ok";
	case V92_PHASE3_ANALOG_STATION_COMPLETE:
		return "complete";
	case V92_PHASE3_ANALOG_STATION_NEED_MD_SYMBOL:
		return "need MD symbol";
	case V92_PHASE3_ANALOG_STATION_FAILED:
		return "failed";
	case V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PHASE3_ANALOG_STATION_INVALID_STATE:
		return "invalid state";
	case V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
