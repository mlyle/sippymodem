// SPDX-License-Identifier: BSD-3-Clause

#include "v34_cleardown.h"

#include "memory_region.h"

#include <stddef.h>

enum {
	S_LAST_ROTATION = 3,
	DECODER_POWER_Q15 = 0x2000,
	MAX_TAIL_SAMPLES =
		((V34_TRAINING_LINE_PULSE_TAIL_SYMBOLS + 1u) * 8000u +
		 2399u) / 2400u + V34_TRAINING_LINE_FILTER_TAIL_SAMPLES,
};

bool v34_cleardown_is_valid(const struct v34_cleardown *cleardown)
{
	const struct v34_mp_parameters *parameters;

	if (cleardown == NULL)
		return false;
	parameters = &cleardown->parameters;
	return v34_training_line_is_valid(&cleardown->transmit_line) &&
	       v34_line_receiver_is_valid(&cleardown->receive_line) &&
	       v34_receive_timing_is_acquired(&cleardown->receive_line.timing) &&
	       !cleardown->receive_line.equalizer.decision_pending &&
	       !cleardown->receive_line.equalizer.retained.pending &&
	       v34_phase3_s_sequence_is_valid(&cleardown->transmit_s) &&
	       v34_phase3_s_receiver_is_valid(&cleardown->receive_s) &&
	       v34_phase4_mp_is_valid(&cleardown->transmit_mp) &&
	       v34_phase4_mp_receiver_is_valid(&cleardown->receive_mp) &&
	       v34_mp_parameters_are_valid(parameters) &&
	       parameters->maximum_call_to_answer_rate_2400_bps_units == 0u &&
	       parameters->maximum_answer_to_call_rate_2400_bps_units == 0u &&
	       cleardown->transmit_mp.constellation == V34_CONSTELLATION_4_POINT &&
	       cleardown->receive_mp.constellation == V34_CONSTELLATION_4_POINT &&
	       cleardown->transmit_mp.role ==
		       cleardown->transmit_line.profile.local_role &&
	       cleardown->receive_mp.transmitting_role ==
		       v34_station_peer_role(cleardown->transmit_mp.role) &&
	       cleardown->receive_line.timing.transmitting_profile.local_role ==
		       cleardown->receive_mp.transmitting_role &&
	       cleardown->tail_samples <= MAX_TAIL_SAMPLES &&
	       cleardown->stage >= V34_CLEARDOWN_S &&
	       cleardown->stage <= V34_CLEARDOWN_FINISHED &&
	       (cleardown->stage == V34_CLEARDOWN_S ||
		v34_phase3_s_sequence_is_complete(&cleardown->transmit_s)) &&
	       (!cleardown->peer_mp_received ||
		v34_mp_parameters_are_valid(&cleardown->peer_parameters)) &&
	       (!parameters->acknowledges_peer_mp ||
		cleardown->peer_mp_received) &&
	       (!cleardown->mp_prime_sent ||
		parameters->acknowledges_peer_mp) &&
	       (cleardown->stage == V34_CLEARDOWN_TAIL ||
		cleardown->tail_samples == 0u) &&
	       (cleardown->stage < V34_CLEARDOWN_TAIL ||
		(cleardown->peer_mp_received &&
		 cleardown->peer_parameters.acknowledges_peer_mp &&
		 cleardown->mp_prime_sent));
}

