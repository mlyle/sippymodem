// SPDX-License-Identifier: BSD-3-Clause

#ifndef FRAME_CLOCK_H
#define FRAME_CLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum frame_clock_result {
	FRAME_CLOCK_OK,
	FRAME_CLOCK_NOT_DUE,
	FRAME_CLOCK_NOT_RUNNING,
	FRAME_CLOCK_INVALID_ARGUMENT,
	FRAME_CLOCK_INEXACT_INTERVAL,
	FRAME_CLOCK_OVERRUN,
};

struct frame_clock_stats {
	uint64_t frames_due;
	uint64_t overruns;
};

struct frame_clock {
	uint64_t interval_ns;
	uint64_t next_deadline_ns;
	size_t maximum_catchup_frames;
	bool running;
	struct frame_clock_stats stats;
};

/* The frame duration must be exactly representable in integer nanoseconds. */
enum frame_clock_result frame_clock_init(
	struct frame_clock *clock,
	size_t frame_samples,
	unsigned int sample_rate,
	size_t maximum_catchup_frames);
void frame_clock_start(
	struct frame_clock *clock, uint64_t now_ns);
void frame_clock_stop(struct frame_clock *clock);

/* On OK, frames_due is between one and maximum_catchup_frames and the
 * deadline advances transactionally. Backlogs larger than that bound remain
 * due for later calls; only an unrepresentable deadline stops the clock. */
enum frame_clock_result frame_clock_take_due(
	struct frame_clock *clock,
	uint64_t now_ns,
	size_t *frames_due);
uint64_t frame_clock_wait_ns(
	const struct frame_clock *clock, uint64_t now_ns);

const char *frame_clock_result_name(
	enum frame_clock_result result);

#endif
