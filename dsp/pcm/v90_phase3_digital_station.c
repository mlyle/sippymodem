// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_digital_station.h"

#include "v90_info1.h"
#include "v90_phase3_signs.h"

#include <stddef.h>

static bool receive_is_valid(
	const struct v90_phase3_digital_station *station)
{
	switch (station->receive_stage) {
	case V90_PHASE3_DIGITAL_RECEIVE_INITIAL:
		return v90_phase3_analog_line_rx_is_valid(
			&station->receive.initial);
	case V90_PHASE3_DIGITAL_RECEIVE_RETURN:
	case V90_PHASE3_DIGITAL_RECEIVE_DIL_RETURN:
		return v90_phase3_return_line_rx_is_valid(
			&station->receive.returning);
	}
	return false;
}

static bool receive_has_failed(
	const struct v90_phase3_digital_station *station)
{
	return station->receive_stage == V90_PHASE3_DIGITAL_RECEIVE_INITIAL ?
		       v90_phase3_analog_line_rx_has_failed(
			       &station->receive.initial) :
		       v90_phase3_return_line_rx_has_failed(
			       &station->receive.returning);
}

static bool finish_request_is_causal(
	const struct v90_phase3_digital_station *station)
{
	if (!station->transmit_started ||
	    !station->transmit.finish_jd_requested)
		return true;
	if (station->receive_stage == V90_PHASE3_DIGITAL_RECEIVE_INITIAL)
		return false;
	if (station->receive_stage == V90_PHASE3_DIGITAL_RECEIVE_RETURN &&
	    station->receive.returning.marker.stage <
		    V90_PHASE3_RETURN_RX_STAGE_S)
		return false;
	return !station->transmit.dil.finish_requested ||
	       (station->receive_stage ==
			V90_PHASE3_DIGITAL_RECEIVE_DIL_RETURN &&
		station->receive.returning.marker.stage >=
			V90_PHASE3_RETURN_RX_STAGE_S);
}

bool v90_phase3_digital_station_is_valid(
	const struct v90_phase3_digital_station *station)
{
	if (station == NULL || !receive_is_valid(station) ||
	    !v90_jd_parameters_are_valid(&station->jd) ||
	    !g711_law_is_valid(station->law) ||
	    station->training_ucode < V90_INFO1A_MIN_TRAINING_UCODE ||
	    station->training_ucode > V90_INFO1A_MAX_TRAINING_UCODE ||
	    station->post_ja_silence_symbols_remaining >
		    V90_PHASE3_DIGITAL_POST_JA_SILENCE_SYMBOLS ||
	    !finish_request_is_causal(station) ||
	    (receive_has_failed(station) && !station->failed))
		return false;
	if (!station->transmit_started)
		return station->receive_stage ==
			       V90_PHASE3_DIGITAL_RECEIVE_INITIAL &&
		       station->post_ja_silence_symbols_remaining == 0u;
	if (station->receive_stage == V90_PHASE3_DIGITAL_RECEIVE_INITIAL ||
	    station->transmit.protocol != PCM_PROTOCOL_V90 ||
	    !pcm_phase3_digital_tx_is_valid(&station->transmit))
		return false;
	if (station->receive_stage == V90_PHASE3_DIGITAL_RECEIVE_DIL_RETURN)
		return station->transmit.dil.parameters.segment_count != 0u &&
		       station->transmit.finish_jd_requested;
	return !station->transmit.dil.finish_requested;
}

bool v90_phase3_digital_station_has_failed(
	const struct v90_phase3_digital_station *station)
{
	return v90_phase3_digital_station_is_valid(station) && station->failed;
}

