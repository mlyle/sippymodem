// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_digital_startup.h"

#include "memory_region.h"

static const struct v90_mp_parameters *mp_parameters(
	const struct v90_mp_record *record)
{
	return record->type == V90_MP_RECORD_TYPE0 ?
		       &record->parameters.type0 :
		       &record->parameters.type1.common;
}

static bool select_initial_mp(
	const struct v90_mp_record *configured,
	const struct v90_phase4_digital_station *station,
	struct v90_mp_record *selected)
{
	struct v90_mp_record limited;
	const struct v90_mp_parameters *parameters;
	uint8_t maximum;

	if (!v90_mp_record_is_initial(configured) ||
	    !v90_phase4_digital_station_is_valid(station) ||
	    !v90_phase4_digital_station_receive_complete(station) ||
	    selected == NULL)
		return false;
	parameters = mp_parameters(configured);
	/* The probe supplies an initial ceiling. Receive quality is measured
	 * under simultaneous TRN2d before the first MP is encoded, not under
	 * the much quieter Ri signal at this CPt boundary. */
	limited = *configured;
	maximum = parameters->maximum_upstream_rate_2400_bps_units;
	if (maximum >
		    station->receive.transmitting_profile
			    .projected_data_rate_2400_bps_units &&
	    !v90_mp_record_limit_maximum_upstream_rate(
		    &limited,
		    station->receive.transmitting_profile
			    .projected_data_rate_2400_bps_units))
		return false;
	/* CPt describes the peer transmitter; it cannot make the digital
	 * receiver's MP capability record invalid.  If no common rate is usable
	 * under this maximum, send the local MP and let the analogue modem
	 * perform the required final common-rate selection. */
	*selected = limited;
	return true;
}

static bool configured_mp_matches(
	const struct v90_phase4_digital_handoff *handoff,
	const struct v90_mp_record *record)
{
	return v90_mp_record_parameters_equal(
		&handoff->transmit.transmitter.control.mp, record);
}

bool v90_phase4_digital_startup_is_valid(
	const struct v90_phase4_digital_startup *startup)
{
	if (startup == NULL ||
	    !v90_mp_record_is_initial(&startup->initial_mp))
		return false;
	switch (startup->stage) {
	case V90_PHASE4_STARTUP_INITIAL: {
		const struct v90_phase4_digital_station *station =
			&startup->state.initial;

		return v90_phase4_digital_station_is_valid(station) &&
		       (startup->failed ||
			(!v90_phase4_digital_station_receive_complete(station) &&
			 !v90_phase4_digital_station_transmit_complete(station))) &&
		       (startup->failed ||
			!v90_phase4_digital_station_has_failed(station));
	}
	case V90_PHASE4_STARTUP_CONTINUING: {
		const struct v90_phase4_digital_continuation *state =
			&startup->state.continuing;

		return v90_phase4_digital_station_is_valid(&state->station) &&
		       v90_phase4_digital_station_receive_complete(
			       &state->station) &&
		       (startup->failed ||
			!v90_phase4_digital_station_transmit_complete(
				&state->station)) &&
		       v90_phase4_cp_line_rx_is_valid(&state->receive) &&
		       !v90_phase4_cp_line_rx_has_pending_record(
			       &state->receive) &&
		       (startup->failed ||
			(!v90_phase4_digital_station_has_failed(
				 &state->station) &&
			 !v90_phase4_cp_line_rx_has_failed(&state->receive)));
	}
	case V90_PHASE4_STARTUP_COMPLETE:
		return !startup->failed &&
		       (startup->state.complete.peer_upstream_rate_mask &
			(uint16_t)~V90_MP_RATE_CAPABILITY_MASK) == 0u &&
		       v90_phase4_digital_transmit_handoff_is_valid(
			       &startup->state.complete.transmit) &&
		       v90_phase4_cp_line_rx_is_valid(
			       &startup->state.complete.receive) &&
		       !v90_phase4_cp_line_rx_has_failed(
			       &startup->state.complete.receive) &&
		       !v90_phase4_cp_line_rx_has_pending_record(
			       &startup->state.complete.receive) &&
		       configured_mp_matches(
			       &startup->state.complete,
			       &startup->initial_mp);
	}
	return false;
}

bool v90_phase4_digital_startup_has_failed(
	const struct v90_phase4_digital_startup *startup)
{
	return v90_phase4_digital_startup_is_valid(startup) &&
	       startup->failed;
}

bool v90_phase4_digital_startup_is_complete(
	const struct v90_phase4_digital_startup *startup)
{
	return v90_phase4_digital_startup_is_valid(startup) &&
	       !startup->failed &&
	       startup->stage == V90_PHASE4_STARTUP_COMPLETE;
}

