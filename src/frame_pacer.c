// SPDX-License-Identifier: BSD-3-Clause
#include "frame_pacer.h"

static bool is_valid(const struct frame_pacer *pacer)
{
	return pacer != NULL && pacer->interval_ns != 0 &&
	       pacer->capacity_ns >= pacer->interval_ns &&
	       pacer->credit_ns <= pacer->capacity_ns;
}

static uint64_t available_credit(const struct frame_pacer *pacer, uint64_t now_ns)
{
	if (!pacer->started)
		return pacer->capacity_ns;
	uint64_t elapsed = now_ns >= pacer->last_time_ns ?
		now_ns - pacer->last_time_ns : 0;
	uint64_t room = pacer->capacity_ns - pacer->credit_ns;

	return elapsed >= room ? pacer->capacity_ns : pacer->credit_ns + elapsed;
}

bool frame_pacer_init(struct frame_pacer *pacer,
	uint64_t interval_ns, size_t maximum_burst_frames)
{
	if (pacer == NULL || interval_ns == 0 || maximum_burst_frames == 0 ||
	    maximum_burst_frames > UINT64_MAX / interval_ns)
		return false;
	*pacer = (struct frame_pacer) {
		.interval_ns = interval_ns,
		.capacity_ns = interval_ns * maximum_burst_frames,
	};
	return true;
}

void frame_pacer_reset(struct frame_pacer *pacer)
{
	if (pacer == NULL)
		return;
	pacer->credit_ns = 0;
	pacer->last_time_ns = 0;
	pacer->started = false;
}

bool frame_pacer_take(struct frame_pacer *pacer, uint64_t now_ns)
{
	if (!is_valid(pacer))
		return false;
	pacer->credit_ns = available_credit(pacer, now_ns);
	if (!pacer->started || now_ns > pacer->last_time_ns)
		pacer->last_time_ns = now_ns;
	pacer->started = true;
	if (pacer->credit_ns < pacer->interval_ns)
		return false;
	pacer->credit_ns -= pacer->interval_ns;
	return true;
}

uint64_t frame_pacer_wait_ns(const struct frame_pacer *pacer, uint64_t now_ns)
{
	if (!is_valid(pacer))
		return UINT64_MAX;
	uint64_t credit = available_credit(pacer, now_ns);

	return credit < pacer->interval_ns ? pacer->interval_ns - credit : 0;
}
