// SPDX-License-Identifier: BSD-3-Clause

#include "echo_shift.h"

#include <assert.h>
#include <stdio.h>

/* Different signal halves independently favor or reject a candidate. */
static int observe(struct echo_shift_window *window, int candidate,
	bool validation_agrees)
{
	unsigned part = echo_shift_part(window);
	for (unsigned i = 0; i < ECHO_TIMING_BINS; i++)
		window->score[part][i] += i == (unsigned)(candidate + ECHO_TIMING_RADIUS) &&
			(part == 0 || validation_agrees) ? 1.0 : 4.0;
	return echo_shift_advance(window, .5);
}

static int full_window(struct echo_shift_window *window, int candidate,
	bool validation_agrees)
{
	unsigned points = window->points;
	for (unsigned i = 1; i < points; i++)
		assert(observe(window, candidate, validation_agrees) == 0);
	return observe(window, candidate, validation_agrees);
}

int main(void)
{
	for (int delta = -ECHO_TIMING_RADIUS; delta <= ECHO_TIMING_RADIUS; delta++) {
		struct echo_shift_window window = { .points = 8 };
		assert(full_window(&window, delta, true) == 0);
		assert(full_window(&window, delta, true) == delta);
		assert(window.points == 8 && window.position == 0);
	}
	struct echo_shift_window window = { .points = 8 };
	assert(full_window(&window, 97, true) == 0);
	assert(full_window(&window, 96, true) == 0);
	assert(full_window(&window, 96, false) == 0);
	assert(full_window(&window, 96, true) == 0);
	assert(full_window(&window, 96, true) == 96);

	/* One useful half, stale qualification, silence and ties cannot shift. */
	assert(full_window(&window, -97, false) == 0);
	assert(full_window(&window, -97, true) == 0);
	for (unsigned i = 0; i < window.points; i++)
		assert(echo_shift_advance(&window, .5) == 0);
	assert(full_window(&window, -97, true) == 0);
	assert(full_window(&window, -97, true) == -97);
	puts("echo shift selection and independent validation tests passed");
	return 0;
}
