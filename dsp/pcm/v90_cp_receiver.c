// SPDX-License-Identifier: BSD-3-Clause

#include "v90_cp_receiver.h"

#include "pcm_control_frame.h"
#include "packed_bits.h"

#include <string.h>

enum {
	CP_SYNC_BITS = 17,
	CP_INFORMATION_FIRST_BIT = 18,
	CP_MAP_FIRST_BIT = 103,
	CP_MAP_BITS = 4,
	CP_CODEC_DIFFERENCE_BIT = 128,
	CP_HEADER_LAST_BIT = CP_CODEC_DIFFERENCE_BIT,
};

static size_t map_first_bit(size_t interval)
{
	return CP_MAP_FIRST_BIT + interval * CP_MAP_BITS +
	       (interval >= 4u ? 1u : 0u);
}

static bool infer_frame_bit_count(
	const uint8_t *frame, size_t *frame_bit_count)
{
	size_t constellation_count = 1;
	size_t set_count;

	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		uint8_t index = (uint8_t)packed_msb_read_lsb_field(
			frame, map_first_bit(interval), CP_MAP_BITS);

		if (index >= V90_CP_MAX_CONSTELLATIONS)
			return false;
		if ((size_t)index + 1u > constellation_count)
			constellation_count = (size_t)index + 1u;
	}
	set_count = constellation_count *
		    (packed_msb_bit(frame, CP_CODEC_DIFFERENCE_BIT) != 0 ? 2u : 1u);
	*frame_bit_count = V90_CP_HEADER_BITS +
			   set_count * V90_CP_CONSTELLATION_BITS +
			   V90_CP_TRAILER_BITS;
	return true;
}

static bool stage_is_valid(enum v90_cp_receive_stage stage)
{
	return stage == V90_CP_RECEIVER_SEARCH_SYNC ||
	       stage == V90_CP_RECEIVER_READ_HEADER ||
	       stage == V90_CP_RECEIVER_READ_FRAME;
}

static bool bit_must_be_zero(
	size_t bit_index, size_t frame_bit_count)
{
	return bit_index % 17u == 0 ||
	       (frame_bit_count != 0 &&
		bit_index >= frame_bit_count - 3u);
}

bool v90_cp_receiver_is_valid(
	const struct v90_cp_receiver *receiver)
{
	size_t inferred_bits;

	if (receiver == NULL || !stage_is_valid(receiver->stage) ||
	    receiver->sync_one_count > CP_SYNC_BITS ||
	    receiver->bit_index > V90_CP_MAX_FRAME_BITS)
		return false;
	switch (receiver->stage) {
	case V90_CP_RECEIVER_SEARCH_SYNC:
		return receiver->frame_bit_count == 0 &&
		       receiver->bit_index == 0 &&
		       packed_msb_tail_is_zero(
			       receiver->frame, 0,
			       sizeof(receiver->frame));
	case V90_CP_RECEIVER_READ_HEADER:
		/* begin_frame() clears the bounded record, and receive_bit()
		 * rejects every nonzero start bit as it arrives. Reserved
		 * information bits are uninterpreted but still CRC-protected.
		 * Do not rescan the growing prefix on every 8 kHz sample. */
		return receiver->frame_bit_count == 0 &&
		       receiver->bit_index >= CP_INFORMATION_FIRST_BIT &&
		       receiver->bit_index <= CP_HEADER_LAST_BIT &&
		       receiver->sync_one_count == 0 &&
		       pcm_control_frame_prefix_is_valid(receiver->frame);
	case V90_CP_RECEIVER_READ_FRAME:
		return receiver->frame_bit_count >= V90_CP_MIN_FRAME_BITS &&
		       receiver->frame_bit_count <= V90_CP_MAX_FRAME_BITS &&
		       receiver->bit_index > CP_HEADER_LAST_BIT &&
		       receiver->bit_index < receiver->frame_bit_count &&
		       receiver->sync_one_count == 0 &&
		       pcm_control_frame_prefix_is_valid(receiver->frame) &&
		       infer_frame_bit_count(receiver->frame, &inferred_bits) &&
		       inferred_bits == receiver->frame_bit_count;
	}
	return false;
}

enum v90_cp_result v90_cp_receiver_init(
	struct v90_cp_receiver *receiver)
{
	struct v90_cp_receiver initialized = {
		.stage = V90_CP_RECEIVER_SEARCH_SYNC,
	};

