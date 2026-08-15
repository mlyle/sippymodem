// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_FRAMING_H
#define V32_FRAMING_H

#include "differential_qpsk.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V32_SCRAMBLER_BITS = 23,
	V32_4800_BITS_PER_SYMBOL = 2,
	V32_9600_BITS_PER_SYMBOL = 4,
	V32_9600_SYMBOL_COUNT = 16,
};

/* The role identifies the transmitter whose polynomial is in use. */
enum v32_transmitter_role {
	V32_CALLING_TRANSMITTER,
	V32_ANSWERING_TRANSMITTER,
};

enum v32_framing_result {
	V32_FRAMING_OK,
	V32_FRAMING_INVALID_ARGUMENT,
};

struct v32_scrambler {
	uint32_t history;
	enum v32_transmitter_role transmitter_role;
};

struct v32_symbol_encoder {
	struct v32_scrambler scrambler;
	uint8_t phase_quadrant;
};

struct v32_symbol_decoder {
	struct v32_scrambler descrambler;
	uint8_t phase_quadrant;
};

enum v32_framing_result v32_scrambler_init(
	struct v32_scrambler *scrambler,
	enum v32_transmitter_role transmitter_role);
bool v32_scrambler_is_valid(
	const struct v32_scrambler *scrambler);

/* Process between one and eight bits, most-significant bit first. */
enum v32_framing_result v32_scramble_group(
	struct v32_scrambler *scrambler, uint8_t input,
	unsigned int bit_count, uint8_t *output);
enum v32_framing_result v32_descramble_group(
	struct v32_scrambler *descrambler, uint8_t input,
	unsigned int bit_count, uint8_t *output);

/* Bit 1 of each dibit is transmitted first. */
enum v32_framing_result v32_scramble_dibit(
	struct v32_scrambler *scrambler, uint8_t input,
	uint8_t *output);
enum v32_framing_result v32_descramble_dibit(
	struct v32_scrambler *descrambler, uint8_t input,
	uint8_t *output);

enum v32_framing_result v32_symbol_encoder_init(
	struct v32_symbol_encoder *encoder,
	enum v32_transmitter_role transmitter_role,
	uint8_t initial_quadrant);
/*
 * TRN carries scrambled dibits as absolute phases.  The resulting phase is
 * retained so that a following differentially encoded symbol is continuous.
 */
enum v32_framing_result v32_trn_symbol_encode(
	struct v32_symbol_encoder *encoder, uint8_t input,
	uint8_t *quadrant);
/*
 * Data symbols are supplied in transmission order, most-significant bit
 * first.
 */
enum v32_framing_result v32_4800_symbol_encode(
	struct v32_symbol_encoder *encoder, uint8_t input,
	uint8_t *quadrant);
enum v32_framing_result v32_9600_symbol_encode(
	struct v32_symbol_encoder *encoder, uint8_t input,
	uint8_t *symbol);
enum v32_framing_result v32_symbol_decoder_init(
	struct v32_symbol_decoder *decoder,
	enum v32_transmitter_role source_role,
	uint8_t initial_quadrant);
enum v32_framing_result v32_trn_symbol_decode(
	struct v32_symbol_decoder *decoder, uint8_t quadrant,
	uint8_t *output);
enum v32_framing_result v32_4800_symbol_decode(
	struct v32_symbol_decoder *decoder, uint8_t quadrant,
	uint8_t *output);
enum v32_framing_result v32_9600_symbol_decode(
	struct v32_symbol_decoder *decoder, uint8_t symbol,
	uint8_t *output);

const char *v32_framing_result_name(
	enum v32_framing_result result);

#endif