enum v90_phase4_startup_result v90_phase4_digital_startup_init(
	struct v90_phase4_digital_startup *startup,
	const struct v90_phase4_digital_station *station,
	const struct v90_mp_record *initial_mp)
{
	struct v90_phase4_digital_startup initialized = {
		.stage = V90_PHASE4_STARTUP_INITIAL,
	};

	if (startup == NULL ||
	    !v90_phase4_digital_station_is_valid(station) ||
	    v90_phase4_digital_station_has_failed(station) ||
	    v90_phase4_digital_station_receive_complete(station) ||
	    v90_phase4_digital_station_transmit_complete(station) ||
	    !v90_mp_record_is_initial(initial_mp))
		return V90_PHASE4_STARTUP_INVALID_ARGUMENT;
	initialized.state.initial = *station;
	initialized.initial_mp = *initial_mp;
	if (!v90_phase4_digital_startup_is_valid(&initialized))
		return V90_PHASE4_STARTUP_NUMERIC_FAILURE;
	*startup = initialized;
	return V90_PHASE4_STARTUP_OK;
}

static enum v90_phase4_startup_result station_result(
	enum v90_phase4_digital_station_result result)
{
	switch (result) {
	case V90_PHASE4_DIGITAL_STATION_OK:
		return V90_PHASE4_STARTUP_OK;
	case V90_PHASE4_DIGITAL_STATION_FAILED:
		return V90_PHASE4_STARTUP_FAILED;
	case V90_PHASE4_DIGITAL_STATION_COMPLETE:
	case V90_PHASE4_DIGITAL_STATION_INVALID_STATE:
		return V90_PHASE4_STARTUP_INVALID_STATE;
	case V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT:
	case V90_PHASE4_DIGITAL_STATION_NUMERIC_FAILURE:
		break;
	}
	return V90_PHASE4_STARTUP_NUMERIC_FAILURE;
}

static struct v90_phase4_digital_station *active_station(
	struct v90_phase4_digital_startup *startup)
{
	return startup->stage == V90_PHASE4_STARTUP_INITIAL ?
		       &startup->state.initial :
		       &startup->state.continuing.station;
}

static enum v90_phase4_startup_result transmit_character(
	struct v90_phase4_digital_startup *startup, uint8_t *octet)
{
	return station_result(v90_phase4_digital_station_transmit(
		active_station(startup), octet));
}

static bool mapping_matches(
	const struct v90_pcm_mapping *mapping,
	const struct v90_cp_parameters *parameters)
{
	struct v90_pcm_mapping expected;

	return v90_pcm_mapping_init_transmit(
		       &expected, parameters, mapping->law) ==
		       V90_PCM_MAPPING_OK &&
	       v90_pcm_mapping_equal(mapping, &expected);
}

static enum v90_phase4_startup_result receive_continued(
	struct v90_phase4_digital_continuation *state, int16_t sample)
{
	struct v90_cp_receive_event event = { 0 };
	enum v90_phase4_cp_line_rx_result result =
		v90_phase4_cp_line_rx_sample(&state->receive, sample, &event, NULL);

	switch (result) {
	case V90_PHASE4_CP_LINE_RX_OK:
	case V90_PHASE4_CP_LINE_RX_SYMBOL:
		return V90_PHASE4_STARTUP_OK;
	case V90_PHASE4_CP_LINE_RX_RECORD_READY: {
		bool matches =
			!v90_phase4_cp_line_rx_pending_record_is_valid(
				&state->receive) ||
			mapping_matches(
				&state->station.training_mapping,
				&event.parameters);

		/* A rejected CPt candidate cannot revoke the accepted mapping
		 * while Ri drains. Only a valid conflicting record can do that. */
		if (v90_phase4_cp_line_rx_continue(&state->receive) !=
		    V90_PHASE4_CP_LINE_RX_OK)
			return V90_PHASE4_STARTUP_NUMERIC_FAILURE;
		return matches ? V90_PHASE4_STARTUP_OK :
				 V90_PHASE4_STARTUP_SEQUENCE_MISMATCH;
	}
	case V90_PHASE4_CP_LINE_RX_FAILED:
		return V90_PHASE4_STARTUP_FAILED;
	case V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT:
	case V90_PHASE4_CP_LINE_RX_INVALID_STATE:
	case V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE:
		break;
	}
	return V90_PHASE4_STARTUP_NUMERIC_FAILURE;
}

static enum v90_phase4_startup_result receive_sample(
	struct v90_phase4_digital_startup *startup, int16_t sample)
{
	if (startup->stage == V90_PHASE4_STARTUP_INITIAL)
		return station_result(v90_phase4_digital_station_receive(
			&startup->state.initial, sample));
	return receive_continued(&startup->state.continuing, sample);
}