enum v34_cleardown_result v34_cleardown_init(
	struct v34_cleardown *cleardown,
	const struct v34_training_line *transmit_line,
	const struct v34_line_receiver *receive_line,
	const struct v34_mp_parameters *parameters)
{
	const struct memory_region regions[] = {
		{ cleardown, cleardown == NULL ? 0 : sizeof(*cleardown) },
		{ transmit_line, sizeof(*transmit_line) },
		{ receive_line, sizeof(*receive_line) },
		{ parameters, sizeof(*parameters) },
	};
	struct v34_cleardown initialized = { 0 };
	struct v34_phase4_mp *mp = &initialized.transmit_mp;

	if (cleardown == NULL ||
	    !v34_training_line_is_valid(transmit_line) ||
	    !v34_line_receiver_is_valid(receive_line) ||
	    receive_line->equalizer.decision_pending ||
	    receive_line->equalizer.retained.pending ||
	    !v34_mp_parameters_are_valid(parameters) ||
	    memory_region_set_overlaps(regions, sizeof regions / sizeof *regions))
		return V34_CLEARDOWN_INVALID_ARGUMENT;
	initialized.transmit_line = *transmit_line;
	initialized.receive_line = *receive_line;
	initialized.parameters = *parameters;
	initialized.parameters.maximum_call_to_answer_rate_2400_bps_units = 0u;
	initialized.parameters.maximum_answer_to_call_rate_2400_bps_units = 0u;
	initialized.parameters.acknowledges_peer_mp = false;
	/* Unlike startup, cleardown has no TRN. Its continuous MP stream
	 * self-synchronizes after the retained final S-bar rotation. */
	mp->role = transmit_line->profile.local_role;
	mp->constellation = V34_CONSTELLATION_4_POINT;
	mp->frame_bit_count = V34_MP_TYPE0_FRAME_BITS;
	mp->previous_clockwise_quarter_turns = S_LAST_ROTATION;
	if (v34_transmit_scrambler_init(&mp->scrambler, mp->role) !=
		    V34_FRAMING_OK ||
	    v34_mp_type0_encode(
		    &initialized.parameters, mp->frame, sizeof(mp->frame)) !=
		    V34_MP_OK ||
	    v34_phase3_s_receiver_init_fixed_suffix(
		    &initialized.receive_s,
		    V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS) !=
		    V34_PHASE3_S_RECEIVER_OK ||
	    v34_phase4_mp_receiver_init(
		    &initialized.receive_mp, v34_station_peer_role(mp->role),
		    V34_CONSTELLATION_4_POINT) != V34_PHASE4_MP_RECEIVER_OK ||
	    !v34_cleardown_is_valid(&initialized))
		return V34_CLEARDOWN_INVALID_ARGUMENT;
	*cleardown = initialized;
	return V34_CLEARDOWN_OK;
}

static uint16_t tail_samples(const struct v34_training_line *line)
{
	return (uint16_t)(((V34_TRAINING_LINE_PULSE_TAIL_SYMBOLS + 1u) *
		8000u * line->profile.symbol_rate_denominator +
		line->profile.symbol_rate_numerator - 1u) /
		line->profile.symbol_rate_numerator +
		V34_TRAINING_LINE_FILTER_TAIL_SAMPLES);
}

static bool next_point(
	struct v34_cleardown *cleardown, struct qam_point *point)
{
	uint8_t quarter_point;
	uint8_t rotation;
	bool record_complete;

	if (cleardown->stage == V34_CLEARDOWN_S) {
		enum v34_phase3_s_section section;

		if (!v34_phase3_s_sequence_is_complete(&cleardown->transmit_s))
			return v34_phase3_s_sequence_next(
				       &cleardown->transmit_s, point, &section) ==
			       V34_PHASE3_S_SEQUENCE_OK;
		if (cleardown->peer_mp_received) {
			cleardown->parameters.acknowledges_peer_mp = true;
			if (v34_mp_type0_encode(
				    &cleardown->parameters,
				    cleardown->transmit_mp.frame,
				    sizeof(cleardown->transmit_mp.frame)) !=
				    V34_MP_OK)
				return false;
		}
		cleardown->stage = V34_CLEARDOWN_MP;
	}
	if (v34_phase4_mp_record_is_complete(&cleardown->transmit_mp)) {
		cleardown->mp_prime_sent |=
			cleardown->parameters.acknowledges_peer_mp;
		if (cleardown->mp_prime_sent &&
		    cleardown->peer_parameters.acknowledges_peer_mp) {
			cleardown->stage = V34_CLEARDOWN_TAIL;
			cleardown->tail_samples =
				tail_samples(&cleardown->transmit_line);
			*point = (struct qam_point){ 0 };
			return true;
		}
		cleardown->parameters.acknowledges_peer_mp =
			cleardown->peer_mp_received;
		if (v34_phase4_mp_start_next_type0_record(
			    &cleardown->transmit_mp, &cleardown->parameters) !=
		    V34_PHASE4_MP_OK)
			return false;
	}
	return v34_phase4_mp_next(
		       &cleardown->transmit_mp, point, &quarter_point,
		       &rotation, &record_complete) == V34_PHASE4_MP_OK;
}

