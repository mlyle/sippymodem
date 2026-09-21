// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase3_digital_station.h"

#include "v90_info1.h"

#include <stddef.h>

static bool receive_stage_is_valid(
	enum v92_phase3_digital_receive_stage stage)
{
	return stage >= V92_PHASE3_DIGITAL_RECEIVE_INITIAL &&
	       stage <= V92_PHASE3_DIGITAL_RECEIVE_COMPLETE;
}

static bool transmit_stage_is_valid(
	enum v92_phase3_digital_transmit_stage stage)
{
	return stage >= V92_PHASE3_DIGITAL_TRANSMIT_SILENCE &&
	       stage <= V92_PHASE3_DIGITAL_TRANSMIT_COMPLETE;
}

static bool receive_is_valid(
	const struct v92_phase3_digital_station *station)
{
	switch (station->receive_stage) {
	case V92_PHASE3_DIGITAL_RECEIVE_INITIAL:
		return v92_pcm_phase3_initial_rx_is_valid(
			&station->receive.initial);
	case V92_PHASE3_DIGITAL_RECEIVE_SU_PHASE:
		return station->receive.su.mode == V92_SU_RX_MEASURE_PHASE &&
		       v92_su_rx_is_valid(&station->receive.su);
	case V92_PHASE3_DIGITAL_RECEIVE_SU_RESPONSE:
		return station->receive.su.mode == V92_SU_RX_TRANSITION_ONLY &&
		       v92_su_rx_is_valid(&station->receive.su) &&
		       !v92_su_rx_is_complete(&station->receive.su);
	case V92_PHASE3_DIGITAL_RECEIVE_SECOND_TRN1U:
		return v92_pcm_phase3_trn1u_line_rx_is_valid(
			&station->receive.trn1u);
	case V92_PHASE3_DIGITAL_RECEIVE_CPT:
		return v92_pcm_phase3_cpt_line_rx_is_valid(
			       &station->receive.cpt) &&
		       !v92_pcm_phase3_cpt_line_rx_is_complete(
			       &station->receive.cpt);
	case V92_PHASE3_DIGITAL_RECEIVE_COMPLETE:
		return v92_pcm_phase3_cpt_line_rx_is_complete(
			&station->receive.cpt);
	}
	return false;
}

static bool transmit_is_valid(
	const struct v92_phase3_digital_station *station)
{
	switch (station->transmit_stage) {
	case V92_PHASE3_DIGITAL_TRANSMIT_SILENCE:
		return station->receive_stage ==
		       V92_PHASE3_DIGITAL_RECEIVE_INITIAL;
	case V92_PHASE3_DIGITAL_TRANSMIT_DOWNSTREAM:
		return pcm_phase3_digital_tx_is_valid(&station->downstream) &&
		       station->downstream.stage !=
			       PCM_PHASE3_DIGITAL_TX_COMPLETE;
	case V92_PHASE3_DIGITAL_TRANSMIT_RI:
		return pcm_phase3_digital_tx_is_valid(&station->downstream) &&
		       station->downstream.stage ==
			       PCM_PHASE3_DIGITAL_TX_COMPLETE &&
		       v90_phase4_ri_tx_is_valid(&station->ri) &&
		       !v90_phase4_ri_tx_is_complete(&station->ri);
	case V92_PHASE3_DIGITAL_TRANSMIT_COMPLETE:
		return pcm_phase3_digital_tx_is_valid(&station->downstream) &&
		       station->downstream.stage ==
			       PCM_PHASE3_DIGITAL_TX_COMPLETE &&
		       v90_phase4_ri_tx_is_complete(&station->ri);
	}
	return false;
}

static bool receive_has_failed(
	const struct v92_phase3_digital_station *station)
{
	switch (station->receive_stage) {
	case V92_PHASE3_DIGITAL_RECEIVE_INITIAL:
		return v92_pcm_phase3_initial_rx_has_failed(
			&station->receive.initial);
	case V92_PHASE3_DIGITAL_RECEIVE_SU_PHASE:
	case V92_PHASE3_DIGITAL_RECEIVE_SU_RESPONSE:
		return v92_su_rx_has_failed(&station->receive.su);
	case V92_PHASE3_DIGITAL_RECEIVE_SECOND_TRN1U:
		return station->receive.trn1u.stage ==
		       V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_FAILED;
	case V92_PHASE3_DIGITAL_RECEIVE_CPT:
	case V92_PHASE3_DIGITAL_RECEIVE_COMPLETE:
		return v92_pcm_phase3_cpt_line_rx_has_failed(
			&station->receive.cpt);
	}
	return true;
}

