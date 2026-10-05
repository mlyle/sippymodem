// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_analog_startup.h"

#include "v90_phase4_owned.h"

#include "memory_region.h"

#include <stddef.h>

static bool handoff_mappings_match(
	const struct v90_phase4_analog_handoff *handoff)
{
	struct v90_cp_parameters parameters;
	struct v90_pcm_mapping expected;
	const struct v90_phase4_cp_tx *symbols = &handoff->transmit.symbols;

	return v34_constellation_size_is_valid(
		       handoff->rate_renegotiation_constellation) &&
	       v90_cp_decode(
		       symbols->frame, sizeof(symbols->frame),
		       symbols->frame_bit_count, &parameters) == V90_CP_OK &&
	       v90_pcm_mapping_init(&expected, &parameters) ==
		       V90_PCM_MAPPING_OK &&
	       v90_pcm_mapping_equal(
		       &handoff->receive.stream.decoder.mapping, &expected);
}

static bool continuation_receive_is_valid(
	const struct v90_phase4_analog_continuation *state)
{
	struct v90_pcm_mapping expected;

	if (!state->receive_ready)
		return !v90_phase4_analog_station_receive_complete(
			       &state->station) &&
		       memory_region_is_zero(
			       &state->receive, sizeof(state->receive));
	return v90_phase4_analog_station_receive_complete(
		       &state->station) &&
	       pcm_downstream_rx_is_valid(&state->receive) &&
	       state->receive.stage == PCM_DOWNSTREAM_RX_TRN2D &&
	       v90_pcm_mapping_init(
		       &expected, &state->station.training_parameters) ==
		       V90_PCM_MAPPING_OK &&
	       v90_pcm_mapping_equal(
		       &state->receive.stream.decoder.mapping, &expected);
}

static bool continuation_receive_owned_state_is_valid(
	const struct v90_phase4_analog_continuation *state)
{
	if (!state->receive_ready)
		return !v90_phase4_analog_station_receive_complete_owned(
			       &state->station) &&
		       memory_region_is_zero(
			       &state->receive, sizeof(state->receive));
	return v90_phase4_analog_station_receive_complete_owned(
		       &state->station) &&
	       pcm_downstream_rx_is_valid(&state->receive) &&
	       state->receive.stage == PCM_DOWNSTREAM_RX_TRN2D;
}

bool v90_phase4_analog_startup_owned_state_is_valid(
	const struct v90_phase4_analog_startup *startup)
{
	if (startup == NULL)
		return false;
	switch (startup->stage) {
	case V90_PHASE4_STARTUP_INITIAL: {
		const struct v90_phase4_analog_station *station =
			&startup->state.initial;

		return v90_phase4_analog_station_owned_state_is_valid(station) &&
		       (startup->failed ||
			(!v90_phase4_analog_station_receive_complete_owned(station) &&
			 !v90_phase4_analog_station_transmit_complete_owned(station))) &&
		       (startup->failed ||
			!v90_phase4_analog_station_has_failed_owned(station));
	}
	case V90_PHASE4_STARTUP_CONTINUING: {
		const struct v90_phase4_analog_continuation *state =
			&startup->state.continuing;

		return v90_phase4_analog_station_owned_state_is_valid(&state->station) &&
		       continuation_receive_owned_state_is_valid(state) &&
		       (startup->failed ||
			(!v90_phase4_analog_station_has_failed_owned(
				 &state->station) &&
			 (!state->receive_ready ||
			  !v90_phase4_analog_station_transmit_complete_owned(
				  &state->station)) &&
			 (state->receive_ready ||
			  (state->station.finish_cpt_requested &&
			   v90_phase4_cp_line_tx_record_is_complete_owned(
				   &state->station.transmit)))));
	}
	case V90_PHASE4_STARTUP_COMPLETE:
		return !startup->failed &&
		       v90_phase4_cp_line_tx_is_handoff_ready_owned(
			       &startup->state.complete.transmit) &&
		       pcm_downstream_rx_is_valid(
			       &startup->state.complete.receive) &&
		       startup->state.complete.receive.stage ==
			       PCM_DOWNSTREAM_RX_TRN2D &&
		       v34_constellation_size_is_valid(
			       startup->state.complete.rate_renegotiation_constellation);
	}
	return false;
}

