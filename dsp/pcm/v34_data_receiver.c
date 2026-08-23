// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_receiver.h"

#include "v34_mapping_parameters.h"

#include <stddef.h>

static bool b1_progress_is_valid(
	const struct v34_data_receiver *receiver)
{
	const struct v34_frame_schedule *schedule =
		&receiver->schedule;
	uint16_t completed_mapping_frames;
	uint16_t expected_bits;

	if (!receiver->b1_active)
		return receiver->b1_received_bits ==
		       schedule->geometry.data_bits_per_frame;
	if (schedule->data_frame_in_superframe + 1u !=
		    schedule->geometry.data_frames_per_superframe ||
	    v34_frame_schedule_is_complete(schedule))
		return false;
	completed_mapping_frames = schedule->next_mapping_frame;
	expected_bits = (uint16_t)(
		completed_mapping_frames *
			(schedule->geometry.high_mapping_frame_bits - 1u) +
		completed_mapping_frames *
			schedule->geometry.high_mapping_frame_count /
			schedule->geometry.mapping_frames_per_data_frame);
	return receiver->b1_received_bits == expected_bits;
}

bool v34_data_receiver_is_valid(
	const struct v34_data_receiver *receiver)
{
	struct v34_mapping_parameters expected;

	return receiver != NULL &&
	       v34_frame_schedule_is_valid(&receiver->schedule) &&
	       v34_transmit_scrambler_is_valid(
		       &receiver->descrambler, receiver->transmitting_role) &&
	       v34_4d_decoder_is_valid(&receiver->mapping_decoder) &&
	       v34_mapping_parameters_init(
		       &expected, &receiver->schedule.geometry,
		       receiver->mapping_decoder.parameters.expanded_shaping) ==
		       V34_MAPPING_PARAMETERS_OK &&
	       v34_mapping_parameters_are_equal(
		       &receiver->mapping_decoder.parameters, &expected) &&
	       receiver->b1_error_bits <= receiver->b1_received_bits &&
	       b1_progress_is_valid(receiver);
}

enum v34_data_receiver_result v34_data_receiver_init(
	struct v34_data_receiver *receiver,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	enum v34_station_role transmitting_role)
{
	struct v34_mapping_parameters parameters;
	struct v34_data_receiver initialized = {
		.transmitting_role = transmitting_role,
		.b1_active = true,
	};

	if (receiver == NULL ||
	    v34_mapping_parameters_init(
		    &parameters, geometry, expanded_shaping) !=
		    V34_MAPPING_PARAMETERS_OK ||
	    v34_b1_frame_schedule_init(
		    &initialized.schedule, geometry) !=
		    V34_FRAME_SCHEDULE_OK ||
	    v34_transmit_scrambler_init(
		    &initialized.descrambler, transmitting_role) !=
		    V34_FRAMING_OK ||
	    v34_4d_decoder_init(
		    &initialized.mapping_decoder, &parameters) !=
		    V34_4D_DECODER_OK ||
	    !v34_data_receiver_is_valid(&initialized))
		return V34_DATA_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_DATA_RECEIVER_OK;
}

bool v34_data_receiver_is_receiving_b1(
	const struct v34_data_receiver *receiver)
{
	return v34_data_receiver_is_valid(receiver) &&
	       receiver->b1_active;
}

static bool prepare_next_schedule(
	struct v34_frame_schedule *schedule)
{
	return !v34_frame_schedule_is_complete(schedule) ||
	       v34_frame_schedule_advance_data_frame(schedule) ==
		       V34_FRAME_SCHEDULE_OK;
}

static uint16_t b1_error_count(
	const struct v34_received_mapping_frame *received)
{
	uint16_t errors = 0;

	for (uint8_t index = 0; index < received->primary_bit_count; ++index) {
		if (received->primary_bits[index] != 1u)
			errors++;
	}
	if (received->auxiliary_bit_count != 0u &&
	    received->auxiliary_bit != 1u)
		errors++;
	return errors;
}

enum v34_data_receiver_result
v34_data_receiver_receive_mapping_frame(
	struct v34_data_receiver *receiver,
	const struct v34_decided_4d_interval
		decided[V34_4D_INTERVALS_PER_MAPPING_FRAME],
	struct v34_received_mapping_frame *received)
{
	struct v34_data_receiver next;
	struct v34_received_mapping_frame decoded;
	enum v34_mapping_frame_receiver_result result;

	if (!v34_data_receiver_is_valid(receiver) ||
	    decided == NULL || received == NULL)
		return V34_DATA_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	if (!next.b1_active && !prepare_next_schedule(&next.schedule))
		return V34_DATA_RECEIVER_INVALID_ARGUMENT;
	result = v34_receive_mapping_frame(
		&next.schedule, &next.descrambler, &next.mapping_decoder,
		decided, &decoded);
	if (result != V34_MAPPING_FRAME_RECEIVER_OK)
		return V34_DATA_RECEIVER_INVALID_ARGUMENT;
	if (next.b1_active) {
		uint16_t frame_bits = (uint16_t)(decoded.primary_bit_count +
					       decoded.auxiliary_bit_count);
		bool complete = decoded.mapping_frame.completes_data_frame;

		next.b1_received_bits =
			(uint16_t)(next.b1_received_bits + frame_bits);
		next.b1_error_bits = (uint16_t)(
			next.b1_error_bits + b1_error_count(&decoded));
		next.b1_active = !complete;
		if (!v34_data_receiver_is_valid(&next))
			return V34_DATA_RECEIVER_INVALID_ARGUMENT;
		*receiver = next;
		*received = decoded;
		return complete ? V34_DATA_RECEIVER_B1_COMPLETE :
				  V34_DATA_RECEIVER_B1_IN_PROGRESS;
	}
	if (!v34_data_receiver_is_valid(&next))
		return V34_DATA_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*received = decoded;
	return V34_DATA_RECEIVER_OK;
}
