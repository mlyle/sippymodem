// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_digital_station.h"

#include <stddef.h>

static bool training_mapping_is_supported(
	const struct v90_pcm_mapping *mapping, enum g711_law law,
	uint8_t maximum_spectral_lookahead)
{
	return v90_pcm_mapping_is_valid(mapping) &&
	       mapping->kind == V90_CP_TRAINING &&
	       mapping->law == law &&
	       mapping->spectral_lookahead <=
		       maximum_spectral_lookahead;
}

static bool initial_cpt_mapping(
	const struct v90_cp_parameters *parameters, enum g711_law law,
	uint8_t maximum_spectral_lookahead,
	struct v90_pcm_mapping *mapping)
{
	return parameters != NULL && mapping != NULL &&
	       parameters->kind == V90_CP_TRAINING &&
	       parameters->law == law &&
	       parameters->spectral_lookahead <=
		       maximum_spectral_lookahead &&
	       !parameters->request_silence &&
	       !parameters->acknowledges_peer &&
	       v90_pcm_mapping_init(mapping, parameters) ==
		       V90_PCM_MAPPING_OK;
}

bool v90_phase4_digital_station_is_valid(
	const struct v90_phase4_digital_station *station)
{
	bool parameters_are_valid;

	if (station == NULL ||
	    !v90_phase4_cp_line_rx_is_valid(&station->receive) ||
	    !v90_phase4_ri_tx_is_valid(&station->transmit) ||
	    station->maximum_spectral_lookahead <
		    V90_JD_MINIMUM_LOOKAHEAD ||
	    station->maximum_spectral_lookahead >
		    V90_JD_MAXIMUM_LOOKAHEAD ||
	    v90_phase4_cp_line_rx_has_pending_record(&station->receive) ||
	    (v90_phase4_cp_line_rx_has_failed(&station->receive) &&
	     !station->failed) ||
	    station->cpt_received != station->transmit.finish_requested)
		return false;
	parameters_are_valid = training_mapping_is_supported(
		&station->training_mapping, station->transmit.law,
		station->maximum_spectral_lookahead);
	return station->cpt_received == parameters_are_valid;
}

bool v90_phase4_digital_station_has_failed(
	const struct v90_phase4_digital_station *station)
{
	return v90_phase4_digital_station_is_valid(station) && station->failed;
}

enum v90_phase4_digital_station_result v90_phase4_digital_station_init(
	struct v90_phase4_digital_station *station,
	const struct v90_phase3_return_line_handoff *phase3_receive,
	const struct v90_jd_parameters *jd, enum g711_law law,
	uint8_t training_ucode, double equalizer_adaptation)
{
	struct v90_phase4_digital_station initialized = { 0 };

	if (station == NULL || !v90_jd_parameters_are_valid(jd))
		return V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if (v90_phase4_cp_line_rx_init(
		    &initialized.receive, phase3_receive,
		    jd->training_constellation,
		    equalizer_adaptation) != V90_PHASE4_CP_LINE_RX_OK ||
	    v90_phase4_ri_tx_init(
		    &initialized.transmit, law, training_ucode) !=
		    V90_PHASE4_RI_OK)
		return V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	initialized.maximum_spectral_lookahead =
		jd->maximum_spectral_shaping_lookahead;
	if (!v90_phase4_digital_station_is_valid(&initialized))
		return V90_PHASE4_DIGITAL_STATION_NUMERIC_FAILURE;
	*station = initialized;
	return V90_PHASE4_DIGITAL_STATION_OK;
}

bool v90_phase4_digital_station_transmit_complete(
	const struct v90_phase4_digital_station *station)
{
	return v90_phase4_digital_station_is_valid(station) &&
	       !station->failed &&
	       v90_phase4_ri_tx_is_complete(&station->transmit);
}

enum v90_phase4_digital_station_result v90_phase4_digital_station_transmit(
	struct v90_phase4_digital_station *station, uint8_t *network_octet)
{
	struct v90_phase4_digital_station next;
	uint8_t generated;

	if (!v90_phase4_digital_station_is_valid(station) ||
	    network_octet == NULL)
		return V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE4_DIGITAL_STATION_FAILED;
	if (v90_phase4_digital_station_transmit_complete(station))
		return V90_PHASE4_DIGITAL_STATION_COMPLETE;
	next = *station;
	if (v90_phase4_ri_tx_next(&next.transmit, &generated) !=
		    V90_PHASE4_RI_OK ||
	    !v90_phase4_digital_station_is_valid(&next)) {
		station->failed = true;
		return V90_PHASE4_DIGITAL_STATION_NUMERIC_FAILURE;
	}
	*station = next;
	*network_octet = generated;
	return V90_PHASE4_DIGITAL_STATION_OK;
}

bool v90_phase4_digital_station_receive_complete(
	const struct v90_phase4_digital_station *station)
{
	return v90_phase4_digital_station_is_valid(station) &&
	       !station->failed && station->cpt_received;
}

