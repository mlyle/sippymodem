// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_equalizer_receiver.h"

#include "v34_constellation.h"
#include "v34_survivor.h"

#include <stddef.h>

enum {
	V34_DATA_RRN_MINIMUM_POINT_POWER_Q8 = 64,
};

_Static_assert(
	V34_DATA_RRN_CANDIDATE_PREFIX_SYMBOLS *
		(V34_PHASE3_S_RECEIVER_MAX_S_ERRORS + 1u) <=
		(V34_SURVIVOR_TRACEBACK_INTERVALS - 1u) *
			V34_2D_SYMBOLS_PER_4D_INTERVAL,
	"error-bounded online S must be gated before traceback can publish it");

static bool rate_renegotiation_prefix_is_candidate(
	const struct v34_phase3_s_prefix *prefix)
{
	return v34_phase3_s_prefix_is_valid(prefix) &&
	       prefix->alternating_turns == V34_PHASE3_S_PREFIX_TURNS &&
	       prefix->recurrence_hits >=
		       V34_DATA_RRN_CANDIDATE_RECURRENCE_HITS;
}

bool v34_data_equalizer_receiver_is_valid(
	const struct v34_data_equalizer_receiver *receiver)
{
	const struct v34_data_rate_renegotiation_monitor *monitor;

	if (receiver == NULL)
		return false;
	monitor = &receiver->rate_renegotiation;
	if (!qam_equalizer_is_valid(&receiver->equalizer) ||
	    !v34_data_channel_receiver_is_valid(
		    &receiver->data_receiver) ||
	    !qam_adaptation_is_valid(receiver->adaptation) ||
	    (!monitor->rate_renegotiation_enabled &&
	     (monitor->rate_renegotiation_candidate ||
	      monitor->rate_renegotiation_candidate_symbols != 0u)) ||
	    (monitor->rate_renegotiation_enabled &&
	     (!v34_phase3_s_receiver_is_valid(
		      &monitor->rate_renegotiation_marker) ||
	      !v34_phase3_s_prefix_is_valid(
		      &monitor->rate_renegotiation_prefix) ||
	      monitor->rate_renegotiation_candidate_symbols >
		      V34_DATA_RRN_CANDIDATE_SYMBOLS ||
	      monitor->rate_renegotiation_candidate !=
		      rate_renegotiation_prefix_is_candidate(
			      &monitor->rate_renegotiation_prefix) ||
	      (monitor->rate_renegotiation_candidate !=
		       (monitor->rate_renegotiation_candidate_symbols != 0u)))))
		return false;
	return receiver->first_observation.pending ?
		       qam_equalizer_observation_is_pending(
			       &receiver->first_observation) :
		       !receiver->first_observation_freezes_adaptation;
}

enum v34_data_receive_result
v34_data_equalizer_receiver_init_from_handoff(
	struct v34_data_equalizer_receiver *receiver,
	const struct v34_equalizer_handoff *handoff,
	const struct v34_power_normalizer *normalizer,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection,
	enum v34_station_role transmitting_role,
	double adaptation)
{
	struct v34_data_equalizer_receiver initialized = { 0 };

	if (!v34_equalizer_handoff_is_valid(handoff))
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	if (receiver == NULL || !qam_adaptation_is_valid(adaptation) ||
	    v34_data_channel_receiver_init(
		    &initialized.data_receiver, normalizer, geometry,
		    expanded_shaping, coefficients, selection,
		    transmitting_role) != V34_DATA_RECEIVE_OK)
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	initialized.equalizer = handoff->equalizer;
	initialized.adaptation = adaptation;
	if (!v34_data_equalizer_receiver_is_valid(&initialized))
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_DATA_RECEIVE_OK;
}

bool v34_data_equalizer_receiver_is_receiving_b1(
	const struct v34_data_equalizer_receiver *receiver)
{
	return v34_data_equalizer_receiver_is_valid(receiver) &&
	       v34_data_channel_receiver_is_receiving_b1(
		       &receiver->data_receiver);
}

enum v34_data_receive_result
v34_data_equalizer_receiver_enable_rate_renegotiation(
	struct v34_data_equalizer_receiver *receiver)
{
	struct v34_data_rate_renegotiation_monitor monitor = { 0 };

