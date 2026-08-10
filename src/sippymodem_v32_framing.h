// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V32_FRAMING_H
#define SIPPYMODEM_V32_FRAMING_H

#include "sippymodem_differential_qpsk.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V32_SCRAMBLER_BITS = 23,
};

/* The role identifies the transmitter whose polynomial is in use. */
enum sippymodem_v32_transmitter_role {
	SIPPYMODEM_V32_CALLING_TRANSMITTER,
	SIPPYMODEM_V32_ANSWERING_TRANSMITTER,
};

enum sippymodem_v32_framing_result {
	SIPPYMODEM_V32_FRAMING_OK,
	SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT,
};

struct sippymodem_v32_scrambler {
	uint32_t history;
	enum sippymodem_v32_transmitter_role transmitter_role;
};

struct sippymodem_v32_symbol_encoder {
	struct sippymodem_v32_scrambler scrambler;
	uint8_t phase_quadrant;
};

struct sippymodem_v32_symbol_decoder {
	struct sippymodem_v32_scrambler descrambler;
	uint8_t phase_quadrant;
};

enum sippymodem_v32_framing_result sippymodem_v32_scrambler_init(
	struct sippymodem_v32_scrambler *scrambler,
	enum sippymodem_v32_transmitter_role transmitter_role);
bool sippymodem_v32_scrambler_is_valid(
	const struct sippymodem_v32_scrambler *scrambler);

/* Bit 1 of each dibit is transmitted first. */
enum sippymodem_v32_framing_result sippymodem_v32_scramble_dibit(
	struct sippymodem_v32_scrambler *scrambler, uint8_t input,
	uint8_t *output);
enum sippymodem_v32_framing_result sippymodem_v32_descramble_dibit(
	struct sippymodem_v32_scrambler *descrambler, uint8_t input,
	uint8_t *output);

enum sippymodem_v32_framing_result sippymodem_v32_symbol_encoder_init(
	struct sippymodem_v32_symbol_encoder *encoder,
	enum sippymodem_v32_transmitter_role transmitter_role,
	uint8_t initial_quadrant);
enum sippymodem_v32_framing_result sippymodem_v32_symbol_encode(
	struct sippymodem_v32_symbol_encoder *encoder, uint8_t input,
	uint8_t *quadrant);
enum sippymodem_v32_framing_result sippymodem_v32_symbol_decoder_init(
	struct sippymodem_v32_symbol_decoder *decoder,
	enum sippymodem_v32_transmitter_role source_role,
	uint8_t initial_quadrant);
enum sippymodem_v32_framing_result sippymodem_v32_symbol_decode(
	struct sippymodem_v32_symbol_decoder *decoder, uint8_t quadrant,
	uint8_t *output);

const char *sippymodem_v32_framing_result_name(
	enum sippymodem_v32_framing_result result);

#endif
