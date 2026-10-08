// SPDX-License-Identifier: BSD-3-Clause

#ifndef V22_FRAMING_H
#define V22_FRAMING_H

#include "differential_qpsk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V22_SCRAMBLER_BITS = 17,
	V22_SCRAMBLER_HISTORY_MASK = (UINT32_C(1) << V22_SCRAMBLER_BITS) - 1,
	V22_SCRAMBLER_ONES_LIMIT = 64,
	/* V.22bis sends 100 ms at 600 symbols/s; report a stable 20 ms prefix. */
	V22BIS_RATE_SIGNAL_SYMBOLS = 60,
	V22BIS_RATE_SIGNAL_MIN_SYMBOLS = 12,
	/* A finite marker can lose one differential observation at its edges. */
	V22BIS_RATE_SIGNAL_COMPLETE_SYMBOLS =
		V22BIS_RATE_SIGNAL_SYMBOLS - 1,
	/* Retraining can retain a slightly longer alternating candidate. */
	V22BIS_ALTERNATING_RUN_MAX_SYMBOLS = 64,
};

enum v22_framing_result {
	V22_FRAMING_OK,
	V22_FRAMING_INVALID_ARGUMENT,
};

struct v22_scrambler {
	uint32_t history;
	uint8_t consecutive_ones;
};

/* V.22 and V.22bis share scrambling and differential-quadrant state. */
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

static inline bool v22_symbol_encoder_is_valid(
	const struct v22_symbol_encoder *encoder)
{
	return encoder != NULL &&
	       encoder->phase_quadrant < QPSK_QUADRANT_COUNT &&
	       v22_scrambler_is_valid(&encoder->scrambler);
}

static inline bool v22_symbol_decoder_is_valid(
	const struct v22_symbol_decoder *decoder)
{
	return decoder != NULL &&
	       decoder->phase_quadrant < QPSK_QUADRANT_COUNT &&
	       v22_scrambler_is_valid(&decoder->descrambler);
}

/* Output must not overlap the scrambler. Rejected calls preserve both. */
/* Disable repair during V.22 handshaking; V.22bis keeps it enabled.
 * A disabled detector clears its count while preserving polynomial history. */
enum v22_framing_result v22_scramble_bit(
	struct v22_scrambler *scrambler, uint8_t input,
	uint8_t *output, bool repair);
enum v22_framing_result v22_descramble_bit(
	struct v22_scrambler *descrambler, uint8_t input,
	uint8_t *output, bool repair);

enum v22_framing_result v22_symbol_encoder_init(
	struct v22_symbol_encoder *encoder,
	uint8_t initial_quadrant);
/* Symbol outputs must not overlap their encoder or decoder.
 * Rejected calls preserve owner and outputs. */
enum v22_framing_result v22_symbol_encode(
	struct v22_symbol_encoder *encoder, uint8_t first_bit,
	uint8_t second_bit, uint8_t *quadrant, bool repair);
enum v22_framing_result v22_600_symbol_encode(
	struct v22_symbol_encoder *encoder, uint8_t bit,
	uint8_t *quadrant, bool repair);
enum v22_framing_result v22_symbol_decoder_init(
	struct v22_symbol_decoder *decoder,
	uint8_t initial_quadrant);
/* Write two binary bytes in transmit order. */
enum v22_framing_result v22_symbol_decode(
	struct v22_symbol_decoder *decoder, uint8_t quadrant,
	uint8_t bits[2], bool repair);
enum v22_framing_result v22_600_symbol_decode(
	struct v22_symbol_decoder *decoder, uint8_t quadrant,
	uint8_t *bit, bool repair);

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

#endif /* V22_FRAMING_H */
