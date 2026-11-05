// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_MP_RECEIVER_H
#define PCM_MP_RECEIVER_H

#include "pcm_control_frame.h"

#include <string.h>

/* V.90's aligned Type 1 MP is the longest supported MP sequence. */
enum {
	PCM_MP_MAX_FRAME_BITS = 210,
	PCM_MP_MAX_FRAME_BYTES = (PCM_MP_MAX_FRAME_BITS + 7) / 8,
};

enum pcm_mp_receive_stage {
	PCM_MP_SEARCH_SYNC,
	PCM_MP_READ_TYPE,
	PCM_MP_READ_FRAME,
};

/* Own the common V.34/V.90 MP wire candidate. Protocol owners provide the
 * two lengths and decode complete records; no input pointers are retained. */
struct pcm_mp_receiver {
	uint8_t frame[PCM_MP_MAX_FRAME_BYTES];
	size_t frame_bit_count, bit_index;
	uint8_t sync_one_count;
	enum pcm_mp_receive_stage stage;
};

static inline bool pcm_mp_receiver_is_valid(
	const struct pcm_mp_receiver *receiver, size_t type0_bits, size_t type1_bits)
{
	if (receiver == NULL || type0_bits <= 18u || type1_bits <= 18u ||
	    type0_bits > PCM_MP_MAX_FRAME_BITS || type1_bits > PCM_MP_MAX_FRAME_BITS ||
	    receiver->sync_one_count > 17u ||
	    receiver->bit_index > PCM_MP_MAX_FRAME_BITS ||
	    !packed_msb_tail_is_zero(receiver->frame, receiver->bit_index,
		    sizeof(receiver->frame)))
		return false;
	if (receiver->stage == PCM_MP_SEARCH_SYNC)
		return receiver->frame_bit_count == 0 && receiver->bit_index == 0;
	if (!pcm_control_frame_prefix_is_valid(receiver->frame) ||
	    receiver->sync_one_count != pcm_control_frame_trailing_ones(
		    receiver->frame, receiver->bit_index))
		return false;
	if (receiver->stage == PCM_MP_READ_TYPE)
		return receiver->frame_bit_count == 0 && receiver->bit_index == 18u;
	return receiver->stage == PCM_MP_READ_FRAME && receiver->bit_index > 18u &&
	       receiver->bit_index < receiver->frame_bit_count &&
	       receiver->frame_bit_count == (packed_msb_bit(receiver->frame, 18u) ?
		       type1_bits : type0_bits);
}

static inline void pcm_mp_receiver_clear(
	struct pcm_mp_receiver *receiver, uint8_t suffix_ones)
{
	memset(receiver->frame, 0, sizeof(receiver->frame));
	receiver->frame_bit_count = 0;
	receiver->bit_index = 0;
	receiver->sync_one_count = suffix_ones;
	receiver->stage = PCM_MP_SEARCH_SYNC;
}

/* Consume validated state, lengths and one binary bit. A newer prefix can
 * replace a truncated candidate: valid MP start bits exclude 17 ones inside
 * a record. On completion, leave the frame buffered for the typed decoder.
 * The owner must clear it, retaining sync_one_count only after rejection,
 * before validating or consuming another bit. */
static inline bool pcm_mp_receiver_receive_bit(struct pcm_mp_receiver *receiver,
	uint8_t bit, size_t type0_bits, size_t type1_bits)
{
	if (pcm_control_frame_sync_bit(&receiver->sync_one_count, bit != 0)) {
		pcm_mp_receiver_clear(receiver, 0);
		pcm_control_frame_write_prefix(receiver->frame);
		receiver->bit_index = 18u;
		receiver->stage = PCM_MP_READ_TYPE;
		return false;
	}
	if (receiver->stage == PCM_MP_SEARCH_SYNC)
		return false;
	packed_msb_set_bit(receiver->frame, receiver->bit_index++, bit != 0);
	if (receiver->stage == PCM_MP_READ_TYPE) {
		receiver->frame_bit_count = bit ? type1_bits : type0_bits;
		receiver->stage = PCM_MP_READ_FRAME;
	}
	return receiver->bit_index == receiver->frame_bit_count;
}

#endif
