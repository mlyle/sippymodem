// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_SURVIVOR_H
#define V34_SURVIVOR_H

#include "v34_branches.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_MAX_TRELLIS_STATES = 64,
	V34_SURVIVOR_TRACEBACK_INTERVALS = 32,
};

enum v34_survivor_result {
	V34_SURVIVOR_OK,
	V34_SURVIVOR_INVALID_ARGUMENT,
	V34_SURVIVOR_NUMERIC_FAILURE,
};

struct v34_survivor_decision {
	struct v34_grid_point
		channel_output[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	/* Y0(m), before the modulo-encoder and scheduled inversion terms. */
	uint8_t convolutional_output;
	/* Scheduled V0(m), retained across the traceback delay. */
	uint8_t inversion_bit;
};

struct v34_survivor_traceback_step {
	uint8_t predecessor[V34_MAX_TRELLIS_STATES];
	uint8_t branch_index[V34_MAX_TRELLIS_STATES];
	struct v34_survivor_decision
		branch[V34_MAX_BRANCH_COUNT];
};

struct v34_survivor {
	double path_metric[V34_MAX_TRELLIS_STATES];
	struct v34_survivor_traceback_step
		path[V34_SURVIVOR_TRACEBACK_INTERVALS];
	enum v34_trellis_selection selection;
	uint8_t path_index;
	uint8_t path_intervals;
};

bool v34_survivor_decision_is_valid(
	const struct v34_survivor_decision *decision);

/* Start the selected trellis path at the all-zero convolutional state required
 * at the beginning of B1. The destination is unchanged when the selection is
 * invalid. */
enum v34_survivor_result v34_survivor_init(
	struct v34_survivor *survivor,
	enum v34_trellis_selection selection);

bool v34_survivor_is_valid(
	const struct v34_survivor *survivor);

/* Advance one 4-D interval. `current` always receives the best present-time
 * decision for decision-directed tracking. `traceback` is unchanged until
 * `traceback_ready` becomes true on the 32nd interval; thereafter it receives
 * exactly one decision in transmission order with a fixed 32-interval delay.
 * Equal metrics prefer the smaller destination for the current decision and
 * the smaller predecessor, then branch index, for each destination. State and
 * all outputs are unchanged on failure. */
enum v34_survivor_result v34_survivor_advance(
	struct v34_survivor *survivor,
	const struct v34_branch_set *branches,
	struct v34_survivor_decision *current,
	struct v34_survivor_decision *traceback,
	bool *traceback_ready);

#endif /* V34_SURVIVOR_H */
