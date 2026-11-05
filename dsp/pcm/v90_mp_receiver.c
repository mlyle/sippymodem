// SPDX-License-Identifier: BSD-3-Clause

#include "v90_mp_receiver.h"

_Static_assert((unsigned)V90_MP_MAXIMUM_SEQUENCE_BITS == PCM_MP_MAX_FRAME_BITS,
	"shared MP storage must cover the V.90 sequence");

static bool frame_lengths(const struct v90_mp_receiver *receiver,
	size_t *type0_bits, size_t *type1_bits)
{
	return receiver != NULL && v90_mp_sequence_size(V90_MP_RECORD_TYPE0,
		receiver->training_frame_bits, type0_bits) == V90_MP_OK &&
	       v90_mp_sequence_size(V90_MP_RECORD_TYPE1,
		       receiver->training_frame_bits, type1_bits) == V90_MP_OK;
}

bool v90_mp_receiver_is_valid(const struct v90_mp_receiver *receiver)
{
	size_t type0_bits, type1_bits;

	return frame_lengths(receiver, &type0_bits, &type1_bits) &&
	       pcm_mp_receiver_is_valid(&receiver->wire, type0_bits, type1_bits);
}

enum v90_mp_receiver_result v90_mp_receiver_init(
	struct v90_mp_receiver *receiver, uint8_t training_frame_bits)
{
	struct v90_mp_receiver initialized = {
		.training_frame_bits = training_frame_bits,
		.wire.stage = PCM_MP_SEARCH_SYNC,
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

static void finish_candidate(
	struct v90_mp_receiver *receiver, struct v90_mp_event *event)
{
	struct v90_mp_record record;
	enum v90_mp_result result = v90_mp_decode(
		receiver->wire.frame, receiver->wire.frame_bit_count,
		receiver->training_frame_bits, &record);
	uint8_t suffix_ones = receiver->wire.sync_one_count;

	event->frame_result = result;
	if (result == V90_MP_OK) {
		event->type = record.type == V90_MP_RECORD_TYPE0 ?
				      V90_MP_EVENT_TYPE0 :
				      V90_MP_EVENT_TYPE1;
		event->record = record;
	} else {
		event->type = V90_MP_EVENT_REJECTED;
	}
	/* Rejection can occur inside the next valid record's sync prefix. */
	pcm_mp_receiver_clear(&receiver->wire, result == V90_MP_OK ? 0 : suffix_ones);
}

enum v90_mp_receiver_result v90_mp_receive_bit(
	struct v90_mp_receiver *receiver, uint8_t bit,
	struct v90_mp_event *event)
{
	struct v90_mp_receiver next;
	struct v90_mp_event emitted = { 0 };
	size_t type0_bits, type1_bits;

	if (receiver == NULL || event == NULL || bit > 1u)
		return V90_MP_RECEIVER_INVALID_ARGUMENT;
	if (!frame_lengths(receiver, &type0_bits, &type1_bits) ||
	    !pcm_mp_receiver_is_valid(&receiver->wire, type0_bits, type1_bits))
		return V90_MP_RECEIVER_INVALID_STATE;
	next = *receiver;
	if (pcm_mp_receiver_receive_bit(&next.wire, bit, type0_bits, type1_bits))
		finish_candidate(&next, &emitted);
	if (!pcm_mp_receiver_is_valid(&next.wire, type0_bits, type1_bits))
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
