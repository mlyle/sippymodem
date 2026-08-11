// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V22BIS_SYMBOLS_H
#define SIPPYMODEM_V22BIS_SYMBOLS_H

#include "sippymodem_qam.h"
#include "v22_framing.h"

#include <stdint.h>

enum {
	SIPPYMODEM_V22BIS_BITS_PER_SYMBOL = 4,
	SIPPYMODEM_V22BIS_SYMBOL_COUNT = 16,
};

struct sippymodem_v22bis_symbol_encoder {
	struct sippymodem_v22_scrambler scrambler;
	uint8_t phase_quadrant;
};

struct sippymodem_v22bis_symbol_decoder {
	struct sippymodem_v22_scrambler descrambler;
	uint8_t phase_quadrant;
};

enum sippymodem_v22_framing_result sippymodem_v22bis_symbol_encoder_init(
	struct sippymodem_v22bis_symbol_encoder *encoder,
	uint8_t initial_quadrant);
enum sippymodem_v22_framing_result sippymodem_v22bis_symbol_encode(
	struct sippymodem_v22bis_symbol_encoder *encoder,
	const uint8_t bits[SIPPYMODEM_V22BIS_BITS_PER_SYMBOL],
	uint8_t *symbol);

enum sippymodem_v22_framing_result sippymodem_v22bis_symbol_decoder_init(
	struct sippymodem_v22bis_symbol_decoder *decoder,
	uint8_t initial_quadrant);
enum sippymodem_v22_framing_result sippymodem_v22bis_symbol_decode(
	struct sippymodem_v22bis_symbol_decoder *decoder, uint8_t symbol,
	uint8_t bits[SIPPYMODEM_V22BIS_BITS_PER_SYMBOL]);

enum sippymodem_v22_framing_result sippymodem_v22bis_constellation_point(
	uint8_t symbol, struct sippymodem_qam_point *point);

#endif /* SIPPYMODEM_V22BIS_SYMBOLS_H */
