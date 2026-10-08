// SPDX-License-Identifier: BSD-3-Clause

#ifndef V22BIS_SYMBOLS_H
#define V22BIS_SYMBOLS_H

#include "qam.h"
#include "v22_framing.h"

#include <stdint.h>

enum {
	V22BIS_BITS_PER_SYMBOL = 4,
	V22BIS_SYMBOL_COUNT = 16,
};

/* Input may overlap the encoder or output. Output must not overlap its
 * encoder/decoder; rejected calls preserve both owner and output. */
enum v22_framing_result v22bis_symbol_encode(
	struct v22_symbol_encoder *encoder,
	const uint8_t bits[V22BIS_BITS_PER_SYMBOL],
	uint8_t *symbol);

enum v22_framing_result v22bis_symbol_decode(
	struct v22_symbol_decoder *decoder, uint8_t symbol,
	uint8_t bits[V22BIS_BITS_PER_SYMBOL]);

enum v22_framing_result v22bis_constellation_point(
	uint8_t symbol, struct qam_point *point);
enum v22_framing_result v22bis_1200_constellation_point(
	uint8_t quadrant, struct qam_point *point);

#endif /* V22BIS_SYMBOLS_H */