enum v90_phase3_digital_station_result v90_phase3_digital_station_init(
	struct v90_phase3_digital_station *station,
	const struct v34_phase3_profile *analog_profile,
	int16_t initial_receive_gain, enum g711_law law, uint8_t training_ucode,
	const struct v90_jd_parameters *jd)
{
	struct v90_phase3_digital_station initialized = {
		.law = law,
		.training_ucode = training_ucode,
	};
	enum v90_phase3_analog_line_rx_result result;

	if (station == NULL || jd == NULL || !g711_law_is_valid(law) ||
	    !v90_jd_parameters_are_valid(jd) ||
	    training_ucode < V90_INFO1A_MIN_TRAINING_UCODE ||
	    training_ucode > V90_INFO1A_MAX_TRAINING_UCODE)
		return V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT;
	result = v90_phase3_analog_line_rx_init(
		&initialized.receive.initial, analog_profile,
		initial_receive_gain);
	if (result == V90_PHASE3_ANALOG_LINE_RX_OUT_OF_RANGE)
		return V90_PHASE3_DIGITAL_STATION_OUT_OF_RANGE;
	if (result != V90_PHASE3_ANALOG_LINE_RX_OK)
		return V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT;
	initialized.jd = *jd;
	if (!v90_phase3_digital_station_is_valid(&initialized))
		return V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT;
	*station = initialized;
	return V90_PHASE3_DIGITAL_STATION_OK;
}

bool v90_phase3_digital_station_transmit_complete(
	const struct v90_phase3_digital_station *station)
{
	return v90_phase3_digital_station_is_valid(station) &&
	       !station->failed && station->transmit_started &&
	       station->transmit.stage == PCM_PHASE3_DIGITAL_TX_COMPLETE;
}

enum v90_phase3_digital_station_result v90_phase3_digital_station_transmit(
	struct v90_phase3_digital_station *station, uint8_t *network_octet)
{
	struct v90_phase3_digital_station next;
	struct v90_pcm_symbol symbol;

	if (!v90_phase3_digital_station_is_valid(station) ||
	    network_octet == NULL)
		return V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE3_DIGITAL_STATION_FAILED;
	if (station->transmit_started &&
	    station->transmit.stage == PCM_PHASE3_DIGITAL_TX_COMPLETE)
		return V90_PHASE3_DIGITAL_STATION_COMPLETE;
	if (!station->transmit_started) {
		*network_octet = g711_encode(station->law, 0);
		return V90_PHASE3_DIGITAL_STATION_OK;
	}
	if (station->post_ja_silence_symbols_remaining != 0u) {
		next = *station;
		next.post_ja_silence_symbols_remaining--;
		if (!v90_phase3_digital_station_is_valid(&next)) {
			station->failed = true;
			return V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
		}
		*station = next;
		*network_octet = g711_encode(station->law, 0);
		return V90_PHASE3_DIGITAL_STATION_OK;
	}
	next = *station;
	if (pcm_phase3_digital_tx_next(&next.transmit, &symbol) !=
		    PCM_PHASE3_DIGITAL_TX_OK ||
	    !v90_phase3_digital_station_is_valid(&next)) {
		station->failed = true;
		return V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
	}
	*station = next;
	*network_octet = symbol.octet;
	return V90_PHASE3_DIGITAL_STATION_OK;
}

static enum v90_phase3_digital_station_result receive_initial(
	struct v90_phase3_digital_station *station, int16_t sample)
{
	enum v90_phase3_analog_line_rx_result result =
		v90_phase3_analog_line_rx_sample(
			&station->receive.initial, sample);

