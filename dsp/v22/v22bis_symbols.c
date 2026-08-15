// SPDX-License-Identifier: BSD-3-Clause

#include "v22bis_symbols.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const double V22BIS_COORDINATE_SCALE =
	0.23570226039551584146694812070162; /* 1 / sqrt(18) */
static const double V22BIS_1200_POINT_SCALE =
	1.3416407864998738178455042012388; /* sqrt(18 / 10) */

static bool bits_are_valid(
	const uint8_t bits[V22BIS_BITS_PER_SYMBOL])
{
	if (bits == NULL)
		return false;
	for (size_t index = 0; index < V22BIS_BITS_PER_SYMBOL;
	     ++index)
		if (bits[index] > 1)
			return false;
	return true;
}

static bool encoder_is_valid(
	const struct v22bis_symbol_encoder *encoder)
{
	return encoder != NULL &&
	       encoder->phase_quadrant < QPSK_QUADRANT_COUNT &&
	       v22_scrambler_is_valid(&encoder->scrambler);
}

static bool decoder_is_valid(
	const struct v22bis_symbol_decoder *decoder)
{
	return decoder != NULL &&
	       decoder->phase_quadrant < QPSK_QUADRANT_COUNT &&
	       v22_scrambler_is_valid(&decoder->descrambler);
}

enum v22_framing_result v22bis_symbol_encoder_init(
	struct v22bis_symbol_encoder *encoder,
	uint8_t initial_quadrant)
{
	if (encoder == NULL ||
	    initial_quadrant >= QPSK_QUADRANT_COUNT)
		return V22_FRAMING_INVALID_ARGUMENT;
	memset(encoder, 0, sizeof(*encoder));
	encoder->phase_quadrant = initial_quadrant;
	return V22_FRAMING_OK;
}

enum v22_framing_result v22bis_symbol_encode(
	struct v22bis_symbol_encoder *encoder,
	const uint8_t bits[V22BIS_BITS_PER_SYMBOL],
	uint8_t *symbol)
{
	struct v22bis_symbol_encoder next;
	uint8_t scrambled[V22BIS_BITS_PER_SYMBOL];
	uint8_t quadrant;

	if (!encoder_is_valid(encoder) || !bits_are_valid(bits) ||
	    symbol == NULL)
		return V22_FRAMING_INVALID_ARGUMENT;
	next = *encoder;
	for (size_t index = 0; index < V22BIS_BITS_PER_SYMBOL;
	     ++index)
		if (v22_scramble_bit(
			    &next.scrambler, bits[index], &scrambled[index]) !=
		    V22_FRAMING_OK)
			return V22_FRAMING_INVALID_ARGUMENT;
	if (differential_qpsk_encode(
		    next.phase_quadrant,
		    (uint8_t)((scrambled[0] << 1) | scrambled[1]),
		    &quadrant) != DIFFERENTIAL_QPSK_OK)
		return V22_FRAMING_INVALID_ARGUMENT;
	next.phase_quadrant = quadrant;
	*encoder = next;
	*symbol = (uint8_t)((quadrant << 2) | (scrambled[2] << 1) |
			    scrambled[3]);
	return V22_FRAMING_OK;
}

enum v22_framing_result v22bis_symbol_decoder_init(
	struct v22bis_symbol_decoder *decoder,
	uint8_t initial_quadrant)
{
	if (decoder == NULL ||
	    initial_quadrant >= QPSK_QUADRANT_COUNT)
		return V22_FRAMING_INVALID_ARGUMENT;
	memset(decoder, 0, sizeof(*decoder));
	decoder->phase_quadrant = initial_quadrant;
	return V22_FRAMING_OK;
}

enum v22_framing_result v22bis_symbol_decode(
	struct v22bis_symbol_decoder *decoder, uint8_t symbol,
	uint8_t bits[V22BIS_BITS_PER_SYMBOL])
{
	struct v22bis_symbol_decoder next;
	uint8_t decoded[V22BIS_BITS_PER_SYMBOL];
	uint8_t scrambled[V22BIS_BITS_PER_SYMBOL];
	uint8_t quadrant = (uint8_t)(symbol >> 2);
	uint8_t dibit;

	if (!decoder_is_valid(decoder) ||
	    symbol >= V22BIS_SYMBOL_COUNT || bits == NULL)
		return V22_FRAMING_INVALID_ARGUMENT;
	next = *decoder;
	if (differential_qpsk_decode(
		    next.phase_quadrant, quadrant, &dibit) !=
	    DIFFERENTIAL_QPSK_OK)
		return V22_FRAMING_INVALID_ARGUMENT;
	scrambled[0] = (uint8_t)(dibit >> 1);
	scrambled[1] = (uint8_t)(dibit & 1u);
	scrambled[2] = (uint8_t)((symbol >> 1) & 1u);
	scrambled[3] = (uint8_t)(symbol & 1u);
	for (size_t index = 0; index < V22BIS_BITS_PER_SYMBOL;
	     ++index)
		if (v22_descramble_bit(
			    &next.descrambler, scrambled[index],
			    &decoded[index]) != V22_FRAMING_OK)
			return V22_FRAMING_INVALID_ARGUMENT;
	next.phase_quadrant = quadrant;
	*decoder = next;
	memcpy(bits, decoded, sizeof(decoded));
	return V22_FRAMING_OK;
}

enum v22_framing_result v22bis_constellation_point(
	uint8_t symbol, struct qam_point *point)
{
	double in_phase;
	double quadrature;
	double base_in_phase;
	double base_quadrature;

	if (symbol >= V22BIS_SYMBOL_COUNT || point == NULL)
		return V22_FRAMING_INVALID_ARGUMENT;
	base_in_phase = (symbol & 1u) != 0 ? 3.0 : 1.0;
	base_quadrature = (symbol & 2u) != 0 ? 3.0 : 1.0;
	switch (symbol >> 2) {
	case 0:
		in_phase = base_in_phase;
		quadrature = base_quadrature;
		break;
	case 1:
		in_phase = -base_quadrature;
		quadrature = base_in_phase;
		break;
	case 2:
		in_phase = -base_in_phase;
		quadrature = -base_quadrature;
		break;
	default:
		in_phase = base_quadrature;
		quadrature = -base_in_phase;
		break;
	}
	*point = (struct qam_point){
		.in_phase = in_phase * V22BIS_COORDINATE_SCALE,
		.quadrature = quadrature * V22BIS_COORDINATE_SCALE,
	};
	return V22_FRAMING_OK;
}

enum v22_framing_result v22bis_1200_constellation_point(
	uint8_t quadrant, struct qam_point *point)
{
	struct qam_point next;

	if (quadrant >= QPSK_QUADRANT_COUNT || point == NULL ||
	    v22bis_constellation_point(
		    (uint8_t)((quadrant << 2) | 1u), &next) !=
		    V22_FRAMING_OK)
		return V22_FRAMING_INVALID_ARGUMENT;
	/* The 1200-bit/s output gain keeps the selected 01 ray at unit power. */
	next.in_phase *= V22BIS_1200_POINT_SCALE;
	next.quadrature *= V22BIS_1200_POINT_SCALE;
	if (!point_is_finite(&next))
		return V22_FRAMING_INVALID_ARGUMENT;
	*point = next;
	return V22_FRAMING_OK;
}
