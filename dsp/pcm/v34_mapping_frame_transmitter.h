// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_MAPPING_FRAME_TRANSMITTER_H
#define V34_MAPPING_FRAME_TRANSMITTER_H

#include "v34_framing.h"
#include "v34_mapping_frame_encoder.h"
#include "v34_nonlinear.h"

#include <stddef.h>
#include <stdint.h>

enum {
	V34_SYMBOLS_PER_MAPPING_FRAME =
		V34_4D_INTERVALS_PER_MAPPING_FRAME *
		V34_2D_SYMBOLS_PER_4D_INTERVAL,
};

enum v34_mapping_frame_transmitter_result {
	V34_MAPPING_FRAME_TRANSMITTER_OK,
	V34_MAPPING_FRAME_TRANSMITTER_FRAME_COMPLETE,
	V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT,
	V34_MAPPING_FRAME_TRANSMITTER_OUT_OF_RANGE,
	V34_MAPPING_FRAME_TRANSMITTER_NUMERIC_FAILURE,
};

struct v34_transmitted_mapping_frame {
	struct v34_mapping_frame mapping_frame;
	struct v34_encoded_mapping_frame encoded;
	/* Clause 9.7 outputs in interval-major, then symbol-major order. */
	struct qam_point
		projected_symbol[V34_SYMBOLS_PER_MAPPING_FRAME];
};

/* Transmit one scheduled mapping frame from raw primary bits. Primary data is
 * scrambled in transmission order; an auxiliary bit is never scrambled. The
 * projected symbols retain constellation-grid units and therefore still need
 * the data-mode modulation owner's power normalization. Schedule, scrambler,
 * mapping encoder, and destination are unchanged unless all eight symbols
 * succeed. */
enum v34_mapping_frame_transmitter_result
v34_transmit_mapping_frame(
	struct v34_frame_schedule *schedule,
	struct v34_scrambler *scrambler,
	struct v34_4d_encoder *mapping_encoder,
	const uint8_t *primary_bits, size_t primary_bit_count,
	const uint8_t *auxiliary_bit,
	enum v34_nonlinear_parameter nonlinear_parameter,
	double mean_squared_magnitude,
	struct v34_transmitted_mapping_frame *transmitted);

#endif
