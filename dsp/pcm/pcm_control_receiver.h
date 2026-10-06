// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_CONTROL_RECEIVER_H
#define PCM_CONTROL_RECEIVER_H

#include "pcm_control_frame.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* V.90 Table 14 and V.92 Table 23 share a 136-bit header and up to two
 * planes of six 136-bit rows. At that maximum row count, either format
 * ends 20 bits after the payload; shorter CPt records still align to 12 symbols. */
enum {
	PCM_CONTROL_MAX_FRAME_BITS =
		(1 + 2 * PCM_CONSTELLATION_MAX_ROWS) * 136 + 20,
	PCM_CONTROL_MAX_FRAME_BYTES = (PCM_CONTROL_MAX_FRAME_BITS + 7) / 8,
};

enum pcm_control_receive_stage {
	PCM_CONTROL_SEARCH_SYNC,
	PCM_CONTROL_READ_HEADER,
	PCM_CONTROL_READ_FRAME,
};

/* Owned wire storage. Typed CP/CPt receivers select framing and retain
 * their own codecs; no pointers to input or decoded parameters are retained. */
struct pcm_control_receiver {
	uint8_t frame[PCM_CONTROL_MAX_FRAME_BYTES];
	size_t frame_bit_count;
	size_t bit_index;
	uint8_t sync_one_count;
	enum pcm_control_receive_stage stage;
};

enum pcm_control_format {
	PCM_CONTROL_CP,
	PCM_CONTROL_CPT,
};

enum pcm_control_receive_event_type {
	PCM_CONTROL_NO_EVENT,
	PCM_CONTROL_FRAME_READY,
	PCM_CONTROL_FRAME_REJECTED,
};

struct pcm_control_receive_event {
	enum pcm_control_receive_event_type type;
	size_t observed_bits;
	uint8_t sync_one_count;
};

static inline size_t pcm_control_receiver_frame_bits(
	const uint8_t *frame, enum pcm_control_format format)
{
	size_t count = pcm_control_frame_read_map(frame, NULL);

	if (count == 0)
		return 0;
	size_t bits = pcm_control_frame_payload_end(count,
		packed_msb_bit(frame, 128u) != 0u);

	return format == PCM_CONTROL_CP ? bits + 20u :
		pcm_control_frame_align_bits(bits + 18u, 12u);
}

static inline bool pcm_control_receiver_is_valid(
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
		    receiver->bit_index > 128u)
			return false;
		break;
	case PCM_CONTROL_READ_FRAME:
		/* A valid map supplies the complete bounds and protocol alignment. */
		if (receiver->bit_index <= 128u ||
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

static inline void pcm_control_receiver_clear(
	struct pcm_control_receiver *receiver, uint8_t sync_one_count)
{
	memset(receiver->frame, 0, sizeof(receiver->frame));
	receiver->frame_bit_count = 0;
	receiver->bit_index = 0;
	receiver->sync_one_count = sync_one_count;
	receiver->stage = PCM_CONTROL_SEARCH_SYNC;
}

static inline void pcm_control_receiver_begin(struct pcm_control_receiver *receiver)
{
	pcm_control_receiver_clear(receiver, 0);
	pcm_control_frame_write_prefix(receiver->frame);
	receiver->bit_index = 18u;
	receiver->stage = PCM_CONTROL_READ_HEADER;
}

/* Consume a validated state and binary bit. An event leaves the candidate
 * buffered for the typed decoder. The owner must then clear it with the
 * event's retained sync suffix before validating or consuming another bit. */
static inline struct pcm_control_receive_event pcm_control_receiver_receive_bit(
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
	    receiver->bit_index == 129u) {
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

#endif /* PCM_CONTROL_RECEIVER_H */
