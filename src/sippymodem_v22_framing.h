// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V22_FRAMING_H
#define SIPPYMODEM_V22_FRAMING_H

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V22_SCRAMBLER_BITS = 17,
	SIPPYMODEM_V22_SCRAMBLER_ONES_LIMIT = 64,
	SIPPYMODEM_V22_PHASE_QUADRANTS = 4,
};

enum sippymodem_v22_framing_result {
	SIPPYMODEM_V22_FRAMING_OK,
	SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT,
};

struct sippymodem_v22_scrambler {
	uint32_t history;
	uint8_t consecutive_ones;
};

struct sippymodem_v22_symbol_encoder {
	struct sippymodem_v22_scrambler scrambler;
	uint8_t phase_quadrant;
};

struct sippymodem_v22_symbol_decoder {
	struct sippymodem_v22_scrambler descrambler;
	uint8_t phase_quadrant;
};

void sippymodem_v22_scrambler_reset(
	struct sippymodem_v22_scrambler *scrambler);
bool sippymodem_v22_scrambler_is_valid(
	const struct sippymodem_v22_scrambler *scrambler);
enum sippymodem_v22_framing_result sippymodem_v22_scramble_bit(
	struct sippymodem_v22_scrambler *scrambler, uint8_t input,
	uint8_t *output);
enum sippymodem_v22_framing_result sippymodem_v22_descramble_bit(
	struct sippymodem_v22_scrambler *descrambler, uint8_t input,
	uint8_t *output);

enum sippymodem_v22_framing_result sippymodem_v22_differential_encode(
	uint8_t previous_quadrant, uint8_t dibit, uint8_t *quadrant);
enum sippymodem_v22_framing_result sippymodem_v22_differential_decode(
	uint8_t previous_quadrant, uint8_t quadrant, uint8_t *dibit);

enum sippymodem_v22_framing_result sippymodem_v22_symbol_encoder_init(
	struct sippymodem_v22_symbol_encoder *encoder,
	uint8_t initial_quadrant);
enum sippymodem_v22_framing_result sippymodem_v22_symbol_encode(
	struct sippymodem_v22_symbol_encoder *encoder, uint8_t first_bit,
	uint8_t second_bit, uint8_t *quadrant);
enum sippymodem_v22_framing_result sippymodem_v22_symbol_decoder_init(
	struct sippymodem_v22_symbol_decoder *decoder,
	uint8_t initial_quadrant);
enum sippymodem_v22_framing_result sippymodem_v22_symbol_decode(
	struct sippymodem_v22_symbol_decoder *decoder, uint8_t quadrant,
	uint8_t *first_bit, uint8_t *second_bit);

const char *sippymodem_v22_framing_result_name(
	enum sippymodem_v22_framing_result result);

#endif
