// SPDX-License-Identifier: BSD-3-Clause

#include "v34_mapping_frame_receiver.h"

#include <stddef.h>

enum sippymodem_v34_mapping_frame_receiver_result
sippymodem_v34_receive_mapping_frame(
	struct sippymodem_v34_frame_schedule *schedule,
	struct sippymodem_v34_scrambler *descrambler,
	struct sippymodem_v34_4d_decoder *mapping_decoder,
	const struct sippymodem_v34_decided_4d_interval
		decided[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME],
	struct sippymodem_v34_received_mapping_frame *received)
{
	struct sippymodem_v34_frame_schedule next_schedule;
	struct sippymodem_v34_scrambler next_descrambler;
	struct sippymodem_v34_4d_decoder next_decoder;
	struct sippymodem_v34_received_mapping_frame generated = { 0 };
	struct sippymodem_v34_recovered_mapping_frame recovered;
	enum sippymodem_v34_frame_schedule_result schedule_result;

	if (!sippymodem_v34_frame_schedule_is_valid(schedule) ||
	    !sippymodem_v34_scrambler_is_valid(descrambler) ||
	    !sippymodem_v34_4d_decoder_is_valid(mapping_decoder) ||
	    schedule->geometry.high_mapping_frame_bits !=
		    mapping_decoder->parameters.mapping_frame_bits ||
	    decided == NULL || received == NULL)
		return SIPPYMODEM_V34_MAPPING_FRAME_RECEIVER_INVALID_ARGUMENT;
	next_schedule = *schedule;
	next_descrambler = *descrambler;
	next_decoder = *mapping_decoder;
	schedule_result = sippymodem_v34_frame_schedule_next(
		&next_schedule, &generated.mapping_frame);
	if (schedule_result == SIPPYMODEM_V34_FRAME_SCHEDULE_FRAME_COMPLETE)
		return SIPPYMODEM_V34_MAPPING_FRAME_RECEIVER_FRAME_COMPLETE;
	if (schedule_result != SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    sippymodem_v34_decode_mapping_frame(
		    &next_decoder, &generated.mapping_frame, decided,
		    &recovered) != SIPPYMODEM_V34_MAPPING_FRAME_DECODER_OK)
		return SIPPYMODEM_V34_MAPPING_FRAME_RECEIVER_INVALID_ARGUMENT;
	generated.primary_bit_count = recovered.primary_bit_count;
	generated.auxiliary_bit_count = recovered.auxiliary_bit_count;
	generated.auxiliary_bit = recovered.auxiliary_bit;
	for (uint8_t index = 0; index < recovered.primary_bit_count; ++index) {
		if (sippymodem_v34_descramble_group(
			    &next_descrambler, recovered.primary_bits[index], 1,
			    &generated.primary_bits[index]) !=
		    SIPPYMODEM_V34_FRAMING_OK)
			return SIPPYMODEM_V34_MAPPING_FRAME_RECEIVER_INVALID_ARGUMENT;
	}
	*schedule = next_schedule;
	*descrambler = next_descrambler;
	*mapping_decoder = next_decoder;
	*received = generated;
	return SIPPYMODEM_V34_MAPPING_FRAME_RECEIVER_OK;
}

const char *sippymodem_v34_mapping_frame_receiver_result_name(
	enum sippymodem_v34_mapping_frame_receiver_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_MAPPING_FRAME_RECEIVER_OK:
		return "ok";
	case SIPPYMODEM_V34_MAPPING_FRAME_RECEIVER_FRAME_COMPLETE:
		return "frame complete";
	case SIPPYMODEM_V34_MAPPING_FRAME_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
