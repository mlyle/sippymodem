// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_analog_station.h"

#include "v90_info1.h"
#include "v90_phase3_signs.h"

#include <stddef.h>
#include <string.h>

static bool transmit_is_valid(
	const struct v90_phase3_analog_station *station)
{
	switch (station->transmit_stage) {
	case V90_PHASE3_ANALOG_TRANSMIT_INITIAL:
		return v90_phase3_analog_line_tx_is_valid(
			&station->transmit.initial);
	case V90_PHASE3_ANALOG_TRANSMIT_RETURN:
		return v90_phase3_return_line_tx_is_valid(
			&station->transmit.returning);
	}
	return false;
}

static bool receive_is_valid(
	const struct v90_phase3_analog_station *station)
{
	switch (station->receive_stage) {
	case V90_PHASE3_ANALOG_RECEIVE_SD:
		return v90_phase3_sd_receiver_is_valid(&station->receive.sd);
	case V90_PHASE3_ANALOG_RECEIVE_SIGNS:
		return v90_phase3_sign_receiver_is_valid(
			&station->receive.signs);
	}
	return false;
}

static bool return_controls_are_causal(
	const struct v90_phase3_analog_station *station)
{
	const struct v90_phase3_return_tx *returning;

	if (station->transmit_stage != V90_PHASE3_ANALOG_TRANSMIT_RETURN)
		return true;
	returning = &station->transmit.returning.points;
	if (returning->stage == V90_PHASE3_RETURN_TX_SILENCE)
		return true;
	if (station->receive_stage != V90_PHASE3_ANALOG_RECEIVE_SIGNS ||
	    station->receive.signs.stage < V90_PHASE3_SIGN_RECEIVER_JD)
		return false;
	return !returning->finish_s_requested ||
	       v90_phase3_sign_receiver_is_complete(&station->receive.signs);
}

bool v90_phase3_analog_station_is_valid(
	const struct v90_phase3_analog_station *station)
{
	return station != NULL && transmit_is_valid(station) &&
	       receive_is_valid(station) && return_controls_are_causal(station);
}

bool v90_phase3_analog_station_has_failed(
	const struct v90_phase3_analog_station *station)
{
	return v90_phase3_analog_station_is_valid(station) && station->failed;
}

enum v90_phase3_analog_station_result v90_phase3_analog_station_init(
	struct v90_phase3_analog_station *station,
	const struct v34_phase3_profile *analog_profile,
	enum g711_law law, uint8_t training_ucode,
	int16_t nominal_peak_amplitude)
{
	struct v90_phase3_analog_station initialized = { 0 };
	enum v90_phase3_analog_line_tx_result transmit_result;

	if (station == NULL || !g711_law_is_valid(law) ||
	    training_ucode < V90_INFO1A_MIN_TRAINING_UCODE ||
	    training_ucode > V90_INFO1A_MAX_TRAINING_UCODE)
		return V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	transmit_result = v90_phase3_analog_line_tx_init(
		&initialized.transmit.initial, analog_profile,
		nominal_peak_amplitude);
	if (transmit_result == V90_PHASE3_ANALOG_LINE_TX_OUT_OF_RANGE)
		return V90_PHASE3_ANALOG_STATION_OUT_OF_RANGE;
	if (transmit_result == V90_PHASE3_ANALOG_LINE_TX_CLIPPING_RISK)
		return V90_PHASE3_ANALOG_STATION_CLIPPING_RISK;
	if (transmit_result != V90_PHASE3_ANALOG_LINE_TX_OK ||
	    v90_phase3_sd_receiver_init(
		    &initialized.receive.sd, law, training_ucode) !=
		    V90_PHASE3_SD_RECEIVER_OK ||
	    !v90_phase3_analog_station_is_valid(&initialized))
		return V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	*station = initialized;
	return V90_PHASE3_ANALOG_STATION_OK;
}

static bool sd_bar_seen(
	const struct v90_phase3_analog_station *station)
{
	return station->receive_stage == V90_PHASE3_ANALOG_RECEIVE_SIGNS ||
	       station->receive.sd.stage >= V90_PHASE3_SD_RECEIVER_SD_BAR;
}

static enum v90_phase3_analog_station_result start_or_finish_ja(
	struct v90_phase3_analog_station *station)
{
	struct v90_phase3_analog_tx *symbols =
		&station->transmit.initial.symbols;
	enum v90_phase3_analog_line_tx_result result;

