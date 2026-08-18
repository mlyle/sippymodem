// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_analog_station.h"

static bool training_parameters_are_supported(
	const struct v90_cp_parameters *parameters,
	enum g711_law law, uint8_t maximum_spectral_lookahead)
{
	struct v90_pcm_mapping mapping;

	return parameters != NULL &&
	       parameters->kind == V90_CP_TRAINING &&
	       parameters->law == law &&
	       parameters->spectral_lookahead <=
		       maximum_spectral_lookahead &&
	       v90_pcm_mapping_init(&mapping, parameters) ==
		       V90_PCM_MAPPING_OK;
}

bool v90_phase4_analog_station_is_valid(
	const struct v90_phase4_analog_station *station)
{
	if (station == NULL ||
	    !v90_phase4_cp_line_tx_is_valid(&station->transmit) ||
	    !v90_phase4_ri_search_is_valid(&station->receive) ||
	    station->maximum_spectral_lookahead <
		    V90_JD_MINIMUM_LOOKAHEAD ||
	    station->maximum_spectral_lookahead >
		    V90_JD_MAXIMUM_LOOKAHEAD ||
	    !training_parameters_are_supported(
		    &station->training_parameters,
		    station->receive.law,
		    station->maximum_spectral_lookahead))
		return false;
	return station->finish_cpt_requested ==
	       (station->receive.stage >= V90_PHASE4_RI_SEARCH_BAR);
}

bool v90_phase4_analog_station_has_failed(
	const struct v90_phase4_analog_station *station)
{
	return v90_phase4_analog_station_is_valid(station) && station->failed;
}

static enum v90_phase4_analog_station_result transmit_result(
	enum v90_phase4_cp_line_tx_result result)
{
	switch (result) {
	case V90_PHASE4_CP_LINE_TX_OK:
		return V90_PHASE4_ANALOG_STATION_OK;
	case V90_PHASE4_CP_LINE_TX_COMPLETE:
		return V90_PHASE4_ANALOG_STATION_COMPLETE;
	case V90_PHASE4_CP_LINE_TX_INVALID_PARAMETERS:
		return V90_PHASE4_ANALOG_STATION_INVALID_PARAMETERS;
	case V90_PHASE4_CP_LINE_TX_OUT_OF_RANGE:
		return V90_PHASE4_ANALOG_STATION_OUT_OF_RANGE;
	case V90_PHASE4_CP_LINE_TX_CLIPPING_RISK:
		return V90_PHASE4_ANALOG_STATION_CLIPPING_RISK;
	case V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT:
		return V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT;
	case V90_PHASE4_CP_LINE_TX_RECORD_IN_PROGRESS:
	case V90_PHASE4_CP_LINE_TX_INVALID_STATE:
		return V90_PHASE4_ANALOG_STATION_INVALID_STATE;
	case V90_PHASE4_CP_LINE_TX_INTERNAL_ERROR:
		return V90_PHASE4_ANALOG_STATION_NUMERIC_FAILURE;
	}
	return V90_PHASE4_ANALOG_STATION_NUMERIC_FAILURE;
}

enum v90_phase4_analog_station_result v90_phase4_analog_station_init(
	struct v90_phase4_analog_station *station,
	const struct v34_training_line *phase3_transmit,
	const struct v90_jd_parameters *jd, enum g711_law law,
	uint8_t training_ucode,
	const struct v90_cp_parameters *training_parameters)
{
	struct v90_phase4_ri_search receive;

	if (v90_phase4_ri_search_init(&receive, law, training_ucode) !=
	    V90_PHASE4_RI_SEARCH_OK)
		return V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT;
	return v90_phase4_analog_station_resume(
		station, phase3_transmit, &receive, jd,
		training_parameters);
}

