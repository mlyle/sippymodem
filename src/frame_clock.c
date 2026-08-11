// SPDX-License-Identifier: BSD-3-Clause

#include "frame_clock.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	NANOSECONDS_PER_SECOND = 1000000000,
};

static void add_saturated(uint64_t *value, uint64_t increment)
{
	*value = increment > UINT64_MAX - *value ? UINT64_MAX :
						    *value + increment;
}

enum sippymodem_frame_clock_result sippymodem_frame_clock_init(
	struct sippymodem_frame_clock *clock, size_t frame_samples,
	unsigned int sample_rate, size_t maximum_catchup_frames)
{
	struct sippymodem_frame_clock initialized;
	uint64_t duration_numerator;

	if (clock == NULL || frame_samples == 0 || sample_rate == 0 ||
	    maximum_catchup_frames == 0 ||
	    frame_samples > UINT64_MAX / NANOSECONDS_PER_SECOND)
		return SIPPYMODEM_FRAME_CLOCK_INVALID_ARGUMENT;
	duration_numerator =
		(uint64_t)frame_samples * NANOSECONDS_PER_SECOND;
	if (duration_numerator % sample_rate != 0)
		return SIPPYMODEM_FRAME_CLOCK_INEXACT_INTERVAL;

	memset(&initialized, 0, sizeof(initialized));
	initialized.interval_ns = duration_numerator / sample_rate;
	initialized.maximum_catchup_frames = maximum_catchup_frames;
	*clock = initialized;
	return SIPPYMODEM_FRAME_CLOCK_OK;
}

void sippymodem_frame_clock_start(
	struct sippymodem_frame_clock *clock, uint64_t now_ns)
{
	if (clock == NULL || clock->interval_ns == 0 ||
	    clock->maximum_catchup_frames == 0)
		return;
	clock->next_deadline_ns = now_ns;
	clock->running = true;
}

void sippymodem_frame_clock_stop(struct sippymodem_frame_clock *clock)
{
	if (clock == NULL)
		return;
	clock->next_deadline_ns = 0;
	clock->running = false;
}

enum sippymodem_frame_clock_result sippymodem_frame_clock_take_due(
	struct sippymodem_frame_clock *clock, uint64_t now_ns,
	size_t *frames_due)
{
	uint64_t elapsed_frames;
	uint64_t due;

	if (frames_due != NULL)
		*frames_due = 0;
	if (clock == NULL || frames_due == NULL || clock->interval_ns == 0 ||
	    clock->maximum_catchup_frames == 0)
		return SIPPYMODEM_FRAME_CLOCK_INVALID_ARGUMENT;
	if (!clock->running)
		return SIPPYMODEM_FRAME_CLOCK_NOT_RUNNING;
	if (now_ns < clock->next_deadline_ns)
		return SIPPYMODEM_FRAME_CLOCK_NOT_DUE;

	elapsed_frames =
		(now_ns - clock->next_deadline_ns) / clock->interval_ns;
	if (elapsed_frames == UINT64_MAX)
		goto overrun;
	due = elapsed_frames + 1;
	if (due > clock->maximum_catchup_frames ||
	    due > (UINT64_MAX - clock->next_deadline_ns) /
			  clock->interval_ns)
		goto overrun;
	clock->next_deadline_ns += due * clock->interval_ns;
	add_saturated(&clock->stats.frames_due, due);
	*frames_due = (size_t)due;
	return SIPPYMODEM_FRAME_CLOCK_OK;

overrun:
	add_saturated(&clock->stats.overruns, 1);
	sippymodem_frame_clock_stop(clock);
	return SIPPYMODEM_FRAME_CLOCK_OVERRUN;
}

uint64_t sippymodem_frame_clock_wait_ns(
	const struct sippymodem_frame_clock *clock, uint64_t now_ns)
{
	if (clock == NULL || !clock->running)
		return UINT64_MAX;
	return now_ns < clock->next_deadline_ns ?
		       clock->next_deadline_ns - now_ns :
		       0;
}

const char *sippymodem_frame_clock_result_name(
	enum sippymodem_frame_clock_result result)
{
	switch (result) {
	case SIPPYMODEM_FRAME_CLOCK_OK:
		return "ok";
	case SIPPYMODEM_FRAME_CLOCK_NOT_DUE:
		return "frame is not due";
	case SIPPYMODEM_FRAME_CLOCK_NOT_RUNNING:
		return "frame clock is not running";
	case SIPPYMODEM_FRAME_CLOCK_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_FRAME_CLOCK_INEXACT_INTERVAL:
		return "frame interval is not exactly representable";
	case SIPPYMODEM_FRAME_CLOCK_OVERRUN:
		return "frame clock catch-up limit exceeded";
	}
	return "unknown frame clock result";
}