static enum v90_phase4_digital_station_result fail_receive(
	struct v90_phase4_digital_station *station,
	struct v90_phase4_digital_station *next)
{
	next->failed = true;
	if (v90_phase4_digital_station_is_valid(next))
		*station = *next;
	else
		station->failed = true;
	return V90_PHASE4_DIGITAL_STATION_NUMERIC_FAILURE;
}

enum v90_phase4_digital_station_result v90_phase4_digital_station_receive(
	struct v90_phase4_digital_station *station, int16_t sample)
{
	struct v90_phase4_digital_station next;
	struct v90_cp_receive_event event = { 0 };
	enum v90_phase4_cp_line_rx_result receive_result;

	if (!v90_phase4_digital_station_is_valid(station))
		return V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE4_DIGITAL_STATION_FAILED;
	if (v90_phase4_digital_station_receive_complete(station))
		return V90_PHASE4_DIGITAL_STATION_COMPLETE;
	next = *station;
	receive_result = v90_phase4_cp_line_rx_sample(
		&next.receive, sample, &event);
	if (receive_result == V90_PHASE4_CP_LINE_RX_FAILED ||
	    receive_result == V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE)
		return fail_receive(station, &next);
	if (receive_result == V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT ||
	    receive_result == V90_PHASE4_CP_LINE_RX_INVALID_STATE)
		return fail_receive(station, &next);
	if (receive_result == V90_PHASE4_CP_LINE_RX_RECORD_READY) {
		struct v90_pcm_mapping mapping;
		bool supported =
			v90_phase4_cp_line_rx_pending_record_is_valid(
				&next.receive) &&
			initial_cpt_mapping(
				&event.parameters, next.transmit.law,
				next.maximum_spectral_lookahead,
				&mapping);

		if (v90_phase4_cp_line_rx_continue(&next.receive) !=
		    V90_PHASE4_CP_LINE_RX_OK)
			return fail_receive(station, &next);
		if (supported) {
			next.training_mapping = mapping;
			next.cpt_received = true;
			if (v90_phase4_ri_tx_request_finish(&next.transmit) !=
			    V90_PHASE4_RI_OK)
				return fail_receive(station, &next);
		}
	}
	if (!v90_phase4_digital_station_is_valid(&next))
		return fail_receive(station, &next);
	*station = next;
	return V90_PHASE4_DIGITAL_STATION_OK;
}

bool v90_phase4_digital_transmit_handoff_is_valid(
	const struct v90_phase4_digital_transmit_handoff *handoff)
{
	return handoff != NULL &&
	       v90_pcm_mapping_is_valid(&handoff->training_mapping) &&
	       handoff->training_mapping.kind == V90_CP_TRAINING;
}

enum v90_phase4_digital_station_result
v90_phase4_digital_station_transmit_handoff(
	const struct v90_phase4_digital_station *station,
	struct v90_phase4_digital_transmit_handoff *handoff)
{
	struct v90_phase4_digital_transmit_handoff generated;

	if (!v90_phase4_digital_station_is_valid(station) || handoff == NULL)
		return V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE4_DIGITAL_STATION_FAILED;
	if (!v90_phase4_digital_station_transmit_complete(station))
		return V90_PHASE4_DIGITAL_STATION_INVALID_STATE;
	generated = (struct v90_phase4_digital_transmit_handoff){
		.training_mapping = station->training_mapping,
	};
	if (!v90_phase4_digital_transmit_handoff_is_valid(&generated))
		return V90_PHASE4_DIGITAL_STATION_NUMERIC_FAILURE;
	*handoff = generated;
	return V90_PHASE4_DIGITAL_STATION_OK;
}

enum v90_phase4_digital_station_result
v90_phase4_digital_station_receive_handoff(
	const struct v90_phase4_digital_station *station,
	struct v90_phase4_cp_line_rx *receive)
{
	if (!v90_phase4_digital_station_is_valid(station) || receive == NULL)
		return V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE4_DIGITAL_STATION_FAILED;
	if (!v90_phase4_digital_station_receive_complete(station))
		return V90_PHASE4_DIGITAL_STATION_INVALID_STATE;
	*receive = station->receive;
	return V90_PHASE4_DIGITAL_STATION_OK;
}

const char *v90_phase4_digital_station_result_name(
	enum v90_phase4_digital_station_result result)
{
	switch (result) {
	case V90_PHASE4_DIGITAL_STATION_OK:
		return "ok";
	case V90_PHASE4_DIGITAL_STATION_COMPLETE:
		return "complete";
	case V90_PHASE4_DIGITAL_STATION_FAILED:
		return "failed";
	case V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE4_DIGITAL_STATION_INVALID_STATE:
		return "invalid state";
	case V90_PHASE4_DIGITAL_STATION_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
