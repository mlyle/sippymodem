// SPDX-License-Identifier: BSD-3-Clause

#include "audio_scheduler.h"
#include "integer_math.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

bool audio_scheduler_init(
	struct audio_scheduler *scheduler,
	struct audio_path *path, unsigned int sample_rate,
	size_t maximum_catchup_frames)
{
	struct audio_scheduler initialized;
	enum frame_clock_result clock_result;
	size_t frame_samples;

	if (scheduler == NULL || path == NULL)
		return false;
	frame_samples = audio_path_frame_samples(path);
	memset(&initialized, 0, sizeof(initialized));
	clock_result = frame_clock_init(
		&initialized.clock, frame_samples, sample_rate,
		maximum_catchup_frames);
	if (clock_result != FRAME_CLOCK_OK)
		return false;
	initialized.path = path;
	initialized.last_clock_result = FRAME_CLOCK_OK;
	initialized.last_path_result = AUDIO_PATH_OK;
	*scheduler = initialized;
	return true;
}

bool audio_scheduler_set_active(
	struct audio_scheduler *scheduler, bool active)
{
	if (scheduler == NULL || scheduler->path == NULL)
		return false;
	if (scheduler->active == active)
		return true;
	scheduler->active = active;
	if (active) {
		scheduler->stats.activations = add_u64_saturating(
			scheduler->stats.activations, 1);
	} else {
		frame_clock_stop(&scheduler->clock);
		scheduler->stats.deactivations = add_u64_saturating(
			scheduler->stats.deactivations, 1);
	}
	return true;
}

bool audio_scheduler_is_active(
	const struct audio_scheduler *scheduler)
{
	return scheduler != NULL && scheduler->active;
}

static enum audio_scheduler_result fail_scheduler(
	struct audio_scheduler *scheduler,
	enum audio_scheduler_result result)
{
	scheduler->active = false;
	frame_clock_stop(&scheduler->clock);
	scheduler->stats.failures = add_u64_saturating(
		scheduler->stats.failures, 1);
	return result;
}

static enum audio_scheduler_result process_frames(
	struct audio_scheduler *scheduler, size_t frames_due,
	size_t *frames_processed)
{
	size_t processed = 0;

	for (; processed < frames_due; ++processed) {
		scheduler->last_path_result =
			audio_path_process_frame(scheduler->path);
		if (scheduler->last_path_result ==
		    AUDIO_PATH_MODEM_STOPPED) {
			scheduler->stats.frames_processed = add_u64_saturating(
				scheduler->stats.frames_processed, processed);
			*frames_processed = processed;
			return fail_scheduler(
				scheduler,
				AUDIO_SCHEDULER_MODEM_STOPPED);
		}
		if (scheduler->last_path_result != AUDIO_PATH_OK) {
			scheduler->stats.frames_processed = add_u64_saturating(
				scheduler->stats.frames_processed, processed);
			*frames_processed = processed;
			return fail_scheduler(
				scheduler,
				AUDIO_SCHEDULER_PATH_ERROR);
		}
	}
	scheduler->stats.frames_processed = add_u64_saturating(
		scheduler->stats.frames_processed, processed);
	*frames_processed = processed;
	return AUDIO_SCHEDULER_OK;
}

enum audio_scheduler_result audio_scheduler_step(
	struct audio_scheduler *scheduler, uint64_t now_ns,
	size_t *frames_processed)
{
	size_t frames_due;

	if (frames_processed != NULL)
		*frames_processed = 0;
	if (scheduler == NULL || scheduler->path == NULL ||
	    frames_processed == NULL)
		return AUDIO_SCHEDULER_INVALID_ARGUMENT;
	if (!scheduler->active)
		return AUDIO_SCHEDULER_IDLE;
	if (!scheduler->clock.running)
		frame_clock_start(&scheduler->clock, now_ns);
	scheduler->last_clock_result = frame_clock_take_due(
		&scheduler->clock, now_ns, &frames_due);
	if (scheduler->last_clock_result == FRAME_CLOCK_NOT_DUE)
		return AUDIO_SCHEDULER_IDLE;
	if (scheduler->last_clock_result != FRAME_CLOCK_OK)
		return fail_scheduler(
			scheduler, AUDIO_SCHEDULER_CLOCK_ERROR);
	return process_frames(scheduler, frames_due, frames_processed);
}

enum audio_scheduler_result audio_scheduler_step_remote(
	struct audio_scheduler *scheduler, size_t frames_due,
	size_t *frames_processed)
{
	if (frames_processed != NULL)
		*frames_processed = 0;
	if (scheduler == NULL || scheduler->path == NULL ||
	    frames_processed == NULL)
		return AUDIO_SCHEDULER_INVALID_ARGUMENT;
	if (!scheduler->active || frames_due == 0)
		return AUDIO_SCHEDULER_IDLE;
	frame_clock_stop(&scheduler->clock);
	scheduler->last_clock_result = FRAME_CLOCK_OK;
	return process_frames(scheduler, frames_due, frames_processed);
}

uint64_t audio_scheduler_wait_ns(
	const struct audio_scheduler *scheduler, uint64_t now_ns)
{
	if (scheduler == NULL || !scheduler->active)
		return UINT64_MAX;
	if (!scheduler->clock.running)
		return 0;
	return frame_clock_wait_ns(&scheduler->clock, now_ns);
}

const char *audio_scheduler_result_name(
	enum audio_scheduler_result result)
{
	switch (result) {
	case AUDIO_SCHEDULER_OK:
		return "ok";
	case AUDIO_SCHEDULER_IDLE:
		return "idle";
	case AUDIO_SCHEDULER_INVALID_ARGUMENT:
		return "invalid argument";
	case AUDIO_SCHEDULER_CLOCK_ERROR:
		return "frame clock failed";
	case AUDIO_SCHEDULER_PATH_ERROR:
		return "audio path failed";
	case AUDIO_SCHEDULER_MODEM_STOPPED:
		return "modem core stopped";
	}
	return "unknown audio scheduler result";
}
