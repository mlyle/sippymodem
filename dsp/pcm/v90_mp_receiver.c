// SPDX-License-Identifier: BSD-3-Clause

#include "v90_mp_receiver.h"

#include "packed_bits.h"

#include <string.h>

static bool prefix_is_valid(const uint8_t *sequence)
{
	for (size_t bit = 0; bit < V90_MP_FRAME_SYNC_BITS; ++bit) {
		if (packed_msb_bit(sequence, bit) == 0)
			return false;
	}
	return packed_msb_bit(sequence, V90_MP_FRAME_SYNC_BITS) == 0;
}

static uint8_t trailing_one_count(
	const uint8_t *sequence, size_t bit_count)
{
	uint8_t count = 0;

	while (bit_count != 0 && count < V90_MP_FRAME_SYNC_BITS) {
		if (packed_msb_bit(sequence, bit_count - 1u) == 0)
			break;
		count++;
		bit_count--;
	}
	return count;
}

static bool stage_is_valid(enum v90_mp_receive_stage stage)
{
	return stage == V90_MP_SEARCH_SYNC ||
	       stage == V90_MP_RECEIVE_TYPE ||
	       stage == V90_MP_RECEIVE_SEQUENCE;
}

bool v90_mp_receiver_is_valid(
	const struct v90_mp_receiver *receiver)
{
	size_t expected;
	enum v90_mp_record_type type;

	if (receiver == NULL || !stage_is_valid(receiver->stage) ||
	    receiver->sync_one_count > V90_MP_FRAME_SYNC_BITS ||
	    receiver->bit_index > V90_MP_MAXIMUM_SEQUENCE_BITS ||
	    !packed_msb_tail_is_zero(
		    receiver->sequence, receiver->bit_index,
		    sizeof(receiver->sequence)) ||
	    v90_mp_sequence_size(
		    V90_MP_RECORD_TYPE0, receiver->training_frame_bits,
		    &expected) != V90_MP_OK)
		return false;
	if (receiver->stage == V90_MP_SEARCH_SYNC)
		return receiver->sequence_bit_count == 0 &&
		       receiver->bit_index == 0;
	if (!prefix_is_valid(receiver->sequence) ||
	    receiver->sync_one_count !=
		    trailing_one_count(receiver->sequence, receiver->bit_index))
		return false;
	if (receiver->stage == V90_MP_RECEIVE_TYPE)
		return receiver->sequence_bit_count == 0 &&
		       receiver->bit_index == V90_MP_FRAME_SYNC_BITS + 1u &&
		       receiver->sync_one_count == 0;
	type = packed_msb_bit(receiver->sequence, V90_MP_FRAME_SYNC_BITS + 1u) !=
		       0 ?
		       V90_MP_RECORD_TYPE1 :
		       V90_MP_RECORD_TYPE0;
	return v90_mp_sequence_size(
		       type, receiver->training_frame_bits, &expected) ==
		       V90_MP_OK &&
	       receiver->sequence_bit_count == expected &&
	       receiver->bit_index > V90_MP_FRAME_SYNC_BITS + 1u &&
	       receiver->bit_index < receiver->sequence_bit_count;
}

enum v90_mp_receiver_result v90_mp_receiver_init(
	struct v90_mp_receiver *receiver, uint8_t training_frame_bits)
{
	struct v90_mp_receiver initialized = {
		.training_frame_bits = training_frame_bits,
		.stage = V90_MP_SEARCH_SYNC,
	};
	size_t ignored;

	if (receiver == NULL)
		return V90_MP_RECEIVER_INVALID_ARGUMENT;
	if (v90_mp_sequence_size(
		    V90_MP_RECORD_TYPE0, training_frame_bits, &ignored) !=
		    V90_MP_OK)
		return V90_MP_RECEIVER_INVALID_PARAMETERS;
	if (!v90_mp_receiver_is_valid(&initialized))
		return V90_MP_RECEIVER_INTERNAL_ERROR;
	*receiver = initialized;
	return V90_MP_RECEIVER_OK;
}

