// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V32_TRELLIS_H
#define SIPPYMODEM_V32_TRELLIS_H

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V32_TRELLIS_MIN_DATA_BITS = 3,
	SIPPYMODEM_V32_TRELLIS_MAX_DATA_BITS = 6,
	SIPPYMODEM_V32_TRELLIS_STATE_COUNT = 8,
	SIPPYMODEM_V32_TRELLIS_QUADRANT_COUNT = 4,
};

enum sippymodem_v32_trellis_result {
	SIPPYMODEM_V32_TRELLIS_OK,
	SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT,
};

struct sippymodem_v32_trellis_encoder {
	uint8_t trellis_state;
	uint8_t differential_quadrant;
	uint8_t carrier_phase_quadrant;
	uint8_t data_bits_per_symbol;
};

enum sippymodem_v32_trellis_result sippymodem_v32_trellis_encoder_init(
	struct sippymodem_v32_trellis_encoder *encoder,
	uint8_t data_bits_per_symbol, uint8_t initial_carrier_phase_quadrant);
bool sippymodem_v32_trellis_encoder_is_valid(
	const struct sippymodem_v32_trellis_encoder *encoder);

/*
 * Map one already-scrambled data group to a signal-point label.  V.32bis
 * adds one coded bit, so an n-bit input produces a label below 2^(n + 1).
 */
enum sippymodem_v32_trellis_result sippymodem_v32_trellis_encode(
	struct sippymodem_v32_trellis_encoder *encoder, uint8_t data,
	uint8_t *signal_symbol);

const char *sippymodem_v32_trellis_result_name(
	enum sippymodem_v32_trellis_result result);

#endif