static bool coordination_is_valid(
	const struct v92_phase3_digital_station *station)
{
	if (station->transmit_stage == V92_PHASE3_DIGITAL_TRANSMIT_SILENCE)
		return station->receive_stage ==
		       V92_PHASE3_DIGITAL_RECEIVE_INITIAL;
	if (station->downstream.protocol != PCM_PROTOCOL_V92)
		return false;
	switch (station->receive_stage) {
	case V92_PHASE3_DIGITAL_RECEIVE_INITIAL:
		return false;
	case V92_PHASE3_DIGITAL_RECEIVE_SU_PHASE:
		return !station->downstream.finish_jd_requested;
	case V92_PHASE3_DIGITAL_RECEIVE_SU_RESPONSE:
		return station->downstream.finish_jd_requested &&
		       !station->downstream.finish_jp_requested;
	case V92_PHASE3_DIGITAL_RECEIVE_SECOND_TRN1U:
		return station->downstream.finish_jd_requested &&
		       station->downstream.finish_jp_requested &&
		       !station->downstream.finish_scr_requested;
	case V92_PHASE3_DIGITAL_RECEIVE_CPT:
	case V92_PHASE3_DIGITAL_RECEIVE_COMPLETE:
		if (station->downstream.dil.parameters.segment_count != 0u) {
			if (station->downstream.stage < PCM_PHASE3_DIGITAL_TX_DIL ||
			    station->downstream.finish_scr_requested)
				return false;
			if (station->receive_stage == V92_PHASE3_DIGITAL_RECEIVE_CPT)
				return !station->ri.finish_requested;
			return station->downstream.dil.complete &&
			       station->ri.finish_requested;
		}
		return station->downstream.finish_jd_requested &&
		       station->downstream.finish_jp_requested &&
		       station->downstream.finish_scr_requested;
	}
	return false;
}

bool v92_phase3_digital_station_is_valid(
	const struct v92_phase3_digital_station *station)
{
	if (station == NULL ||
	    !receive_stage_is_valid(station->receive_stage) ||
	    !transmit_stage_is_valid(station->transmit_stage) ||
	    !g711_law_is_valid(station->law) ||
	    station->training_ucode < V90_INFO1A_MIN_TRAINING_UCODE ||
	    station->training_ucode > V90_INFO1A_MAX_TRAINING_UCODE ||
	    !pcm_phase3_digital_prefix_duration_is_valid(station->trn1d_symbols) ||
	    !qam_adaptation_is_valid(station->equalizer_adaptation) ||
	    station->su_minimum_magnitude == 0u ||
	    !v92_jd_is_valid(&station->jd) || !v92_jp_is_valid(&station->jp) ||
	    !v92_pcm_dil_rate_mask_is_valid(station->upstream_rate_mask) ||
	    station->early_trn2u_samples >
		    sizeof(station->early_trn2u) / sizeof(station->early_trn2u[0]) ||
	    (station->early_trn2u_samples != 0u &&
	     station->receive_stage != V92_PHASE3_DIGITAL_RECEIVE_COMPLETE) ||
	    !receive_is_valid(station) || !transmit_is_valid(station) ||
	    !coordination_is_valid(station) ||
	    (receive_has_failed(station) && !station->failed))
		return false;
	if (station->transmit_stage != V92_PHASE3_DIGITAL_TRANSMIT_SILENCE &&
	    station->upstream_rate_mask == 0u)
		return false;
	return true;
}

bool v92_phase3_digital_station_has_failed(
	const struct v92_phase3_digital_station *station)
{
	return v92_phase3_digital_station_is_valid(station) && station->failed;
}

