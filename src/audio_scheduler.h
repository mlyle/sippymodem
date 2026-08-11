// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_AUDIO_SCHEDULER_H
#define SIPPYMODEM_AUDIO_SCHEDULER_H

#include "audio_path.h"
#include "frame_clock.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum sippymodem_audio_scheduler_result {
	SIPPYMODEM_AUDIO_SCHEDULER_OK,
	SIPPYMODEM_AUDIO_SCHEDULER_IDLE,
	SIPPYMODEM_AUDIO_SCHEDULER_INVALID_ARGUMENT,
	SIPPYMODEM_AUDIO_SCHEDULER_CLOCK_ERROR,
	SIPPYMODEM_AUDIO_SCHEDULER_PATH_ERROR,
	SIPPYMODEM_AUDIO_SCHEDULER_MODEM_STOPPED,
};

struct sippymodem_audio_scheduler_stats {
	uint64_t activations;
	uint64_t deactivations;
	uint64_t frames_processed;
	uint64_t failures;
};

struct sippymodem_audio_scheduler {
	struct sippymodem_audio_path *path;
	struct sippymodem_frame_clock clock;
	enum sippymodem_frame_clock_result last_clock_result;
	enum sippymodem_audio_path_result last_path_result;
	bool active;
	struct sippymodem_audio_scheduler_stats stats;
};

bool sippymodem_audio_scheduler_init(
	struct sippymodem_audio_scheduler *scheduler,
	struct sippymodem_audio_path *path,
	unsigned int sample_rate,
	size_t maximum_catchup_frames);
bool sippymodem_audio_scheduler_set_active(
	struct sippymodem_audio_scheduler *scheduler, bool active);
bool sippymodem_audio_scheduler_is_active(
	const struct sippymodem_audio_scheduler *scheduler);

enum sippymodem_audio_scheduler_result sippymodem_audio_scheduler_step(
	struct sippymodem_audio_scheduler *scheduler,
	uint64_t now_ns,
	size_t *frames_processed);
uint64_t sippymodem_audio_scheduler_wait_ns(
	const struct sippymodem_audio_scheduler *scheduler,
	uint64_t now_ns);

const char *sippymodem_audio_scheduler_result_name(
	enum sippymodem_audio_scheduler_result result);

#endif
