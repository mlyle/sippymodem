// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_TRELLIS_H
#define V32_TRELLIS_H

#include <stdbool.h>
#include <stdint.h>

struct qam_point;

enum {
	V32_TRELLIS_MIN_DATA_BITS = 3,
	V32_TRELLIS_MAX_DATA_BITS = 6,
	V32_TRELLIS_STATE_COUNT = 8,
	V32_TRELLIS_QUADRANT_COUNT = 4,
	V32_7200_DATA_BITS = 3,
	V32_9600_TRELLIS_DATA_BITS = 4,
	V32_12000_DATA_BITS = 5,
	V32_14400_DATA_BITS = 6,
	V32_TRELLIS_TRACEBACK_SYMBOLS = 24,
};

enum v32_trellis_result {
	V32_TRELLIS_OK,
	V32_TRELLIS_INVALID_ARGUMENT,
};

struct v32_trellis_encoder {
	uint8_t trellis_state;
	uint8_t differential_output;
	uint8_t data_bits_per_symbol;
};

struct v32_trellis_path_step {
	uint8_t predecessor[V32_TRELLIS_STATE_COUNT];
	uint8_t differential_symbol[V32_TRELLIS_STATE_COUNT];
};

struct v32_trellis_decoder {
	double path_metric[V32_TRELLIS_STATE_COUNT];
	struct v32_trellis_path_step
		path[V32_TRELLIS_TRACEBACK_SYMBOLS];
	uint8_t path_index;
	uint8_t path_symbols;
	uint8_t previous_differential_output;
	uint8_t data_bits_per_symbol;
};

enum v32_trellis_result v32_trellis_encoder_init(
	struct v32_trellis_encoder *encoder,
	uint8_t data_bits_per_symbol, uint8_t previous_differential_output);
bool v32_trellis_encoder_is_valid(
	const struct v32_trellis_encoder *encoder);

/*
 * Map one already-scrambled data group to a signal-point label.  Coded V.32
 * and V.32bis modes add one bit, so an n-bit input produces a label below
 * 2^(n + 1).
 */
enum v32_trellis_result v32_trellis_encode(
	struct v32_trellis_encoder *encoder, uint8_t data,
	uint8_t *signal_symbol);

enum v32_trellis_result v32_trellis_decoder_init(
	struct v32_trellis_decoder *decoder,
	uint8_t data_bits_per_symbol, uint8_t previous_differential_output);
bool v32_trellis_decoder_is_valid(
	const struct v32_trellis_decoder *decoder);
/*
 * Decode one normalized baseband point.  Once ready is true, scrambled_data
 * contains the next configured-width group in transmission order.  Traceback
 * adds a fixed delay while retaining every input group.  decided_signal_symbol
 * always identifies the best survivor's constellation point for the current
 * input, so a channel tracker need not wait for traceback.
 */
enum v32_trellis_result v32_trellis_decode(
	struct v32_trellis_decoder *decoder,
	const struct qam_point *point, uint8_t *scrambled_data,
	bool *ready, uint8_t *decided_signal_symbol);

const char *v32_trellis_result_name(
	enum v32_trellis_result result);

#endif
