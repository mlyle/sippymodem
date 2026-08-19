// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_TRELLIS_DECODER_H
#define V32_TRELLIS_DECODER_H

#include "equalizer.h"
#include "trellis.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS = 16,
};

enum v32_trellis_decoder_result {
	V32_TRELLIS_DECODER_OK,
	V32_TRELLIS_DECODER_INVALID_ARGUMENT,
	V32_TRELLIS_DECODER_INVALID_STATE,
};

struct v32_trellis_decoder_path {
	uint8_t signal_symbol;
	uint8_t predecessor;
};

struct v32_trellis_decoder {
	int16_t path_metrics[V32_TRELLIS_STATE_COUNT];
	struct v32_trellis_decoder_path
		paths[V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS]
		     [V32_TRELLIS_STATE_COUNT];
	uint8_t active_slot;
	uint8_t history_fill;
	uint8_t previous_differential_output;
	uint8_t data_bits_per_symbol;
};

struct v32_trellis_decoded_symbol {
	uint8_t scrambled_group;
	bool ready;
};

enum v32_trellis_decoder_result v32_trellis_decoder_init(
	struct v32_trellis_decoder *decoder,
	uint8_t data_bits_per_symbol,
	uint8_t previous_differential_output);
bool v32_trellis_decoder_is_valid(
	const struct v32_trellis_decoder *decoder);

/*
 * Advance one fixed equalizer observation.  The first 16 observations fill
 * the traceback and report ready=false; the seventeenth publishes the first
 * decoded group.  Rejected calls leave the owner and output unchanged.
 */
enum v32_trellis_decoder_result v32_trellis_decoder_decode(
	struct v32_trellis_decoder *decoder,
	const struct v32_equalizer_observation *observation,
	struct v32_trellis_decoded_symbol *symbol);

#endif