	switch (result) {
	case V90_PHASE3_ANALOG_LINE_RX_OK:
	case V90_PHASE3_ANALOG_LINE_RX_TIMING_ACQUIRED:
	case V90_PHASE3_ANALOG_LINE_RX_S_DETECTED:
	case V90_PHASE3_ANALOG_LINE_RX_PP_COMPLETE:
	case V90_PHASE3_ANALOG_LINE_RX_JA_REJECTED:
		return V90_PHASE3_DIGITAL_STATION_OK;
	case V90_PHASE3_ANALOG_LINE_RX_JA_DETECTED: {
		struct v90_phase3_analog_line_handoff handoff;
		struct v90_phase3_return_line_rx returning;
		struct pcm_phase3_digital_tx transmit;
		if (v90_phase3_analog_line_rx_handoff(
			    &station->receive.initial, &handoff) !=
			    V90_PHASE3_ANALOG_LINE_RX_OK ||
		    v90_phase3_return_line_rx_init(
			    &returning, &handoff) !=
			    V90_PHASE3_RETURN_LINE_RX_OK ||
		    pcm_phase3_digital_tx_init_v90(
			    &transmit, station->law, station->training_ucode,
			    &station->jd, &handoff.ja.dil) !=
			    PCM_PHASE3_DIGITAL_TX_OK)
			return V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
		if (handoff.ja.dil.segment_count != 0u &&
		    v90_phase3_return_marker_allow_implicit_s_end(
			    &returning.marker) != V90_PHASE3_RETURN_RX_OK)
			return V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
		station->receive =
			(union v90_phase3_digital_station_receive){ 0 };
		station->receive.returning = returning;
		station->transmit = transmit;
		station->receive_stage = V90_PHASE3_DIGITAL_RECEIVE_RETURN;
		station->post_ja_silence_symbols_remaining =
			V90_PHASE3_DIGITAL_POST_JA_SILENCE_SYMBOLS;
		station->transmit_started = true;
		return V90_PHASE3_DIGITAL_STATION_OK;
	}
	case V90_PHASE3_ANALOG_LINE_RX_OUT_OF_RANGE:
		return V90_PHASE3_DIGITAL_STATION_OUT_OF_RANGE;
	case V90_PHASE3_ANALOG_LINE_RX_FAILED:
		return V90_PHASE3_DIGITAL_STATION_FAILED;
	case V90_PHASE3_ANALOG_LINE_RX_COMPLETE:
	case V90_PHASE3_ANALOG_LINE_RX_INVALID_ARGUMENT:
	case V90_PHASE3_ANALOG_LINE_RX_INVALID_STATE:
	case V90_PHASE3_ANALOG_LINE_RX_NUMERIC_FAILURE:
		break;
	}
	return V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
}

static enum v90_phase3_digital_station_result receive_return(
	struct v90_phase3_digital_station *station, int16_t sample)
{
	/* V.90 9.3.1.4 arms the return S detector when Jd starts. The peer
	 * cannot acknowledge Jd during Sd/TRN1d. Keep the physical receiver
	 * continuous, but do not let pre-Jd echo seed a return marker. */
	if (station->transmit.stage == PCM_PHASE3_DIGITAL_TX_PREFIX) {
		struct v34_analytic_sample decision;
		enum v90_upstream_training_rx_result result =
			v90_upstream_training_rx_sample(
				&station->receive.returning.upstream, sample, &decision);

		return result == V90_UPSTREAM_TRAINING_RX_OK ||
		       result == V90_UPSTREAM_TRAINING_RX_DECISION_READY ?
			V90_PHASE3_DIGITAL_STATION_OK :
			V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
	}
	enum v90_phase3_return_line_rx_result result =
		v90_phase3_return_line_rx_sample(
			&station->receive.returning, sample);

