// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_MAPPING_FRAME_RECEIVER_H
#define V34_MAPPING_FRAME_RECEIVER_H

#include "v34_framing.h"
#include "v34_mapping_frame_decoder.h"

#include <stdint.h>

enum v34_mapping_frame_receiver_result {
	V34_MAPPING_FRAME_RECEIVER_OK,
	V34_MAPPING_FRAME_RECEIVER_FRAME_COMPLETE,
	V34_MAPPING_FRAME_RECEIVER_INVALID_ARGUMENT,
};

struct v34_received_mapping_frame {
	struct v34_mapping_frame mapping_frame;
	/* Primary bits are descrambled and returned earliest first. */
	uint8_t primary_bits[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t primary_bit_count;
	/* Zero or one; auxiliary_bit is meaningful only when this is one. */
	uint8_t auxiliary_bit_count;
	uint8_t auxiliary_bit;
};

/* Receive one complete scheduled mapping frame from hard 4D decisions.
 * Equalization, inverse precoding, and trellis survivor selection precede
 * this boundary. An out-of-codebook shell decision still advances decoder
 * and descrambler state, but its delivered bits are replaced with idle marks.
 * The auxiliary bit does not pass through the descrambler.
 * Schedule, descrambler, decoder, and destination are unchanged unless the
 * complete frame succeeds. */
enum v34_mapping_frame_receiver_result
v34_receive_mapping_frame(
	struct v34_frame_schedule *schedule,
	struct v34_scrambler *descrambler,
	struct v34_4d_decoder *mapping_decoder,
	const struct v34_decided_4d_interval
		decided[V34_4D_INTERVALS_PER_MAPPING_FRAME],
	struct v34_received_mapping_frame *received);

#endif /* V34_MAPPING_FRAME_RECEIVER_H */
