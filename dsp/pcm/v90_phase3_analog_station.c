// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_analog_station.h"

#include "v90_info1.h"
#include "v90_phase3_sd.h"
#include "v90_phase3_signs.h"

#include <stddef.h>
#include <string.h>

static bool transmit_owned_state_is_valid(
	const struct v90_phase3_analog_station *station)
{
	switch (station->transmit_stage) {
	case V90_PHASE3_ANALOG_TRANSMIT_INITIAL:
		return v90_phase3_analog_line_tx_owned_state_is_valid(
			&station->transmit.initial);
	case V90_PHASE3_ANALOG_TRANSMIT_RETURN:
	case V90_PHASE3_ANALOG_TRANSMIT_DIL_RETURN:
		return v90_phase3_return_line_tx_is_valid(
			&station->transmit.returning);
	}
	return false;
}

static bool dil_state_is_valid(
	const struct v90_phase3_analog_station *station)
{
	bool signs_complete =
		station->receive_stage == V90_PHASE3_ANALOG_RECEIVE_SIGNS &&
		pcm_phase3_j_receiver_is_complete(&station->receive.signs);

	if (!v90_dil_profile_is_valid(station->dil_profile))
		return false;
	if (station->dil_profile == V90_DIL_PROFILE_NONE)
		return station->dil_training_symbols == 0u &&
		       station->dil_received_symbols == 0u &&
		       station->transmit_stage !=
			       V90_PHASE3_ANALOG_TRANSMIT_DIL_RETURN;
	if (station->dil_training_symbols == 0u ||
	    station->dil_received_symbols > station->dil_training_symbols + 1u ||
	    (!signs_complete && station->dil_received_symbols != 0u))
		return false;
	if (station->dil_received_symbols <= station->dil_training_symbols)
		return true;
	return station->transmit_stage ==
		       V90_PHASE3_ANALOG_TRANSMIT_DIL_RETURN &&
	       station->transmit.returning.points.stage !=
		       V90_PHASE3_RETURN_TX_SILENCE;
}

static bool receive_is_valid(
	const struct v90_phase3_analog_station *station)
{
	switch (station->receive_stage) {
	case V90_PHASE3_ANALOG_RECEIVE_SD:
		return pcm_s_pattern_rx_is_valid(&station->receive.sd) &&
		       station->receive.sd.signal_ucode ==
			       ((station->training_ucode +
			         V90_PHASE3_SD_TRAINING_OFFSET) & 0x7fu);
	case V90_PHASE3_ANALOG_RECEIVE_SIGNS:
		return station->receive.signs.protocol == PCM_PROTOCOL_V90 &&
		       pcm_phase3_j_receiver_is_valid(
			&station->receive.signs) &&
		       station->receive.signs.training_ucode == station->training_ucode;
	}
	return false;
}

static bool return_controls_are_causal(
	const struct v90_phase3_analog_station *station)
{
	const struct v90_phase3_return_tx *returning;

	if (station->transmit_stage == V90_PHASE3_ANALOG_TRANSMIT_INITIAL)
		return true;
	returning = &station->transmit.returning.points;
	if (station->transmit_stage ==
	    V90_PHASE3_ANALOG_TRANSMIT_DIL_RETURN) {
		if (station->dil_profile != V90_DIL_PROFILE_CONVENTIONAL ||
		    station->receive_stage != V90_PHASE3_ANALOG_RECEIVE_SIGNS ||
		    !pcm_phase3_j_receiver_is_complete(
			    &station->receive.signs))
			return false;
		if (returning->stage == V90_PHASE3_RETURN_TX_SILENCE)
			return station->dil_received_symbols <=
			       station->dil_training_symbols;
		return station->dil_received_symbols >=
			       station->dil_training_symbols &&
		       returning->finish_s_requested;
	}
	if (returning->stage == V90_PHASE3_RETURN_TX_SILENCE)
		return true;
	if (station->receive_stage != V90_PHASE3_ANALOG_RECEIVE_SIGNS ||
	    station->receive.signs.stage < PCM_PHASE3_J_RECEIVER_JD)
		return false;
	return !returning->finish_s_requested ||
	       pcm_phase3_j_receiver_is_complete(&station->receive.signs);
}

