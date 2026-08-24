// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_equalizer_receiver.h"

#include <math.h>
#include <stddef.h>

bool v34_data_equalizer_receiver_is_valid(
	const struct v34_data_equalizer_receiver *receiver)
{
	if (receiver == NULL ||
	    !qam_equalizer_is_valid(&receiver->equalizer) ||
	    !v34_data_channel_receiver_is_valid(
		    &receiver->data_receiver) ||
	    !isfinite(receiver->adaptation) || receiver->adaptation < 0.0 ||
	    receiver->adaptation > 1.0)
		return false;
	return !receiver->first_observation.pending ||
	       qam_equalizer_observation_is_pending(
		       &receiver->first_observation);
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
	if (receiver == NULL || !isfinite(adaptation) || adaptation < 0.0 ||
	    adaptation > 1.0 ||
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

static enum v34_data_receive_result
receive_interval(
	struct v34_data_equalizer_receiver *receiver,
	const struct qam_equalizer *equalizer,
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
	if (qam_equalizer_adapt_observation(
		    &next.equalizer, &next.first_observation,
		    &decision_targets[0], next.adaptation) !=
		    QAM_EQUALIZER_OK ||
	    qam_equalizer_adapt_observation(
		    &next.equalizer, second_observation, &decision_targets[1],
		    next.adaptation) != QAM_EQUALIZER_OK)
		return V34_DATA_RECEIVE_NUMERIC_FAILURE;
	next.first_observation =
		(struct qam_equalizer_observation){ 0 };
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
	bool equalized_ready;

	if (!v34_data_equalizer_receiver_is_valid(receiver) ||
	    !point_is_finite(received_point) || current == NULL ||
	    received == NULL)
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
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
	if (!receiver->first_observation.pending) {
		receiver->equalizer = next_equalizer;
		receiver->first_observation = observation;
		return V34_DATA_RECEIVE_OK;
	}
	return receive_interval(
		receiver, &next_equalizer, &observation, current, received);
}
