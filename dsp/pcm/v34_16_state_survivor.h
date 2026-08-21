// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_16_STATE_SURVIVOR_H
#define V34_16_STATE_SURVIVOR_H

#include "v34_survivor.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_16_STATE_COUNT = 16,
	V34_16_STATE_BRANCH_COUNT = 8,
	V34_16_STATE_TRACEBACK_INTERVALS = 32,
};

enum v34_16_state_survivor_result {
	V34_16_STATE_SURVIVOR_OK,
	V34_16_STATE_SURVIVOR_INVALID_ARGUMENT,
	V34_16_STATE_SURVIVOR_INVALID_STATE,
};

/* Branch order is the fixed 16-state decoder order. The high branch-index bit
 * is Y0; the lower two bits identify the selected 4-D lattice subset. */
struct v34_16_state_branch {
	struct v34_survivor_decision decision;
	uint16_t metric;
};

struct v34_16_state_traceback_step {
	struct v34_survivor_decision decision[V34_16_STATE_COUNT];
	uint8_t predecessor[V34_16_STATE_COUNT];
};

struct v34_16_state_survivor {
	uint16_t score[V34_16_STATE_COUNT];
	struct v34_16_state_traceback_step
		path[V34_16_STATE_TRACEBACK_INTERVALS];
	uint8_t path_index;
	uint8_t path_intervals;
};

/* All states begin with equal cost. B1 resolves the actual encoder state while
 * preserving the normative fixed-metric tie order. */
enum v34_16_state_survivor_result v34_16_state_survivor_init(
	struct v34_16_state_survivor *survivor);

bool v34_16_state_survivor_is_valid(
	const struct v34_16_state_survivor *survivor);

/* Advance one 4-D interval. Scores add and normalize with uint16_t wrapping.
 * `current` receives the present-time decision. `traceback` remains unchanged
 * until the 32nd interval, then receives one decision in transmission order.
 * State and every output are unchanged on rejection. */
enum v34_16_state_survivor_result v34_16_state_survivor_advance(
	struct v34_16_state_survivor *survivor,
	const struct v34_16_state_branch
		branches[V34_16_STATE_BRANCH_COUNT],
	struct v34_survivor_decision *current,
	struct v34_survivor_decision *traceback,
	bool *traceback_ready);

#endif /* V34_16_STATE_SURVIVOR_H */
