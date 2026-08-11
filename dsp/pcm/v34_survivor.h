// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_SURVIVOR_H
#define SIPPYMODEM_V34_SURVIVOR_H

#include "v34_branches.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_MAX_TRELLIS_STATES = 64,
	SIPPYMODEM_V34_SURVIVOR_TRACEBACK_INTERVALS = 32,
};

enum sippymodem_v34_survivor_result {
	SIPPYMODEM_V34_SURVIVOR_OK,
	SIPPYMODEM_V34_SURVIVOR_INVALID_ARGUMENT,
	SIPPYMODEM_V34_SURVIVOR_NUMERIC_FAILURE,
};

struct sippymodem_v34_survivor_decision {
	struct sippymodem_v34_grid_point
		channel_output[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];
	/* Y0(m), before the modulo-encoder and scheduled inversion terms. */
	uint8_t convolutional_output;
	/* Scheduled V0(m), retained across the traceback delay. */
	uint8_t inversion_bit;
};

struct sippymodem_v34_survivor_traceback_step {
	uint8_t predecessor[SIPPYMODEM_V34_MAX_TRELLIS_STATES];
	uint8_t branch_index[SIPPYMODEM_V34_MAX_TRELLIS_STATES];
	struct sippymodem_v34_survivor_decision
		branch[SIPPYMODEM_V34_MAX_BRANCH_COUNT];
};

struct sippymodem_v34_survivor {
	double path_metric[SIPPYMODEM_V34_MAX_TRELLIS_STATES];
	struct sippymodem_v34_survivor_traceback_step
		path[SIPPYMODEM_V34_SURVIVOR_TRACEBACK_INTERVALS];
	enum sippymodem_v34_trellis_selection selection;
	uint8_t path_index;
	uint8_t path_intervals;
};

/* Start at the all-zero convolutional state required at the beginning of B1.
 * The destination is unchanged when the selection is invalid. */
enum sippymodem_v34_survivor_result sippymodem_v34_survivor_init(
	struct sippymodem_v34_survivor *survivor,
	enum sippymodem_v34_trellis_selection selection);

bool sippymodem_v34_survivor_is_valid(
	const struct sippymodem_v34_survivor *survivor);

/* Advance one 4-D interval. `current` always receives the best present-time
 * decision for decision-directed tracking. `traceback` is unchanged until
 * `traceback_ready` becomes true on the 32nd interval; thereafter it receives
 * exactly one decision in transmission order with a fixed 32-interval delay.
 * Equal metrics prefer the smaller destination for the current decision and
 * the smaller predecessor, then branch index, for each destination. State and
 * all outputs are unchanged on failure. */
enum sippymodem_v34_survivor_result sippymodem_v34_survivor_advance(
	struct sippymodem_v34_survivor *survivor,
	const struct sippymodem_v34_branch_set *branches,
	struct sippymodem_v34_survivor_decision *current,
	struct sippymodem_v34_survivor_decision *traceback,
	bool *traceback_ready);

const char *sippymodem_v34_survivor_result_name(
	enum sippymodem_v34_survivor_result result);

#endif /* SIPPYMODEM_V34_SURVIVOR_H */
