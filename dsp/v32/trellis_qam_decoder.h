// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_TRELLIS_QAM_DECODER_H
#define V32_TRELLIS_QAM_DECODER_H

#include "trellis.h"

#include <stdbool.h>
#include <stdint.h>

struct qam_point;

enum {
	V32_TRELLIS_QAM_TRACEBACK_SYMBOLS = 24,
};

struct v32_trellis_qam_path_step {
	uint8_t predecessor[V32_TRELLIS_STATE_COUNT];
	uint8_t differential_symbol[V32_TRELLIS_STATE_COUNT];
};

struct v32_trellis_qam_decoder {
	double path_metric[V32_TRELLIS_STATE_COUNT];
	struct v32_trellis_qam_path_step
		path[V32_TRELLIS_QAM_TRACEBACK_SYMBOLS];
	uint8_t path_index;
	uint8_t path_symbols;
	uint8_t previous_differential_output;
	uint8_t data_bits_per_symbol;
};

enum v32_trellis_result v32_trellis_qam_decoder_init(
	struct v32_trellis_qam_decoder *decoder,
	uint8_t data_bits_per_symbol, uint8_t previous_differential_output);
bool v32_trellis_qam_decoder_is_valid(
	const struct v32_trellis_qam_decoder *decoder);

/*
 * Decode one normalized baseband point. Once ready is true, scrambled_data
 * contains the next configured-width group in transmission order. Traceback
 * adds a fixed delay while retaining every input group. decided_signal_symbol
 * always identifies the best survivor's constellation point for the current
 * input, so the compatibility channel tracker need not wait for traceback.
 */
enum v32_trellis_result v32_trellis_qam_decode(
	struct v32_trellis_qam_decoder *decoder,
	const struct qam_point *point, uint8_t *scrambled_data,
	bool *ready, uint8_t *decided_signal_symbol);

#endif