enum v90_phase4_analog_station_result v90_phase4_analog_station_resume(
	struct v90_phase4_analog_station *station,
	const struct v34_training_line *phase3_transmit,
	const struct v90_phase4_ri_search *receive,
	const struct v90_jd_parameters *jd,
	const struct v90_cp_parameters *training_parameters)
{
	struct v90_phase4_analog_station initialized = { 0 };
	enum v90_phase4_analog_station_result result;

	if (station == NULL ||
	    !v90_phase4_ri_search_is_valid(receive) ||
	    !v90_jd_parameters_are_valid(jd))
		return V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT;
	if (!training_parameters_are_supported(
		    training_parameters, receive->law,
		    jd->maximum_spectral_shaping_lookahead))
		return V90_PHASE4_ANALOG_STATION_INVALID_PARAMETERS;
	result = transmit_result(v90_phase4_cp_line_tx_init(
		&initialized.transmit, phase3_transmit,
		jd->training_constellation,
		training_parameters));
	if (result != V90_PHASE4_ANALOG_STATION_OK)
		return result;
	initialized.receive = *receive;
	initialized.training_parameters = *training_parameters;
	initialized.maximum_spectral_lookahead =
		jd->maximum_spectral_shaping_lookahead;
	initialized.finish_cpt_requested =
		receive->stage >= V90_PHASE4_RI_SEARCH_BAR;
	if (!v90_phase4_analog_station_is_valid(&initialized))
		return V90_PHASE4_ANALOG_STATION_NUMERIC_FAILURE;
	*station = initialized;
	return V90_PHASE4_ANALOG_STATION_OK;
}

bool v90_phase4_analog_station_transmit_complete(
	const struct v90_phase4_analog_station *station)
{
	return v90_phase4_analog_station_is_valid(station) &&
	       !station->failed && station->finish_cpt_requested &&
	       v90_phase4_cp_line_tx_is_handoff_ready(&station->transmit);
}

enum v90_phase4_analog_station_result v90_phase4_analog_station_transmit(
	struct v90_phase4_analog_station *station, int16_t *sample)
{
	struct v90_phase4_analog_station next;
	enum v90_phase4_analog_station_result result;
	int16_t generated;

	if (!v90_phase4_analog_station_is_valid(station) || sample == NULL)
		return V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE4_ANALOG_STATION_FAILED;
	if (v90_phase4_analog_station_transmit_complete(station))
		return V90_PHASE4_ANALOG_STATION_COMPLETE;
	next = *station;
	if (v90_phase4_cp_line_tx_record_is_complete(&next.transmit) &&
	    !next.finish_cpt_requested) {
		result = transmit_result(v90_phase4_cp_line_tx_start_next_record(
			&next.transmit, &next.training_parameters));
		if (result != V90_PHASE4_ANALOG_STATION_OK) {
			station->failed = true;
			return result;
		}
	}
	result = transmit_result(
		v90_phase4_cp_line_tx_sample(&next.transmit, &generated));
	if (result != V90_PHASE4_ANALOG_STATION_OK ||
	    !v90_phase4_analog_station_is_valid(&next)) {
		station->failed = true;
		return result == V90_PHASE4_ANALOG_STATION_OK ||
		       result == V90_PHASE4_ANALOG_STATION_COMPLETE ?
			       V90_PHASE4_ANALOG_STATION_NUMERIC_FAILURE :
			       result;
	}
	*station = next;
	*sample = generated;
	return V90_PHASE4_ANALOG_STATION_OK;
}

bool v90_phase4_analog_station_receive_complete(
	const struct v90_phase4_analog_station *station)
{
	return v90_phase4_analog_station_is_valid(station) &&
	       !station->failed &&
	       v90_phase4_ri_search_is_complete(&station->receive);
}

enum v90_phase4_analog_station_result v90_phase4_analog_station_receive(
	struct v90_phase4_analog_station *station, uint8_t network_octet)
{
	struct v90_phase4_analog_station next;
	enum v90_phase4_ri_search_result receive_result;