bool v92_phase3_digital_station_parameters_are_valid(
	enum g711_law law, uint8_t training_ucode, uint32_t trn1d_symbols,
	double equalizer_adaptation, uint16_t su_minimum_magnitude,
	const struct v92_jd *jd, const struct v92_jp *jp)
{
	return g711_law_is_valid(law) &&
	       training_ucode >= V90_INFO1A_MIN_TRAINING_UCODE &&
	       training_ucode <= V90_INFO1A_MAX_TRAINING_UCODE &&
	       pcm_phase3_digital_prefix_duration_is_valid(trn1d_symbols) &&
	       qam_adaptation_is_valid(equalizer_adaptation) &&
	       su_minimum_magnitude != 0u &&
	       v92_jd_is_valid(jd) && v92_jp_is_valid(jp);
}

enum v92_phase3_digital_station_result v92_phase3_digital_station_init(
	struct v92_phase3_digital_station *station, enum g711_law law,
	uint8_t training_ucode, uint32_t trn1d_symbols, double equalizer_adaptation,
	uint16_t su_minimum_magnitude, const struct v92_jd *jd,
	const struct v92_jp *jp)
{
	struct v92_phase3_digital_station initialized = {
		.jd = jd != NULL ? *jd : (struct v92_jd){ 0 },
		.jp = jp != NULL ? *jp : (struct v92_jp){ 0 },
		.equalizer_adaptation = equalizer_adaptation,
		.trn1d_symbols = trn1d_symbols,
		.receive_stage = V92_PHASE3_DIGITAL_RECEIVE_INITIAL,
		.transmit_stage = V92_PHASE3_DIGITAL_TRANSMIT_SILENCE,
		.law = law,
		.su_minimum_magnitude = su_minimum_magnitude,
		.training_ucode = training_ucode,
	};

	if (station == NULL ||
	    !v92_phase3_digital_station_parameters_are_valid(
		    law, training_ucode, trn1d_symbols, equalizer_adaptation,
		    su_minimum_magnitude, jd, jp) ||
	    v92_pcm_phase3_initial_rx_init(
		    &initialized.receive.initial, equalizer_adaptation) !=
		    V92_PCM_PHASE3_INITIAL_RX_OK ||
	    !v92_phase3_digital_station_is_valid(&initialized))
		return V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT;
	*station = initialized;
	return V92_PHASE3_DIGITAL_STATION_OK;
}

bool v92_phase3_digital_station_transmit_complete(
	const struct v92_phase3_digital_station *station)
{
	return v92_phase3_digital_station_is_valid(station) &&
	       !station->failed &&
	       station->transmit_stage ==
		       V92_PHASE3_DIGITAL_TRANSMIT_COMPLETE;
}

static bool begin_ri_if_downstream_complete(
	struct v92_phase3_digital_station *station)
{
	if (station->downstream.stage != PCM_PHASE3_DIGITAL_TX_COMPLETE)
		return true;
	if (station->transmit_stage == V92_PHASE3_DIGITAL_TRANSMIT_RI)
		return true;
	if (station->transmit_stage != V92_PHASE3_DIGITAL_TRANSMIT_DOWNSTREAM ||
	    v90_phase4_ri_tx_init(
		    &station->ri, station->law,
		    station->training_ucode) != V90_PHASE4_RI_OK)
		return false;
	station->transmit_stage = V92_PHASE3_DIGITAL_TRANSMIT_RI;
	return true;
}

enum v92_phase3_digital_station_result v92_phase3_digital_station_transmit(
	struct v92_phase3_digital_station *station, uint8_t *network_octet)
{
	struct v92_phase3_digital_station next;
	struct v90_pcm_symbol symbol;
	uint8_t generated;

	if (!v92_phase3_digital_station_is_valid(station) ||
	    network_octet == NULL)
		return V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V92_PHASE3_DIGITAL_STATION_FAILED;
	if (station->transmit_stage == V92_PHASE3_DIGITAL_TRANSMIT_COMPLETE)
		return V92_PHASE3_DIGITAL_STATION_COMPLETE;
	if (station->transmit_stage == V92_PHASE3_DIGITAL_TRANSMIT_SILENCE) {
		*network_octet = g711_encode(station->law, 0);
		return V92_PHASE3_DIGITAL_STATION_OK;
	}
	next = *station;
	if (next.transmit_stage == V92_PHASE3_DIGITAL_TRANSMIT_DOWNSTREAM) {
		if (pcm_phase3_digital_tx_next(&next.downstream, &symbol) !=
		    PCM_PHASE3_DIGITAL_TX_OK)
			goto fail;
		generated = symbol.octet;
		if (!begin_ri_if_downstream_complete(&next))
			goto fail;
	} else {
		if (v90_phase4_ri_tx_next(&next.ri, &generated) !=
		    V90_PHASE4_RI_OK)
			goto fail;
		if (v90_phase4_ri_tx_is_complete(&next.ri))
			next.transmit_stage =
				V92_PHASE3_DIGITAL_TRANSMIT_COMPLETE;
	}
	if (!v92_phase3_digital_station_is_valid(&next))
		goto fail;
	*station = next;
	*network_octet = generated;
	return V92_PHASE3_DIGITAL_STATION_OK;

fail:
	station->failed = true;
	return V92_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
}

