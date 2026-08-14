// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_info.h"

#include "crc16_ccitt_false.h"
#include "packed_bits.h"

#include <string.h>

enum {
	V34_PHASE2_INFO_PREFIX = 0x0f72,
	V34_PHASE2_INFO_PREFIX_MASK = 0x0fff,
};

static bool payload_arguments_are_valid(
	const uint8_t *payload, size_t payload_bit_count)
{
	return payload_bit_count <=
		       V34_PHASE2_INFO_MAX_PAYLOAD_BITS &&
	       (payload != NULL || payload_bit_count == 0);
}

static void set_packed_bit(uint8_t *packed, size_t bit_index, uint8_t bit)
{
	packed[bit_index / 8u] |=
		(uint8_t)(bit << (7u - bit_index % 8u));
}

static uint16_t payload_crc(
	const uint8_t *payload, size_t payload_bit_count)
{
	uint16_t crc =
		CRC16_CCITT_FALSE_INITIAL_REMAINDER;

	for (size_t index = 0; index < payload_bit_count; ++index)
		crc = crc16_ccitt_false_step(
			crc, packed_msb_bit(payload, index) != 0);
	return crc;
}

size_t v34_phase2_info_frame_bit_count(
	size_t payload_bit_count)
{
	if (payload_bit_count >
	    V34_PHASE2_INFO_MAX_PAYLOAD_BITS)
		return 0;
	return V34_PHASE2_INFO_PREFIX_BITS + payload_bit_count +
	       V34_PHASE2_INFO_CRC_BITS +
	       V34_PHASE2_INFO_TRAILING_FILL_BITS;
}

enum v34_phase2_info_result v34_phase2_info_crc(
	const uint8_t *payload, size_t payload_bit_count, uint16_t *crc)
{
	if (!payload_arguments_are_valid(payload, payload_bit_count) ||
	    crc == NULL)
		return V34_PHASE2_INFO_INVALID_ARGUMENT;
	*crc = payload_crc(payload, payload_bit_count);
	return V34_PHASE2_INFO_OK;
}

bool v34_phase2_info_generator_is_valid(
	const struct v34_phase2_info_generator *generator)
{
	size_t frame_bit_count;

	if (generator == NULL ||
	    generator->payload_bit_count >
		    V34_PHASE2_INFO_MAX_PAYLOAD_BITS)
		return false;
	frame_bit_count = v34_phase2_info_frame_bit_count(
		generator->payload_bit_count);
	return generator->bit_index <= frame_bit_count &&
	       packed_msb_tail_is_zero(
		       generator->payload, generator->payload_bit_count,
		       sizeof(generator->payload)) &&
	       generator->crc == payload_crc(
				 generator->payload,
				 generator->payload_bit_count);
}

enum v34_phase2_info_result
v34_phase2_info_generator_init(
	struct v34_phase2_info_generator *generator,
	const uint8_t *payload, size_t payload_bit_count)
{
	struct v34_phase2_info_generator initialized = {
		.payload_bit_count = payload_bit_count,
	};

	if (generator == NULL ||
	    !payload_arguments_are_valid(payload, payload_bit_count))
		return V34_PHASE2_INFO_INVALID_ARGUMENT;
	for (size_t index = 0; index < payload_bit_count; ++index)
		set_packed_bit(initialized.payload, index,
			       packed_msb_bit(payload, index));
	initialized.crc = payload_crc(initialized.payload, payload_bit_count);
	if (!v34_phase2_info_generator_is_valid(&initialized))
		return V34_PHASE2_INFO_INVALID_ARGUMENT;
	*generator = initialized;
	return V34_PHASE2_INFO_OK;
}

enum v34_phase2_info_result
v34_phase2_info_generator_reset(
	struct v34_phase2_info_generator *generator)
{
	struct v34_phase2_info_generator reset;

	if (!v34_phase2_info_generator_is_valid(generator))
		return V34_PHASE2_INFO_INVALID_ARGUMENT;
	reset = *generator;
	reset.bit_index = 0;
	*generator = reset;
	return V34_PHASE2_INFO_OK;
}

static uint8_t info_frame_bit(
	const struct v34_phase2_info_generator *generator,
	size_t bit_index)
{
	if (bit_index < V34_PHASE2_INFO_PREFIX_BITS)
		return (uint8_t)(
			(V34_PHASE2_INFO_PREFIX >>
			 (V34_PHASE2_INFO_PREFIX_BITS - bit_index - 1u)) &
			1u);
	bit_index -= V34_PHASE2_INFO_PREFIX_BITS;
	if (bit_index < generator->payload_bit_count)
		return packed_msb_bit(generator->payload, bit_index);
	bit_index -= generator->payload_bit_count;
	if (bit_index < V34_PHASE2_INFO_CRC_BITS)
		return (uint8_t)(
			(generator->crc >>
			 (V34_PHASE2_INFO_CRC_BITS - bit_index - 1u)) &
			1u);
	return 1;
}

