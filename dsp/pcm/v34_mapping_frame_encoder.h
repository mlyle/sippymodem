// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_H
#define SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_H

#include "v34_4d_encoder.h"
#include "v34_shell_mapper.h"

#include <stddef.h>
#include <stdint.h>

enum sippymodem_v34_mapping_frame_encoder_result {
	SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OK,
	SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT,
	SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OUT_OF_RANGE,
};

/* Compact evidence for the full parser-to-precoder mapping-frame boundary. */
struct sippymodem_v34_encoded_mapping_frame {
	struct sippymodem_v34_parsed_mapping_frame parsed;
	struct sippymodem_v34_ring_indices ring_indices;
	struct sippymodem_v34_4d_encoder_output
		interval[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME];
};

/* Parse and encode one scheduled mapping frame. Primary bits have already
 * passed through the data-mode scrambler; an auxiliary bit remains raw. The
 * encoder state and destination are unchanged if any interval fails. */
enum sippymodem_v34_mapping_frame_encoder_result
sippymodem_v34_encode_mapping_frame(
	struct sippymodem_v34_4d_encoder *encoder,
	const struct sippymodem_v34_mapping_frame *mapping_frame,
	const uint8_t *scrambled_primary_bits,
	size_t scrambled_primary_bit_count, const uint8_t *auxiliary_bit,
	struct sippymodem_v34_encoded_mapping_frame *encoded);

const char *sippymodem_v34_mapping_frame_encoder_result_name(
	enum sippymodem_v34_mapping_frame_encoder_result result);

#endif