	if (!v34_data_equalizer_receiver_is_valid(receiver) ||
	    receiver->rate_renegotiation.rate_renegotiation_enabled)
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	if (v34_phase3_s_receiver_init_suffix(
		    &monitor.rate_renegotiation_marker,
		    V34_PHASE3_S_SYMBOLS) != V34_PHASE3_S_RECEIVER_OK ||
	    v34_phase3_s_prefix_init_recurrent(
		    &monitor.rate_renegotiation_prefix) !=
		    V34_PHASE3_S_PREFIX_OK)
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	monitor.rate_renegotiation_enabled = true;
	if (!v34_phase3_s_receiver_is_valid(
		    &monitor.rate_renegotiation_marker) ||
	    !v34_phase3_s_prefix_is_valid(
		    &monitor.rate_renegotiation_prefix))
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	receiver->rate_renegotiation = monitor;
	return V34_DATA_RECEIVE_OK;
}

bool v34_data_equalizer_receiver_detects_rate_renegotiation(
	const struct v34_data_equalizer_receiver *receiver)
{
	return v34_data_equalizer_receiver_is_valid(receiver) &&
	       receiver->rate_renegotiation.rate_renegotiation_enabled;
}

bool v34_data_equalizer_receiver_has_rate_renegotiation_candidate(
	const struct v34_data_equalizer_receiver *receiver)
{
	return v34_data_equalizer_receiver_detects_rate_renegotiation(
		       receiver) &&
	       receiver->rate_renegotiation.rate_renegotiation_candidate;
}

static bool reset_rate_renegotiation_prefix(
	struct v34_data_rate_renegotiation_monitor *monitor)
{
	monitor->rate_renegotiation_candidate = false;
	monitor->rate_renegotiation_candidate_symbols = 0u;
	return v34_phase3_s_prefix_init_recurrent(
		       &monitor->rate_renegotiation_prefix) ==
	       V34_PHASE3_S_PREFIX_OK;
}

static enum v34_data_receive_result observe_rate_renegotiation(
	struct v34_data_rate_renegotiation_monitor *monitor,
	const struct qam_equalizer_observation *observation)
{
	struct v34_phase3_s_receiver marker;
	struct v34_phase3_s_prefix prefix;
	enum v34_phase3_s_receiver_result result;
	enum v34_phase3_s_prefix_result prefix_result;
	uint8_t quarter_point;
	uint8_t rotation;

	if (!monitor->rate_renegotiation_enabled)
		return V34_DATA_RECEIVE_OK;
	if (v34_phase3_s_receiver_is_complete(
		    &monitor->rate_renegotiation_marker))
		return V34_DATA_RECEIVE_RATE_RENEGOTIATION;
	if (v34_training_constellation_slice(
		    V34_CONSTELLATION_4_POINT, &observation->output,
		    &quarter_point, &rotation) != V34_CONSTELLATION_OK)
		return V34_DATA_RECEIVE_NUMERIC_FAILURE;
	marker = monitor->rate_renegotiation_marker;
	result = v34_phase3_s_receiver_receive(
		&marker, quarter_point, rotation);
	if (result != V34_PHASE3_S_RECEIVER_OK &&
	    result != V34_PHASE3_S_RECEIVER_DETECTED)
		return V34_DATA_RECEIVE_NUMERIC_FAILURE;
	monitor->rate_renegotiation_marker = marker;
	if (result == V34_PHASE3_S_RECEIVER_DETECTED)
		return V34_DATA_RECEIVE_RATE_RENEGOTIATION;
	if (monitor->rate_renegotiation_candidate) {
		if (--monitor->rate_renegotiation_candidate_symbols == 0u &&
		    !reset_rate_renegotiation_prefix(monitor))
			return V34_DATA_RECEIVE_NUMERIC_FAILURE;
		return V34_DATA_RECEIVE_OK;
	}
	prefix = monitor->rate_renegotiation_prefix;
	prefix_result = v34_phase3_s_prefix_receive(
		&prefix, &observation->output,
		(double)V34_DATA_RRN_MINIMUM_POINT_POWER_Q8 / 256.0);
	if (prefix_result != V34_PHASE3_S_PREFIX_OK &&
	    prefix_result != V34_PHASE3_S_PREFIX_DETECTED &&
	    prefix_result != V34_PHASE3_S_PREFIX_REBASE_TIMING)
		return V34_DATA_RECEIVE_NUMERIC_FAILURE;
	monitor->rate_renegotiation_prefix = prefix;
	if (rate_renegotiation_prefix_is_candidate(&prefix)) {
		monitor->rate_renegotiation_candidate = true;
		monitor->rate_renegotiation_candidate_symbols =
			V34_DATA_RRN_CANDIDATE_SYMBOLS;
	}
	return V34_DATA_RECEIVE_OK;
}