	if (symbols->stage == V90_PHASE3_ANALOG_TX_PREFIX &&
	    symbols->sequence.prefix.stage == V34_PHASE3_TRANSMITTER_TRN &&
	    symbols->sequence.prefix.trn_symbols == V34_PHASE3_TRN_MIN_SYMBOLS) {
		result = v90_phase3_analog_line_tx_start_ja(
			&station->transmit.initial);
		if (result != V90_PHASE3_ANALOG_LINE_TX_OK)
			return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	}
	if (symbols->stage != V90_PHASE3_ANALOG_TX_JA || !sd_bar_seen(station))
		return V90_PHASE3_ANALOG_STATION_OK;
	result = v90_phase3_analog_line_tx_finish_ja(
		&station->transmit.initial);
	if (result == V90_PHASE3_ANALOG_LINE_TX_INVALID_STATE)
		return V90_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH;
	return result == V90_PHASE3_ANALOG_LINE_TX_OK ?
		       V90_PHASE3_ANALOG_STATION_OK :
		       V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
}

static enum v90_phase3_analog_station_result enter_return_transmit(
	struct v90_phase3_analog_station *station)
{
	struct v34_training_line line;
	struct v90_phase3_return_line_tx returning;

	if (!v90_phase3_analog_line_tx_is_handoff_ready(
		    &station->transmit.initial))
		return V90_PHASE3_ANALOG_STATION_OK;
	if (v90_phase3_analog_line_tx_handoff(
		    &station->transmit.initial, &line) !=
		    V90_PHASE3_ANALOG_LINE_TX_OK ||
	    v90_phase3_return_line_tx_init(&returning, &line) !=
		    V90_PHASE3_RETURN_LINE_TX_OK)
		return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	station->transmit = (union v90_phase3_analog_station_transmit){ 0 };
	station->transmit.returning = returning;
	station->transmit_stage = V90_PHASE3_ANALOG_TRANSMIT_RETURN;
	return V90_PHASE3_ANALOG_STATION_OK;
}

static enum v90_phase3_analog_station_result control_return_transmit(
	struct v90_phase3_analog_station *station)
{
	struct v90_phase3_return_line_tx *returning =
		&station->transmit.returning;
	bool jd_received =
		station->receive_stage == V90_PHASE3_ANALOG_RECEIVE_SIGNS &&
		station->receive.signs.stage >= V90_PHASE3_SIGN_RECEIVER_JD;
	bool jd_prime_received = jd_received &&
		v90_phase3_sign_receiver_is_complete(&station->receive.signs);

	if (jd_received &&
	    returning->points.stage == V90_PHASE3_RETURN_TX_SILENCE &&
	    v90_phase3_return_line_tx_start_s(returning) !=
		    V90_PHASE3_RETURN_LINE_TX_OK)
		return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	if (jd_prime_received &&
	    returning->points.stage == V90_PHASE3_RETURN_TX_S &&
	    v90_phase3_return_line_tx_finish_s(returning) !=
		    V90_PHASE3_RETURN_LINE_TX_OK)
		return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	return V90_PHASE3_ANALOG_STATION_OK;
}

static enum v90_phase3_analog_station_result control_transmit(
	struct v90_phase3_analog_station *station)
{
	enum v90_phase3_analog_station_result result;

	if (station->transmit_stage == V90_PHASE3_ANALOG_TRANSMIT_INITIAL) {
		result = start_or_finish_ja(station);
		if (result != V90_PHASE3_ANALOG_STATION_OK)
			return result;
		result = enter_return_transmit(station);
		if (result != V90_PHASE3_ANALOG_STATION_OK ||
		    station->transmit_stage == V90_PHASE3_ANALOG_TRANSMIT_INITIAL)
			return result;
	}
	return control_return_transmit(station);
}

bool v90_phase3_analog_station_transmit_complete(
	const struct v90_phase3_analog_station *station)
{
	return v90_phase3_analog_station_is_valid(station) &&
	       !station->failed &&
	       station->transmit_stage == V90_PHASE3_ANALOG_TRANSMIT_RETURN &&
	       v90_phase3_return_line_tx_is_handoff_ready(
		       &station->transmit.returning);
}

