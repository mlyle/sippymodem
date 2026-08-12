// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_equalizer_receiver.h"

#include <math.h>
#include <stddef.h>

static bool point_is_finite(const struct qam_point *point)
{
	return point != NULL && isfinite(point->in_phase) &&
	       isfinite(point->quadrature);
}

bool v34_data_equalizer_receiver_is_valid(
	const struct v34_data_equalizer_receiver *receiver)
{
	bool observation_pending;

	if (receiver == NULL ||
	    !qam_equalizer_is_valid(&receiver->equalizer) ||
	    !v34_data_channel_receiver_is_valid(
		    &receiver->data_receiver) ||
	    !point_is_finite(&receiver->first_symbol) ||
	    !isfinite(receiver->adaptation) || receiver->adaptation < 0.0 ||
	    receiver->adaptation > 1.0 ||
	    receiver->training_outputs_remaining >
		    QAM_EQUALIZER_DELAY ||
	    (receiver->training_outputs_remaining != 0 &&
	     receiver->has_first_symbol))
		return false;
	if (receiver->has_first_symbol != receiver->first_observation.pending)
		return false;
	if (!receiver->has_first_symbol)
		return true;
	observation_pending =
		qam_equalizer_observation_is_pending(
			&receiver->first_observation);
	return observation_pending &&
	       receiver->first_symbol.in_phase ==
		       receiver->first_observation.output.in_phase &&
	       receiver->first_symbol.quadrature ==
		       receiver->first_observation.output.quadrature;
}

static enum v34_data_equalizer_receiver_result initialize_receiver(
	struct v34_data_equalizer_receiver *receiver,
	const struct qam_equalizer *equalizer,
	const struct v34_power_normalizer *normalizer,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection,
	enum v34_station_role transmitting_role,
	double adaptation, uint8_t training_outputs_remaining)
{
	struct v34_data_equalizer_receiver initialized = { 0 };

	if (receiver == NULL || !isfinite(adaptation) || adaptation < 0.0 ||
	    adaptation > 1.0 || !qam_equalizer_is_valid(equalizer) ||
	    training_outputs_remaining > QAM_EQUALIZER_DELAY ||
	    v34_data_channel_receiver_init(
		    &initialized.data_receiver, normalizer, geometry,
		    expanded_shaping, coefficients, selection,
		    transmitting_role) !=
		    V34_DATA_CHANNEL_RECEIVER_OK)
		return V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	initialized.equalizer = *equalizer;
	/* The new owner closes any immediate update not retained by a caller. */
	initialized.equalizer.can_adapt = false;
	initialized.adaptation = adaptation;
	initialized.training_outputs_remaining = training_outputs_remaining;
	if (!v34_data_equalizer_receiver_is_valid(&initialized))
		return V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_DATA_EQUALIZER_RECEIVER_OK;
}

enum v34_data_equalizer_receiver_result
v34_data_equalizer_receiver_init(
	struct v34_data_equalizer_receiver *receiver,
	const struct qam_equalizer *equalizer,
	const struct v34_power_normalizer *normalizer,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection,
	enum v34_station_role transmitting_role,
	double adaptation)
{
	uint8_t remaining;

	if (!qam_equalizer_is_valid(equalizer))
		return V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	remaining = equalizer->fill < QAM_EQUALIZER_DELAY ?
			    (uint8_t)equalizer->fill :
			    QAM_EQUALIZER_DELAY;
	return initialize_receiver(
		receiver, equalizer, normalizer, geometry, expanded_shaping,
		coefficients, selection, transmitting_role, adaptation,
		remaining);
}

enum v34_data_equalizer_receiver_result
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
	if (!v34_equalizer_handoff_is_valid(handoff))
		return V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	return initialize_receiver(
		receiver, &handoff->equalizer, normalizer, geometry,
		expanded_shaping, coefficients, selection, transmitting_role,
		adaptation, 0);
}

bool v34_data_equalizer_receiver_is_receiving_b1(
	const struct v34_data_equalizer_receiver *receiver)
{
	return v34_data_equalizer_receiver_is_valid(receiver) &&
	       v34_data_channel_receiver_is_receiving_b1(
		       &receiver->data_receiver);
}