	switch (result) {
	case V90_PHASE3_RETURN_LINE_RX_OK:
		return V90_PHASE3_DIGITAL_STATION_OK;
	case V90_PHASE3_RETURN_LINE_RX_S_ENDED:
	case V90_PHASE3_RETURN_LINE_RX_S_BAR_DETECTED:
		if (station->receive_stage ==
			    V90_PHASE3_DIGITAL_RECEIVE_RETURN &&
		    station->transmit.dil.parameters.segment_count != 0u) {
			if (station->transmit.stage !=
			    PCM_PHASE3_DIGITAL_TX_DIL)
				return V90_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH;
			if (v90_phase3_return_line_rx_restart(
				    &station->receive.returning) !=
			    V90_PHASE3_RETURN_LINE_RX_OK)
				return V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
			station->receive_stage =
				V90_PHASE3_DIGITAL_RECEIVE_DIL_RETURN;
			/* V.90 8.4.2: Jd selects the SCR constellation during DIL.
			 * Keep the trained line, and adapt to the points actually sent. */
			station->receive.returning.upstream.tracking_constellation =
				station->jd.training_constellation;
			return V90_PHASE3_DIGITAL_STATION_OK;
		}
		if (station->receive_stage ==
		    V90_PHASE3_DIGITAL_RECEIVE_DIL_RETURN)
			return pcm_phase3_digital_tx_finish_dil(
				       &station->transmit) ==
				       PCM_PHASE3_DIGITAL_TX_OK ?
				       V90_PHASE3_DIGITAL_STATION_OK :
				       V90_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH;
		return station->transmit.stage ==
			       PCM_PHASE3_DIGITAL_TX_COMPLETE ?
			       V90_PHASE3_DIGITAL_STATION_OK :
			       V90_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH;
	case V90_PHASE3_RETURN_LINE_RX_S_DETECTED:
		if (station->receive_stage ==
		    V90_PHASE3_DIGITAL_RECEIVE_DIL_RETURN)
			return V90_PHASE3_DIGITAL_STATION_OK;
		return pcm_phase3_digital_tx_finish_jd(&station->transmit) ==
			       PCM_PHASE3_DIGITAL_TX_OK ?
			       V90_PHASE3_DIGITAL_STATION_OK :
			       V90_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH;
	case V90_PHASE3_RETURN_LINE_RX_FAILED:
		return V90_PHASE3_DIGITAL_STATION_FAILED;
	case V90_PHASE3_RETURN_LINE_RX_COMPLETE:
	case V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT:
	case V90_PHASE3_RETURN_LINE_RX_INVALID_STATE:
	case V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE:
		break;
	}
	return V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
}

static bool result_is_terminal(
	enum v90_phase3_digital_station_result result)
{
	return result == V90_PHASE3_DIGITAL_STATION_FAILED ||
	       result == V90_PHASE3_DIGITAL_STATION_OUT_OF_RANGE ||
	       result == V90_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH ||
	       result == V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
}

bool v90_phase3_digital_station_receive_complete(
	const struct v90_phase3_digital_station *station)
{
	return v90_phase3_digital_station_is_valid(station) &&
	       !station->failed && station->transmit_started &&
	       v90_phase3_return_marker_is_complete(
		       &station->receive.returning.marker);
}

enum v90_phase3_digital_station_result v90_phase3_digital_station_receive(
	struct v90_phase3_digital_station *station, int16_t sample)
{
	struct v90_phase3_digital_station next;
	enum v90_phase3_digital_station_result result;

	if (!v90_phase3_digital_station_is_valid(station))
		return V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE3_DIGITAL_STATION_FAILED;
	if (station->transmit_started &&
	    v90_phase3_return_marker_is_complete(&station->receive.returning.marker))
		return V90_PHASE3_DIGITAL_STATION_COMPLETE;
	next = *station;
	result = next.receive_stage == V90_PHASE3_DIGITAL_RECEIVE_INITIAL ?
			 receive_initial(&next, sample) :
			 receive_return(&next, sample);
	if (result_is_terminal(result)) {
		next.failed = true;
		if (v90_phase3_digital_station_is_valid(&next))
			*station = next;
		else
			station->failed = true;
		return result;
	}
	if (result != V90_PHASE3_DIGITAL_STATION_OK)
		return result;
	if (!v90_phase3_digital_station_is_valid(&next)) {
		station->failed = true;
		return V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
	}
	*station = next;
	return V90_PHASE3_DIGITAL_STATION_OK;
}

enum v90_phase3_digital_station_result
v90_phase3_digital_station_advance_completed_receive(
	struct v90_phase3_digital_station *station, int16_t sample)
{
	struct v90_phase3_digital_station next;
	enum v90_phase3_return_line_rx_result result;