static void clear_candidate(struct v90_mp_receiver *receiver)
{
	memset(receiver->sequence, 0, sizeof(receiver->sequence));
	receiver->sequence_bit_count = 0;
	receiver->bit_index = 0;
	receiver->sync_one_count = 0;
	receiver->stage = V90_MP_SEARCH_SYNC;
}

static void begin_candidate(struct v90_mp_receiver *receiver)
{
	clear_candidate(receiver);
	for (size_t bit = 0; bit < V90_MP_FRAME_SYNC_BITS; ++bit)
		packed_msb_set_bit(receiver->sequence, bit, true);
	receiver->bit_index = V90_MP_FRAME_SYNC_BITS + 1u;
	receiver->stage = V90_MP_RECEIVE_TYPE;
}

static bool advance_sync_run(
	struct v90_mp_receiver *receiver, uint8_t bit)
{
	if (bit != 0) {
		if (receiver->sync_one_count < V90_MP_FRAME_SYNC_BITS)
			receiver->sync_one_count++;
		return false;
	}
	if (receiver->sync_one_count == V90_MP_FRAME_SYNC_BITS) {
		receiver->sync_one_count = 0;
		return true;
	}
	receiver->sync_one_count = 0;
	return false;
}

static void finish_candidate(
	struct v90_mp_receiver *receiver, struct v90_mp_event *event)
{
	struct v90_mp_record record;
	enum v90_mp_result result = v90_mp_decode(
		receiver->sequence, receiver->sequence_bit_count,
		receiver->training_frame_bits, &record);

	event->frame_result = result;
	if (result == V90_MP_OK) {
		event->type = record.type == V90_MP_RECORD_TYPE0 ?
				      V90_MP_EVENT_TYPE0 :
				      V90_MP_EVENT_TYPE1;
		event->record = record;
	} else {
		event->type = V90_MP_EVENT_REJECTED;
	}
	clear_candidate(receiver);
}

static void receive_valid_bit(
	struct v90_mp_receiver *receiver, uint8_t bit,
	struct v90_mp_event *event)
{
	if (receiver->stage == V90_MP_SEARCH_SYNC) {
		if (advance_sync_run(receiver, bit))
			begin_candidate(receiver);
		return;
	}
	if (advance_sync_run(receiver, bit)) {
		/* A valid MP cannot contain 17 consecutive ones between its
		 * mandatory start bits, so this is an unambiguous newer prefix. */
		begin_candidate(receiver);
		return;
	}
	packed_msb_set_bit(
		receiver->sequence, receiver->bit_index, bit != 0);
	receiver->bit_index++;
	if (receiver->stage == V90_MP_RECEIVE_TYPE) {
		enum v90_mp_record_type type = bit != 0 ?
					       V90_MP_RECORD_TYPE1 :
					       V90_MP_RECORD_TYPE0;

		(void)v90_mp_sequence_size(
			type, receiver->training_frame_bits,
			&receiver->sequence_bit_count);
		receiver->stage = V90_MP_RECEIVE_SEQUENCE;
	}
	if (receiver->bit_index == receiver->sequence_bit_count)
		finish_candidate(receiver, event);
}

enum v90_mp_receiver_result v90_mp_receive_bit(
	struct v90_mp_receiver *receiver, uint8_t bit,
	struct v90_mp_event *event)
{
	struct v90_mp_receiver next;
	struct v90_mp_event emitted = { 0 };

	if (receiver == NULL || event == NULL || bit > 1u)
		return V90_MP_RECEIVER_INVALID_ARGUMENT;
	if (!v90_mp_receiver_is_valid(receiver))
		return V90_MP_RECEIVER_INVALID_STATE;
	next = *receiver;
	receive_valid_bit(&next, bit, &emitted);
	if (!v90_mp_receiver_is_valid(&next))
		return V90_MP_RECEIVER_INTERNAL_ERROR;
	*receiver = next;
	*event = emitted;
	return V90_MP_RECEIVER_OK;
}

const char *v90_mp_receiver_result_name(
	enum v90_mp_receiver_result result)
{
	switch (result) {
	case V90_MP_RECEIVER_OK:
		return "ok";
	case V90_MP_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_MP_RECEIVER_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_MP_RECEIVER_INVALID_STATE:
		return "invalid state";
	case V90_MP_RECEIVER_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
