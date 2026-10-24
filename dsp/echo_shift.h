// SPDX-License-Identifier: BSD-3-Clause

#ifndef ECHO_SHIFT_H
#define ECHO_SHIFT_H

#include "echo_timing.h"

#include <string.h>

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

static int echo_shift_finish(struct echo_shift_window *window, double ratio)
{
	unsigned best = ECHO_TIMING_RADIUS;
	for (unsigned i = 0; i < ECHO_TIMING_BINS; ++i)
		if (window->score[0][i] < window->score[0][best])
			best = i;
	bool qualified = best != ECHO_TIMING_RADIUS &&
		window->score[0][best] <= ratio * window->score[0][ECHO_TIMING_RADIUS] &&
		window->score[1][best] <= ratio * window->score[1][ECHO_TIMING_RADIUS];
	int delta = qualified && window->previous_qualified &&
		(int)best == window->previous_bin ? (int)best - ECHO_TIMING_RADIUS : 0;

	window->previous_qualified = qualified;
	window->previous_bin = (int)best;
	window->position = 0;
	memset(window->score, 0, sizeof(window->score));
	return delta;
}

static inline int echo_shift_advance(struct echo_shift_window *window, double ratio)
{
	return ++window->position == window->points ? echo_shift_finish(window, ratio) : 0;
}

#endif
