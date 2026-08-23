// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_4D_DECODER_H
#define V34_4D_DECODER_H

#include "v34_mapper.h"
#include "v34_parser.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_4d_decoder_result {
	V34_4D_DECODER_OK,
	V34_4D_DECODER_INVALID_ARGUMENT,
};

struct v34_4d_decoder {
	struct v34_mapping_parameters parameters;
	/* Z(m - 1), expressed as clockwise quarter turns. */
	uint8_t previous_differential_output;
};

/* Exact mapper outputs selected by the precoder and trellis survivor path. */
struct v34_decided_4d_interval {
	struct v34_grid_point
		mapper_output[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	/* U0(m), including the scheduled 4D inversion. */
	uint8_t trellis_output;
};

struct v34_decoded_4d_interval {
	struct v34_4d_mapping_input mapping;
	uint8_t ring_index[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	/* Z(m), retained as compact decision evidence. */
	uint8_t differential_output;
};

enum v34_4d_decoder_result v34_4d_decoder_init(
	struct v34_4d_decoder *decoder,
	const struct v34_mapping_parameters *parameters);

bool v34_4d_decoder_is_valid(
	const struct v34_4d_decoder *decoder);

/* Recover one 4D mapper input from exact decided mapper outputs and U0.
 * Equalization, precoder reversal, and trellis survivor selection precede
 * this boundary. State and output are unchanged on rejection. */
enum v34_4d_decoder_result v34_decode_4d_interval(
	struct v34_4d_decoder *decoder,
	const struct v34_decided_4d_interval *decided,
	struct v34_decoded_4d_interval *decoded);

#endif
