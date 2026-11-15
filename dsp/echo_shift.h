// SPDX-License-Identifier: BSD-3-Clause

#ifndef ECHO_SHIFT_H
#define ECHO_SHIFT_H

#include "echo_timing.h"

/* Owned costs for two disjoint selection/validation halves. Accept a shift
 * only when it improves both halves and wins again in the next full window.
 * The caller configures points, accumulates finite nonnegative costs, and
 * advances once per observation. Configuration and ratio stay fixed across
 * each pair of windows; reset the owner at a change of signal coordinates. */
struct echo_shift_window {
	double score[2][ECHO_TIMING_BINS];
	unsigned points, position;
	int previous_bin;
	bool previous_qualified;
};

static inline unsigned echo_shift_part(const struct echo_shift_window *window)
{
	return window->position >= window->points / 2u;
}

/* Complete one owned selection/validation window, then clear its costs. */
int echo_shift_finish(struct echo_shift_window *window, double ratio);

static inline int echo_shift_advance(struct echo_shift_window *window, double ratio)
{
	return ++window->position == window->points ? echo_shift_finish(window, ratio) : 0;
}

#endif
