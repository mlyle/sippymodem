// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_audio_scheduler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void add_saturated(uint64_t *value, uint64_t increment)
{
	*value = increment > UINT64_MAX - *value ? UINT64_MAX :
						    *value + increment;
}

bool sippymodem_audio_scheduler_init(
	struct sippymodem_audio_scheduler *scheduler,
	struct sippymodem_audio_path *path, unsigned int sample_rate,
	size_t maximum_catchup_frames)
{
	struct sippymodem_audio_scheduler initialized;
	enum sippymodem_frame_clock_result clock_result;
	size_t frame_samples;

	if (scheduler == NULL || path == NULL)
		return false;
	frame_samples = sippymodem_audio_path_frame_samples(path);
	memset(&initialized, 0, sizeof(initialized));
	clock_result = sippymodem_frame_clock_init(
		&initialized.clock, frame_samples, sample_rate,
		maximum_catchup_frames);
	if (clock_result != SIPPYMODEM_FRAME_CLOCK_OK)
		return false;
	initialized.path = path;
	initialized.last_clock_result = SIPPYMODEM_FRAME_CLOCK_OK;
	initialized.last_path_result = SIPPYMODEM_AUDIO_PATH_OK;
	*scheduler = initialized;
	return true;
}

bool sippymodem_audio_scheduler_set_active(
	struct sippymodem_audio_scheduler *scheduler, bool active)
{
	if (scheduler == NULL || scheduler->path == NULL)
		return false;
	if (scheduler->active == active)
		return true;
	scheduler->active = active;
	if (active) {
		add_saturated(&scheduler->stats.activations, 1);
	} else {
		sippymodem_frame_clock_stop(&scheduler->clock);
		add_saturated(&scheduler->stats.deactivations, 1);
	}
	return true;
}

bool sippymodem_audio_scheduler_is_active(
	const struct sippymodem_audio_scheduler *scheduler)
{
	return scheduler != NULL && scheduler->active;
}

static enum sippymodem_audio_scheduler_result fail_scheduler(
	struct sippymodem_audio_scheduler *scheduler,
	enum sippymodem_audio_scheduler_result result)
{
	scheduler->active = false;
	sippymodem_frame_clock_stop(&scheduler->clock);
	add_saturated(&scheduler->stats.failures, 1);
	return result;
}

enum sippymodem_audio_scheduler_result sippymodem_audio_scheduler_step(
	struct sippymodem_audio_scheduler *scheduler, uint64_t now_ns,
	size_t *frames_processed)
{
	size_t frames_due;
	size_t processed = 0;

	if (frames_processed != NULL)
		*frames_processed = 0;
	if (scheduler == NULL || scheduler->path == NULL ||
	    frames_processed == NULL)
		return SIPPYMODEM_AUDIO_SCHEDULER_INVALID_ARGUMENT;
	if (!scheduler->active)
		return SIPPYMODEM_AUDIO_SCHEDULER_IDLE;
	if (!scheduler->clock.running)
		sippymodem_frame_clock_start(&scheduler->clock, now_ns);
	scheduler->last_clock_result = sippymodem_frame_clock_take_due(
		&scheduler->clock, now_ns, &frames_due);
	if (scheduler->last_clock_result == SIPPYMODEM_FRAME_CLOCK_NOT_DUE)
		return SIPPYMODEM_AUDIO_SCHEDULER_IDLE;
	if (scheduler->last_clock_result != SIPPYMODEM_FRAME_CLOCK_OK)
		return fail_scheduler(
			scheduler, SIPPYMODEM_AUDIO_SCHEDULER_CLOCK_ERROR);

	for (; processed < frames_due; ++processed) {
		scheduler->last_path_result =
			sippymodem_audio_path_process_frame(scheduler->path);
		if (scheduler->last_path_result ==
		    SIPPYMODEM_AUDIO_PATH_MODEM_STOPPED) {
			add_saturated(&scheduler->stats.frames_processed,
				      processed);
			*frames_processed = processed;
			return fail_scheduler(
				scheduler,
				SIPPYMODEM_AUDIO_SCHEDULER_MODEM_STOPPED);
		}
		if (scheduler->last_path_result != SIPPYMODEM_AUDIO_PATH_OK) {
			add_saturated(&scheduler->stats.frames_processed,
				      processed);
			*frames_processed = processed;
			return fail_scheduler(
				scheduler,
				SIPPYMODEM_AUDIO_SCHEDULER_PATH_ERROR);
		}
	}
	add_saturated(&scheduler->stats.frames_processed, processed);
	*frames_processed = processed;
	return SIPPYMODEM_AUDIO_SCHEDULER_OK;
}

uint64_t sippymodem_audio_scheduler_wait_ns(
	const struct sippymodem_audio_scheduler *scheduler, uint64_t now_ns)
{
	if (scheduler == NULL || !scheduler->active)
		return UINT64_MAX;
	if (!scheduler->clock.running)
		return 0;
	return sippymodem_frame_clock_wait_ns(&scheduler->clock, now_ns);
}

const char *sippymodem_audio_scheduler_result_name(
	enum sippymodem_audio_scheduler_result result)
{
	switch (result) {
	case SIPPYMODEM_AUDIO_SCHEDULER_OK:
		return "ok";
	case SIPPYMODEM_AUDIO_SCHEDULER_IDLE:
		return "idle";
	case SIPPYMODEM_AUDIO_SCHEDULER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_AUDIO_SCHEDULER_CLOCK_ERROR:
		return "frame clock failed";
	case SIPPYMODEM_AUDIO_SCHEDULER_PATH_ERROR:
		return "audio path failed";
	case SIPPYMODEM_AUDIO_SCHEDULER_MODEM_STOPPED:
		return "modem core stopped";
	}
	return "unknown audio scheduler result";
}