enum v90_phase3_analog_station_result v90_phase3_analog_station_transmit(
	struct v90_phase3_analog_station *station, int16_t *sample)
{
	struct v90_phase3_analog_station next;
	enum v90_phase3_analog_station_result control_result;
	int16_t generated;

	if (!v90_phase3_analog_station_is_valid(station) || sample == NULL)
		return V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE3_ANALOG_STATION_FAILED;
	if (v90_phase3_analog_station_transmit_complete(station))
		return V90_PHASE3_ANALOG_STATION_COMPLETE;
	next = *station;
	control_result = control_transmit(&next);
	if (control_result != V90_PHASE3_ANALOG_STATION_OK) {
		station->failed = true;
		return control_result;
	}
	if (next.transmit_stage == V90_PHASE3_ANALOG_TRANSMIT_INITIAL) {
		enum v90_phase3_analog_line_tx_result result =
			v90_phase3_analog_line_tx_sample(
				&next.transmit.initial, &generated);

		if (result == V90_PHASE3_ANALOG_LINE_TX_OUT_OF_RANGE) {
			station->failed = true;
			return V90_PHASE3_ANALOG_STATION_OUT_OF_RANGE;
		}
		if (result != V90_PHASE3_ANALOG_LINE_TX_OK) {
			station->failed = true;
			return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
		}
	} else if (v90_phase3_return_line_tx_sample(
			   &next.transmit.returning, &generated) !=
		   V90_PHASE3_RETURN_LINE_TX_OK) {
		station->failed = true;
		return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	}
	if (!v90_phase3_analog_station_is_valid(&next)) {
		station->failed = true;
		return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	}
	*station = next;
	*sample = generated;
	return V90_PHASE3_ANALOG_STATION_OK;
}

static enum v90_phase3_analog_station_result receive_sd(
	struct v90_phase3_analog_station *station, uint8_t network_octet)
{
	enum v90_phase3_sd_receiver_result result =
		v90_phase3_sd_receiver_receive(
			&station->receive.sd, network_octet);

	if (result == V90_PHASE3_SD_RECEIVER_INVALID_SEQUENCE)
		return V90_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH;
	if (result == V90_PHASE3_SD_RECEIVER_INVALID_ARGUMENT ||
	    result == V90_PHASE3_SD_RECEIVER_INVALID_STATE)
		return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	if (result != V90_PHASE3_SD_RECEIVER_SEQUENCE_COMPLETE)
		return V90_PHASE3_ANALOG_STATION_OK;
	{
		struct v90_phase3_sign_receiver signs;

		if (v90_phase3_sign_receiver_init(
			    &signs, station->receive.sd.law,
			    station->receive.sd.training_ucode) !=
		    V90_PHASE3_SIGN_RECEIVER_OK)
			return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
		station->receive =
			(union v90_phase3_analog_station_receive){ 0 };
		station->receive.signs = signs;
		station->receive_stage = V90_PHASE3_ANALOG_RECEIVE_SIGNS;
	}
	return V90_PHASE3_ANALOG_STATION_OK;
}

static enum v90_phase3_analog_station_result receive_sign(
	struct v90_phase3_analog_station *station, uint8_t network_octet)
{
	enum v90_phase3_sign_receiver_result result =
		v90_phase3_sign_receiver_receive(
			&station->receive.signs, network_octet);

	if (result == V90_PHASE3_SIGN_RECEIVER_INVALID_SEQUENCE)
		return V90_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH;
	if (result == V90_PHASE3_SIGN_RECEIVER_INVALID_ARGUMENT ||
	    result == V90_PHASE3_SIGN_RECEIVER_INVALID_STATE)
		return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	if (result == V90_PHASE3_SIGN_RECEIVER_TRN1D_READY &&
	    v90_phase3_sign_receiver_begin_jd_search(
		    &station->receive.signs) != V90_PHASE3_SIGN_RECEIVER_OK)
		return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	return V90_PHASE3_ANALOG_STATION_OK;
}

bool v90_phase3_analog_station_receive_complete(
	const struct v90_phase3_analog_station *station)
{
	return v90_phase3_analog_station_is_valid(station) &&
	       !station->failed &&
	       station->receive_stage == V90_PHASE3_ANALOG_RECEIVE_SIGNS &&
	       v90_phase3_sign_receiver_is_complete(&station->receive.signs);
}

