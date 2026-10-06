// SPDX-License-Identifier: BSD-3-Clause

#include "v92_cpt_receiver.h"

#include "pcm_control_frame.h"
#include "packed_bits.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert((unsigned)V92_CPT_MAX_FRAME_BYTES == PCM_CONTROL_MAX_FRAME_BYTES,
	"control receive storage must cover the complete frame");

enum {
	CPT_SYNC_BITS = 17,
	CPT_INFORMATION_FIRST_BIT = 18,
	CPT_CODEC_DIFFERENCE_BIT = 128,
	CPT_HEADER_LAST_BIT = CPT_CODEC_DIFFERENCE_BIT,
};

static bool infer_frame_bit_count(
	const uint8_t *frame, size_t *frame_bit_count)
{
	size_t constellation_count;
	size_t inferred;

	if (frame == NULL || frame_bit_count == NULL)
		return false;
	constellation_count = pcm_control_frame_read_map(frame, NULL);
	if (constellation_count == 0)
		return false;
	inferred = pcm_control_frame_align_bits(
		pcm_control_frame_payload_end(constellation_count,
			packed_msb_bit(frame, CPT_CODEC_DIFFERENCE_BIT) != 0) +
			V92_CPT_MIN_TRAILER_BITS, V92_CPT_SYMBOLS_PER_FRAME);
	if (inferred < V92_CPT_MIN_FRAME_BITS ||
	    inferred > V92_CPT_MAX_FRAME_BITS)
		return false;
	*frame_bit_count = inferred;
	return true;
}

static bool stage_is_valid(enum pcm_control_receive_stage stage)
{
	return stage >= PCM_CONTROL_SEARCH_SYNC &&
	       stage <= PCM_CONTROL_READ_FRAME;
}

static bool bit_must_be_zero(size_t bit_index)
{
	return bit_index % 17u == 0u ||
	       (bit_index >= 18u && bit_index <= 20u);
}

static bool received_bits_are_valid(
	const struct v92_cpt_receiver *receiver)
{
	size_t type_end = receiver->wire.bit_index < 21u ? receiver->wire.bit_index : 21u;

	return packed_msb_bits_are_zero(
		       receiver->wire.frame, CPT_INFORMATION_FIRST_BIT,
		       type_end - CPT_INFORMATION_FIRST_BIT) &&
	       pcm_control_frame_starts_are_zero(
		       receiver->wire.frame, 34u, receiver->wire.bit_index);
}

bool v92_cpt_receiver_is_valid(
	const struct v92_cpt_receiver *receiver)
{
	size_t inferred_bits;

	if (receiver == NULL || !stage_is_valid(receiver->wire.stage) ||
	    receiver->wire.sync_one_count > CPT_SYNC_BITS ||
	    receiver->wire.bit_index > V92_CPT_MAX_FRAME_BITS ||
	    !packed_msb_tail_is_zero(
		    receiver->wire.frame, receiver->wire.bit_index,
		    sizeof(receiver->wire.frame)))
		return false;
	switch (receiver->wire.stage) {
	case PCM_CONTROL_SEARCH_SYNC:
		return receiver->wire.frame_bit_count == 0u &&
		       receiver->wire.bit_index == 0u;
	case PCM_CONTROL_READ_HEADER:
		return receiver->wire.frame_bit_count == 0u &&
		       receiver->wire.bit_index >= CPT_INFORMATION_FIRST_BIT &&
		       receiver->wire.bit_index <= CPT_HEADER_LAST_BIT &&
		       receiver->wire.sync_one_count == 0u &&
		       pcm_control_frame_prefix_is_valid(receiver->wire.frame) &&
		       received_bits_are_valid(receiver);
	case PCM_CONTROL_READ_FRAME:
		return receiver->wire.frame_bit_count >= V92_CPT_MIN_FRAME_BITS &&
		       receiver->wire.frame_bit_count <= V92_CPT_MAX_FRAME_BITS &&
		       receiver->wire.frame_bit_count %
			       V92_CPT_SYMBOLS_PER_FRAME == 0u &&
		       receiver->wire.bit_index > CPT_HEADER_LAST_BIT &&
		       receiver->wire.bit_index < receiver->wire.frame_bit_count &&
		       receiver->wire.sync_one_count == 0u &&
		       pcm_control_frame_prefix_is_valid(receiver->wire.frame) &&
		       received_bits_are_valid(receiver) &&
		       infer_frame_bit_count(
			       receiver->wire.frame, &inferred_bits) &&
		       inferred_bits == receiver->wire.frame_bit_count;
	}
	return false;
}

enum v92_cpt_result v92_cpt_receiver_init(
	struct v92_cpt_receiver *receiver)
{
	struct v92_cpt_receiver initialized = {
		.wire.stage = PCM_CONTROL_SEARCH_SYNC,
	};

