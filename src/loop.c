// SPDX-License-Identifier: BSD-3-Clause

#define _GNU_SOURCE

#include "loop.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

enum {
	NANOSECONDS_PER_SECOND = 1000000000,
	MODEM_LOOP_MAXIMUM_WAIT_NS = 20000000,
};

static void clear_error(struct modem_loop_error *error)
{
	if (error != NULL)
		memset(error, 0, sizeof(*error));
}

static enum modem_loop_result monotonic_now(
	uint64_t *now_ns, struct modem_loop_error *error)
{
	struct timespec now;
	uint64_t seconds;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		if (error != NULL)
			error->system_error = errno;
		return MODEM_LOOP_CLOCK_ERROR;
	}
	if (now.tv_sec < 0 || now.tv_nsec < 0 ||
	    now.tv_nsec >= NANOSECONDS_PER_SECOND) {
		if (error != NULL)
			error->system_error = EOVERFLOW;
		return MODEM_LOOP_CLOCK_ERROR;
	}
	seconds = (uint64_t)now.tv_sec;
	if (seconds > (UINT64_MAX - (uint64_t)now.tv_nsec) /
			      NANOSECONDS_PER_SECOND) {
		if (error != NULL)
			error->system_error = EOVERFLOW;
		return MODEM_LOOP_CLOCK_ERROR;
	}
	*now_ns = seconds * NANOSECONDS_PER_SECOND + (uint64_t)now.tv_nsec;
	return MODEM_LOOP_OK;
}

static enum modem_loop_result wait_for_work(
	struct modem_runtime *runtime, uint64_t wait_ns,
	struct modem_loop_error *error)
{
	struct pollfd terminal = {
		.fd = modem_runtime_terminal_poll_fd(runtime),
		.events = POLLIN,
	};
	struct timespec timeout;
	nfds_t descriptor_count = terminal.fd >= 0 ? 1 : 0;
	int result;

	if (wait_ns > MODEM_LOOP_MAXIMUM_WAIT_NS)
		wait_ns = MODEM_LOOP_MAXIMUM_WAIT_NS;
	timeout.tv_sec = 0;
	timeout.tv_nsec = (long)wait_ns;
	result = ppoll(descriptor_count != 0 ? &terminal : NULL,
		       descriptor_count, &timeout, NULL);
	if (result < 0) {
		if (errno == EINTR)
			return MODEM_LOOP_OK;
		if (error != NULL)
			error->system_error = errno;
		return MODEM_LOOP_WAIT_ERROR;
	}
	if (descriptor_count != 0 && (terminal.revents & POLLNVAL) != 0) {
		if (error != NULL)
			error->system_error = EBADF;
		return MODEM_LOOP_WAIT_ERROR;
	}
	return MODEM_LOOP_OK;
}

enum modem_loop_result modem_loop_run(
	struct modem_runtime *runtime, volatile sig_atomic_t *running,
	struct modem_loop_error *error)
{
	enum modem_loop_result result;
	enum modem_runtime_result runtime_result;
	uint64_t now_ns;

	clear_error(error);
	if (runtime == NULL || running == NULL)
		return MODEM_LOOP_INVALID_ARGUMENT;
	while (*running != 0) {
		result = monotonic_now(&now_ns, error);
		if (result != MODEM_LOOP_OK)
			return result;
		runtime_result = modem_runtime_step(
			runtime, now_ns, NULL, NULL, NULL);
		if (runtime_result != MODEM_RUNTIME_OK &&
		    runtime_result != MODEM_RUNTIME_IDLE) {
			if (error != NULL)
				error->runtime_result = runtime_result;
			return MODEM_LOOP_RUNTIME_ERROR;
		}
		if (*running == 0)
			break;
		result = monotonic_now(&now_ns, error);
		if (result != MODEM_LOOP_OK)
			return result;
		result = wait_for_work(
			runtime, modem_runtime_wait_ns(runtime, now_ns), error);
		if (result != MODEM_LOOP_OK)
			return result;
	}
	return MODEM_LOOP_OK;
}

const char *modem_loop_result_name(enum modem_loop_result result)
{
	switch (result) {
	case MODEM_LOOP_OK:
		return "ok";
	case MODEM_LOOP_INVALID_ARGUMENT:
		return "invalid argument";
	case MODEM_LOOP_CLOCK_ERROR:
		return "monotonic clock failed";
	case MODEM_LOOP_WAIT_ERROR:
		return "runtime wait failed";
	case MODEM_LOOP_RUNTIME_ERROR:
		return "modem runtime failed";
	}
	return "unknown loop result";
}