static enum v92_phase3_digital_station_result receive_initial(
	struct v92_phase3_digital_station *station, int16_t sample)
{
	enum v92_pcm_phase3_initial_rx_result result =
		v92_pcm_phase3_initial_rx_sample(
			&station->receive.initial, sample);

	switch (result) {
	case V92_PCM_PHASE3_INITIAL_RX_OK:
	case V92_PCM_PHASE3_INITIAL_RX_ALIGNED:
	case V92_PCM_PHASE3_INITIAL_RX_TRAINED:
	case V92_PCM_PHASE3_INITIAL_RX_SYNCHRONIZED:
	case V92_PCM_PHASE3_INITIAL_RX_REJECTED:
		return V92_PHASE3_DIGITAL_STATION_OK;
	case V92_PCM_PHASE3_INITIAL_RX_DETECTED: {
		struct pcm_phase3_digital_tx downstream;
		struct v92_su_rx su;
		struct v92_pcm_dil_parameters parameters;
		uint16_t phase_minimum;

		if (!v92_pcm_phase3_initial_rx_parameters(
			    &station->receive.initial, &parameters))
			return V92_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
		if (parameters.upstream_rate_mask == 0u)
			return V92_PHASE3_DIGITAL_STATION_UNSUPPORTED;
		/* The received TRN1u level survives Ja. Scale confidence in the
		 * coherently averaged 4 kHz component from that level, not from
		 * the much larger threshold used to qualify individual Su signs. */
		phase_minimum = (uint16_t)(
			station->receive.initial.sequence.ja.line.magnitude / 256.0);
		if (phase_minimum < 8u)
			phase_minimum = 8u;
		if (pcm_phase3_digital_tx_init_v92(
			    &downstream, station->law,
			    station->training_ucode, station->trn1d_symbols,
			    &station->jd,
			    &station->jp, &parameters.downstream) !=
			    PCM_PHASE3_DIGITAL_TX_OK ||
		    v92_su_rx_init(
			    &su, V92_SU_RX_MEASURE_PHASE,
			    station->su_minimum_magnitude,
			    phase_minimum) != V92_SU_RX_OK)
			return V92_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
		station->receive =
			(union v92_phase3_digital_receive){ .su = su };
		station->downstream = downstream;
		station->upstream_rate_mask = parameters.upstream_rate_mask;
		station->receive_stage = V92_PHASE3_DIGITAL_RECEIVE_SU_PHASE;
		station->transmit_stage =
			V92_PHASE3_DIGITAL_TRANSMIT_DOWNSTREAM;
		return V92_PHASE3_DIGITAL_STATION_OK;
	}
	case V92_PCM_PHASE3_INITIAL_RX_FAILED:
		return V92_PHASE3_DIGITAL_STATION_FAILED;
	case V92_PCM_PHASE3_INITIAL_RX_DONE:
	case V92_PCM_PHASE3_INITIAL_RX_INVALID_ARGUMENT:
	case V92_PCM_PHASE3_INITIAL_RX_INTERNAL_ERROR:
		break;
	}
	return V92_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
}

static enum v92_phase3_digital_station_result receive_su_phase(
	struct v92_phase3_digital_station *station, int16_t sample)
{
	enum v92_su_rx_result result =
		v92_su_rx_sample(&station->receive.su, sample);