	if (!v90_phase4_analog_station_is_valid(station))
		return V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE4_ANALOG_STATION_FAILED;
	if (v90_phase4_analog_station_receive_complete(station))
		return V90_PHASE4_ANALOG_STATION_COMPLETE;
	next = *station;
	receive_result = v90_phase4_ri_search_receive(
		&next.receive, network_octet);
	if (receive_result == V90_PHASE4_RI_SEARCH_INVALID_SEQUENCE) {
		station->failed = true;
		return V90_PHASE4_ANALOG_STATION_SEQUENCE_MISMATCH;
	}
	if (receive_result == V90_PHASE4_RI_SEARCH_INVALID_ARGUMENT ||
	    receive_result == V90_PHASE4_RI_SEARCH_INVALID_STATE) {
		station->failed = true;
		return V90_PHASE4_ANALOG_STATION_NUMERIC_FAILURE;
	}
	if (receive_result == V90_PHASE4_RI_SEARCH_TRANSITION_DETECTED)
		next.finish_cpt_requested = true;
	if (!v90_phase4_analog_station_is_valid(&next)) {
		station->failed = true;
		return V90_PHASE4_ANALOG_STATION_NUMERIC_FAILURE;
	}
	*station = next;
	return V90_PHASE4_ANALOG_STATION_OK;
}

enum v90_phase4_analog_station_result
v90_phase4_analog_station_transmit_handoff(
	const struct v90_phase4_analog_station *station,
	struct v90_phase4_cp_line_tx *transmit)
{
	if (!v90_phase4_analog_station_is_valid(station) || transmit == NULL)
		return V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE4_ANALOG_STATION_FAILED;
	if (!v90_phase4_analog_station_transmit_complete(station))
		return V90_PHASE4_ANALOG_STATION_INVALID_STATE;
	*transmit = station->transmit;
	return V90_PHASE4_ANALOG_STATION_OK;
}

bool v90_phase4_analog_receive_handoff_is_valid(
	const struct v90_phase4_analog_receive_handoff *handoff)
{
	return handoff != NULL &&
	       pcm_downstream_rx_is_fresh(&handoff->receiver) &&
	       handoff->receiver.stream.decoder.decision_plane ==
		       V90_PCM_MAPPING_TRANSMIT;
}

enum v90_phase4_analog_station_result
v90_phase4_analog_station_receive_handoff(
	const struct v90_phase4_analog_station *station,
	struct v90_phase4_analog_receive_handoff *handoff)
{
	struct v90_phase4_analog_receive_handoff generated;
	struct v90_pcm_mapping mapping;

	if (!v90_phase4_analog_station_is_valid(station) || handoff == NULL)
		return V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE4_ANALOG_STATION_FAILED;
	if (!v90_phase4_analog_station_receive_complete(station))
		return V90_PHASE4_ANALOG_STATION_INVALID_STATE;
	generated = (struct v90_phase4_analog_receive_handoff){ 0 };
	if (v90_pcm_mapping_init(
		    &mapping, &station->training_parameters) !=
		    V90_PCM_MAPPING_OK ||
	    pcm_downstream_rx_init(
		    &generated.receiver, PCM_PROTOCOL_V90, &mapping,
		    V90_PCM_MAPPING_TRANSMIT) != PCM_DOWNSTREAM_RX_OK)
		return V90_PHASE4_ANALOG_STATION_NUMERIC_FAILURE;
	if (!v90_phase4_analog_receive_handoff_is_valid(&generated))
		return V90_PHASE4_ANALOG_STATION_NUMERIC_FAILURE;
	*handoff = generated;
	return V90_PHASE4_ANALOG_STATION_OK;
}

const char *v90_phase4_analog_station_result_name(
	enum v90_phase4_analog_station_result result)
{
	switch (result) {
	case V90_PHASE4_ANALOG_STATION_OK:
		return "ok";
	case V90_PHASE4_ANALOG_STATION_COMPLETE:
		return "complete";
	case V90_PHASE4_ANALOG_STATION_FAILED:
		return "failed";
	case V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE4_ANALOG_STATION_INVALID_STATE:
		return "invalid state";
	case V90_PHASE4_ANALOG_STATION_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_PHASE4_ANALOG_STATION_OUT_OF_RANGE:
		return "out of range";
	case V90_PHASE4_ANALOG_STATION_CLIPPING_RISK:
		return "clipping risk";
	case V90_PHASE4_ANALOG_STATION_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V90_PHASE4_ANALOG_STATION_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
