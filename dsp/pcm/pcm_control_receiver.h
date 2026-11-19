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
		(1 + 2 * PCM_CONSTELLATION_MAX_ROWS) * PCM_CONTROL_CONSTELLATION_ROW_BITS + 20,
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

bool pcm_control_receiver_is_valid(
	const struct pcm_control_receiver *receiver,
	enum pcm_control_format format);

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
struct pcm_control_receive_event pcm_control_receiver_receive_bit(
	struct pcm_control_receiver *receiver,
	enum pcm_control_format format, uint8_t bit);

#endif /* PCM_CONTROL_RECEIVER_H */