static bool transmit_sample(struct v34_cleardown *cleardown, int16_t *sample)
{
	struct qam_point point = { 0 };
	bool needs_point = v34_training_line_needs_point(&cleardown->transmit_line);
	bool accepted;

	if (cleardown->stage == V34_CLEARDOWN_FINISHED) {
		*sample = 0;
		return true;
	}
	if (cleardown->stage == V34_CLEARDOWN_TAIL &&
	    cleardown->tail_samples == 0u) {
		cleardown->stage = V34_CLEARDOWN_FINISHED;
		*sample = 0;
		return true;
	}
	if (cleardown->stage != V34_CLEARDOWN_TAIL && needs_point &&
	    !next_point(cleardown, &point))
		return false;
	if (v34_training_line_sample(
		    &cleardown->transmit_line, needs_point ? &point : NULL,
		    sample, &accepted) != V34_TRAINING_LINE_OK)
		return false;
	if (cleardown->stage == V34_CLEARDOWN_TAIL)
		cleardown->tail_samples--;
	return true;
}

static bool receive_sample(struct v34_cleardown *cleardown, int16_t sample)
{
	struct v34_adaptive_equalizer_observation observation;
	struct v34_fixed_training_decision hard;
	struct v34_receive_action action =
		v34_receive_action_only(V34_RECEIVE_DISCARD);
	enum v34_line_receiver_result result = v34_line_receiver_sample(
		&cleardown->receive_line, sample, &observation);

	if (result == V34_LINE_RECEIVER_OK)
		return true;
	if (result != V34_LINE_RECEIVER_OBSERVATION_READY ||
	    v34_training_constellation_fixed_slice(
		    V34_CONSTELLATION_4_POINT, observation.decision, &hard) !=
		    V34_CONSTELLATION_OK)
		return false;
	if (!v34_phase3_s_receiver_is_complete(&cleardown->receive_s)) {
		struct v34_phase3_s_adaptation adaptation;
		enum v34_phase3_s_receiver_result received =
			v34_phase3_s_receiver_receive_adaptive(
				&cleardown->receive_s, hard.quarter_point,
				hard.clockwise_quarter_turns,
				observation.decision, &adaptation);

		if (received != V34_PHASE3_S_RECEIVER_OK &&
		    received != V34_PHASE3_S_RECEIVER_DETECTED)
			return false;
	} else {
		struct v34_mp_receive_event event;
		const struct v34_mp_parameters *parameters;

		if (v34_phase4_mp_receive_decision(
			    &cleardown->receive_mp, hard.quarter_point,
			    hard.clockwise_quarter_turns, &event) !=
		    V34_PHASE4_MP_RECEIVER_OK)
			return false;
		parameters = event.type == V34_MP_RECEIVE_TYPE0 ?
			&event.parameters.type0 :
			event.type == V34_MP_RECEIVE_TYPE1 ?
			&event.parameters.type1.common : NULL;
		if (parameters != NULL) {
			if (!cleardown->peer_mp_received ||
			    !cleardown->peer_parameters.acknowledges_peer_mp)
				cleardown->peer_parameters = *parameters;
			cleardown->peer_mp_received = true;
		}
		action = v34_receive_action_commit(
			&observation, hard.target, DECODER_POWER_Q15,
			V34_ADAPTIVE_EQUALIZER_FULL);
	}
	return v34_line_receiver_resolve(
		       &cleardown->receive_line, &observation, &action,
		       DECODER_POWER_Q15) == V34_LINE_RECEIVER_OK;
}

enum v34_cleardown_result v34_cleardown_sample(
	struct v34_cleardown *cleardown, int16_t received,
	int16_t *transmitted)
{
	int16_t generated;

	if (!v34_cleardown_is_valid(cleardown) || transmitted == NULL ||
	    memory_regions_overlap(
		    cleardown, sizeof(*cleardown), transmitted,
		    sizeof(*transmitted)))
		return V34_CLEARDOWN_INVALID_ARGUMENT;
	if (cleardown->failed)
		return V34_CLEARDOWN_INVALID_STATE;
	if (!transmit_sample(cleardown, &generated) ||
	    (cleardown->stage < V34_CLEARDOWN_TAIL &&
	     !receive_sample(cleardown, received))) {
		cleardown->failed = true;
		return V34_CLEARDOWN_NUMERIC_FAILURE;
	}
	*transmitted = generated;
	return cleardown->stage == V34_CLEARDOWN_FINISHED ?
		V34_CLEARDOWN_COMPLETE : V34_CLEARDOWN_OK;
}
