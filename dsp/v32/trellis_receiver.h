// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_TRELLIS_RECEIVER_H
#define V32_TRELLIS_RECEIVER_H

#include "framing.h"
#include "qam_equalizer.h"
#include "trellis.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V32_TRELLIS_RECEIVER_DELAY =
		QAM_EQUALIZER_DELAY +
		V32_TRELLIS_TRACEBACK_SYMBOLS,
};

enum v32_trellis_receiver_result {
	V32_TRELLIS_RECEIVER_OK,
	V32_TRELLIS_RECEIVER_INVALID_ARGUMENT,
};

/* Own the channel, trellis, and framing state after the 4800-bit/s startup
 * exchange hands the receive stream to a V.32bis data rate. */
struct v32_trellis_receiver {
	struct qam_equalizer equalizer;
	struct v32_trellis_encoder known_encoder;
	struct v32_trellis_decoder decoder;
	struct v32_scrambler known_scrambler;
	struct v32_scrambler descrambler;
	struct qam_point known_targets[
		QAM_EQUALIZER_DELAY + 1];
	uint8_t known_inputs[V32_TRELLIS_RECEIVER_DELAY];
	uint8_t known_input_fill;
	uint8_t payload_symbols;
	bool target_known[QAM_EQUALIZER_DELAY + 1];
	bool payload_started;
};

/* Seed the channel estimate and framing state from the final 4800-bit/s
 * receive point immediately before the rate transition, under one possible
 * differential start state. */
enum v32_trellis_receiver_result
v32_trellis_receiver_init(
	struct v32_trellis_receiver *receiver,
	const struct v32_symbol_decoder *framing,
	const struct qam_point *reference_point,
	uint8_t reference_phase_quadrant,
	uint8_t previous_differential_output,
	uint8_t data_bits_per_symbol);
bool v32_trellis_receiver_is_valid(
	const struct v32_trellis_receiver *receiver);

/* Consume a known post-E training group. confirmed reports whether the
 * delayed decoded group agrees with its corresponding known input. */
enum v32_trellis_receiver_result
v32_trellis_receiver_track_known(
	struct v32_trellis_receiver *receiver,
	const struct qam_point *point, uint8_t input,
	bool *confirmed);

/* Consume a payload point. ready remains false while the equalizer and
 * trellis traceback drain the final training symbols. */
enum v32_trellis_receiver_result
v32_trellis_receiver_decode(
	struct v32_trellis_receiver *receiver,
	const struct qam_point *point, uint8_t *output, bool *ready);

const char *v32_trellis_receiver_result_name(
	enum v32_trellis_receiver_result result);

#endif