static enum v34_data_equalizer_receiver_result channel_result(
	enum v34_data_channel_receiver_result result)
{
	switch (result) {
	case V34_DATA_CHANNEL_RECEIVER_OK:
		return V34_DATA_EQUALIZER_RECEIVER_INTERVAL_READY;
	case V34_DATA_CHANNEL_RECEIVER_MAPPING_FRAME_READY:
		return V34_DATA_EQUALIZER_RECEIVER_MAPPING_FRAME_READY;
	case V34_DATA_CHANNEL_RECEIVER_B1_IN_PROGRESS:
		return V34_DATA_EQUALIZER_RECEIVER_B1_IN_PROGRESS;
	case V34_DATA_CHANNEL_RECEIVER_B1_COMPLETE:
		return V34_DATA_EQUALIZER_RECEIVER_B1_COMPLETE;
	case V34_DATA_CHANNEL_RECEIVER_NUMERIC_FAILURE:
		return V34_DATA_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	case V34_DATA_CHANNEL_RECEIVER_OUT_OF_RANGE:
		return V34_DATA_EQUALIZER_RECEIVER_OUT_OF_RANGE;
	case V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT:
		break;
	}
	return V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
}

static bool emits_mapping_frame(
	enum v34_data_equalizer_receiver_result result)
{
	return result ==
		       V34_DATA_EQUALIZER_RECEIVER_MAPPING_FRAME_READY ||
	       result ==
		       V34_DATA_EQUALIZER_RECEIVER_B1_IN_PROGRESS ||
	       result == V34_DATA_EQUALIZER_RECEIVER_B1_COMPLETE;
}

static enum v34_data_equalizer_receiver_result
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
	enum v34_data_equalizer_receiver_result emitted;
	enum v34_data_channel_receiver_result channel_status;

	next = *receiver;
	next.equalizer = *equalizer;
	normalized[0] = next.first_symbol;
	normalized[1] = second_observation->output;
	channel_status = v34_data_channel_receiver_advance(
		&next.data_receiver, normalized, &generated_current,
		decision_targets, &generated_received);
	emitted = channel_result(channel_status);
	if (emitted == V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT ||
	    emitted == V34_DATA_EQUALIZER_RECEIVER_NUMERIC_FAILURE ||
	    emitted == V34_DATA_EQUALIZER_RECEIVER_OUT_OF_RANGE)
		return emitted;
	if (qam_equalizer_adapt_observation(
		    &next.equalizer, &next.first_observation,
		    &decision_targets[0], next.adaptation) !=
		    QAM_EQUALIZER_OK ||
	    qam_equalizer_adapt_observation(
		    &next.equalizer, second_observation, &decision_targets[1],
		    next.adaptation) != QAM_EQUALIZER_OK)
		return V34_DATA_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	next.first_observation =
		(struct qam_equalizer_observation){ 0 };
	next.first_symbol = (struct qam_point){ 0 };
	next.has_first_symbol = false;
	if (!v34_data_equalizer_receiver_is_valid(&next))
		return V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*current = generated_current;
	if (emits_mapping_frame(emitted))
		*received = generated_received;
	return emitted;
}

enum v34_data_equalizer_receiver_result
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
		return V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	next_equalizer = receiver->equalizer;
	if (qam_equalizer_observe(
		    &next_equalizer, received_point, &observation,
		    &equalized_ready) != QAM_EQUALIZER_OK)
		return V34_DATA_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	if (!equalized_ready) {
		if (receiver->has_first_symbol)
			return V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
		receiver->equalizer = next_equalizer;
		return V34_DATA_EQUALIZER_RECEIVER_OK;
	}
	if (receiver->training_outputs_remaining != 0) {
		receiver->equalizer = next_equalizer;
		receiver->training_outputs_remaining--;
		return V34_DATA_EQUALIZER_RECEIVER_OK;
	}
	if (!receiver->has_first_symbol) {
		receiver->equalizer = next_equalizer;
		receiver->first_symbol = observation.output;
		receiver->first_observation = observation;
		receiver->has_first_symbol = true;
		return V34_DATA_EQUALIZER_RECEIVER_OK;
	}
	return receive_interval(
		receiver, &next_equalizer, &observation, current, received);
}

const char *v34_data_equalizer_receiver_result_name(
	enum v34_data_equalizer_receiver_result result)
{
	switch (result) {
	case V34_DATA_EQUALIZER_RECEIVER_OK:
		return "ok";
	case V34_DATA_EQUALIZER_RECEIVER_INTERVAL_READY:
		return "interval ready";
	case V34_DATA_EQUALIZER_RECEIVER_MAPPING_FRAME_READY:
		return "mapping frame ready";
	case V34_DATA_EQUALIZER_RECEIVER_B1_IN_PROGRESS:
		return "B1 in progress";
	case V34_DATA_EQUALIZER_RECEIVER_B1_COMPLETE:
		return "B1 complete";
	case V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_DATA_EQUALIZER_RECEIVER_NUMERIC_FAILURE:
		return "numeric failure";
	case V34_DATA_EQUALIZER_RECEIVER_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
