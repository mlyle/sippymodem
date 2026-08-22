// SPDX-License-Identifier: BSD-3-Clause

#include "trellis.h"

#include <stddef.h>

/* Rows select the new high dibit; columns select the previous quadrant. */
static const uint8_t differential_output[4][4] = {
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

static bool data_bits_are_valid(uint8_t data_bits_per_symbol)
{
	return data_bits_per_symbol >= V32_TRELLIS_MIN_DATA_BITS &&
	       data_bits_per_symbol <= V32_TRELLIS_MAX_DATA_BITS;
}

bool v32_trellis_encoder_is_valid(
	const struct v32_trellis_encoder *encoder)
{
	return encoder != NULL &&
	       encoder->trellis_state < V32_TRELLIS_STATE_COUNT &&
	       encoder->differential_output <
		       V32_TRELLIS_QUADRANT_COUNT &&
	       data_bits_are_valid(encoder->data_bits_per_symbol);
}

enum v32_trellis_result v32_trellis_encoder_init(
	struct v32_trellis_encoder *encoder,
	uint8_t data_bits_per_symbol, uint8_t previous_differential_output)
{
	struct v32_trellis_encoder initialized = {
		.differential_output = previous_differential_output,
		.data_bits_per_symbol = data_bits_per_symbol,
	};

	if (encoder == NULL || !data_bits_are_valid(data_bits_per_symbol) ||
	    previous_differential_output >=
		    V32_TRELLIS_QUADRANT_COUNT)
		return V32_TRELLIS_INVALID_ARGUMENT;
	*encoder = initialized;
	return V32_TRELLIS_OK;
}

enum v32_trellis_result v32_trellis_encode(
	struct v32_trellis_encoder *encoder, uint8_t data,
	uint8_t *signal_symbol)
{
	struct v32_trellis_encoder next;
	uint8_t high_dibit;
	uint8_t low_bit_count;
	uint8_t low_mask;
	uint8_t next_quadrant;
	uint8_t subset;

	if (!v32_trellis_encoder_is_valid(encoder) ||
	    signal_symbol == NULL ||
	    data >= (UINT8_C(1) << encoder->data_bits_per_symbol))
		return V32_TRELLIS_INVALID_ARGUMENT;

	next = *encoder;
	low_bit_count = (uint8_t)(next.data_bits_per_symbol - 2u);
	low_mask = (uint8_t)((UINT8_C(1) << low_bit_count) - 1u);
	high_dibit = (uint8_t)(data >> low_bit_count);
	next_quadrant = differential_output[high_dibit]
					     [next.differential_output];
	subset = (uint8_t)(next_quadrant |
			   (next.trellis_state >= 4 ? UINT8_C(4) : 0));
	next.trellis_state =
		next_trellis_state[next.trellis_state][next_quadrant];
	next.differential_output = next_quadrant;

	*encoder = next;
	*signal_symbol =
		(uint8_t)((subset << low_bit_count) | (data & low_mask));
	return V32_TRELLIS_OK;
}
