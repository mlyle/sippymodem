// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_receiver.h"

#include "sippymodem_v34_mapping_parameters.h"

#include <stddef.h>

static bool mapping_parameters_are_equal(
	const struct sippymodem_v34_mapping_parameters *left,
	const struct sippymodem_v34_mapping_parameters *right)
{
	return left->constellation_points == right->constellation_points &&
	       left->mapping_frame_bits == right->mapping_frame_bits &&
	       left->shell_mapper_bits == right->shell_mapper_bits &&
	       left->point_index_bits_per_2d_symbol ==
		       right->point_index_bits_per_2d_symbol &&
	       left->ring_count == right->ring_count &&
	       left->expanded_shaping == right->expanded_shaping;
}

static bool b1_progress_is_valid(
	const struct sippymodem_v34_data_receiver *receiver)
{
	const struct sippymodem_v34_frame_schedule *schedule =
		&receiver->schedule;
	uint16_t completed_mapping_frames;
	uint16_t expected_bits;

	if (!receiver->b1_active)
		return receiver->b1_received_bits ==
		       schedule->geometry.data_bits_per_frame;
	if (schedule->data_frame_in_superframe + 1u !=
		    schedule->geometry.data_frames_per_superframe ||
	    sippymodem_v34_frame_schedule_is_complete(schedule))
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

bool sippymodem_v34_data_receiver_is_valid(
	const struct sippymodem_v34_data_receiver *receiver)
{
	struct sippymodem_v34_mapping_parameters expected;

	return receiver != NULL &&
	       sippymodem_v34_frame_schedule_is_valid(&receiver->schedule) &&
	       sippymodem_v34_transmit_scrambler_is_valid(
		       &receiver->descrambler, receiver->transmitting_role) &&
	       sippymodem_v34_4d_decoder_is_valid(&receiver->mapping_decoder) &&
	       sippymodem_v34_mapping_parameters_init(
		       &expected, &receiver->schedule.geometry,
		       receiver->mapping_decoder.parameters.expanded_shaping) ==
		       SIPPYMODEM_V34_MAPPING_PARAMETERS_OK &&
	       mapping_parameters_are_equal(
		       &receiver->mapping_decoder.parameters, &expected) &&
	       receiver->b1_error_bits <= receiver->b1_received_bits &&
	       b1_progress_is_valid(receiver);
}

enum sippymodem_v34_data_receiver_result sippymodem_v34_data_receiver_init(
	struct sippymodem_v34_data_receiver *receiver,
	const struct sippymodem_v34_frame_geometry *geometry,
	bool expanded_shaping,
	enum sippymodem_v34_station_role transmitting_role)
{
	struct sippymodem_v34_mapping_parameters parameters;
	struct sippymodem_v34_data_receiver initialized = {
		.transmitting_role = transmitting_role,
		.b1_active = true,
	};

	if (receiver == NULL ||
	    sippymodem_v34_mapping_parameters_init(
		    &parameters, geometry, expanded_shaping) !=
		    SIPPYMODEM_V34_MAPPING_PARAMETERS_OK ||
	    sippymodem_v34_b1_frame_schedule_init(
		    &initialized.schedule, geometry) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    sippymodem_v34_transmit_scrambler_init(
		    &initialized.descrambler, transmitting_role) !=
		    SIPPYMODEM_V34_FRAMING_OK ||
	    sippymodem_v34_4d_decoder_init(
		    &initialized.mapping_decoder, &parameters) !=
		    SIPPYMODEM_V34_4D_DECODER_OK ||
	    !sippymodem_v34_data_receiver_is_valid(&initialized))
		return SIPPYMODEM_V34_DATA_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return SIPPYMODEM_V34_DATA_RECEIVER_OK;
}

bool sippymodem_v34_data_receiver_is_receiving_b1(
	const struct sippymodem_v34_data_receiver *receiver)
{
	return sippymodem_v34_data_receiver_is_valid(receiver) &&
	       receiver->b1_active;
}

static bool prepare_next_schedule(
	struct sippymodem_v34_frame_schedule *schedule)
{
	return !sippymodem_v34_frame_schedule_is_complete(schedule) ||
	       sippymodem_v34_frame_schedule_advance_data_frame(schedule) ==
		       SIPPYMODEM_V34_FRAME_SCHEDULE_OK;
}

static uint16_t b1_error_count(
	const struct sippymodem_v34_received_mapping_frame *received)
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

enum sippymodem_v34_data_receiver_result
sippymodem_v34_data_receiver_receive_mapping_frame(
	struct sippymodem_v34_data_receiver *receiver,
	const struct sippymodem_v34_decided_4d_interval
		decided[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME],
	struct sippymodem_v34_received_mapping_frame *received)
{
	struct sippymodem_v34_data_receiver next;
	struct sippymodem_v34_received_mapping_frame decoded;
	enum sippymodem_v34_mapping_frame_receiver_result result;

	if (!sippymodem_v34_data_receiver_is_valid(receiver) ||
	    decided == NULL || received == NULL)
		return SIPPYMODEM_V34_DATA_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	if (!next.b1_active && !prepare_next_schedule(&next.schedule))
		return SIPPYMODEM_V34_DATA_RECEIVER_INVALID_ARGUMENT;
	result = sippymodem_v34_receive_mapping_frame(
		&next.schedule, &next.descrambler, &next.mapping_decoder,
		decided, &decoded);
	if (result != SIPPYMODEM_V34_MAPPING_FRAME_RECEIVER_OK)
		return SIPPYMODEM_V34_DATA_RECEIVER_INVALID_ARGUMENT;
	if (next.b1_active) {
		uint16_t frame_bits = (uint16_t)(decoded.primary_bit_count +
					       decoded.auxiliary_bit_count);
		bool complete = decoded.mapping_frame.completes_data_frame;

		next.b1_received_bits =
			(uint16_t)(next.b1_received_bits + frame_bits);
		next.b1_error_bits = (uint16_t)(
			next.b1_error_bits + b1_error_count(&decoded));
		next.b1_active = !complete;
		if (!sippymodem_v34_data_receiver_is_valid(&next))
			return SIPPYMODEM_V34_DATA_RECEIVER_INVALID_ARGUMENT;
		*receiver = next;
		*received = decoded;
		return complete ? SIPPYMODEM_V34_DATA_RECEIVER_B1_COMPLETE :
				  SIPPYMODEM_V34_DATA_RECEIVER_B1_IN_PROGRESS;
	}
	if (!sippymodem_v34_data_receiver_is_valid(&next))
		return SIPPYMODEM_V34_DATA_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*received = decoded;
	return SIPPYMODEM_V34_DATA_RECEIVER_OK;
}

const char *sippymodem_v34_data_receiver_result_name(
	enum sippymodem_v34_data_receiver_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_DATA_RECEIVER_OK:
		return "ok";
	case SIPPYMODEM_V34_DATA_RECEIVER_B1_IN_PROGRESS:
		return "B1 in progress";
	case SIPPYMODEM_V34_DATA_RECEIVER_B1_COMPLETE:
		return "B1 complete";
	case SIPPYMODEM_V34_DATA_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