	if (!v90_phase3_digital_station_is_valid(station))
		return V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE3_DIGITAL_STATION_FAILED;
	if (!v90_phase3_digital_station_receive_complete(station) ||
	    v90_phase3_digital_station_transmit_complete(station))
		return V90_PHASE3_DIGITAL_STATION_INVALID_STATE;
	next = *station;
	result = v90_phase3_return_line_rx_advance_complete(
		&next.receive.returning, sample);
	if (result != V90_PHASE3_RETURN_LINE_RX_OK) {
		if (result == V90_PHASE3_RETURN_LINE_RX_INVALID_STATE)
			return V90_PHASE3_DIGITAL_STATION_INVALID_STATE;
		next.failed = true;
		if (v90_phase3_digital_station_is_valid(&next))
			*station = next;
		else
			station->failed = true;
		return result == V90_PHASE3_RETURN_LINE_RX_FAILED ?
			V90_PHASE3_DIGITAL_STATION_FAILED :
			V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
	}
	if (!v90_phase3_digital_station_is_valid(&next)) {
		station->failed = true;
		return V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
	}
	*station = next;
	return V90_PHASE3_DIGITAL_STATION_OK;
}

bool v90_phase3_digital_transmit_handoff_is_valid(
	const struct v90_phase3_digital_transmit_handoff *handoff)
{
	return handoff != NULL &&
	       v34_scrambler_is_valid(&handoff->scrambler) &&
	       handoff->scrambler.polynomial == V34_POLYNOMIAL_GPC &&
	       v90_jd_parameters_are_valid(&handoff->jd) &&
	       g711_law_is_valid(handoff->law) &&
	       handoff->trn1d_symbols >= V90_PHASE3_TRN1D_MIN_SYMBOLS &&
	       handoff->trn1d_symbols % V90_PHASE3_TRN1D_ALIGNMENT == 0 &&
	       handoff->training_ucode >= V90_INFO1A_MIN_TRAINING_UCODE &&
	       handoff->training_ucode <= V90_INFO1A_MAX_TRAINING_UCODE &&
	       handoff->last_sign <= 1;
}

enum v90_phase3_digital_station_result
v90_phase3_digital_station_transmit_handoff(
	const struct v90_phase3_digital_station *station,
	struct v90_phase3_digital_transmit_handoff *handoff)
{
	struct v90_phase3_digital_transmit_handoff generated;
	const struct v90_phase3_signs *signs;

	if (!v90_phase3_digital_station_is_valid(station) || handoff == NULL)
		return V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE3_DIGITAL_STATION_FAILED;
	if (!v90_phase3_digital_station_transmit_complete(station))
		return V90_PHASE3_DIGITAL_STATION_INVALID_STATE;
	signs = &station->transmit.sequence.signs;
	generated = (struct v90_phase3_digital_transmit_handoff){
		.scrambler = signs->scrambler,
		.jd = station->jd,
		.law = signs->law,
		.trn1d_symbols = signs->trn1d_symbols,
		.training_ucode = signs->training_ucode,
		.last_sign = signs->last_sign,
	};
	if (!v90_phase3_digital_transmit_handoff_is_valid(&generated))
		return V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
	*handoff = generated;
	return V90_PHASE3_DIGITAL_STATION_OK;
}

enum v90_phase3_digital_station_result
v90_phase3_digital_station_receive_handoff(
	const struct v90_phase3_digital_station *station,
	struct v90_phase3_return_line_handoff *handoff)
{
	if (!v90_phase3_digital_station_is_valid(station) || handoff == NULL)
		return V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE3_DIGITAL_STATION_FAILED;
	if (!v90_phase3_digital_station_receive_complete(station))
		return V90_PHASE3_DIGITAL_STATION_INVALID_STATE;
	return v90_phase3_return_line_rx_handoff(
		       &station->receive.returning, handoff) ==
		       V90_PHASE3_RETURN_LINE_RX_OK ?
		       V90_PHASE3_DIGITAL_STATION_OK :
		       V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE;
}

const char *v90_phase3_digital_station_result_name(
	enum v90_phase3_digital_station_result result)
{
	switch (result) {
	case V90_PHASE3_DIGITAL_STATION_OK:
		return "ok";
	case V90_PHASE3_DIGITAL_STATION_COMPLETE:
		return "complete";
	case V90_PHASE3_DIGITAL_STATION_FAILED:
		return "failed";
	case V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_DIGITAL_STATION_INVALID_STATE:
		return "invalid state";
	case V90_PHASE3_DIGITAL_STATION_OUT_OF_RANGE:
		return "out of range";
	case V90_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
