// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V32_TRELLIS_H
#define SIPPYMODEM_V32_TRELLIS_H

#include <stdbool.h>
#include <stdint.h>

struct sippymodem_qam_point;

enum {
	SIPPYMODEM_V32_TRELLIS_MIN_DATA_BITS = 3,
	SIPPYMODEM_V32_TRELLIS_MAX_DATA_BITS = 6,
	SIPPYMODEM_V32_TRELLIS_STATE_COUNT = 8,
	SIPPYMODEM_V32_TRELLIS_QUADRANT_COUNT = 4,
	SIPPYMODEM_V32_7200_DATA_BITS = 3,
	SIPPYMODEM_V32_9600_TRELLIS_DATA_BITS = 4,
	SIPPYMODEM_V32_12000_DATA_BITS = 5,
	SIPPYMODEM_V32_14400_DATA_BITS = 6,
	SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS = 24,
};

enum sippymodem_v32_trellis_result {
	SIPPYMODEM_V32_TRELLIS_OK,
	SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT,
};

struct sippymodem_v32_trellis_encoder {
	uint8_t trellis_state;
	uint8_t differential_output;
	uint8_t data_bits_per_symbol;
};

struct sippymodem_v32_trellis_path_step {
	uint8_t predecessor[SIPPYMODEM_V32_TRELLIS_STATE_COUNT];
	uint8_t differential_symbol[SIPPYMODEM_V32_TRELLIS_STATE_COUNT];
};

struct sippymodem_v32_trellis_decoder {
	double path_metric[SIPPYMODEM_V32_TRELLIS_STATE_COUNT];
	struct sippymodem_v32_trellis_path_step
		path[SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS];
	uint8_t path_index;
	uint8_t path_symbols;
	uint8_t previous_differential_output;
	uint8_t data_bits_per_symbol;
};

enum sippymodem_v32_trellis_result sippymodem_v32_trellis_encoder_init(
	struct sippymodem_v32_trellis_encoder *encoder,
	uint8_t data_bits_per_symbol, uint8_t previous_differential_output);
bool sippymodem_v32_trellis_encoder_is_valid(
	const struct sippymodem_v32_trellis_encoder *encoder);

/*
 * Map one already-scrambled data group to a signal-point label.  V.32bis
 * adds one coded bit, so an n-bit input produces a label below 2^(n + 1).
 */
enum sippymodem_v32_trellis_result sippymodem_v32_trellis_encode(
	struct sippymodem_v32_trellis_encoder *encoder, uint8_t data,
	uint8_t *signal_symbol);

enum sippymodem_v32_trellis_result sippymodem_v32_trellis_decoder_init(
	struct sippymodem_v32_trellis_decoder *decoder,
	uint8_t data_bits_per_symbol, uint8_t previous_differential_output);
bool sippymodem_v32_trellis_decoder_is_valid(
	const struct sippymodem_v32_trellis_decoder *decoder);
/*
 * Decode one normalized baseband point.  Once ready is true, scrambled_data
 * contains the next configured-width group in transmission order.  Traceback
 * adds a fixed delay while retaining every input group.
 */
enum sippymodem_v32_trellis_result sippymodem_v32_trellis_decode(
	struct sippymodem_v32_trellis_decoder *decoder,
	const struct sippymodem_qam_point *point, uint8_t *scrambled_data,
	bool *ready);

const char *sippymodem_v32_trellis_result_name(
	enum sippymodem_v32_trellis_result result);

#endif
