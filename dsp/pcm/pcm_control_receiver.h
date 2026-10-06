// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_CONTROL_RECEIVER_H
#define PCM_CONTROL_RECEIVER_H

#include "pcm_constellation.h"

#include <stddef.h>
#include <stdint.h>

/* V.90 Table 14 and V.92 Table 23 share a 136-bit header and up to two
 * planes of six 136-bit rows. The largest CP/CPt trailer occupies 20 bits. */
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

/* Owned wire storage. Typed CP/CPt receivers retain their own codecs and
 * framing rules; no pointers to input or decoded parameters are retained. */
struct pcm_control_receiver {
	uint8_t frame[PCM_CONTROL_MAX_FRAME_BYTES];
	size_t frame_bit_count;
	size_t bit_index;
	uint8_t sync_one_count;
	enum pcm_control_receive_stage stage;
};

#endif /* PCM_CONTROL_RECEIVER_H */