static enum v34_data_receive_result
receive_interval(
	struct v34_data_equalizer_receiver *receiver,
	const struct qam_equalizer *equalizer,
	const struct v34_data_rate_renegotiation_monitor *monitor,
	bool freeze_adaptation,
	struct qam_equalizer_observation *second_observation,
	struct v34_survivor_decision *current,
	struct v34_received_mapping_frame *received)
{
	struct v34_data_equalizer_receiver next;
	struct qam_point normalized[
		V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct qam_point decision_targets[
		V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct v34_survivor_decision generated_current;
	struct v34_received_mapping_frame generated_received;
	enum v34_data_receive_result result;

	next = *receiver;
	next.equalizer = *equalizer;
	next.rate_renegotiation = *monitor;
	normalized[0] = next.first_observation.output;
	normalized[1] = second_observation->output;
	result = v34_data_channel_receiver_advance(
		&next.data_receiver, normalized, &generated_current,
		decision_targets, &generated_received);
	if (result == V34_DATA_RECEIVE_OK)
		result = V34_DATA_RECEIVE_INTERVAL_READY;
	if (result != V34_DATA_RECEIVE_INTERVAL_READY &&
	    !v34_data_receive_emits_mapping_frame(result))
		return result;
	if (!freeze_adaptation &&
	    (qam_equalizer_adapt_observation(
		     &next.equalizer, &next.first_observation,
		     &decision_targets[0], next.adaptation) !=
		     QAM_EQUALIZER_OK ||
	     qam_equalizer_adapt_observation(
		     &next.equalizer, second_observation,
		     &decision_targets[1], next.adaptation) !=
		     QAM_EQUALIZER_OK))
		return V34_DATA_RECEIVE_NUMERIC_FAILURE;
	next.first_observation =
		(struct qam_equalizer_observation){ 0 };
	next.first_observation_freezes_adaptation = false;
	if (!v34_data_equalizer_receiver_is_valid(&next))
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	*receiver = next;
	*current = generated_current;
	if (v34_data_receive_emits_mapping_frame(result))
		*received = generated_received;
	return result;
}

enum v34_data_receive_result
v34_data_equalizer_receive_point(
	struct v34_data_equalizer_receiver *receiver,
	const struct qam_point *received_point,
	struct v34_survivor_decision *current,
	struct v34_received_mapping_frame *received)
{
	struct qam_equalizer next_equalizer;
	struct qam_equalizer_observation observation = { 0 };
	struct v34_data_rate_renegotiation_monitor next_monitor;
	enum v34_data_receive_result result;
	bool candidate_before;
	bool equalized_ready;

	if (!v34_data_equalizer_receiver_is_valid(receiver) ||
	    !point_is_finite(received_point) || current == NULL ||
	    received == NULL)
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	if (receiver->rate_renegotiation.rate_renegotiation_enabled &&
	    v34_phase3_s_receiver_is_complete(
		    &receiver->rate_renegotiation
			     .rate_renegotiation_marker))
		return V34_DATA_RECEIVE_RATE_RENEGOTIATION;
	next_equalizer = receiver->equalizer;
	if (qam_equalizer_observe(
		    &next_equalizer, received_point, &observation,
		    &equalized_ready) != QAM_EQUALIZER_OK)
		return V34_DATA_RECEIVE_NUMERIC_FAILURE;
	if (!equalized_ready) {
		if (receiver->first_observation.pending)
			return V34_DATA_RECEIVE_INVALID_ARGUMENT;
		receiver->equalizer = next_equalizer;
		return V34_DATA_RECEIVE_OK;
	}
	next_monitor = receiver->rate_renegotiation;
	candidate_before =
		next_monitor.rate_renegotiation_enabled &&
		next_monitor.rate_renegotiation_candidate;
	result = v34_data_equalizer_receiver_is_receiving_b1(receiver) ?
		V34_DATA_RECEIVE_OK :
		observe_rate_renegotiation(&next_monitor, &observation);
	if (result != V34_DATA_RECEIVE_OK) {
		if (result == V34_DATA_RECEIVE_RATE_RENEGOTIATION) {
			receiver->equalizer = next_equalizer;
			receiver->rate_renegotiation = next_monitor;
		}
		return result;
	}
	if (!receiver->first_observation.pending) {
		receiver->equalizer = next_equalizer;
		receiver->rate_renegotiation = next_monitor;
		receiver->first_observation = observation;
		receiver->first_observation_freezes_adaptation =
			candidate_before ||
			(next_monitor.rate_renegotiation_enabled &&
			 next_monitor.rate_renegotiation_candidate);
		return V34_DATA_RECEIVE_OK;
	}
	result = receive_interval(
		receiver, &next_equalizer, &next_monitor,
		receiver->first_observation_freezes_adaptation ||
			candidate_before ||
			(next_monitor.rate_renegotiation_enabled &&
			 next_monitor.rate_renegotiation_candidate),
		&observation, current, received);
	return result;
}
