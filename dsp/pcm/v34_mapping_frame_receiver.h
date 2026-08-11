// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_MAPPING_FRAME_RECEIVER_H
#define SIPPYMODEM_V34_MAPPING_FRAME_RECEIVER_H

#include "sippymodem_v34_framing.h"
#include "v34_mapping_frame_decoder.h"

#include <stdint.h>

enum sippymodem_v34_mapping_frame_receiver_result {
	SIPPYMODEM_V34_MAPPING_FRAME_RECEIVER_OK,
	SIPPYMODEM_V34_MAPPING_FRAME_RECEIVER_FRAME_COMPLETE,
	SIPPYMODEM_V34_MAPPING_FRAME_RECEIVER_INVALID_ARGUMENT,
};

struct sippymodem_v34_received_mapping_frame {
	struct sippymodem_v34_mapping_frame mapping_frame;
	/* Primary bits are descrambled and returned earliest first. */
	uint8_t primary_bits[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS];
	uint8_t primary_bit_count;
	/* Zero or one; auxiliary_bit is meaningful only when this is one. */
	uint8_t auxiliary_bit_count;
	uint8_t auxiliary_bit;
};

/* Receive one complete scheduled mapping frame from exact 4D decisions.
 * Equalization, inverse precoding, and trellis survivor selection precede
 * this boundary. The auxiliary bit does not pass through the descrambler.
 * Schedule, descrambler, decoder, and destination are unchanged unless the
 * complete frame succeeds. */
enum sippymodem_v34_mapping_frame_receiver_result
sippymodem_v34_receive_mapping_frame(
	struct sippymodem_v34_frame_schedule *schedule,
	struct sippymodem_v34_scrambler *descrambler,
	struct sippymodem_v34_4d_decoder *mapping_decoder,
	const struct sippymodem_v34_decided_4d_interval
		decided[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME],
	struct sippymodem_v34_received_mapping_frame *received);

const char *sippymodem_v34_mapping_frame_receiver_result_name(
	enum sippymodem_v34_mapping_frame_receiver_result result);

#endif /* SIPPYMODEM_V34_MAPPING_FRAME_RECEIVER_H */
