// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_MAPPING_FRAME_DECODER_H
#define SIPPYMODEM_V34_MAPPING_FRAME_DECODER_H

#include "sippymodem_v34_4d_decoder.h"

#include <stdint.h>

enum sippymodem_v34_mapping_frame_decoder_result {
	SIPPYMODEM_V34_MAPPING_FRAME_DECODER_OK,
	SIPPYMODEM_V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT,
};

struct sippymodem_v34_recovered_mapping_frame {
	/* Primary bits remain scrambled at this mapping boundary. */
	uint8_t primary_bits[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS];
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
enum sippymodem_v34_mapping_frame_decoder_result
sippymodem_v34_decode_mapping_frame(
	struct sippymodem_v34_4d_decoder *decoder,
	const struct sippymodem_v34_mapping_frame *mapping_frame,
	const struct sippymodem_v34_decided_4d_interval
		decided[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME],
	struct sippymodem_v34_recovered_mapping_frame *recovered);

const char *sippymodem_v34_mapping_frame_decoder_result_name(
	enum sippymodem_v34_mapping_frame_decoder_result result);

#endif /* SIPPYMODEM_V34_MAPPING_FRAME_DECODER_H */
