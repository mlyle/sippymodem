// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_channel_receiver.h"

#include <stddef.h>
#include <string.h>

bool v34_data_channel_receiver_is_valid(
	const struct v34_data_channel_receiver *receiver)
{
	const struct v34_channel_frame_decoder *channel_decoder;

	if (receiver == NULL ||
	    !v34_channel_frame_receiver_is_valid(
		    &receiver->channel_receiver) ||
	    !v34_data_receiver_is_valid(
		    &receiver->data_receiver))
		return false;
	channel_decoder = &receiver->channel_receiver.frame_decoder;
	return v34_frame_geometry_is_equal(
		       &channel_decoder->channel_schedule.geometry,
		       &receiver->data_receiver.schedule.geometry) &&
	       v34_mapping_parameters_are_equal(
		       &channel_decoder->channel_decoder.parameters,
		       &receiver->data_receiver.mapping_decoder.parameters);
}

enum v34_data_receive_result
v34_data_channel_receiver_init(
	struct v34_data_channel_receiver *receiver,
	const struct v34_power_normalizer *normalizer,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection,
	enum v34_station_role transmitting_role)
{
	struct v34_data_channel_receiver initialized = { 0 };

	if (receiver == NULL ||
	    v34_channel_frame_receiver_init(
		    &initialized.channel_receiver, normalizer, geometry,
		    expanded_shaping, coefficients, selection) !=
		    V34_DATA_RECEIVE_OK ||
	    v34_data_receiver_init(
		    &initialized.data_receiver, geometry, expanded_shaping,
		    transmitting_role) != V34_DATA_RECEIVE_OK ||
	    !v34_data_channel_receiver_is_valid(&initialized))
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_DATA_RECEIVE_OK;
}

bool v34_data_channel_receiver_is_receiving_b1(
	const struct v34_data_channel_receiver *receiver)
{
	return v34_data_channel_receiver_is_valid(receiver) &&
	       v34_data_receiver_is_receiving_b1(
		       &receiver->data_receiver);
}

enum v34_data_receive_result
v34_data_channel_receiver_advance(
	struct v34_data_channel_receiver *receiver,
	const struct qam_point
		normalized[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct v34_survivor_decision *current,
	struct qam_point
		decision_targets[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct v34_received_mapping_frame *received)
{
	struct v34_channel_frame_receiver saved_channel_receiver;
	struct v34_survivor_decision generated_current;
	struct qam_point generated_targets
		[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct v34_decided_4d_interval decided
		[V34_4D_INTERVALS_PER_MAPPING_FRAME];
	struct v34_received_mapping_frame generated_received;
	enum v34_data_receive_result channel_status;
	enum v34_data_receive_result data_status;
	bool frame_ready;

	if (!v34_data_channel_receiver_is_valid(receiver) ||
	    normalized == NULL || current == NULL || decision_targets == NULL ||
	    received == NULL)
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	/* Retain only the child that commits before frame decoding can fail. */
	saved_channel_receiver = receiver->channel_receiver;
	channel_status = v34_channel_frame_receiver_advance(
		&receiver->channel_receiver, normalized, &generated_current,
		generated_targets, decided, &frame_ready);
	if (channel_status != V34_DATA_RECEIVE_OK)
		return channel_status;
	if (!frame_ready) {
		*current = generated_current;
		memcpy(decision_targets, generated_targets,
		       sizeof(generated_targets));
		return V34_DATA_RECEIVE_OK;
	}
	data_status = v34_data_receiver_receive_mapping_frame(
		&receiver->data_receiver, decided, &generated_received);
	if (data_status == V34_DATA_RECEIVE_INVALID_ARGUMENT) {
		receiver->channel_receiver = saved_channel_receiver;
		return data_status;
	}
	*current = generated_current;
	memcpy(decision_targets, generated_targets, sizeof(generated_targets));
	if (v34_data_receive_emits_mapping_frame(data_status))
		*received = generated_received;
	return data_status;
}
