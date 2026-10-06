// SPDX-License-Identifier: BSD-3-Clause

#include "v90_cp_receiver.h"

#include "pcm_control_frame.h"
#include "packed_bits.h"

#include <string.h>

_Static_assert((unsigned)V90_CP_MAX_FRAME_BYTES == PCM_CONTROL_MAX_FRAME_BYTES,
	"control receive storage must cover the complete frame");

enum {
	CP_SYNC_BITS = 17,
	CP_INFORMATION_FIRST_BIT = 18,
	CP_CODEC_DIFFERENCE_BIT = 128,
	CP_HEADER_LAST_BIT = CP_CODEC_DIFFERENCE_BIT,
};

static bool infer_frame_bit_count(
	const uint8_t *frame, size_t *frame_bit_count)
{
	size_t constellation_count = pcm_control_frame_read_map(frame, NULL);
	if (constellation_count == 0)
		return false;
	*frame_bit_count = pcm_control_frame_payload_end(constellation_count,
		packed_msb_bit(frame, CP_CODEC_DIFFERENCE_BIT) != 0) + V90_CP_TRAILER_BITS;
	return true;
}

static bool stage_is_valid(enum pcm_control_receive_stage stage)
{
	return stage == PCM_CONTROL_SEARCH_SYNC ||
	       stage == PCM_CONTROL_READ_HEADER ||
	       stage == PCM_CONTROL_READ_FRAME;
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

	if (receiver == NULL || !stage_is_valid(receiver->wire.stage) ||
	    receiver->wire.sync_one_count > CP_SYNC_BITS ||
	    receiver->wire.bit_index > V90_CP_MAX_FRAME_BITS)
		return false;
	switch (receiver->wire.stage) {
	case PCM_CONTROL_SEARCH_SYNC:
		return receiver->wire.frame_bit_count == 0 &&
		       receiver->wire.bit_index == 0 &&
		       packed_msb_tail_is_zero(
			       receiver->wire.frame, 0,
			       sizeof(receiver->wire.frame));
	case PCM_CONTROL_READ_HEADER:
		/* begin_frame() clears the bounded record, and receive_bit()
		 * rejects every nonzero start bit as it arrives. Reserved
		 * information bits are uninterpreted but still CRC-protected.
		 * Do not rescan the growing prefix on every 8 kHz sample. */
		return receiver->wire.frame_bit_count == 0 &&
		       receiver->wire.bit_index >= CP_INFORMATION_FIRST_BIT &&
		       receiver->wire.bit_index <= CP_HEADER_LAST_BIT &&
		       receiver->wire.sync_one_count == 0 &&
		       pcm_control_frame_prefix_is_valid(receiver->wire.frame);
	case PCM_CONTROL_READ_FRAME:
		return receiver->wire.frame_bit_count >= V90_CP_MIN_FRAME_BITS &&
		       receiver->wire.frame_bit_count <= V90_CP_MAX_FRAME_BITS &&
		       receiver->wire.bit_index > CP_HEADER_LAST_BIT &&
		       receiver->wire.bit_index < receiver->wire.frame_bit_count &&
		       receiver->wire.sync_one_count == 0 &&
		       pcm_control_frame_prefix_is_valid(receiver->wire.frame) &&
		       infer_frame_bit_count(receiver->wire.frame, &inferred_bits) &&
		       inferred_bits == receiver->wire.frame_bit_count;
	}
	return false;
}

enum v90_cp_result v90_cp_receiver_init(
	struct v90_cp_receiver *receiver)
{
	struct v90_cp_receiver initialized = {
		.wire.stage = PCM_CONTROL_SEARCH_SYNC,
	};

	if (receiver == NULL || !v90_cp_receiver_is_valid(&initialized))
		return V90_CP_INVALID_ARGUMENT;
	*receiver = initialized;
	return V90_CP_OK;
}

static void clear_frame(struct v90_cp_receiver *receiver)
{
	memset(receiver->wire.frame, 0, sizeof(receiver->wire.frame));
	receiver->wire.frame_bit_count = 0;
	receiver->wire.bit_index = 0;
	receiver->wire.sync_one_count = 0;
	receiver->wire.stage = PCM_CONTROL_SEARCH_SYNC;
}

static void begin_frame(struct v90_cp_receiver *receiver)
{
	clear_frame(receiver);
	pcm_control_frame_write_prefix(receiver->wire.frame);
	receiver->wire.bit_index = CP_INFORMATION_FIRST_BIT;
	receiver->wire.stage = PCM_CONTROL_READ_HEADER;
}

static void reject_frame(
	struct v90_cp_receiver *receiver,
	struct v90_cp_receive_event *event,
	enum v90_cp_result result)
{
	uint8_t suffix_ones = pcm_control_frame_trailing_ones(
		receiver->wire.frame, receiver->wire.bit_index);
	size_t observed_bits = receiver->wire.bit_index;

	clear_frame(receiver);
	receiver->wire.sync_one_count = suffix_ones;
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
		receiver->wire.frame, sizeof(receiver->wire.frame),
		receiver->wire.frame_bit_count, &parameters);
	size_t observed_bits = receiver->wire.bit_index;

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
	if (receiver->wire.stage == PCM_CONTROL_SEARCH_SYNC) {
		if (pcm_control_frame_sync_bit(&receiver->wire.sync_one_count, bit != 0u))
			begin_frame(receiver);
		return;
	}

	/* Some peers start the next sync after only one fill zero. The
	 * payload, CRC and first fill bit are already complete here. Validate
	 * with the omitted padding still zero, then retain this first sync one
	 * even on CRC rejection. Standard three-zero padding is unchanged. */
	if (receiver->wire.stage == PCM_CONTROL_READ_FRAME && bit != 0 &&
	    receiver->wire.bit_index >= receiver->wire.frame_bit_count - 2u) {
		complete_frame(receiver, event);
		receiver->wire.sync_one_count = 1u;
		++event->observed_bits;
		return;
	}

	packed_msb_set_bit(
		receiver->wire.frame, receiver->wire.bit_index, bit != 0);
	receiver->wire.bit_index++;
	if (bit != 0 && bit_must_be_zero(
			       receiver->wire.bit_index - 1u,
			       receiver->wire.frame_bit_count)) {
		reject_frame(receiver, event, V90_CP_INVALID_FRAME);
		return;
	}
	if (receiver->wire.stage == PCM_CONTROL_READ_HEADER &&
	    receiver->wire.bit_index == CP_HEADER_LAST_BIT + 1u) {
		if (!infer_frame_bit_count(
			    receiver->wire.frame, &receiver->wire.frame_bit_count)) {
			reject_frame(receiver, event, V90_CP_INVALID_FRAME);
			return;
		}
		receiver->wire.stage = PCM_CONTROL_READ_FRAME;
	}
	if (receiver->wire.stage == PCM_CONTROL_READ_FRAME &&
	    receiver->wire.bit_index == receiver->wire.frame_bit_count)
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
