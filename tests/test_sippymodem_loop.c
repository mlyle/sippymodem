// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_loop.h>

#include "test.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

struct sippymodem_runtime {
	int terminal_fd;
	uint64_t wait_ns;
	unsigned int steps;
	unsigned int stop_after_steps;
	enum sippymodem_runtime_result step_result;
};

static volatile sig_atomic_t *active_running;

enum sippymodem_runtime_result sippymodem_runtime_step(
	struct sippymodem_runtime *runtime, uint64_t now_ns,
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

uint64_t sippymodem_runtime_wait_ns(
	const struct sippymodem_runtime *runtime, uint64_t now_ns)
{
	(void)now_ns;
	return runtime->wait_ns;
}

int sippymodem_runtime_terminal_poll_fd(
	const struct sippymodem_runtime *runtime)
{
	return runtime->terminal_fd;
}

static bool test_stops_after_bounded_steps(void)
{
	struct sippymodem_runtime runtime = {
		.terminal_fd = -1,
		.wait_ns = 0,
		.stop_after_steps = 2,
	};
	struct sippymodem_loop_error error;
	volatile sig_atomic_t running = 1;

	active_running = &running;
	CHECK(sippymodem_loop_run(&runtime, &running, &error) ==
			      SIPPYMODEM_LOOP_OK &&
		      runtime.steps == 2 && running == 0 &&
		      error.system_error == 0,
	      "loop did not stop after bounded idle work");
	return true;
}

static bool test_wakes_for_terminal_input(void)
{
	struct sippymodem_runtime runtime = {
		.wait_ns = UINT64_MAX,
		.stop_after_steps = 2,
	};
	struct sippymodem_loop_error error;
	volatile sig_atomic_t running = 1;
	int descriptors[2];
	char byte = 'x';

	CHECK(pipe(descriptors) == 0 && write(descriptors[1], &byte, 1) == 1,
	      "terminal readiness fixture was not created");
	runtime.terminal_fd = descriptors[0];
	active_running = &running;
	CHECK(sippymodem_loop_run(&runtime, &running, &error) ==
			      SIPPYMODEM_LOOP_OK &&
		      runtime.steps == 2,
	      "terminal readiness did not wake the loop");
	close(descriptors[0]);
	close(descriptors[1]);
	return true;
}

static bool test_reports_runtime_and_wait_errors(void)
{
	struct sippymodem_loop_error error;
	struct sippymodem_runtime runtime = {
		.terminal_fd = -1,
		.wait_ns = 0,
		.stop_after_steps = 2,
		.step_result = SIPPYMODEM_RUNTIME_HOST_ERROR,
	};
	volatile sig_atomic_t running = 1;
	int descriptors[2];

	active_running = &running;
	CHECK(sippymodem_loop_run(&runtime, &running, &error) ==
			      SIPPYMODEM_LOOP_RUNTIME_ERROR &&
		      error.runtime_result == SIPPYMODEM_RUNTIME_HOST_ERROR,
	      "loop hid a runtime failure");
	CHECK(pipe(descriptors) == 0, "invalid-descriptor fixture was not created");
	close(descriptors[0]);
	close(descriptors[1]);
	runtime.terminal_fd = descriptors[0];
	runtime.wait_ns = UINT64_MAX;
	runtime.step_result = SIPPYMODEM_RUNTIME_IDLE;
	runtime.steps = 0;
	running = 1;
	CHECK(sippymodem_loop_run(&runtime, &running, &error) ==
			      SIPPYMODEM_LOOP_WAIT_ERROR &&
		      error.system_error == EBADF,
	      "loop hid an invalid wait descriptor");
	return true;
}

static bool test_rejects_invalid_arguments(void)
{
	struct sippymodem_runtime runtime = { 0 };
	struct sippymodem_loop_error error = {
		.runtime_result = SIPPYMODEM_RUNTIME_HOST_ERROR,
		.system_error = EIO,
	};
	volatile sig_atomic_t running = 0;

	CHECK(sippymodem_loop_run(NULL, &running, &error) ==
			      SIPPYMODEM_LOOP_INVALID_ARGUMENT &&
		      error.runtime_result == SIPPYMODEM_RUNTIME_OK &&
		      error.system_error == 0 &&
		      sippymodem_loop_run(&runtime, NULL, NULL) ==
			      SIPPYMODEM_LOOP_INVALID_ARGUMENT &&
		      strcmp(sippymodem_loop_result_name(
			     SIPPYMODEM_LOOP_RUNTIME_ERROR),
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
