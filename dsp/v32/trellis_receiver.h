// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V32_TRELLIS_RECEIVER_H
#define SIPPYMODEM_V32_TRELLIS_RECEIVER_H

#include "framing.h"
#include "qam_equalizer.h"
#include "trellis.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V32_TRELLIS_RECEIVER_DELAY =
		SIPPYMODEM_QAM_EQUALIZER_DELAY +
		SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS,
};

enum sippymodem_v32_trellis_receiver_result {
	SIPPYMODEM_V32_TRELLIS_RECEIVER_OK,
	SIPPYMODEM_V32_TRELLIS_RECEIVER_INVALID_ARGUMENT,
};

/* Own the channel, trellis, and framing state after the 4800-bit/s startup
 * exchange hands the receive stream to a V.32bis data rate. */
struct sippymodem_v32_trellis_receiver {
	struct sippymodem_qam_equalizer equalizer;
	struct sippymodem_v32_trellis_encoder known_encoder;
	struct sippymodem_v32_trellis_decoder decoder;
	struct sippymodem_v32_scrambler known_scrambler;
	struct sippymodem_v32_scrambler descrambler;
	struct sippymodem_qam_point known_targets[
		SIPPYMODEM_QAM_EQUALIZER_DELAY + 1];
	uint8_t known_inputs[SIPPYMODEM_V32_TRELLIS_RECEIVER_DELAY];
	uint8_t known_input_fill;
	uint8_t payload_symbols;
	bool target_known[SIPPYMODEM_QAM_EQUALIZER_DELAY + 1];
	bool payload_started;
};

/* Seed the channel estimate and framing state from the final 4800-bit/s
 * receive point immediately before the rate transition. */
enum sippymodem_v32_trellis_receiver_result
sippymodem_v32_trellis_receiver_init(
	struct sippymodem_v32_trellis_receiver *receiver,
	const struct sippymodem_v32_symbol_decoder *framing,
	const struct sippymodem_qam_point *reference_point,
	uint8_t data_bits_per_symbol);
bool sippymodem_v32_trellis_receiver_is_valid(
	const struct sippymodem_v32_trellis_receiver *receiver);

/* Consume a known post-E training group. confirmed reports whether the
 * delayed decoded group agrees with its corresponding known input. */
enum sippymodem_v32_trellis_receiver_result
sippymodem_v32_trellis_receiver_track_known(
	struct sippymodem_v32_trellis_receiver *receiver,
	const struct sippymodem_qam_point *point, uint8_t input,
	bool *confirmed);

/* Consume a payload point. ready remains false while the equalizer and
 * trellis traceback drain the final training symbols. */
enum sippymodem_v32_trellis_receiver_result
sippymodem_v32_trellis_receiver_decode(
	struct sippymodem_v32_trellis_receiver *receiver,
	const struct sippymodem_qam_point *point, uint8_t *output, bool *ready);

const char *sippymodem_v32_trellis_receiver_result_name(
	enum sippymodem_v32_trellis_receiver_result result);

#endif
