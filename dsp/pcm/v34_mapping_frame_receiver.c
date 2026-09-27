// SPDX-License-Identifier: BSD-3-Clause

#include "v34_mapping_frame_receiver.h"

#include <stddef.h>

enum v34_mapping_frame_receiver_result
v34_receive_mapping_frame(
	struct v34_frame_schedule *schedule,
	struct v34_scrambler *descrambler,
	struct v34_4d_decoder *mapping_decoder,
	const struct v34_decided_4d_interval
		decided[V34_4D_INTERVALS_PER_MAPPING_FRAME],
	struct v34_received_mapping_frame *received)
{
	struct v34_frame_schedule next_schedule;
	struct v34_scrambler next_descrambler;
	struct v34_4d_decoder next_decoder;
	struct v34_received_mapping_frame generated = { 0 };
	struct v34_recovered_mapping_frame recovered;
	enum v34_frame_schedule_result schedule_result;

	if (!v34_frame_schedule_is_valid(schedule) ||
	    !v34_scrambler_is_valid(descrambler) ||
	    !v34_4d_decoder_is_valid(mapping_decoder) ||
	    schedule->geometry.high_mapping_frame_bits !=
		    mapping_decoder->parameters.mapping_frame_bits ||
	    decided == NULL || received == NULL)
		return V34_MAPPING_FRAME_RECEIVER_INVALID_ARGUMENT;
	next_schedule = *schedule;
	next_descrambler = *descrambler;
	next_decoder = *mapping_decoder;
	schedule_result = v34_frame_schedule_next(
		&next_schedule, &generated.mapping_frame);
	if (schedule_result == V34_FRAME_SCHEDULE_FRAME_COMPLETE)
		return V34_MAPPING_FRAME_RECEIVER_FRAME_COMPLETE;
	if (schedule_result != V34_FRAME_SCHEDULE_OK ||
	    v34_decode_mapping_frame(
		    &next_decoder, &generated.mapping_frame, decided,
		    &recovered) != V34_MAPPING_FRAME_DECODER_OK)
		return V34_MAPPING_FRAME_RECEIVER_INVALID_ARGUMENT;
	generated.primary_bit_count = recovered.primary_bit_count;
	generated.auxiliary_bit_count = recovered.auxiliary_bit_count;
	generated.auxiliary_bit = recovered.auxiliary_bit;
	if (v34_descramble_unpacked_bits(
		    &next_descrambler, recovered.primary_bits,
		    recovered.primary_bit_count, generated.primary_bits) !=
	    V34_FRAMING_OK)
		return V34_MAPPING_FRAME_RECEIVER_INVALID_ARGUMENT;
	*schedule = next_schedule;
	*descrambler = next_descrambler;
	*mapping_decoder = next_decoder;
	*received = generated;
	return V34_MAPPING_FRAME_RECEIVER_OK;
}