enum v90_phase3_analog_station_result v90_phase3_analog_station_receive(
	struct v90_phase3_analog_station *station, uint8_t network_octet)
{
	struct v90_phase3_analog_station next;
	enum v90_phase3_analog_station_result result;

	if (!v90_phase3_analog_station_is_valid(station))
		return V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE3_ANALOG_STATION_FAILED;
	if (v90_phase3_analog_station_receive_complete(station))
		return V90_PHASE3_ANALOG_STATION_COMPLETE;
	next = *station;
	result = next.receive_stage == V90_PHASE3_ANALOG_RECEIVE_SD ?
			 receive_sd(&next, network_octet) :
			 receive_sign(&next, network_octet);
	if (result != V90_PHASE3_ANALOG_STATION_OK) {
		station->failed = true;
		return result;
	}
	if (!v90_phase3_analog_station_is_valid(&next)) {
		station->failed = true;
		return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	}
	*station = next;
	return V90_PHASE3_ANALOG_STATION_OK;
}

enum v90_phase3_analog_station_result
v90_phase3_analog_station_transmit_handoff(
	const struct v90_phase3_analog_station *station,
	struct v34_training_line *line)
{
	if (!v90_phase3_analog_station_is_valid(station) || line == NULL)
		return V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE3_ANALOG_STATION_FAILED;
	if (!v90_phase3_analog_station_transmit_complete(station))
		return V90_PHASE3_ANALOG_STATION_INVALID_STATE;
	return v90_phase3_return_line_tx_handoff(
		       &station->transmit.returning, line) ==
		       V90_PHASE3_RETURN_LINE_TX_OK ?
		       V90_PHASE3_ANALOG_STATION_OK :
		       V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
}

bool v90_phase3_analog_receive_handoff_is_valid(
	const struct v90_phase3_analog_receive_handoff *handoff)
{
	return handoff != NULL &&
	       v34_scrambler_is_valid(&handoff->descrambler) &&
	       handoff->descrambler.polynomial == V34_POLYNOMIAL_GPC &&
	       v90_jd_parameters_are_valid(&handoff->jd) &&
	       g711_law_is_valid(handoff->law) &&
	       handoff->trn1d_symbols >= V90_PHASE3_TRN1D_MIN_SYMBOLS &&
	       handoff->trn1d_symbols % V90_PHASE3_TRN1D_ALIGNMENT == 0 &&
	       handoff->training_ucode >= V90_INFO1A_MIN_TRAINING_UCODE &&
	       handoff->training_ucode <= V90_INFO1A_MAX_TRAINING_UCODE &&
	       handoff->last_sign <= 1;
}

enum v90_phase3_analog_station_result
v90_phase3_analog_station_receive_handoff(
	const struct v90_phase3_analog_station *station,
	struct v90_phase3_analog_receive_handoff *handoff)
{
	struct v90_phase3_analog_receive_handoff generated;
	const struct v90_phase3_sign_receiver *signs;

	if (!v90_phase3_analog_station_is_valid(station) || handoff == NULL)
		return V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE3_ANALOG_STATION_FAILED;
	if (!v90_phase3_analog_station_receive_complete(station))
		return V90_PHASE3_ANALOG_STATION_INVALID_STATE;
	signs = &station->receive.signs;
	generated = (struct v90_phase3_analog_receive_handoff){
		.descrambler = signs->descrambler,
		.jd = signs->jd,
		.law = signs->law,
		.trn1d_symbols = signs->trn1d_symbols,
		.training_ucode = signs->training_ucode,
		.last_sign = signs->last_sign,
	};
	if (!v90_phase3_analog_receive_handoff_is_valid(&generated))
		return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	*handoff = generated;
	return V90_PHASE3_ANALOG_STATION_OK;
}

const char *v90_phase3_analog_station_result_name(
	enum v90_phase3_analog_station_result result)
{
	switch (result) {
	case V90_PHASE3_ANALOG_STATION_OK:
		return "ok";
	case V90_PHASE3_ANALOG_STATION_COMPLETE:
		return "complete";
	case V90_PHASE3_ANALOG_STATION_FAILED:
		return "failed";
	case V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_ANALOG_STATION_INVALID_STATE:
		return "invalid state";
	case V90_PHASE3_ANALOG_STATION_OUT_OF_RANGE:
		return "out of range";
	case V90_PHASE3_ANALOG_STATION_CLIPPING_RISK:
		return "clipping risk";
	case V90_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
