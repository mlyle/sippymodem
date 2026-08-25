// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_MAPPING_FRAME_ENCODER_H
#define V34_MAPPING_FRAME_ENCODER_H

#include "v34_4d_encoder.h"
#include "v34_shell_mapper.h"

#include <stddef.h>
#include <stdint.h>

enum v34_mapping_frame_encoder_result {
	V34_MAPPING_FRAME_ENCODER_OK,
	V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT,
	V34_MAPPING_FRAME_ENCODER_OUT_OF_RANGE,
};

/* Compact evidence for the full parser-to-precoder mapping-frame boundary. */
struct v34_encoded_mapping_frame {
	struct v34_parsed_mapping_frame parsed;
	struct v34_ring_indices ring_indices;
	struct v34_4d_encoder_output
		interval[V34_4D_INTERVALS_PER_MAPPING_FRAME];
};

/* Parse and encode one scheduled mapping frame. Primary bits have already
 * passed through the data-mode scrambler; an auxiliary bit remains raw. The
 * encoder state and destination are unchanged if any interval fails. */
enum v34_mapping_frame_encoder_result
v34_encode_mapping_frame(
	struct v34_4d_encoder *encoder,
	const struct v34_mapping_frame *mapping_frame,
	const uint8_t *scrambled_primary_bits,
	size_t scrambled_primary_bit_count, const uint8_t *auxiliary_bit,
	struct v34_encoded_mapping_frame *encoded);

#endif
