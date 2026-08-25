// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_MAPPING_FRAME_DECODER_H
#define V34_MAPPING_FRAME_DECODER_H

#include "v34_4d_decoder.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_mapping_frame_decoder_result {
	V34_MAPPING_FRAME_DECODER_OK,
	V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT,
};

struct v34_recovered_mapping_frame {
	/* Primary bits remain scrambled at this mapping boundary. */
	uint8_t primary_bits[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t primary_bit_count;
	/* Zero or one; auxiliary_bit is meaningful only when this is one. */
	uint8_t auxiliary_bit_count;
	uint8_t auxiliary_bit;
};

/* Recover one complete mapping frame from exact 4D decisions. Equalization,
 * inverse precoding, and trellis survivor selection precede this boundary.
 * decided[].trellis_output is U0 after the scheduled 4D inversion. Primary
 * bits are returned earliest first, one bit per byte; the auxiliary bit is
 * returned raw. Decoder state and recovered output are unchanged on failure. */
enum v34_mapping_frame_decoder_result
v34_decode_mapping_frame(
	struct v34_4d_decoder *decoder,
	const struct v34_mapping_frame *mapping_frame,
	const struct v34_decided_4d_interval
		decided[V34_4D_INTERVALS_PER_MAPPING_FRAME],
	struct v34_recovered_mapping_frame *recovered);

#endif /* V34_MAPPING_FRAME_DECODER_H */