	if (result == V92_SU_RX_OK || result == V92_SU_RX_ACQUIRED ||
	    result == V92_SU_RX_TRANSITION)
		return V92_PHASE3_DIGITAL_STATION_OK;
	if (result == V92_SU_RX_PHASE_READY) {
		struct v92_su_rx response;
		uint16_t extension;

		if (!v92_su_rx_phase_extension(
			    &station->receive.su, &extension) ||
		    pcm_phase3_digital_tx_set_v92_su_phase(
			    &station->downstream, extension) !=
			    PCM_PHASE3_DIGITAL_TX_OK ||
		    pcm_phase3_digital_tx_finish_jd(&station->downstream) !=
			    PCM_PHASE3_DIGITAL_TX_OK ||
		    v92_su_rx_init(
			    &response, V92_SU_RX_TRANSITION_ONLY,
			    station->su_minimum_magnitude,
			    station->receive.su.phase_minimum_magnitude) != V92_SU_RX_OK)
			return V92_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH;
		station->jp.su_phase_extension_q16 = extension;
		station->receive =
			(union v92_phase3_digital_receive){ .su = response };
		station->receive_stage =
			V92_PHASE3_DIGITAL_RECEIVE_SU_RESPONSE;
		return V92_PHASE3_DIGITAL_STATION_OK;
	}
	return result == V92_SU_RX_FAILURE ?
		       V92_PHASE3_DIGITAL_STATION_FAILED :
		       V92_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
}

static enum v92_phase3_digital_station_result receive_su_response(
	struct v92_phase3_digital_station *station, int16_t sample)
{
	enum v92_su_rx_result result =
		v92_su_rx_sample(&station->receive.su, sample);

	if (result == V92_SU_RX_OK || result == V92_SU_RX_ACQUIRED)
		return V92_PHASE3_DIGITAL_STATION_OK;
	if (result == V92_SU_RX_TRANSITION) {
		struct v92_pcm_phase3_trn1u_line_rx trn1u;

		if (pcm_phase3_digital_tx_finish_jp(&station->downstream) !=
			    PCM_PHASE3_DIGITAL_TX_OK ||
		    v92_pcm_phase3_trn1u_line_rx_init(
			    &trn1u, station->equalizer_adaptation) !=
			    V92_PCM_PHASE3_TRN1U_LINE_RX_OK)
			return V92_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH;
		/* Arm acquisition on the Su reversal. Waiting for our Jp/Jp-prime
		 * tail to finish can discard the peer's zero-seeded TRN1u opening.
		 * The downstream acknowledgement keeps its own complete timing. */
		station->receive =
			(union v92_phase3_digital_receive){ .trn1u = trn1u };
		station->receive_stage =
			V92_PHASE3_DIGITAL_RECEIVE_SECOND_TRN1U;
		return V92_PHASE3_DIGITAL_STATION_OK;
	}
	return result == V92_SU_RX_FAILURE ?
		       V92_PHASE3_DIGITAL_STATION_FAILED :
		       V92_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
}

static enum v92_phase3_digital_station_result receive_second_trn1u(
	struct v92_phase3_digital_station *station, int16_t sample)
{
	enum v92_pcm_phase3_trn1u_line_rx_result result =
		v92_pcm_phase3_trn1u_line_rx_sample(
			&station->receive.trn1u, sample);

	if (result == V92_PCM_PHASE3_TRN1U_LINE_RX_OK ||
	    result == V92_PCM_PHASE3_TRN1U_LINE_RX_ALIGNED)
		return V92_PHASE3_DIGITAL_STATION_OK;
	if (result == V92_PCM_PHASE3_TRN1U_LINE_RX_TRAINED) {
		struct v92_pcm_phase3_trn1u_line_handoff handoff;
		struct v92_pcm_phase3_cpt_line_rx cpt;

		if (v92_pcm_phase3_trn1u_line_rx_handoff(
			    &station->receive.trn1u, &handoff) !=
			    V92_PCM_PHASE3_TRN1U_LINE_RX_OK ||
		    v92_pcm_phase3_cpt_line_rx_init(&cpt, &handoff) !=
			    V92_PCM_PHASE3_CPT_LINE_RX_OK)
			return V92_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH;
		/* Zero DIL sends Ri before CPt; nonzero DIL waits for CPt. */
		if (station->downstream.dil.parameters.segment_count == 0u &&
		    (pcm_phase3_digital_tx_finish_scr(&station->downstream) !=
			     PCM_PHASE3_DIGITAL_TX_OK ||
		     !begin_ri_if_downstream_complete(station)))
			return V92_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH;
		station->receive =
			(union v92_phase3_digital_receive){ .cpt = cpt };
		station->receive_stage = V92_PHASE3_DIGITAL_RECEIVE_CPT;
		return V92_PHASE3_DIGITAL_STATION_OK;
	}
	return result == V92_PCM_PHASE3_TRN1U_LINE_RX_FAILED ?
		       V92_PHASE3_DIGITAL_STATION_FAILED :
		       V92_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
}