bool v90_phase4_analog_startup_is_valid(
	const struct v90_phase4_analog_startup *startup)
{
	if (!v90_phase4_analog_startup_owned_state_is_valid(startup))
		return false;
	switch (startup->stage) {
	case V90_PHASE4_STARTUP_INITIAL:
		return v90_phase4_analog_station_is_valid(&startup->state.initial);
	case V90_PHASE4_STARTUP_CONTINUING:
		return v90_phase4_analog_station_is_valid(
			       &startup->state.continuing.station) &&
		       continuation_receive_is_valid(&startup->state.continuing);
	case V90_PHASE4_STARTUP_COMPLETE:
		return v90_phase4_cp_line_tx_is_handoff_ready(
			       &startup->state.complete.transmit) &&
		       handoff_mappings_match(&startup->state.complete);
	}
	return false;
}

bool v90_phase4_analog_startup_has_failed(
	const struct v90_phase4_analog_startup *startup)
{
	if (!v90_phase4_analog_startup_is_valid(startup))
		return false;
	return v90_phase4_analog_startup_has_failed_owned(startup);
}

bool v90_phase4_analog_startup_has_failed_owned(
	const struct v90_phase4_analog_startup *startup)
{
	return v90_phase4_analog_startup_owned_state_is_valid(startup) &&
	       startup->failed;
}

bool v90_phase4_analog_startup_is_complete(
	const struct v90_phase4_analog_startup *startup)
{
	if (!v90_phase4_analog_startup_is_valid(startup))
		return false;
	return v90_phase4_analog_startup_is_complete_owned(startup);
}

bool v90_phase4_analog_startup_is_complete_owned(
	const struct v90_phase4_analog_startup *startup)
{
	return v90_phase4_analog_startup_owned_state_is_valid(startup) &&
	       !startup->failed &&
	       startup->stage == V90_PHASE4_STARTUP_COMPLETE;
}

enum v90_phase4_startup_result v90_phase4_analog_startup_init(
	struct v90_phase4_analog_startup *startup,
	const struct v90_phase4_analog_station *station)
{
	struct v90_phase4_analog_startup initialized = {
		.stage = V90_PHASE4_STARTUP_INITIAL,
	};

	if (startup == NULL ||
	    !v90_phase4_analog_station_is_valid(station) ||
	    v90_phase4_analog_station_has_failed(station) ||
	    v90_phase4_analog_station_receive_complete(station) ||
	    v90_phase4_analog_station_transmit_complete(station))
		return V90_PHASE4_STARTUP_INVALID_ARGUMENT;
	initialized.state.initial = *station;
	if (!v90_phase4_analog_startup_is_valid(&initialized))
		return V90_PHASE4_STARTUP_NUMERIC_FAILURE;
	*startup = initialized;
	return V90_PHASE4_STARTUP_OK;
}

static enum v90_phase4_startup_result station_result(
	enum v90_phase4_analog_station_result result)
{
	switch (result) {
	case V90_PHASE4_ANALOG_STATION_OK:
		return V90_PHASE4_STARTUP_OK;
	case V90_PHASE4_ANALOG_STATION_FAILED:
		return V90_PHASE4_STARTUP_FAILED;
	case V90_PHASE4_ANALOG_STATION_INVALID_PARAMETERS:
		return V90_PHASE4_STARTUP_INVALID_PARAMETERS;
	case V90_PHASE4_ANALOG_STATION_OUT_OF_RANGE:
		return V90_PHASE4_STARTUP_OUT_OF_RANGE;
	case V90_PHASE4_ANALOG_STATION_CLIPPING_RISK:
		return V90_PHASE4_STARTUP_CLIPPING_RISK;
	case V90_PHASE4_ANALOG_STATION_SEQUENCE_MISMATCH:
		return V90_PHASE4_STARTUP_SEQUENCE_MISMATCH;
	case V90_PHASE4_ANALOG_STATION_COMPLETE:
	case V90_PHASE4_ANALOG_STATION_INVALID_STATE:
		return V90_PHASE4_STARTUP_INVALID_STATE;
	case V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT:
	case V90_PHASE4_ANALOG_STATION_NUMERIC_FAILURE:
		break;
	}
	return V90_PHASE4_STARTUP_NUMERIC_FAILURE;
}