enum v34_phase2_info_result
v34_phase2_info_next_bit(
	struct v34_phase2_info_generator *generator,
	uint8_t *bit, bool *frame_complete)
{
	struct v34_phase2_info_generator next;
	size_t frame_bit_count;
	uint8_t generated;
	bool complete;

	if (!v34_phase2_info_generator_is_valid(generator) ||
	    bit == NULL || frame_complete == NULL)
		return V34_PHASE2_INFO_INVALID_ARGUMENT;
	frame_bit_count = v34_phase2_info_frame_bit_count(
		generator->payload_bit_count);
	if (generator->bit_index == frame_bit_count)
		return V34_PHASE2_INFO_INVALID_ARGUMENT;
	next = *generator;
	generated = info_frame_bit(&next, next.bit_index);
	complete = ++next.bit_index == frame_bit_count;
	*generator = next;
	*bit = generated;
	*frame_complete = complete;
	return V34_PHASE2_INFO_OK;
}

static bool receive_stage_is_valid(
	enum v34_phase2_info_receive_stage stage)
{
	return stage == V34_PHASE2_INFO_SEARCH_SYNC ||
	       stage == V34_PHASE2_INFO_RECEIVE_PAYLOAD ||
	       stage == V34_PHASE2_INFO_RECEIVE_CRC ||
	       stage == V34_PHASE2_INFO_RECEIVE_TAIL;
}

bool v34_phase2_info_receiver_is_valid(
	const struct v34_phase2_info_receiver *receiver)
{
	if (receiver == NULL ||
	    receiver->expected_payload_bit_count >
		    V34_PHASE2_INFO_MAX_PAYLOAD_BITS ||
	    !receive_stage_is_valid(receiver->stage) ||
	    receiver->payload_bit_index >
		    receiver->expected_payload_bit_count ||
	    receiver->crc_bit_count > V34_PHASE2_INFO_CRC_BITS ||
	    receiver->tail_bit_count >=
		    V34_PHASE2_INFO_TRAILING_FILL_BITS ||
	    receiver->sync_window > V34_PHASE2_INFO_PREFIX_MASK ||
	    !packed_msb_tail_is_zero(
		    receiver->payload, receiver->payload_bit_index,
		    sizeof(receiver->payload)))
		return false;

	switch (receiver->stage) {
	case V34_PHASE2_INFO_SEARCH_SYNC:
		return receiver->payload_bit_index == 0 &&
		       receiver->crc ==
			       CRC16_CCITT_FALSE_INITIAL_REMAINDER &&
		       receiver->received_crc == 0 &&
		       receiver->crc_bit_count == 0 &&
		       receiver->tail_bit_count == 0;
	case V34_PHASE2_INFO_RECEIVE_PAYLOAD:
		return receiver->expected_payload_bit_count != 0 &&
		       receiver->payload_bit_index <
			       receiver->expected_payload_bit_count &&
		       receiver->crc == payload_crc(
				       receiver->payload,
				       receiver->payload_bit_index) &&
		       receiver->received_crc == 0 &&
		       receiver->crc_bit_count == 0 &&
		       receiver->tail_bit_count == 0;
	case V34_PHASE2_INFO_RECEIVE_CRC:
		return receiver->payload_bit_index ==
			       receiver->expected_payload_bit_count &&
		       receiver->crc == payload_crc(
				       receiver->payload,
				       receiver->payload_bit_index) &&
		       receiver->crc_bit_count <
			       V34_PHASE2_INFO_CRC_BITS &&
		       (uint32_t)receiver->received_crc <
			       (UINT32_C(1) << receiver->crc_bit_count) &&
		       receiver->tail_bit_count == 0;
	case V34_PHASE2_INFO_RECEIVE_TAIL:
		return receiver->payload_bit_index ==
			       receiver->expected_payload_bit_count &&
		       receiver->crc == payload_crc(
				       receiver->payload,
				       receiver->payload_bit_index) &&
		       receiver->received_crc == receiver->crc &&
		       receiver->crc_bit_count ==
			       V34_PHASE2_INFO_CRC_BITS;
	}
	return false;
}

static void clear_receive_frame(
	struct v34_phase2_info_receiver *receiver)
{
	memset(receiver->payload, 0, sizeof(receiver->payload));
	receiver->payload_bit_index = 0;
	receiver->crc =
		CRC16_CCITT_FALSE_INITIAL_REMAINDER;
	receiver->received_crc = 0;
	receiver->crc_bit_count = 0;
	receiver->tail_bit_count = 0;
}

static void search_for_sync(
	struct v34_phase2_info_receiver *receiver)
{
	clear_receive_frame(receiver);
	receiver->stage = V34_PHASE2_INFO_SEARCH_SYNC;
}