	if (receiver == NULL || !v92_cpt_receiver_is_valid(&initialized))
		return V92_CPT_INVALID_ARGUMENT;
	*receiver = initialized;
	return V92_CPT_OK;
}

static void clear_frame(struct v92_cpt_receiver *receiver)
{
	memset(receiver->wire.frame, 0, sizeof(receiver->wire.frame));
	receiver->wire.frame_bit_count = 0u;
	receiver->wire.bit_index = 0u;
	receiver->wire.sync_one_count = 0u;
	receiver->wire.stage = PCM_CONTROL_SEARCH_SYNC;
}

static void begin_frame(struct v92_cpt_receiver *receiver)
{
	clear_frame(receiver);
	pcm_control_frame_write_prefix(receiver->wire.frame);
	receiver->wire.bit_index = CPT_INFORMATION_FIRST_BIT;
	receiver->wire.stage = PCM_CONTROL_READ_HEADER;
}

enum v92_cpt_result v92_cpt_receiver_begin_after_sync(
	struct v92_cpt_receiver *receiver)
{
	struct v92_cpt_receiver next;

	if (!v92_cpt_receiver_is_valid(receiver))
		return V92_CPT_INVALID_ARGUMENT;
	if (receiver->wire.stage != PCM_CONTROL_SEARCH_SYNC)
		return V92_CPT_INVALID_FRAME;
	next = *receiver;
	begin_frame(&next);
	if (!v92_cpt_receiver_is_valid(&next))
		return V92_CPT_INVALID_FRAME;
	*receiver = next;
	return V92_CPT_OK;
}

static void reject_frame(
	struct v92_cpt_receiver *receiver,
	struct v92_cpt_receive_event *event,
	enum v92_cpt_result result)
{
	uint8_t suffix_ones = pcm_control_frame_trailing_ones(
		receiver->wire.frame, receiver->wire.bit_index);
	size_t observed_bits = receiver->wire.bit_index;

	clear_frame(receiver);
	receiver->wire.sync_one_count = suffix_ones;
	event->type = V92_CPT_RECEIVE_REJECTED;
	event->frame_result = result;
	event->observed_bits = observed_bits;
}

static void complete_frame(
	struct v92_cpt_receiver *receiver,
	struct v92_cpt_receive_event *event)
{
	struct v92_cpt_parameters parameters;
	enum v92_cpt_result result = v92_cpt_decode(
		receiver->wire.frame, sizeof(receiver->wire.frame),
		receiver->wire.frame_bit_count, &parameters);
	size_t observed_bits = receiver->wire.bit_index;

	clear_frame(receiver);
	event->frame_result = result;
	event->observed_bits = observed_bits;
	if (result == V92_CPT_OK) {
		event->type = V92_CPT_RECEIVE_FRAME;
		event->parameters = parameters;
	} else {
		event->type = V92_CPT_RECEIVE_REJECTED;
	}
}

static void receive_bit(
	struct v92_cpt_receiver *receiver, uint8_t bit,
	struct v92_cpt_receive_event *event)
{
	if (receiver->wire.stage == PCM_CONTROL_SEARCH_SYNC) {
		if (pcm_control_frame_sync_bit(
			    &receiver->wire.sync_one_count, bit != 0u))
			begin_frame(receiver);
		return;
	}

	packed_msb_set_bit(
		receiver->wire.frame, receiver->wire.bit_index, bit != 0u);
	receiver->wire.bit_index++;
	if (bit != 0u && bit_must_be_zero(receiver->wire.bit_index - 1u)) {
		reject_frame(receiver, event, V92_CPT_INVALID_FRAME);
		return;
	}
	if (receiver->wire.stage == PCM_CONTROL_READ_HEADER &&
	    receiver->wire.bit_index == CPT_HEADER_LAST_BIT + 1u) {
		if (!infer_frame_bit_count(
			    receiver->wire.frame, &receiver->wire.frame_bit_count)) {
			reject_frame(receiver, event, V92_CPT_INVALID_FRAME);
			return;
		}
		receiver->wire.stage = PCM_CONTROL_READ_FRAME;
	}
	if (receiver->wire.stage == PCM_CONTROL_READ_FRAME &&
	    receiver->wire.bit_index == receiver->wire.frame_bit_count)
		complete_frame(receiver, event);
}

enum v92_cpt_result v92_cpt_receiver_receive_bit(
	struct v92_cpt_receiver *receiver, uint8_t bit,
	struct v92_cpt_receive_event *event)
{
	struct v92_cpt_receiver next;
	struct v92_cpt_receive_event emitted = { 0 };

	if (!v92_cpt_receiver_is_valid(receiver) || bit > 1u || event == NULL)
		return V92_CPT_INVALID_ARGUMENT;
	next = *receiver;
	receive_bit(&next, bit, &emitted);
	if (!v92_cpt_receiver_is_valid(&next))
		return V92_CPT_INVALID_FRAME;
	*receiver = next;
	*event = emitted;
	return V92_CPT_OK;
}