static struct v90_phase4_analog_station *active_station(
	struct v90_phase4_analog_startup *startup)
{
	return startup->stage == V90_PHASE4_STARTUP_INITIAL ?
		       &startup->state.initial :
		       &startup->state.continuing.station;
}

static enum v90_phase4_startup_result line_result(
	enum v90_phase4_cp_line_tx_result result)
{
	switch (result) {
	case V90_PHASE4_CP_LINE_TX_OK:
		return V90_PHASE4_STARTUP_OK;
	case V90_PHASE4_CP_LINE_TX_INVALID_PARAMETERS:
		return V90_PHASE4_STARTUP_INVALID_PARAMETERS;
	case V90_PHASE4_CP_LINE_TX_OUT_OF_RANGE:
		return V90_PHASE4_STARTUP_OUT_OF_RANGE;
	case V90_PHASE4_CP_LINE_TX_CLIPPING_RISK:
		return V90_PHASE4_STARTUP_CLIPPING_RISK;
	case V90_PHASE4_CP_LINE_TX_COMPLETE:
	case V90_PHASE4_CP_LINE_TX_RECORD_IN_PROGRESS:
	case V90_PHASE4_CP_LINE_TX_INVALID_STATE:
		return V90_PHASE4_STARTUP_INVALID_STATE;
	case V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT:
	case V90_PHASE4_CP_LINE_TX_INTERNAL_ERROR:
		break;
	}
	return V90_PHASE4_STARTUP_NUMERIC_FAILURE;
}

static enum v90_phase4_startup_result transmit_character(
	struct v90_phase4_analog_startup *startup, uint8_t *octet)
{
	struct v90_phase4_analog_station *station =
		active_station(startup);
	int16_t sample;
	enum v90_phase4_startup_result result;

	if (startup->stage == V90_PHASE4_STARTUP_CONTINUING &&
	    v90_phase4_cp_line_tx_record_is_complete_owned(
		    &station->transmit) &&
	    station->finish_cpt_requested)
		result = line_result(v90_phase4_cp_line_tx_scr_sample_owned(
			&station->transmit,
			startup->state.continuing.receive_ready, &sample));
	else
		result = station_result(
			v90_phase4_analog_station_transmit_owned(station, &sample));

	if (result == V90_PHASE4_STARTUP_OK)
		*octet = g711_encode(station->receive.law, sample);
	return result;
}

static enum v90_phase4_startup_result receive_downstream(
	struct pcm_downstream_rx *receiver, uint8_t octet)
{
	struct pcm_downstream_event event;
	enum pcm_downstream_rx_result result =
		pcm_downstream_rx_receive(receiver, octet, &event);

	switch (result) {
	case PCM_DOWNSTREAM_RX_OK:
	case PCM_DOWNSTREAM_RX_BUFFERING:
	case PCM_DOWNSTREAM_RX_TRN2D_FRAME:
		return receiver->stage == PCM_DOWNSTREAM_RX_TRN2D ?
			       V90_PHASE4_STARTUP_OK :
			       V90_PHASE4_STARTUP_SEQUENCE_MISMATCH;
	case PCM_DOWNSTREAM_RX_INVALID_SYMBOL:
	case PCM_DOWNSTREAM_RX_MP:
	case PCM_DOWNSTREAM_RX_MP_PRIME:
	case PCM_DOWNSTREAM_RX_MP_REJECTED:
	case PCM_DOWNSTREAM_RX_ED:
	case PCM_DOWNSTREAM_RX_ED_REJECTED:
		return V90_PHASE4_STARTUP_SEQUENCE_MISMATCH;
	case PCM_DOWNSTREAM_RX_INVALID_PARAMETERS:
		return V90_PHASE4_STARTUP_INVALID_PARAMETERS;
	case PCM_DOWNSTREAM_RX_OUT_OF_RANGE:
		return V90_PHASE4_STARTUP_OUT_OF_RANGE;
	case PCM_DOWNSTREAM_RX_SUVD:
	case PCM_DOWNSTREAM_RX_SUVD_REJECTED:
	case PCM_DOWNSTREAM_RX_CPD:
	case PCM_DOWNSTREAM_RX_CPD_REJECTED:
	case PCM_DOWNSTREAM_RX_INVALID_ARGUMENT:
	case PCM_DOWNSTREAM_RX_INVALID_STATE:
	case PCM_DOWNSTREAM_RX_INTERNAL_ERROR:
		break;
	}
	return V90_PHASE4_STARTUP_NUMERIC_FAILURE;
}

