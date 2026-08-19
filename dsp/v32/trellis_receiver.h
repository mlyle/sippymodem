// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_TRELLIS_RECEIVER_H
#define V32_TRELLIS_RECEIVER_H

#include "framing.h"
#include "trellis_decoder.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V32_TRELLIS_RECEIVER_B1_SYMBOLS = 128,
};

enum v32_trellis_receiver_result {
	V32_TRELLIS_RECEIVER_OK,
	V32_TRELLIS_RECEIVER_INVALID_ARGUMENT,
	V32_TRELLIS_RECEIVER_INVALID_STATE,
};

/* Own the selected-rate trellis and continuous descrambler state across B1
 * and the delayed B1-to-data boundary. */
struct v32_trellis_receiver {
	struct v32_trellis_decoder decoder;
	struct v32_scrambler descrambler;
	uint8_t b1_observations;
	uint8_t b1_flush_symbols;
	bool b1_matches;
};

struct v32_trellis_receiver_symbol {
	uint8_t group;
	bool ready;
};

/* Selected-rate differential state starts at zero.  Only the continuously
 * clocked descrambler crosses rate/E. */
enum v32_trellis_receiver_result v32_trellis_receiver_init(
	struct v32_trellis_receiver *receiver,
	const struct v32_scrambler *continuous_descrambler,
	uint8_t data_bits_per_symbol);
bool v32_trellis_receiver_is_valid(
	const struct v32_trellis_receiver *receiver);

/* Consume exactly 128 physical B1 observations.  Traceback output is
 * descrambled and compared with the all-one B1 group as it becomes ready. */
enum v32_trellis_receiver_result v32_trellis_receiver_track_b1(
	struct v32_trellis_receiver *receiver,
	const struct v32_equalizer_observation *observation);

/* Consume payload observations after all 128 B1 observations.  The first
 * 16 calls drain delayed B1 groups; call 17 returns the first payload group. */
enum v32_trellis_receiver_result v32_trellis_receiver_decode(
	struct v32_trellis_receiver *receiver,
	const struct v32_equalizer_observation *observation,
	struct v32_trellis_receiver_symbol *symbol);

/* Becomes true after all 16 delayed B1 groups have also been checked. */
bool v32_trellis_receiver_b1_is_confirmed(
	const struct v32_trellis_receiver *receiver);

#endif
