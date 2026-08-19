// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_DECISION_H
#define V32_DECISION_H

#include "equalizer.h"

#include <stdbool.h>
#include <stdint.h>

enum v32_decision_result {
	V32_DECISION_OK,
	V32_DECISION_INVALID_ARGUMENT,
};

/* The 4800-bit/s and training signal sets use the normalized constellation's
 * canonical quadrant numbering, not the private decision-table ordering. */
struct v32_quadrant_decision {
	uint8_t quadrant;
	struct v32_equalizer_decision equalizer;
};

struct v32_uncoded_decision {
	uint8_t symbol;
	struct v32_equalizer_decision equalizer;
};

enum v32_decision_result v32_decide_known_quadrant(
	const struct v32_equalizer_observation *observation,
	uint8_t quadrant, bool adapt_on_error,
	struct v32_quadrant_decision *decision);

enum v32_decision_result v32_decide_4_point(
	const struct v32_equalizer_observation *observation,
	bool adapt_on_error, struct v32_quadrant_decision *decision);

enum v32_decision_result v32_decide_uncoded_16(
	const struct v32_equalizer_observation *observation,
	bool adapt_on_error, struct v32_uncoded_decision *decision);

#endif
