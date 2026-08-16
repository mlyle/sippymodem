// SPDX-License-Identifier: BSD-3-Clause

#ifndef V22_FRAMING_H
#define V22_FRAMING_H

#include "differential_qpsk.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V22_SCRAMBLER_BITS = 17,
	V22_SCRAMBLER_ONES_LIMIT = 64,
	/* V.22bis sends 100 ms at 600 symbols/s; report a stable 20 ms prefix. */
	V22BIS_RATE_SIGNAL_SYMBOLS = 60,
	V22BIS_RATE_SIGNAL_MIN_SYMBOLS = 12,
};

enum v22_framing_result {
	V22_FRAMING_OK,
	V22_FRAMING_INVALID_ARGUMENT,
};

struct v22_scrambler {
	uint32_t history;
	uint8_t consecutive_ones;
};

struct v22_symbol_encoder {
	struct v22_scrambler scrambler;
	uint8_t phase_quadrant;
};

struct v22_symbol_decoder {
	struct v22_scrambler descrambler;
	uint8_t phase_quadrant;
};

enum v22bis_rate_signal_event {
	V22BIS_RATE_SIGNAL_NONE,
	V22BIS_RATE_SIGNAL_STARTED,
	V22BIS_RATE_SIGNAL_ENDED,
	V22BIS_RATE_SIGNAL_FULLY_ENDED,
};

struct v22bis_rate_signal_detector {
	uint8_t alternating_symbols;
	uint8_t previous_turn;
	bool present;
};

void v22_scrambler_reset(
	struct v22_scrambler *scrambler);
bool v22_scrambler_is_valid(
	const struct v22_scrambler *scrambler);
enum v22_framing_result v22_scramble_bit(
	struct v22_scrambler *scrambler, uint8_t input,
	uint8_t *output);
enum v22_framing_result v22_descramble_bit(
	struct v22_scrambler *descrambler, uint8_t input,
	uint8_t *output);

enum v22_framing_result v22_symbol_encoder_init(
	struct v22_symbol_encoder *encoder,
	uint8_t initial_quadrant);
enum v22_framing_result v22_symbol_encode(
	struct v22_symbol_encoder *encoder, uint8_t first_bit,
	uint8_t second_bit, uint8_t *quadrant);
enum v22_framing_result v22_600_symbol_encode(
	struct v22_symbol_encoder *encoder, uint8_t bit,
	uint8_t *quadrant);
enum v22_framing_result v22_symbol_decoder_init(
	struct v22_symbol_decoder *decoder,
	uint8_t initial_quadrant);
enum v22_framing_result v22_symbol_decode(
	struct v22_symbol_decoder *decoder, uint8_t quadrant,
	uint8_t *first_bit, uint8_t *second_bit);
enum v22_framing_result v22_600_symbol_decode(
	struct v22_symbol_decoder *decoder, uint8_t quadrant,
	uint8_t *bit);

enum v22_framing_result v22bis_rate_signal_turn(
	uint32_t symbol_index, uint8_t *quarter_turns);
void v22bis_rate_signal_detector_reset(
	struct v22bis_rate_signal_detector *detector);
bool v22bis_rate_signal_detector_is_valid(
	const struct v22bis_rate_signal_detector *detector);
enum v22_framing_result v22bis_rate_signal_observe(
	struct v22bis_rate_signal_detector *detector,
	uint8_t quarter_turns,
	enum v22bis_rate_signal_event *event);

const char *v22_framing_result_name(
	enum v22_framing_result result);

#endif /* V22_FRAMING_H */