static enum v90_phase4_startup_result receive_character(
	struct v90_phase4_analog_startup *startup, uint8_t octet)
{
	if (startup->stage == V90_PHASE4_STARTUP_CONTINUING &&
	    startup->state.continuing.receive_ready)
		return receive_downstream(
			&startup->state.continuing.receive, octet);
	return station_result(v90_phase4_analog_station_receive_owned(
		active_station(startup), octet));
}

static enum v90_phase4_startup_result prepare_receive(
	struct v90_phase4_analog_continuation *continuing)
{
	struct v90_phase4_analog_receive_handoff handoff;

	if (continuing->receive_ready)
		return V90_PHASE4_STARTUP_OK;
	if (v90_phase4_analog_station_receive_handoff(
		    &continuing->station, &handoff) !=
	    V90_PHASE4_ANALOG_STATION_OK)
		return V90_PHASE4_STARTUP_NUMERIC_FAILURE;
	continuing->receive = handoff.receiver;
	continuing->receive_ready = true;
	return V90_PHASE4_STARTUP_OK;
}

static enum v90_phase4_startup_result advance_startup(
	struct v90_phase4_analog_startup *startup)
{
	if (startup->stage == V90_PHASE4_STARTUP_INITIAL &&
	    (v90_phase4_analog_station_receive_complete_owned(
		     &startup->state.initial) ||
	     v90_phase4_analog_station_transmit_complete_owned(
		     &startup->state.initial))) {
		struct v90_phase4_analog_continuation continuing = {
			.station = startup->state.initial,
		};

		if (v90_phase4_analog_station_receive_complete_owned(
			    &continuing.station) &&
		    prepare_receive(&continuing) !=
			    V90_PHASE4_STARTUP_OK)
			return V90_PHASE4_STARTUP_NUMERIC_FAILURE;
		startup->state = (union v90_phase4_analog_startup_state){ 0 };
		startup->state.continuing = continuing;
		startup->stage = V90_PHASE4_STARTUP_CONTINUING;
	}
	if (startup->stage == V90_PHASE4_STARTUP_CONTINUING &&
	    !startup->state.continuing.receive_ready &&
	    !startup->state.continuing.station.finish_cpt_requested) {
		struct v90_phase4_analog_station initial =
			startup->state.continuing.station;

		startup->state = (union v90_phase4_analog_startup_state){ 0 };
		startup->state.initial = initial;
		startup->stage = V90_PHASE4_STARTUP_INITIAL;
		return V90_PHASE4_STARTUP_OK;
	}
	if (startup->stage == V90_PHASE4_STARTUP_CONTINUING &&
	    !startup->state.continuing.receive_ready &&
	    v90_phase4_analog_station_receive_complete_owned(
		    &startup->state.continuing.station) &&
	    prepare_receive(&startup->state.continuing) !=
		    V90_PHASE4_STARTUP_OK)
		return V90_PHASE4_STARTUP_NUMERIC_FAILURE;
	if (startup->stage == V90_PHASE4_STARTUP_CONTINUING &&
	    startup->state.continuing.receive_ready &&
	    v90_phase4_analog_station_transmit_complete_owned(
		    &startup->state.continuing.station)) {
		struct v90_phase4_analog_handoff complete = {
			.receive = startup->state.continuing.receive,
			.rate_renegotiation_constellation =
				startup->state.continuing.station
					.rate_renegotiation_constellation,
		};

		if (v90_phase4_analog_station_transmit_handoff(
			    &startup->state.continuing.station,
			    &complete.transmit) !=
		    V90_PHASE4_ANALOG_STATION_OK)
			return V90_PHASE4_STARTUP_NUMERIC_FAILURE;
		startup->state = (union v90_phase4_analog_startup_state){ 0 };
		startup->state.complete = complete;
		startup->stage = V90_PHASE4_STARTUP_COMPLETE;
	}
	return V90_PHASE4_STARTUP_OK;
}