static enum v92_phase3_digital_station_result receive_cpt(
	struct v92_phase3_digital_station *station, int16_t sample)
{
	struct v92_pcm_phase3_cpt_rx_event event;
	enum v92_pcm_phase3_cpt_line_rx_result result =
		v92_pcm_phase3_cpt_line_rx_sample(
			&station->receive.cpt, sample, &event);

	if (result == V92_PCM_PHASE3_CPT_LINE_RX_SIGN)
		return V92_PHASE3_DIGITAL_STATION_OK;
	if (result == V92_PCM_PHASE3_CPT_LINE_RX_EVENT) {
		bool nonzero_dil =
			station->downstream.dil.parameters.segment_count != 0u;

		if (event.type == V92_PCM_PHASE3_CPT_RX_PARAMETERS) {
			if (event.parameters.spectral_lookahead >
			    station->jd.maximum_spectral_shaping_lookahead)
				return V92_PHASE3_DIGITAL_STATION_UNSUPPORTED;
			if (nonzero_dil) {
				if (station->transmit_stage !=
					    V92_PHASE3_DIGITAL_TRANSMIT_DOWNSTREAM ||
				    pcm_phase3_digital_tx_finish_dil(
					    &station->downstream) != PCM_PHASE3_DIGITAL_TX_OK ||
				    !begin_ri_if_downstream_complete(station))
					return V92_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH;
			} else if (station->transmit_stage !=
				    V92_PHASE3_DIGITAL_TRANSMIT_RI ||
			    v90_phase4_ri_tx_request_finish(&station->ri) !=
				    V90_PHASE4_RI_OK)
				return V92_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH;
		} else if (event.type == V92_PCM_PHASE3_CPT_RX_E1U) {
			/* For nonzero DIL, E1u acknowledges Ri. Do not send
			 * Ri-bar while the peer is still repeating CPt. */
			if (nonzero_dil &&
			    (station->transmit_stage != V92_PHASE3_DIGITAL_TRANSMIT_RI ||
			     v90_phase4_ri_tx_request_finish(&station->ri) !=
				     V90_PHASE4_RI_OK))
				return V92_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH;
			if ((station->transmit_stage !=
				     V92_PHASE3_DIGITAL_TRANSMIT_RI &&
			     station->transmit_stage !=
				     V92_PHASE3_DIGITAL_TRANSMIT_COMPLETE) ||
			    !station->ri.finish_requested)
				return V92_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH;
			station->receive_stage =
				V92_PHASE3_DIGITAL_RECEIVE_COMPLETE;
			return V92_PHASE3_DIGITAL_STATION_COMPLETE;
		}
		return V92_PHASE3_DIGITAL_STATION_OK;
	}
	return result == V92_PCM_PHASE3_CPT_LINE_RX_FAILED ?
		       V92_PHASE3_DIGITAL_STATION_FAILED :
		       V92_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
}

static bool result_is_terminal(
	enum v92_phase3_digital_station_result result)
{
	return result == V92_PHASE3_DIGITAL_STATION_FAILED ||
	       result == V92_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH ||
	       result == V92_PHASE3_DIGITAL_STATION_UNSUPPORTED ||
	       result == V92_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
}

bool v92_phase3_digital_station_receive_complete(
	const struct v92_phase3_digital_station *station)
{
	return v92_phase3_digital_station_is_valid(station) &&
	       !station->failed &&
	       station->receive_stage == V92_PHASE3_DIGITAL_RECEIVE_COMPLETE;
}

enum v92_phase3_digital_station_result v92_phase3_digital_station_receive(
	struct v92_phase3_digital_station *station, int16_t sample)
{
	struct v92_phase3_digital_station next;
	enum v92_phase3_digital_station_result result;

