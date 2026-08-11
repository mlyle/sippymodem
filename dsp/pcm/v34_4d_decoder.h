// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_4D_DECODER_H
#define SIPPYMODEM_V34_4D_DECODER_H

#include "v34_mapper.h"
#include "v34_parser.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_4d_decoder_result {
	SIPPYMODEM_V34_4D_DECODER_OK,
	SIPPYMODEM_V34_4D_DECODER_INVALID_ARGUMENT,
};

struct sippymodem_v34_4d_decoder {
	struct sippymodem_v34_mapping_parameters parameters;
	/* Z(m - 1), expressed as clockwise quarter turns. */
	uint8_t previous_differential_output;
};

/* Exact mapper outputs selected by the precoder and trellis survivor path. */
struct sippymodem_v34_decided_4d_interval {
	struct sippymodem_v34_grid_point
		mapper_output[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];
	/* U0(m), including the scheduled 4D inversion. */
	uint8_t trellis_output;
};

struct sippymodem_v34_decoded_4d_interval {
	struct sippymodem_v34_4d_mapping_input mapping;
	uint8_t ring_index[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];
	/* Z(m), retained as compact decision evidence. */
	uint8_t differential_output;
};

enum sippymodem_v34_4d_decoder_result sippymodem_v34_4d_decoder_init(
	struct sippymodem_v34_4d_decoder *decoder,
	const struct sippymodem_v34_mapping_parameters *parameters);

bool sippymodem_v34_4d_decoder_is_valid(
	const struct sippymodem_v34_4d_decoder *decoder);

/* Recover one 4D mapper input from exact decided mapper outputs and U0.
 * Equalization, precoder reversal, and trellis survivor selection precede
 * this boundary. State and output are unchanged on rejection. */
enum sippymodem_v34_4d_decoder_result sippymodem_v34_decode_4d_interval(
	struct sippymodem_v34_4d_decoder *decoder,
	const struct sippymodem_v34_decided_4d_interval *decided,
	struct sippymodem_v34_decoded_4d_interval *decoded);

const char *sippymodem_v34_4d_decoder_result_name(
	enum sippymodem_v34_4d_decoder_result result);

#endif
