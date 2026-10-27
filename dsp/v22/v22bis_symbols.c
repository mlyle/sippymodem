// SPDX-License-Identifier: BSD-3-Clause

#include "v22bis_symbols.h"

#include "memory_region.h"
#include "packed_bits.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const double V22BIS_COORDINATE_SCALE =
	0.23570226039551584146694812070162; /* 1 / sqrt(18) */

enum v22_framing_result v22bis_symbol_encode(
	struct v22_symbol_encoder *encoder,
	const uint8_t bits[V22BIS_BITS_PER_SYMBOL],
	uint8_t *symbol)
{
	struct v22_symbol_encoder next;
	uint8_t scrambled[V22BIS_BITS_PER_SYMBOL];
	uint8_t quadrant;

	if (!v22_symbol_encoder_is_valid(encoder) ||
	    !unpacked_bits_are_binary(bits, V22BIS_BITS_PER_SYMBOL) ||
	    symbol == NULL ||
	    memory_regions_overlap(encoder, sizeof(*encoder), symbol, sizeof(*symbol)))
		return V22_FRAMING_INVALID_ARGUMENT;
	/* Binary input preserves valid state. The copy keeps overlapping caller
	 * bits unchanged while the scrambler advances. */
	next = *encoder;
	for (size_t index = 0; index < V22BIS_BITS_PER_SYMBOL;
	     ++index)
		(void)v22_scramble_bit(
			&next.scrambler, bits[index], &scrambled[index], true);
	(void)differential_qpsk_encode(
		next.phase_quadrant,
		(uint8_t)((scrambled[0] << 1) | scrambled[1]), &quadrant);
	next.phase_quadrant = quadrant;
	*encoder = next;
	*symbol = (uint8_t)((quadrant << 2) | (scrambled[2] << 1) |
			    scrambled[3]);
	return V22_FRAMING_OK;
}

enum v22_framing_result v22bis_symbol_decode(
	struct v22_symbol_decoder *decoder, uint8_t symbol,
	uint8_t bits[V22BIS_BITS_PER_SYMBOL])
{
	uint8_t decoded[V22BIS_BITS_PER_SYMBOL];
	uint8_t scrambled[V22BIS_BITS_PER_SYMBOL];
	uint8_t quadrant = (uint8_t)(symbol >> 2);
	uint8_t dibit;

	if (!v22_symbol_decoder_is_valid(decoder) ||
	    symbol >= V22BIS_SYMBOL_COUNT || bits == NULL ||
	    memory_regions_overlap(decoder, sizeof(*decoder), bits, V22BIS_BITS_PER_SYMBOL))
		return V22_FRAMING_INVALID_ARGUMENT;
	/* Validated quadrants and binary descrambler steps cannot fail. */
	(void)differential_qpsk_decode(
		decoder->phase_quadrant, quadrant, &dibit);
	scrambled[0] = (uint8_t)(dibit >> 1);
	scrambled[1] = (uint8_t)(dibit & 1u);
	scrambled[2] = (uint8_t)((symbol >> 1) & 1u);
	scrambled[3] = (uint8_t)(symbol & 1u);
	for (size_t index = 0; index < V22BIS_BITS_PER_SYMBOL;
	     ++index)
		(void)v22_descramble_bit(
			&decoder->descrambler, scrambled[index], &decoded[index], true);
	decoder->phase_quadrant = quadrant;
	memcpy(bits, decoded, sizeof(decoded));
	return V22_FRAMING_OK;
}

enum v22_framing_result v22bis_constellation_point(
	uint8_t symbol, struct qam_point *point)
{
	if (symbol >= V22BIS_SYMBOL_COUNT || point == NULL)
		return V22_FRAMING_INVALID_ARGUMENT;
	struct qam_point base = {
		(symbol & 1u) != 0 ? 3.0 : 1.0,
		(symbol & 2u) != 0 ? 3.0 : 1.0,
	};
	struct qam_point rotated = qam_point_rotate_quarters(base, symbol >> 2);

	*point = (struct qam_point){
		.in_phase = rotated.in_phase * V22BIS_COORDINATE_SCALE,
		.quadrature = rotated.quadrature * V22BIS_COORDINATE_SCALE,
	};
	return V22_FRAMING_OK;
}

enum v22_framing_result v22bis_1200_constellation_point(
	uint8_t quadrant, struct qam_point *point)
{
	if (quadrant >= QPSK_QUADRANT_COUNT)
		return V22_FRAMING_INVALID_ARGUMENT;
	/* V.22bis 2.5.2.2 selects the unscaled Figure 2 element labelled 01. */
	return v22bis_constellation_point(
		(uint8_t)((quadrant << 2) | 1u), point);
}