static enum v90_phase4_startup_result advance_startup(
	struct v90_phase4_digital_startup *startup)
{
	if (startup->stage == V90_PHASE4_STARTUP_INITIAL &&
	    v90_phase4_digital_station_receive_complete(
		    &startup->state.initial)) {
		struct v90_mp_record selected_mp;
		struct v90_phase4_digital_continuation continuing = {
			.station = startup->state.initial,
		};

		if (!select_initial_mp(
			    &startup->initial_mp, &continuing.station,
			    &selected_mp))
			return V90_PHASE4_STARTUP_INVALID_PARAMETERS;
		if (v90_phase4_digital_station_receive_handoff(
			    &continuing.station, &continuing.receive) !=
		    V90_PHASE4_DIGITAL_STATION_OK)
			return V90_PHASE4_STARTUP_NUMERIC_FAILURE;
		startup->initial_mp = selected_mp;
		startup->state = (union v90_phase4_digital_startup_state){ 0 };
		startup->state.continuing = continuing;
		startup->stage = V90_PHASE4_STARTUP_CONTINUING;
	}
	if (startup->stage == V90_PHASE4_STARTUP_CONTINUING &&
	    v90_phase4_digital_station_transmit_complete(
		    &startup->state.continuing.station)) {
		struct v90_phase4_digital_handoff complete = {
			.receive = startup->state.continuing.receive,
			.peer_upstream_rate_mask = startup->state.continuing
				.station.peer_upstream_rate_mask,
		};

		if (v90_phase4_digital_station_transmit_handoff(
			    &startup->state.continuing.station,
			    &startup->initial_mp, &complete.transmit) !=
		    V90_PHASE4_DIGITAL_STATION_OK)
			return V90_PHASE4_STARTUP_NUMERIC_FAILURE;
		startup->state = (union v90_phase4_digital_startup_state){ 0 };
		startup->state.complete = complete;
		startup->stage = V90_PHASE4_STARTUP_COMPLETE;
	}
	return V90_PHASE4_STARTUP_OK;
}

static bool output_overlaps_startup(
	const struct v90_phase4_digital_startup *startup,
	const uint8_t *output)
{
	return memory_regions_overlap(
		startup, sizeof(*startup), output, sizeof(*output));
}

static enum v90_phase4_startup_result fail_startup(
	struct v90_phase4_digital_startup *startup,
	struct v90_phase4_digital_startup *next,
	enum v90_phase4_startup_result result,
	bool output_is_valid, uint8_t output, uint8_t *transmit_octet)
{
	next->failed = true;
	if (v90_phase4_digital_startup_is_valid(next))
		*startup = *next;
	else
		startup->failed = true;
	if (output_is_valid)
		*transmit_octet = output;
	return result;
}

enum v90_phase4_startup_result v90_phase4_digital_startup_sample(
	struct v90_phase4_digital_startup *startup,
	int16_t received_sample, uint8_t *transmit_octet)
{
	struct v90_phase4_digital_startup next;
	uint8_t generated = 0;
	enum v90_phase4_startup_result result;

	if (!v90_phase4_digital_startup_is_valid(startup) ||
	    transmit_octet == NULL ||
	    output_overlaps_startup(startup, transmit_octet))
		return V90_PHASE4_STARTUP_INVALID_ARGUMENT;
	if (startup->failed)
		return V90_PHASE4_STARTUP_FAILED;
	if (v90_phase4_digital_startup_is_complete(startup))
		return V90_PHASE4_STARTUP_DONE;
	next = *startup;
	result = transmit_character(&next, &generated);
	if (result != V90_PHASE4_STARTUP_OK)
		return fail_startup(
			startup, &next, result, false, generated,
			transmit_octet);
	result = receive_sample(&next, received_sample);
	if (result != V90_PHASE4_STARTUP_OK)
		return fail_startup(
			startup, &next, result, true, generated,
			transmit_octet);
	result = advance_startup(&next);
	if (result != V90_PHASE4_STARTUP_OK ||
	    !v90_phase4_digital_startup_is_valid(&next))
		return fail_startup(
			startup, &next,
			result == V90_PHASE4_STARTUP_OK ?
				V90_PHASE4_STARTUP_NUMERIC_FAILURE : result,
			true, generated, transmit_octet);
	*startup = next;
	*transmit_octet = generated;
	return V90_PHASE4_STARTUP_OK;
}

enum v90_phase4_startup_result v90_phase4_digital_startup_handoff(
	const struct v90_phase4_digital_startup *startup,
	struct v90_phase4_digital_handoff *handoff)
{
	if (!v90_phase4_digital_startup_is_valid(startup) || handoff == NULL)
		return V90_PHASE4_STARTUP_INVALID_ARGUMENT;
	if (startup->failed)
		return V90_PHASE4_STARTUP_FAILED;
	if (startup->stage != V90_PHASE4_STARTUP_COMPLETE)
		return V90_PHASE4_STARTUP_INVALID_STATE;
	*handoff = startup->state.complete;
	return V90_PHASE4_STARTUP_OK;
}