bool v90_phase3_analog_station_owned_state_is_valid(
	const struct v90_phase3_analog_station *station)
{
	return station != NULL &&
	       station->training_ucode >= V90_INFO1A_MIN_TRAINING_UCODE &&
	       station->training_ucode <= V90_INFO1A_MAX_TRAINING_UCODE &&
	       transmit_owned_state_is_valid(station) &&
	       receive_is_valid(station) && dil_state_is_valid(station) &&
	       return_controls_are_causal(station);
}

bool v90_phase3_analog_station_is_valid(
	const struct v90_phase3_analog_station *station)
{
	return v90_phase3_analog_station_owned_state_is_valid(station) &&
	       (station->transmit_stage != V90_PHASE3_ANALOG_TRANSMIT_INITIAL ||
		v90_phase3_analog_line_tx_is_valid(
			&station->transmit.initial));
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
	int16_t nominal_peak_amplitude, enum v90_dil_profile dil_profile)
{
	struct v90_phase3_analog_station initialized = {
		.dil_profile = dil_profile,
		.training_ucode = training_ucode,
	};
	struct v90_dil_parameters dil;
	enum v90_phase3_analog_line_tx_result transmit_result;

	if (station == NULL || !g711_law_is_valid(law) ||
	    training_ucode < V90_INFO1A_MIN_TRAINING_UCODE ||
	    training_ucode > V90_INFO1A_MAX_TRAINING_UCODE ||
	    v90_dil_profile_init(&dil, dil_profile) != V90_DIL_OK ||
	    !v90_dil_training_symbol_count(
		    &dil, &initialized.dil_training_symbols))
		return V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	transmit_result = v90_phase3_analog_line_tx_init(
		&initialized.transmit.initial, analog_profile,
		nominal_peak_amplitude);
	if (transmit_result == V90_PHASE3_ANALOG_LINE_TX_OUT_OF_RANGE)
		return V90_PHASE3_ANALOG_STATION_OUT_OF_RANGE;
	if (transmit_result == V90_PHASE3_ANALOG_LINE_TX_CLIPPING_RISK)
		return V90_PHASE3_ANALOG_STATION_CLIPPING_RISK;
	if (transmit_result != V90_PHASE3_ANALOG_LINE_TX_OK ||
	    pcm_s_pattern_rx_init(
		    &initialized.receive.sd, law,
		    (training_ucode + V90_PHASE3_SD_TRAINING_OFFSET) & 0x7fu) !=
		    PCM_S_PATTERN_RX_OK ||
	    !v90_phase3_analog_station_is_valid(&initialized))
		return V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	if (dil.segment_count != 0 &&
	    !pcm_dil_receiver_init(&initialized.dil_receiver, &dil, law))
		return V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	*station = initialized;
	return V90_PHASE3_ANALOG_STATION_OK;
}

static bool sd_bar_seen(
	const struct v90_phase3_analog_station *station)
{
	return station->receive_stage == V90_PHASE3_ANALOG_RECEIVE_SIGNS ||
	       station->receive.sd.stage >= PCM_S_PATTERN_RX_REVERSED;
}

static enum v90_phase3_analog_station_result start_or_finish_ja(
	struct v90_phase3_analog_station *station)
{
	struct v90_phase3_analog_tx *symbols =
		&station->transmit.initial.symbols;
	struct v90_dil_parameters dil;
	enum v90_phase3_analog_line_tx_result result;

