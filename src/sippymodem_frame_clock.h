// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_FRAME_CLOCK_H
#define SIPPYMODEM_FRAME_CLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum sippymodem_frame_clock_result {
	SIPPYMODEM_FRAME_CLOCK_OK,
	SIPPYMODEM_FRAME_CLOCK_NOT_DUE,
	SIPPYMODEM_FRAME_CLOCK_NOT_RUNNING,
	SIPPYMODEM_FRAME_CLOCK_INVALID_ARGUMENT,
	SIPPYMODEM_FRAME_CLOCK_INEXACT_INTERVAL,
	SIPPYMODEM_FRAME_CLOCK_OVERRUN,
};

struct sippymodem_frame_clock_stats {
	uint64_t frames_due;
	uint64_t overruns;
};

struct sippymodem_frame_clock {
	uint64_t interval_ns;
	uint64_t next_deadline_ns;
	size_t maximum_catchup_frames;
	bool running;
	struct sippymodem_frame_clock_stats stats;
};

/* The frame duration must be exactly representable in integer nanoseconds. */
enum sippymodem_frame_clock_result sippymodem_frame_clock_init(
	struct sippymodem_frame_clock *clock,
	size_t frame_samples,
	unsigned int sample_rate,
	size_t maximum_catchup_frames);
void sippymodem_frame_clock_start(
	struct sippymodem_frame_clock *clock, uint64_t now_ns);
void sippymodem_frame_clock_stop(struct sippymodem_frame_clock *clock);

/* On OK, frames_due is between one and maximum_catchup_frames and the
 * deadline advances transactionally. An overrun stops the clock. */
enum sippymodem_frame_clock_result sippymodem_frame_clock_take_due(
	struct sippymodem_frame_clock *clock,
	uint64_t now_ns,
	size_t *frames_due);
uint64_t sippymodem_frame_clock_wait_ns(
	const struct sippymodem_frame_clock *clock, uint64_t now_ns);

const char *sippymodem_frame_clock_result_name(
	enum sippymodem_frame_clock_result result);

#endif