	if (receiver == NULL || !v90_cp_receiver_is_valid(&initialized))
		return V90_CP_INVALID_ARGUMENT;
	*receiver = initialized;
	return V90_CP_OK;
}

static void clear_frame(struct v90_cp_receiver *receiver)
{
	memset(receiver->frame, 0, sizeof(receiver->frame));
	receiver->frame_bit_count = 0;
	receiver->bit_index = 0;
	receiver->sync_one_count = 0;
	receiver->stage = V90_CP_RECEIVER_SEARCH_SYNC;
}

static void begin_frame(struct v90_cp_receiver *receiver)
{
	clear_frame(receiver);
	for (size_t index = 0; index < CP_SYNC_BITS; ++index)
		packed_msb_set_bit(receiver->frame, index, true);
	receiver->bit_index = CP_INFORMATION_FIRST_BIT;
	receiver->stage = V90_CP_RECEIVER_READ_HEADER;
}

static void reject_frame(
	struct v90_cp_receiver *receiver,
	struct v90_cp_receive_event *event,
	enum v90_cp_result result)
{
	uint8_t suffix_ones = pcm_control_frame_trailing_ones(
		receiver->frame, receiver->bit_index);
	size_t observed_bits = receiver->bit_index;

	clear_frame(receiver);
	receiver->sync_one_count = suffix_ones;
	event->type = V90_CP_RECEIVE_REJECTED;
	event->frame_result = result;
	event->observed_bits = observed_bits;
}

static void complete_frame(
	struct v90_cp_receiver *receiver,
	struct v90_cp_receive_event *event)
{
	struct v90_cp_parameters parameters;
	enum v90_cp_result result = v90_cp_decode(
		receiver->frame, sizeof(receiver->frame),
		receiver->frame_bit_count, &parameters);
	size_t observed_bits = receiver->bit_index;

	clear_frame(receiver);
	event->frame_result = result;
	event->observed_bits = observed_bits;
	if (result == V90_CP_OK) {
		event->type = V90_CP_RECEIVE_FRAME;
		event->parameters = parameters;
	} else {
		event->type = V90_CP_RECEIVE_REJECTED;
	}
}

static void receive_bit(
	struct v90_cp_receiver *receiver, uint8_t bit,
	struct v90_cp_receive_event *event)
{
	if (receiver->stage == V90_CP_RECEIVER_SEARCH_SYNC) {
		if (bit != 0) {
			if (receiver->sync_one_count < CP_SYNC_BITS)
				receiver->sync_one_count++;
		} else if (receiver->sync_one_count == CP_SYNC_BITS) {
			begin_frame(receiver);
		} else {
			receiver->sync_one_count = 0;
		}
		return;
	}

	packed_msb_set_bit(
		receiver->frame, receiver->bit_index, bit != 0);
	receiver->bit_index++;
	if (bit != 0 && bit_must_be_zero(
			       receiver->bit_index - 1u,
			       receiver->frame_bit_count)) {
		reject_frame(receiver, event, V90_CP_INVALID_FRAME);
		return;
	}
	if (receiver->stage == V90_CP_RECEIVER_READ_HEADER &&
	    receiver->bit_index == CP_HEADER_LAST_BIT + 1u) {
		if (!infer_frame_bit_count(
			    receiver->frame, &receiver->frame_bit_count)) {
			reject_frame(receiver, event, V90_CP_INVALID_FRAME);
			return;
		}
		receiver->stage = V90_CP_RECEIVER_READ_FRAME;
	}
	if (receiver->stage == V90_CP_RECEIVER_READ_FRAME &&
	    receiver->bit_index == receiver->frame_bit_count)
		complete_frame(receiver, event);
}

enum v90_cp_result v90_cp_receiver_receive_bit(
	struct v90_cp_receiver *receiver, uint8_t bit,
	struct v90_cp_receive_event *event)
{
	struct v90_cp_receiver next;
	struct v90_cp_receive_event emitted = { 0 };

	if (!v90_cp_receiver_is_valid(receiver) || bit > 1u || event == NULL)
		return V90_CP_INVALID_ARGUMENT;
	next = *receiver;
	receive_bit(&next, bit, &emitted);
	if (!v90_cp_receiver_is_valid(&next))
		return V90_CP_INVALID_ARGUMENT;
	*receiver = next;
	*event = emitted;
	return V90_CP_OK;
}