	if (symbols->stage == V90_PHASE3_ANALOG_TX_PREFIX &&
	    symbols->sequence.prefix.stage == V34_PHASE3_TRANSMITTER_TRN &&
	    symbols->sequence.prefix.trn_symbols ==
		    V34_TRAINING_SEQUENCE_MIN_SYMBOLS) {
		if (v90_dil_profile_init(&dil, station->dil_profile) !=
		    V90_DIL_OK)
			return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
		result = v90_phase3_analog_line_tx_start_dil_ja(
			&station->transmit.initial, &dil);
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

static enum v90_phase3_analog_station_result enter_dil_return_transmit(
	struct v90_phase3_analog_station *station)
{
	struct v34_training_line line;
	struct v90_phase3_return_line_tx returning;

	if (station->dil_profile == V90_DIL_PROFILE_NONE ||
	    !v90_phase3_return_line_tx_is_handoff_ready(
		    &station->transmit.returning))
		return V90_PHASE3_ANALOG_STATION_OK;
	if (v90_phase3_return_line_tx_handoff(
		    &station->transmit.returning, &line) !=
		    V90_PHASE3_RETURN_LINE_TX_OK ||
	    v90_phase3_return_line_tx_init_scr(
		    &returning, &line,
		    station->receive.signs.parameters.v90.training_constellation) !=
		    V90_PHASE3_RETURN_LINE_TX_OK)
		return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	station->transmit.returning = returning;
	station->transmit_stage = V90_PHASE3_ANALOG_TRANSMIT_DIL_RETURN;
	return V90_PHASE3_ANALOG_STATION_OK;
}

static enum v90_phase3_analog_station_result enter_return_transmit(
	struct v90_phase3_analog_station *station)
{
	struct v34_training_line line;
	struct v90_phase3_return_line_tx returning;

	if (!v90_phase3_analog_line_tx_owned_handoff_is_ready(
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
		station->receive.signs.stage >= PCM_PHASE3_J_RECEIVER_JD;
	bool jd_prime_received = jd_received &&
		pcm_phase3_j_receiver_is_complete(&station->receive.signs);

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

static enum v90_phase3_analog_station_result control_dil_return_transmit(
	struct v90_phase3_analog_station *station)
{
	struct v90_phase3_return_line_tx *returning =
		&station->transmit.returning;

	if (station->dil_received_symbols < station->dil_training_symbols ||
	    returning->points.stage != V90_PHASE3_RETURN_TX_SILENCE)
		return V90_PHASE3_ANALOG_STATION_OK;
	if (v90_phase3_return_line_tx_start_s(returning) !=
		    V90_PHASE3_RETURN_LINE_TX_OK ||
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
	if (station->transmit_stage == V90_PHASE3_ANALOG_TRANSMIT_RETURN) {
		result = control_return_transmit(station);
		if (result != V90_PHASE3_ANALOG_STATION_OK)
			return result;
		result = enter_dil_return_transmit(station);
		if (result != V90_PHASE3_ANALOG_STATION_OK ||
		    station->transmit_stage == V90_PHASE3_ANALOG_TRANSMIT_RETURN)
			return result;
	}
	return control_dil_return_transmit(station);
}

/* Completion predicates below require validated owned state. */
static bool transmit_is_complete(
	const struct v90_phase3_analog_station *station)
{
	return !station->failed &&
	       station->transmit_stage ==
		       (station->dil_profile == V90_DIL_PROFILE_NONE ?
			       V90_PHASE3_ANALOG_TRANSMIT_RETURN :
			       V90_PHASE3_ANALOG_TRANSMIT_DIL_RETURN) &&
	       v90_phase3_return_line_tx_is_handoff_ready(
		       &station->transmit.returning);
}

bool v90_phase3_analog_station_transmit_complete(
	const struct v90_phase3_analog_station *station)
{
	return v90_phase3_analog_station_owned_state_is_valid(station) &&
	       transmit_is_complete(station);
}

enum v90_phase3_analog_station_result v90_phase3_analog_station_transmit(
	struct v90_phase3_analog_station *station, int16_t *sample)
{
	struct v90_phase3_analog_station next;
	enum v90_phase3_analog_station_result control_result;
	int16_t generated;

	if (!v90_phase3_analog_station_owned_state_is_valid(station) ||
	    sample == NULL)
		return V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE3_ANALOG_STATION_FAILED;
	if (transmit_is_complete(station))
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
	if (!v90_phase3_analog_station_owned_state_is_valid(&next)) {
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
	enum pcm_s_pattern_rx_result result =
		pcm_s_pattern_rx_receive(
			&station->receive.sd, network_octet);

	if (result == PCM_S_PATTERN_RX_INVALID_SEQUENCE)
		return V90_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH;
	if (result == PCM_S_PATTERN_RX_INVALID_ARGUMENT ||
	    result == PCM_S_PATTERN_RX_INVALID_STATE)
		return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	if (result != PCM_S_PATTERN_RX_SEQUENCE_COMPLETE)
		return V90_PHASE3_ANALOG_STATION_OK;
	{
		struct pcm_phase3_j_receiver signs;

		if (pcm_phase3_j_receiver_init(
			    &signs, PCM_PROTOCOL_V90, station->receive.sd.law,
			    station->training_ucode) !=
		    PCM_PHASE3_J_RECEIVER_OK)
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
	enum pcm_phase3_j_receiver_result result =
		pcm_phase3_j_receiver_receive(
			&station->receive.signs, network_octet);

	if (result == PCM_PHASE3_J_RECEIVER_INVALID_SEQUENCE)
		return V90_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH;
	if (result == PCM_PHASE3_J_RECEIVER_INVALID_ARGUMENT ||
	    result == PCM_PHASE3_J_RECEIVER_INVALID_STATE)
		return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	if (result == PCM_PHASE3_J_RECEIVER_TRN1D_READY &&
	    pcm_phase3_j_receiver_begin_jd_search(
		    &station->receive.signs) != PCM_PHASE3_J_RECEIVER_OK)
		return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
	return V90_PHASE3_ANALOG_STATION_OK;
}

static enum v90_phase3_analog_station_result receive_dil(
	struct v90_phase3_analog_station *station, uint8_t octet)
{
	if (station->dil_received_symbols < station->dil_training_symbols) {
		if (!pcm_dil_receiver_receive(&station->dil_receiver, octet))
			return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
		station->dil_received_symbols++;
		if (station->dil_received_symbols == station->dil_training_symbols &&
		    !pcm_dil_receiver_finish(&station->dil_receiver, &station->dil_result))
			return V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE;
		return V90_PHASE3_ANALOG_STATION_OK;
	}
	if (station->dil_received_symbols == station->dil_training_symbols &&
	    station->transmit_stage ==
		    V90_PHASE3_ANALOG_TRANSMIT_DIL_RETURN &&
	    station->transmit.returning.points.stage !=
		    V90_PHASE3_RETURN_TX_SILENCE)
		station->dil_received_symbols++;
	return V90_PHASE3_ANALOG_STATION_OK;
}

static bool receive_is_complete(
	const struct v90_phase3_analog_station *station)
{
	return !station->failed &&
	       station->receive_stage == V90_PHASE3_ANALOG_RECEIVE_SIGNS &&
	       pcm_phase3_j_receiver_is_complete(&station->receive.signs) &&
	       (station->dil_profile == V90_DIL_PROFILE_NONE ||
		station->dil_received_symbols > station->dil_training_symbols);
}

bool v90_phase3_analog_station_receive_complete(
	const struct v90_phase3_analog_station *station)
{
	return v90_phase3_analog_station_owned_state_is_valid(station) &&
	       receive_is_complete(station);
}

enum v90_phase3_analog_station_result v90_phase3_analog_station_receive(
	struct v90_phase3_analog_station *station, uint8_t network_octet)
{
	struct v90_phase3_analog_station next;
	enum v90_phase3_analog_station_result result;

	if (!v90_phase3_analog_station_owned_state_is_valid(station))
		return V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE3_ANALOG_STATION_FAILED;
	if (receive_is_complete(station))
		return V90_PHASE3_ANALOG_STATION_COMPLETE;
	next = *station;
	if (next.receive_stage == V90_PHASE3_ANALOG_RECEIVE_SD)
		result = receive_sd(&next, network_octet);
	else if (!pcm_phase3_j_receiver_is_complete(&next.receive.signs))
		result = receive_sign(&next, network_octet);
	else
		result = receive_dil(&next, network_octet);
	if (result != V90_PHASE3_ANALOG_STATION_OK) {
		station->failed = true;
		return result;
	}
	if (!v90_phase3_analog_station_owned_state_is_valid(&next)) {
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
	const struct pcm_phase3_j_receiver *signs;

	if (!v90_phase3_analog_station_is_valid(station) || handoff == NULL)
		return V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT;
	if (station->failed)
		return V90_PHASE3_ANALOG_STATION_FAILED;
	if (!v90_phase3_analog_station_receive_complete(station))
		return V90_PHASE3_ANALOG_STATION_INVALID_STATE;
	signs = &station->receive.signs;
	generated = (struct v90_phase3_analog_receive_handoff){
		.descrambler = signs->descrambler,
		.jd = signs->parameters.v90,
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
