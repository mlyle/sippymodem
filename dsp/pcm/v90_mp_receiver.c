// SPDX-License-Identifier: BSD-3-Clause

#include "v90_mp_receiver.h"

#include "memory_region.h"

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
	return v90_mp_receive_bits(receiver, &bit, 1u, event);
}

enum v90_mp_receiver_result v90_mp_receive_bits(
	struct v90_mp_receiver *receiver, const uint8_t *bits, size_t count,
	struct v90_mp_event *event)
{
	struct v90_mp_receiver next;
	struct v90_mp_event emitted = { 0 };
	size_t type0_bits, type1_bits;

	if (receiver == NULL || event == NULL || bits == NULL ||
	    count == 0u || count > V90_MP_TRAINING_FRAME_MAXIMUM_BITS ||
	    memory_regions_overlap(receiver, sizeof(*receiver), event, sizeof(*event)) ||
	    memory_regions_overlap(receiver, sizeof(*receiver), bits, count) ||
	    memory_regions_overlap(event, sizeof(*event), bits, count) ||
	    !unpacked_bits_are_binary(bits, count))
		return V90_MP_RECEIVER_INVALID_ARGUMENT;
	if (!frame_lengths(receiver, &type0_bits, &type1_bits) ||
	    !pcm_mp_receiver_is_valid(&receiver->wire, type0_bits, type1_bits))
		return V90_MP_RECEIVER_INVALID_STATE;
	next = *receiver;
	for (size_t index = 0; index < count; ++index) {
		if (!pcm_mp_receiver_receive_bit(&next.wire, bits[index],
			    type0_bits, type1_bits))
			continue;
		/* At most one record can end in a training frame. */
		finish_candidate(&next, &emitted);
		emitted.record_boundary = index + 1u == count;
	}
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