static bool output_overlaps_startup(
	const struct v90_phase4_analog_startup *startup,
	const uint8_t *output)
{
	return memory_regions_overlap(
		startup, sizeof(*startup), output, sizeof(*output));
}

static enum v90_phase4_startup_result fail_startup(
	struct v90_phase4_analog_startup *startup,
	struct v90_phase4_analog_startup *next,
	enum v90_phase4_startup_result result,
	bool output_is_valid, uint8_t output,
	uint8_t *transmit_octet)
{
	next->failed = true;
	if (v90_phase4_analog_startup_owned_state_is_valid(next))
		*startup = *next;
	else
		startup->failed = true;
	if (output_is_valid)
		*transmit_octet = output;
	return result;
}

enum v90_phase4_startup_result v90_phase4_analog_startup_octet(
	struct v90_phase4_analog_startup *startup,
	uint8_t received_octet, uint8_t *transmit_octet)
{
	if (!v90_phase4_analog_startup_is_valid(startup))
		return V90_PHASE4_STARTUP_INVALID_ARGUMENT;
	return v90_phase4_analog_startup_octet_owned(startup, received_octet, transmit_octet);
}

enum v90_phase4_startup_result v90_phase4_analog_startup_octet_owned(
	struct v90_phase4_analog_startup *startup,
	uint8_t received_octet, uint8_t *transmit_octet)
{
	struct v90_phase4_analog_startup next;
	uint8_t generated = 0;
	enum v90_phase4_startup_result result;

	if (!v90_phase4_analog_startup_owned_state_is_valid(startup) ||
	    transmit_octet == NULL ||
	    output_overlaps_startup(startup, transmit_octet))
		return V90_PHASE4_STARTUP_INVALID_ARGUMENT;
	if (startup->failed)
		return V90_PHASE4_STARTUP_FAILED;
	if (v90_phase4_analog_startup_is_complete_owned(startup))
		return V90_PHASE4_STARTUP_DONE;
	next = *startup;
	result = transmit_character(&next, &generated);
	if (result != V90_PHASE4_STARTUP_OK)
		return fail_startup(
			startup, &next, result, false, generated,
			transmit_octet);
	result = receive_character(&next, received_octet);
	if (result != V90_PHASE4_STARTUP_OK)
		return fail_startup(
			startup, &next, result, true, generated,
			transmit_octet);
	result = advance_startup(&next);
	if (result != V90_PHASE4_STARTUP_OK ||
	    !v90_phase4_analog_startup_owned_state_is_valid(&next))
		return fail_startup(
			startup, &next,
			result == V90_PHASE4_STARTUP_OK ?
				V90_PHASE4_STARTUP_NUMERIC_FAILURE : result,
			true, generated, transmit_octet);
	*startup = next;
	*transmit_octet = generated;
	return V90_PHASE4_STARTUP_OK;
}

enum v90_phase4_startup_result v90_phase4_analog_startup_handoff(
	const struct v90_phase4_analog_startup *startup,
	struct v90_phase4_analog_handoff *handoff)
{
	if (!v90_phase4_analog_startup_is_valid(startup) || handoff == NULL)
		return V90_PHASE4_STARTUP_INVALID_ARGUMENT;
	if (startup->failed)
		return V90_PHASE4_STARTUP_FAILED;
	if (startup->stage != V90_PHASE4_STARTUP_COMPLETE)
		return V90_PHASE4_STARTUP_INVALID_STATE;
	*handoff = startup->state.complete;
	return V90_PHASE4_STARTUP_OK;
}
