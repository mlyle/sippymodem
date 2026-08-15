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

enum v34_data_channel_receiver_result
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
		    V34_CHANNEL_FRAME_RECEIVER_OK ||
	    v34_data_receiver_init(
		    &initialized.data_receiver, geometry, expanded_shaping,
		    transmitting_role) != V34_DATA_RECEIVER_OK ||
	    !v34_data_channel_receiver_is_valid(&initialized))
		return V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_DATA_CHANNEL_RECEIVER_OK;
}

bool v34_data_channel_receiver_is_receiving_b1(
	const struct v34_data_channel_receiver *receiver)
{
	return v34_data_channel_receiver_is_valid(receiver) &&
	       v34_data_receiver_is_receiving_b1(
		       &receiver->data_receiver);
}

static enum v34_data_channel_receiver_result channel_result(
	enum v34_channel_frame_receiver_result result)
{
	switch (result) {
	case V34_CHANNEL_FRAME_RECEIVER_NUMERIC_FAILURE:
		return V34_DATA_CHANNEL_RECEIVER_NUMERIC_FAILURE;
	case V34_CHANNEL_FRAME_RECEIVER_OUT_OF_RANGE:
		return V34_DATA_CHANNEL_RECEIVER_OUT_OF_RANGE;
	case V34_CHANNEL_FRAME_RECEIVER_OK:
	case V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT:
		break;
	}
	return V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT;
}

static enum v34_data_channel_receiver_result data_result(
	enum v34_data_receiver_result result)
{
	switch (result) {
	case V34_DATA_RECEIVER_OK:
		return V34_DATA_CHANNEL_RECEIVER_MAPPING_FRAME_READY;
	case V34_DATA_RECEIVER_B1_IN_PROGRESS:
		return V34_DATA_CHANNEL_RECEIVER_B1_IN_PROGRESS;
	case V34_DATA_RECEIVER_B1_COMPLETE:
		return V34_DATA_CHANNEL_RECEIVER_B1_COMPLETE;
	case V34_DATA_RECEIVER_INVALID_ARGUMENT:
		break;
	}
	return V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT;
}

enum v34_data_channel_receiver_result
v34_data_channel_receiver_advance(
	struct v34_data_channel_receiver *receiver,
	const struct qam_point
		normalized[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct v34_survivor_decision *current,
	struct qam_point
		decision_targets[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct v34_received_mapping_frame *received)
{
	struct v34_data_channel_receiver next;
	struct v34_survivor_decision generated_current;
	struct qam_point generated_targets
		[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct v34_decided_4d_interval decided
		[V34_4D_INTERVALS_PER_MAPPING_FRAME];
	struct v34_received_mapping_frame generated_received;
	enum v34_data_channel_receiver_result emitted_status;
	enum v34_channel_frame_receiver_result channel_status;
	enum v34_data_receiver_result data_status;
	bool frame_ready;

	if (!v34_data_channel_receiver_is_valid(receiver) ||
	    normalized == NULL || current == NULL || decision_targets == NULL ||
	    received == NULL)
		return V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	channel_status = v34_channel_frame_receiver_advance(
		&next.channel_receiver, normalized, &generated_current,
		generated_targets, decided, &frame_ready);
	if (channel_status != V34_CHANNEL_FRAME_RECEIVER_OK)
		return channel_result(channel_status);
	if (!frame_ready) {
		if (!v34_data_channel_receiver_is_valid(&next))
			return V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT;
		*receiver = next;
		*current = generated_current;
		memcpy(decision_targets, generated_targets,
		       sizeof(generated_targets));
		return V34_DATA_CHANNEL_RECEIVER_OK;
	}
	data_status = v34_data_receiver_receive_mapping_frame(
		&next.data_receiver, decided, &generated_received);
	emitted_status = data_result(data_status);
	if (emitted_status ==
		    V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT ||
	    !v34_data_channel_receiver_is_valid(&next))
		return V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*current = generated_current;
	memcpy(decision_targets, generated_targets, sizeof(generated_targets));
	*received = generated_received;
	return emitted_status;
}

const char *v34_data_channel_receiver_result_name(
	enum v34_data_channel_receiver_result result)
{
	switch (result) {
	case V34_DATA_CHANNEL_RECEIVER_OK:
		return "ok";
	case V34_DATA_CHANNEL_RECEIVER_MAPPING_FRAME_READY:
		return "mapping frame ready";
	case V34_DATA_CHANNEL_RECEIVER_B1_IN_PROGRESS:
		return "B1 in progress";
	case V34_DATA_CHANNEL_RECEIVER_B1_COMPLETE:
		return "B1 complete";
	case V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_DATA_CHANNEL_RECEIVER_NUMERIC_FAILURE:
		return "numeric failure";
	case V34_DATA_CHANNEL_RECEIVER_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
