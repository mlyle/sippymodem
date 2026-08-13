// SPDX-License-Identifier: BSD-3-Clause

#ifndef AUDIO_SCHEDULER_H
#define AUDIO_SCHEDULER_H

#include "audio_path.h"
#include "frame_clock.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum audio_scheduler_result {
	AUDIO_SCHEDULER_OK,
	AUDIO_SCHEDULER_IDLE,
	AUDIO_SCHEDULER_INVALID_ARGUMENT,
	AUDIO_SCHEDULER_CLOCK_ERROR,
	AUDIO_SCHEDULER_PATH_ERROR,
	AUDIO_SCHEDULER_MODEM_STOPPED,
};

struct audio_scheduler_stats {
	uint64_t activations;
	uint64_t deactivations;
	uint64_t frames_processed;
	uint64_t failures;
};

struct audio_scheduler {
	struct audio_path *path;
	struct frame_clock clock;
	enum frame_clock_result last_clock_result;
	enum audio_path_result last_path_result;
	bool active;
	struct audio_scheduler_stats stats;
};

bool audio_scheduler_init(
	struct audio_scheduler *scheduler,
	struct audio_path *path,
	unsigned int sample_rate,
	size_t maximum_catchup_frames);
bool audio_scheduler_set_active(
	struct audio_scheduler *scheduler, bool active);
bool audio_scheduler_is_active(
	const struct audio_scheduler *scheduler);

enum audio_scheduler_result audio_scheduler_step(
	struct audio_scheduler *scheduler,
	uint64_t now_ns,
	size_t *frames_processed);
/* Process an externally clocked batch without consulting wall time. */
enum audio_scheduler_result audio_scheduler_step_remote(
	struct audio_scheduler *scheduler,
	size_t frames_due,
	size_t *frames_processed);
uint64_t audio_scheduler_wait_ns(
	const struct audio_scheduler *scheduler,
	uint64_t now_ns);

const char *audio_scheduler_result_name(
	enum audio_scheduler_result result);

#endif
