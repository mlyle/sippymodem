// SPDX-License-Identifier: BSD-3-Clause

#include "trellis.h"

#include <stddef.h>

/* Rows select the new high dibit; columns select the previous quadrant. */
static const uint8_t differential_quadrant[4][4] = {
	{ 0, 1, 2, 3 },
	{ 1, 0, 3, 2 },
	{ 2, 3, 1, 0 },
	{ 3, 2, 0, 1 },
};

/* Rows select the old convolutional state; columns select the new quadrant. */
static const uint8_t next_trellis_state[8][4] = {
	{ 0, 6, 2, 4 },
	{ 2, 4, 0, 6 },
	{ 4, 2, 6, 0 },
	{ 6, 0, 4, 2 },
	{ 1, 5, 7, 3 },
	{ 3, 7, 5, 1 },
	{ 7, 3, 1, 5 },
	{ 5, 1, 3, 7 },
};

/*
 * The first index is the three-bit trellis subset.  The second applies the
 * carrier phase at the current 2400-baud instant without disturbing the
 * uncoded low bits of the signal label.
 */
static const uint8_t rotated_subset[8][4] = {
	{ 0, 7, 1, 6 },
	{ 1, 6, 0, 7 },
	{ 2, 4, 3, 5 },
	{ 3, 5, 2, 4 },
	{ 4, 3, 5, 2 },
	{ 5, 2, 4, 3 },
	{ 6, 0, 7, 1 },
	{ 7, 1, 6, 0 },
};

static bool data_bits_are_valid(uint8_t data_bits_per_symbol)
{
	return data_bits_per_symbol >= SIPPYMODEM_V32_TRELLIS_MIN_DATA_BITS &&
	       data_bits_per_symbol <= SIPPYMODEM_V32_TRELLIS_MAX_DATA_BITS;
}

bool sippymodem_v32_trellis_encoder_is_valid(
	const struct sippymodem_v32_trellis_encoder *encoder)
{
	return encoder != NULL &&
	       encoder->trellis_state < SIPPYMODEM_V32_TRELLIS_STATE_COUNT &&
	       encoder->differential_quadrant <
		       SIPPYMODEM_V32_TRELLIS_QUADRANT_COUNT &&
	       encoder->carrier_phase_quadrant <
		       SIPPYMODEM_V32_TRELLIS_QUADRANT_COUNT &&
	       data_bits_are_valid(encoder->data_bits_per_symbol);
}

enum sippymodem_v32_trellis_result sippymodem_v32_trellis_encoder_init(
	struct sippymodem_v32_trellis_encoder *encoder,
	uint8_t data_bits_per_symbol, uint8_t initial_carrier_phase_quadrant)
{
	struct sippymodem_v32_trellis_encoder initialized = {
		.carrier_phase_quadrant = initial_carrier_phase_quadrant,
		.data_bits_per_symbol = data_bits_per_symbol,
	};

	if (encoder == NULL || !data_bits_are_valid(data_bits_per_symbol) ||
	    initial_carrier_phase_quadrant >=
		    SIPPYMODEM_V32_TRELLIS_QUADRANT_COUNT)
		return SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT;
	*encoder = initialized;
	return SIPPYMODEM_V32_TRELLIS_OK;
}

enum sippymodem_v32_trellis_result sippymodem_v32_trellis_encode(
	struct sippymodem_v32_trellis_encoder *encoder, uint8_t data,
	uint8_t *signal_symbol)
{
	struct sippymodem_v32_trellis_encoder next;
	uint8_t high_dibit;
	uint8_t low_bit_count;
	uint8_t low_mask;
	uint8_t next_quadrant;
	uint8_t subset;

	if (!sippymodem_v32_trellis_encoder_is_valid(encoder) ||
	    signal_symbol == NULL ||
	    data >= (UINT8_C(1) << encoder->data_bits_per_symbol))
		return SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT;

	next = *encoder;
	low_bit_count = (uint8_t)(next.data_bits_per_symbol - 2u);
	low_mask = (uint8_t)((UINT8_C(1) << low_bit_count) - 1u);
	high_dibit = (uint8_t)(data >> low_bit_count);
	next_quadrant = differential_quadrant[high_dibit]
					       [next.differential_quadrant];
	subset = (uint8_t)(next_quadrant |
			   (next.trellis_state >= 4 ? UINT8_C(4) : 0));
	next.carrier_phase_quadrant =
		(uint8_t)((next.carrier_phase_quadrant + 3u) & 3u);
	subset = rotated_subset[subset][next.carrier_phase_quadrant];
	next.trellis_state =
		next_trellis_state[next.trellis_state][next_quadrant];
	next.differential_quadrant = next_quadrant;

	*encoder = next;
	*signal_symbol =
		(uint8_t)((subset << low_bit_count) | (data & low_mask));
	return SIPPYMODEM_V32_TRELLIS_OK;
}

const char *sippymodem_v32_trellis_result_name(
	enum sippymodem_v32_trellis_result result)
{
	switch (result) {
	case SIPPYMODEM_V32_TRELLIS_OK:
		return "ok";
	case SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
