// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_UNCODED_RECEIVER_H
#define V32_UNCODED_RECEIVER_H

#include "decision.h"
#include "framing.h"

#include <stdbool.h>
#include <stdint.h>

enum v32_uncoded_receiver_result {
	V32_UNCODED_RECEIVER_OK,
	V32_UNCODED_RECEIVER_INVALID_ARGUMENT,
	V32_UNCODED_RECEIVER_INVALID_STATE,
};

struct v32_uncoded_receiver {
	struct v32_symbol_decoder decoder;
};

struct v32_uncoded_symbol {
	struct v32_equalizer_decision equalizer;
	uint8_t group;
};

/* The phase owner supplies both differential phase and descrambler history.
 * Entering uncoded 9600 resets phase at the B1 boundary, not here. */
enum v32_uncoded_receiver_result v32_uncoded_receiver_init(
	struct v32_uncoded_receiver *receiver,
	const struct v32_symbol_decoder *seed);
bool v32_uncoded_receiver_is_valid(
	const struct v32_uncoded_receiver *receiver);

/* Decode one four-point (2-bit) or uncoded 16-point (4-bit) observation.
 * The complete observation is echoed for commit to that same equalizer owner.
 * Rejected calls leave the receiver and output unchanged. */
enum v32_uncoded_receiver_result v32_uncoded_receiver_observe(
	struct v32_uncoded_receiver *receiver, uint8_t bits_per_symbol,
	const struct v32_equalizer_observation *observation,
	bool adapt_on_error, struct v32_uncoded_symbol *symbol);

#endif
