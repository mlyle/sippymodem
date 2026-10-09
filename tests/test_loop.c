// SPDX-License-Identifier: BSD-3-Clause

#include <loop.h>

#include "test.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

struct modem_runtime {
	int terminal_fd;
	int media_fd;
	uint64_t wait_ns;
	unsigned int steps;
	unsigned int stop_after_steps;
	enum modem_runtime_result step_result;
};

static volatile sig_atomic_t *active_running;

enum modem_runtime_result modem_runtime_step(
	struct modem_runtime *runtime, uint64_t now_ns,
	size_t *terminal_bytes_received, size_t *datagrams_received,
	size_t *frames_processed)
{
	(void)now_ns;
	(void)terminal_bytes_received;
	(void)datagrams_received;
	(void)frames_processed;
	runtime->steps++;
	if (runtime->steps >= runtime->stop_after_steps)
		*active_running = 0;
	return runtime->step_result;
}

uint64_t modem_runtime_wait_ns(
	const struct modem_runtime *runtime, uint64_t now_ns)
{
	(void)now_ns;
	return runtime->wait_ns;
}

int modem_runtime_terminal_poll_fd(
	const struct modem_runtime *runtime)
{
	return runtime->terminal_fd;
}

int modem_runtime_media_poll_fd(
	const struct modem_runtime *runtime)
{
	return runtime->media_fd;
}

static bool test_stops_after_bounded_steps(void)
{
	struct modem_runtime runtime = {
		.terminal_fd = -1,
		.media_fd = -1,
		.wait_ns = 0,
		.stop_after_steps = 2,
	};
	struct modem_loop_error error;
	volatile sig_atomic_t running = 1;

	const uint64_t waits[] = { 0, 1, UINT64_MAX };
	for (size_t index = 0; index < ARRAY_LENGTH(waits); ++index) {
		runtime.wait_ns = waits[index];
		runtime.steps = 0;
		running = 1;
		active_running = &running;
		CHECK(modem_loop_run(&runtime, &running, &error) ==
				      MODEM_LOOP_OK &&
			      runtime.steps == 2 && running == 0 &&
			      error.system_error == 0,
		      "loop did not stop after bounded idle work");
	}
	return true;
}

static bool test_wakes_for_terminal_input(void)
{
	struct modem_runtime runtime = {
		.media_fd = -1,
		.wait_ns = UINT64_MAX,
		.stop_after_steps = 2,
	};
	struct modem_loop_error error;
	volatile sig_atomic_t running = 1;
	int descriptors[2];
	char byte = 'x';

	CHECK(pipe(descriptors) == 0 && write(descriptors[1], &byte, 1) == 1,
	      "terminal readiness fixture was not created");
	runtime.terminal_fd = descriptors[0];
	active_running = &running;
	CHECK(modem_loop_run(&runtime, &running, &error) ==
			      MODEM_LOOP_OK &&
		      runtime.steps == 2,
	      "terminal readiness did not wake the loop");
	close(descriptors[0]);
	close(descriptors[1]);
	return true;
}

static bool test_wakes_for_media_input(void)
{
	struct modem_runtime runtime = {
		.terminal_fd = -1,
		.wait_ns = UINT64_MAX,
		.stop_after_steps = 2,
	};
	struct modem_loop_error error;
	volatile sig_atomic_t running = 1;
	int descriptors[2];
	char byte = 'x';

	CHECK(pipe(descriptors) == 0 && write(descriptors[1], &byte, 1) == 1,
	      "media readiness fixture was not created");
	runtime.media_fd = descriptors[0];
	active_running = &running;
	CHECK(modem_loop_run(&runtime, &running, &error) ==
		      MODEM_LOOP_OK &&
		      runtime.steps == 2,
	      "media readiness did not wake the loop");
	close(descriptors[0]);
	close(descriptors[1]);
	return true;
}

static bool test_reports_runtime_and_wait_errors(void)
{
	struct modem_loop_error error;
	struct modem_runtime runtime = {
		.terminal_fd = -1,
		.media_fd = -1,
		.wait_ns = 0,
		.stop_after_steps = 2,
		.step_result = MODEM_RUNTIME_HOST_ERROR,
	};
	volatile sig_atomic_t running = 1;
	int descriptors[2];

	active_running = &running;
	CHECK(modem_loop_run(&runtime, &running, &error) ==
			      MODEM_LOOP_RUNTIME_ERROR &&
		      error.runtime_result == MODEM_RUNTIME_HOST_ERROR,
	      "loop hid a runtime failure");
	CHECK(pipe(descriptors) == 0, "invalid-descriptor fixture was not created");
	close(descriptors[0]);
	close(descriptors[1]);
	runtime.terminal_fd = descriptors[0];
	runtime.wait_ns = UINT64_MAX;
	runtime.step_result = MODEM_RUNTIME_IDLE;
	runtime.steps = 0;
	running = 1;
	CHECK(modem_loop_run(&runtime, &running, &error) ==
			      MODEM_LOOP_WAIT_ERROR &&
		      error.system_error == EBADF,
	      "loop hid an invalid wait descriptor");
	return true;
}

static bool test_rejects_invalid_arguments(void)
{
	struct modem_runtime runtime = { 0 };
	struct modem_loop_error error = {
		.runtime_result = MODEM_RUNTIME_HOST_ERROR,
		.system_error = EIO,
	};
	volatile sig_atomic_t running = 0;

	CHECK(modem_loop_run(NULL, &running, &error) ==
			      MODEM_LOOP_INVALID_ARGUMENT &&
		      error.runtime_result == MODEM_RUNTIME_OK &&
		      error.system_error == 0 &&
		      modem_loop_run(&runtime, NULL, NULL) ==
			      MODEM_LOOP_INVALID_ARGUMENT &&
		      strcmp(modem_loop_result_name(
			     MODEM_LOOP_RUNTIME_ERROR),
			     "modem runtime failed") == 0,
	      "loop invalid-argument diagnostics were incomplete");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "bounded steps", test_stops_after_bounded_steps },
		{ "terminal wake", test_wakes_for_terminal_input },
		{ "media wake", test_wakes_for_media_input },
		{ "runtime and wait errors", test_reports_runtime_and_wait_errors },
		{ "invalid arguments", test_rejects_invalid_arguments },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
