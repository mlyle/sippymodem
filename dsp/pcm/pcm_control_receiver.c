// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_control_receiver.h"

static inline size_t pcm_control_receiver_frame_bits(
	const uint8_t *frame, enum pcm_control_format format)
{
	struct pcm_control_constellation_layout layout =
		pcm_control_frame_read_constellation_layout(frame, NULL);

	return pcm_control_frame_constellation_bits(layout.rows, layout.codec_differs,
		format == PCM_CONTROL_CP ? 4u : 12u);
}

bool pcm_control_receiver_is_valid(
	const struct pcm_control_receiver *receiver,
	enum pcm_control_format format)
{
	if (receiver == NULL ||
	    (format != PCM_CONTROL_CP && format != PCM_CONTROL_CPT) ||
	    (receiver->stage != PCM_CONTROL_SEARCH_SYNC &&
	     receiver->stage != PCM_CONTROL_READ_HEADER &&
	     receiver->stage != PCM_CONTROL_READ_FRAME) ||
	    receiver->sync_one_count > 17u ||
	    receiver->bit_index > PCM_CONTROL_MAX_FRAME_BITS ||
	    (format == PCM_CONTROL_CPT && !packed_msb_tail_is_zero(
		    receiver->frame, receiver->bit_index, sizeof(receiver->frame))))
		return false;
	switch (receiver->stage) {
	case PCM_CONTROL_SEARCH_SYNC:
		return receiver->frame_bit_count == 0 && receiver->bit_index == 0 &&
		       (format == PCM_CONTROL_CPT || packed_msb_tail_is_zero(
			       receiver->frame, 0, sizeof(receiver->frame)));
	case PCM_CONTROL_READ_HEADER:
		if (receiver->frame_bit_count != 0 || receiver->bit_index < 18u ||
		    receiver->bit_index > PCM_CONTROL_CODEC_DIFFERENCE_BIT)
			return false;
		break;
	case PCM_CONTROL_READ_FRAME:
		/* A valid map supplies the complete bounds and protocol alignment. */
		if (receiver->bit_index <= PCM_CONTROL_CODEC_DIFFERENCE_BIT ||
		    receiver->bit_index >= receiver->frame_bit_count ||
		    receiver->frame_bit_count !=
			    pcm_control_receiver_frame_bits(receiver->frame, format))
			return false;
		break;
	}
	if (receiver->sync_one_count != 0 ||
	    !pcm_control_frame_prefix_is_valid(receiver->frame))
		return false;
	/* CP validates arriving start bits incrementally. CPt also checks its
	 * fixed type and all retained framing bits; reserved payload stays CRC-only. */
	size_t type_end = receiver->bit_index < 21u ? receiver->bit_index : 21u;

	return format == PCM_CONTROL_CP ||
	       (packed_msb_bits_are_zero(receiver->frame, 18u, type_end - 18u) &&
		pcm_control_frame_starts_are_zero(
			receiver->frame, 34u, receiver->bit_index));
}

struct pcm_control_receive_event pcm_control_receiver_receive_bit(
	struct pcm_control_receiver *receiver,
	enum pcm_control_format format, uint8_t bit)
{
	struct pcm_control_receive_event event = { 0 };

	if (receiver->stage == PCM_CONTROL_SEARCH_SYNC) {
		if (pcm_control_frame_sync_bit(&receiver->sync_one_count, bit != 0u))
			pcm_control_receiver_begin(receiver);
		return event;
	}
	/* Preserve CP interop with a next sync after one or two fill zeros.
	 * CRC and the first fill zero remain mandatory; CPt keeps its full fill. */
	if (format == PCM_CONTROL_CP &&
	    receiver->stage == PCM_CONTROL_READ_FRAME && bit != 0 &&
	    receiver->bit_index >= receiver->frame_bit_count - 2u)
		return (struct pcm_control_receive_event){
			.type = PCM_CONTROL_FRAME_READY,
			.observed_bits = receiver->bit_index + 1u,
			.sync_one_count = 1,
		};
	size_t index = receiver->bit_index++;

	packed_msb_set_bit(receiver->frame, index, bit != 0);
	bool rejected = bit != 0 && (index % 17u == 0 ||
		(format == PCM_CONTROL_CPT ? index >= 18u && index <= 20u :
		 receiver->frame_bit_count != 0 &&
		 index >= receiver->frame_bit_count - 3u));

	if (!rejected && receiver->stage == PCM_CONTROL_READ_HEADER &&
	    receiver->bit_index == PCM_CONTROL_CODEC_DIFFERENCE_BIT + 1u) {
		receiver->frame_bit_count =
			pcm_control_receiver_frame_bits(receiver->frame, format);
		rejected = receiver->frame_bit_count == 0;
		if (!rejected)
			receiver->stage = PCM_CONTROL_READ_FRAME;
	}
	if (rejected)
		return (struct pcm_control_receive_event){
			.type = PCM_CONTROL_FRAME_REJECTED,
			.observed_bits = receiver->bit_index,
			.sync_one_count = pcm_control_frame_trailing_ones(
				receiver->frame, receiver->bit_index),
		};
	if (receiver->stage == PCM_CONTROL_READ_FRAME &&
	    receiver->bit_index == receiver->frame_bit_count)
		return (struct pcm_control_receive_event){
			.type = PCM_CONTROL_FRAME_READY,
			.observed_bits = receiver->bit_index,
		};
	return event;
}
