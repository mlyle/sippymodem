// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_FOUR_POINT_RECEIVER_H
#define V32_FOUR_POINT_RECEIVER_H

#include "decision.h"
#include "framing.h"

#include <stdbool.h>
#include <stdint.h>

enum v32_four_point_receiver_result {
	V32_FOUR_POINT_RECEIVER_OK,
	V32_FOUR_POINT_RECEIVER_INVALID_ARGUMENT,
	V32_FOUR_POINT_RECEIVER_INVALID_STATE,
};

/* Four-point rate/E and 4800-bit/s framing share one continuous absolute
 * phase and descrambler state. */
struct v32_four_point_receiver {
	struct v32_symbol_decoder decoder;
};

struct v32_four_point_symbol {
	struct v32_equalizer_decision equalizer;
	uint8_t dibit;
};

/* The seed is the post-conditioning absolute phase and descrambler state. */
enum v32_four_point_receiver_result v32_four_point_receiver_init(
	struct v32_four_point_receiver *receiver,
	const struct v32_symbol_decoder *seed);
bool v32_four_point_receiver_is_valid(
	const struct v32_four_point_receiver *receiver);

/* Decode one pending equalizer observation. The complete observation is
 * echoed for commit to that same equalizer owner. Rejected calls leave the
 * receiver and output unchanged. */
enum v32_four_point_receiver_result v32_four_point_receiver_observe(
	struct v32_four_point_receiver *receiver,
	const struct v32_equalizer_observation *observation,
	bool adapt_on_error, struct v32_four_point_symbol *symbol);

#endif