static void begin_receive_frame(
	struct v34_phase2_info_receiver *receiver)
{
	clear_receive_frame(receiver);
	receiver->stage = receiver->expected_payload_bit_count == 0 ?
				  V34_PHASE2_INFO_RECEIVE_CRC :
				  V34_PHASE2_INFO_RECEIVE_PAYLOAD;
}

enum v34_phase2_info_result
v34_phase2_info_receiver_init(
	struct v34_phase2_info_receiver *receiver,
	size_t expected_payload_bit_count)
{
	struct v34_phase2_info_receiver initialized = {
		.expected_payload_bit_count = expected_payload_bit_count,
	};

	if (receiver == NULL ||
	    expected_payload_bit_count >
		    V34_PHASE2_INFO_MAX_PAYLOAD_BITS)
		return V34_PHASE2_INFO_INVALID_ARGUMENT;
	search_for_sync(&initialized);
	if (!v34_phase2_info_receiver_is_valid(&initialized))
		return V34_PHASE2_INFO_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE2_INFO_OK;
}

enum v34_phase2_info_result
v34_phase2_info_receiver_reset(
	struct v34_phase2_info_receiver *receiver)
{
	struct v34_phase2_info_receiver reset;

	if (!v34_phase2_info_receiver_is_valid(receiver))
		return V34_PHASE2_INFO_INVALID_ARGUMENT;
	reset = (struct v34_phase2_info_receiver){
		.expected_payload_bit_count =
			receiver->expected_payload_bit_count,
	};
	search_for_sync(&reset);
	*receiver = reset;
	return V34_PHASE2_INFO_OK;
}

static void reject_receive_frame(
	struct v34_phase2_info_receiver *receiver,
	struct v34_phase2_info_event *event)
{
	event->type = V34_PHASE2_INFO_FRAME_REJECTED;
	if (receiver->sync_window == V34_PHASE2_INFO_PREFIX)
		begin_receive_frame(receiver);
	else
		search_for_sync(receiver);
}

static void accept_receive_frame(
	struct v34_phase2_info_receiver *receiver,
	struct v34_phase2_info_event *event)
{
	event->type = V34_PHASE2_INFO_FRAME_RECEIVED;
	event->payload_bit_count = receiver->expected_payload_bit_count;
	memcpy(event->payload, receiver->payload, sizeof(event->payload));
	search_for_sync(receiver);
}

enum v34_phase2_info_result
v34_phase2_info_receive_bit(
	struct v34_phase2_info_receiver *receiver, uint8_t bit,
	struct v34_phase2_info_event *event)
{
	struct v34_phase2_info_receiver next;
	struct v34_phase2_info_event emitted = { 0 };

	if (!v34_phase2_info_receiver_is_valid(receiver) ||
	    bit > 1 || event == NULL)
		return V34_PHASE2_INFO_INVALID_ARGUMENT;
	next = *receiver;
	next.sync_window = (uint16_t)(
		((next.sync_window << 1) | bit) &
		V34_PHASE2_INFO_PREFIX_MASK);

	switch (next.stage) {
	case V34_PHASE2_INFO_SEARCH_SYNC:
		if (next.sync_window == V34_PHASE2_INFO_PREFIX)
			begin_receive_frame(&next);
		break;
	case V34_PHASE2_INFO_RECEIVE_PAYLOAD:
		set_packed_bit(next.payload, next.payload_bit_index, bit);
		next.payload_bit_index++;
		next.crc = crc16_ccitt_false_step(
			next.crc, bit != 0);
		if (next.payload_bit_index ==
		    next.expected_payload_bit_count)
			next.stage = V34_PHASE2_INFO_RECEIVE_CRC;
		break;
	case V34_PHASE2_INFO_RECEIVE_CRC:
		next.received_crc =
			(uint16_t)((next.received_crc << 1) | bit);
		next.crc_bit_count++;
		if (next.crc_bit_count ==
		    V34_PHASE2_INFO_CRC_BITS) {
			if (next.received_crc == next.crc)
				next.stage =
					V34_PHASE2_INFO_RECEIVE_TAIL;
			else
				reject_receive_frame(&next, &emitted);
		}
		break;
	case V34_PHASE2_INFO_RECEIVE_TAIL:
		if (++next.tail_bit_count ==
		    V34_PHASE2_INFO_TRAILING_FILL_BITS)
			accept_receive_frame(&next, &emitted);
		break;
	}
	if (!v34_phase2_info_receiver_is_valid(&next))
		return V34_PHASE2_INFO_INVALID_ARGUMENT;
	*receiver = next;
	*event = emitted;
	return V34_PHASE2_INFO_OK;
}

const char *v34_phase2_info_result_name(
	enum v34_phase2_info_result result)
{
	switch (result) {
	case V34_PHASE2_INFO_OK:
		return "ok";
	case V34_PHASE2_INFO_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
