// SPDX-License-Identifier: BSD-3-Clause

#include <audio_scheduler.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_RESULTS = 8,
};

struct sippymodem_audio_path {
	size_t frame_samples;
	enum sippymodem_audio_path_result results[TEST_RESULTS];
	size_t result_count;
	size_t result_index;
	uint64_t calls;
};

size_t sippymodem_audio_path_frame_samples(
	const struct sippymodem_audio_path *path)
{
	return path != NULL ? path->frame_samples : 0;
}

enum sippymodem_audio_path_result sippymodem_audio_path_process_frame(
	struct sippymodem_audio_path *path)
{
	enum sippymodem_audio_path_result result = SIPPYMODEM_AUDIO_PATH_OK;

	path->calls++;
	if (path->result_index < path->result_count)
		result = path->results[path->result_index++];
	return result;
}

static bool test_activation_deadlines_and_catchup(void)
{
	struct sippymodem_audio_path path = {
		.frame_samples = 80,
	};
	struct sippymodem_audio_scheduler scheduler;
	size_t frames = 99;

	CHECK(sippymodem_audio_scheduler_init(&scheduler, &path, 8000, 3),
	      "audio scheduler initialization failed");
	CHECK(sippymodem_audio_scheduler_step(
		      &scheduler, 1000000000, &frames) ==
		      SIPPYMODEM_AUDIO_SCHEDULER_IDLE &&
		      frames == 0 && path.calls == 0 &&
		      sippymodem_audio_scheduler_wait_ns(
			      &scheduler, 1000000000) == UINT64_MAX,
	      "inactive scheduler processed a frame");
	CHECK(sippymodem_audio_scheduler_set_active(&scheduler, true) &&
		      sippymodem_audio_scheduler_wait_ns(
			      &scheduler, 1000000000) == 0 &&
		      scheduler.stats.activations == 1,
	      "activation did not arm an immediate frame");
	CHECK(sippymodem_audio_scheduler_step(
		      &scheduler, 1000000000, &frames) ==
		      SIPPYMODEM_AUDIO_SCHEDULER_OK &&
		      frames == 1 && path.calls == 1,
	      "scheduler did not process its first frame");
	CHECK(sippymodem_audio_scheduler_step(
		      &scheduler, 1005000000, &frames) ==
		      SIPPYMODEM_AUDIO_SCHEDULER_IDLE &&
		      frames == 0 && path.calls == 1 &&
		      sippymodem_audio_scheduler_wait_ns(
			      &scheduler, 1005000000) == 5000000,
	      "scheduler processed before its deadline");
	CHECK(sippymodem_audio_scheduler_step(
		      &scheduler, 1030000000, &frames) ==
		      SIPPYMODEM_AUDIO_SCHEDULER_OK &&
		      frames == 3 && path.calls == 4 &&
		      scheduler.stats.frames_processed == 4,
	      "scheduler did not perform bounded catch-up");
	CHECK(sippymodem_audio_scheduler_set_active(&scheduler, true) &&
		      scheduler.stats.activations == 1 &&
		      sippymodem_audio_scheduler_set_active(&scheduler, false) &&
		      !sippymodem_audio_scheduler_is_active(&scheduler) &&
		      scheduler.stats.deactivations == 1,
	      "scheduler activation transitions are not idempotent");
	return true;
}

static bool test_failures_deactivate_the_path(void)
{
	struct sippymodem_audio_path path = {
		.frame_samples = 80,
	};
	struct sippymodem_audio_scheduler scheduler;
	size_t frames;

	CHECK(sippymodem_audio_scheduler_init(&scheduler, &path, 8000, 2) &&
		      sippymodem_audio_scheduler_set_active(&scheduler, true) &&
		      sippymodem_audio_scheduler_step(&scheduler, 0, &frames) ==
			      SIPPYMODEM_AUDIO_SCHEDULER_OK,
	      "overrun fixture did not process its first frame");
	CHECK(sippymodem_audio_scheduler_step(
		      &scheduler, 30000000, &frames) ==
		      SIPPYMODEM_AUDIO_SCHEDULER_CLOCK_ERROR &&
		      frames == 0 && !scheduler.active && path.calls == 1 &&
		      scheduler.stats.failures == 1,
	      "clock overrun did not deactivate before audio processing");

	memset(&scheduler, 0, sizeof(scheduler));
	memset(&path, 0, sizeof(path));
	path.frame_samples = 80;
	path.results[0] = SIPPYMODEM_AUDIO_PATH_OK;
	path.results[1] = SIPPYMODEM_AUDIO_PATH_OK;
	path.results[2] = SIPPYMODEM_AUDIO_PATH_TRANSMIT_ERROR;
	path.result_count = 3;
	CHECK(sippymodem_audio_scheduler_init(&scheduler, &path, 8000, 3) &&
		      sippymodem_audio_scheduler_set_active(&scheduler, true) &&
		      sippymodem_audio_scheduler_step(&scheduler, 0, &frames) ==
			      SIPPYMODEM_AUDIO_SCHEDULER_OK &&
		      sippymodem_audio_scheduler_step(
			      &scheduler, 20000000, &frames) ==
			      SIPPYMODEM_AUDIO_SCHEDULER_PATH_ERROR &&
		      frames == 1 && !scheduler.active && path.calls == 3 &&
		      scheduler.stats.frames_processed == 2,
	      "partial audio batch did not retain completed frames");

	memset(&scheduler, 0, sizeof(scheduler));
	path.result_index = 0;
	path.result_count = 1;
	path.results[0] = SIPPYMODEM_AUDIO_PATH_MODEM_STOPPED;
	CHECK(sippymodem_audio_scheduler_init(&scheduler, &path, 8000, 2) &&
		      sippymodem_audio_scheduler_set_active(&scheduler, true) &&
		      sippymodem_audio_scheduler_step(&scheduler, 0, &frames) ==
			      SIPPYMODEM_AUDIO_SCHEDULER_MODEM_STOPPED &&
		      frames == 0 && !scheduler.active,
	      "modem stop did not deactivate the scheduler");
	return true;
}

static bool test_initialization_is_transactional(void)
{
	struct sippymodem_audio_path path = {
		.frame_samples = 1,
	};
	struct sippymodem_audio_scheduler scheduler = {
		.active = true,
	};
	struct sippymodem_audio_scheduler original = scheduler;
	size_t frames = 99;

	CHECK(!sippymodem_audio_scheduler_init(&scheduler, &path, 3, 2) &&
		      memcmp(&scheduler, &original, sizeof(scheduler)) == 0,
	      "inexact scheduler initialization partially changed state");
	CHECK(!sippymodem_audio_scheduler_init(NULL, &path, 8000, 2) &&
		      !sippymodem_audio_scheduler_set_active(NULL, true) &&
		      sippymodem_audio_scheduler_step(NULL, 0, &frames) ==
			      SIPPYMODEM_AUDIO_SCHEDULER_INVALID_ARGUMENT &&
		      frames == 0,
	      "scheduler null validation is inconsistent");
	CHECK(strcmp(sippymodem_audio_scheduler_result_name(
			     SIPPYMODEM_AUDIO_SCHEDULER_PATH_ERROR),
		     "audio path failed") == 0,
	      "scheduler result name is wrong");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "activation and catch-up", test_activation_deadlines_and_catchup },
		{ "failure deactivation", test_failures_deactivate_the_path },
		{ "transactional initialization",
		  test_initialization_is_transactional },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