	if (!v92_phase3_digital_station_is_valid(station))
		return V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V92_PHASE3_DIGITAL_STATION_FAILED;
	if (station->receive_stage == V92_PHASE3_DIGITAL_RECEIVE_COMPLETE) {
		if (station->early_trn2u_samples ==
		    sizeof(station->early_trn2u) / sizeof(station->early_trn2u[0])) {
			station->failed = true;
			return V92_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH;
		}
		station->early_trn2u[station->early_trn2u_samples++] = sample;
		return V92_PHASE3_DIGITAL_STATION_COMPLETE;
	}
	next = *station;
	switch (next.receive_stage) {
	case V92_PHASE3_DIGITAL_RECEIVE_INITIAL:
		result = receive_initial(&next, sample);
		break;
	case V92_PHASE3_DIGITAL_RECEIVE_SU_PHASE:
		result = receive_su_phase(&next, sample);
		break;
	case V92_PHASE3_DIGITAL_RECEIVE_SU_RESPONSE:
		result = receive_su_response(&next, sample);
		break;
	case V92_PHASE3_DIGITAL_RECEIVE_SECOND_TRN1U:
		result = receive_second_trn1u(&next, sample);
		break;
	case V92_PHASE3_DIGITAL_RECEIVE_CPT:
		result = receive_cpt(&next, sample);
		break;
	case V92_PHASE3_DIGITAL_RECEIVE_COMPLETE:
		return V92_PHASE3_DIGITAL_STATION_COMPLETE;
	default:
		return V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT;
	}
	if (result_is_terminal(result)) {
		next.failed = true;
		if (v92_phase3_digital_station_is_valid(&next))
			*station = next;
		else
			station->failed = true;
		return result;
	}
	if ((result != V92_PHASE3_DIGITAL_STATION_OK &&
	     result != V92_PHASE3_DIGITAL_STATION_COMPLETE) ||
	    !v92_phase3_digital_station_is_valid(&next)) {
		station->failed = true;
		return V92_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
	}
	*station = next;
	return result;
}

bool v92_phase3_digital_station_upstream_rate_mask(
	const struct v92_phase3_digital_station *station,
	uint32_t *rate_mask)
{
	if (!v92_phase3_digital_station_is_valid(station) ||
	    station->failed || rate_mask == NULL ||
	    station->transmit_stage == V92_PHASE3_DIGITAL_TRANSMIT_SILENCE)
		return false;
	*rate_mask = station->upstream_rate_mask;
	return true;
}

enum v92_phase3_digital_station_result
v92_phase3_digital_station_downstream_handoff(
	const struct v92_phase3_digital_station *station,
	struct v92_cpt_parameters *parameters)
{
	if (!v92_phase3_digital_station_is_valid(station) || parameters == NULL)
		return V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V92_PHASE3_DIGITAL_STATION_FAILED;
	if (!v92_phase3_digital_station_transmit_complete(station) ||
	    !v92_pcm_phase3_cpt_line_rx_parameters(
		    &station->receive.cpt, parameters))
		return V92_PHASE3_DIGITAL_STATION_INVALID_STATE;
	return V92_PHASE3_DIGITAL_STATION_OK;
}

enum v92_phase3_digital_station_result
v92_phase3_digital_station_receive_handoff(
	const struct v92_phase3_digital_station *station,
	struct v92_pcm_phase3_cpt_line_handoff *handoff)
{
	if (!v92_phase3_digital_station_is_valid(station) || handoff == NULL)
		return V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V92_PHASE3_DIGITAL_STATION_FAILED;
	if (!v92_phase3_digital_station_receive_complete(station))
		return V92_PHASE3_DIGITAL_STATION_INVALID_STATE;
	return v92_pcm_phase3_cpt_line_rx_handoff(
		       &station->receive.cpt, handoff) ==
		       V92_PCM_PHASE3_CPT_LINE_RX_OK ?
		       V92_PHASE3_DIGITAL_STATION_OK :
		       V92_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
}

const char *v92_phase3_digital_station_result_name(
	enum v92_phase3_digital_station_result result)
{
	switch (result) {
	case V92_PHASE3_DIGITAL_STATION_OK:
		return "ok";
	case V92_PHASE3_DIGITAL_STATION_COMPLETE:
		return "complete";
	case V92_PHASE3_DIGITAL_STATION_FAILED:
		return "failed";
	case V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PHASE3_DIGITAL_STATION_INVALID_STATE:
		return "invalid state";
	case V92_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V92_PHASE3_DIGITAL_STATION_UNSUPPORTED:
		return "unsupported";
	case V92_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
