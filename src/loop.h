// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_LOOP_H
#define SIPPYMODEM_LOOP_H

#include "runtime.h"

#include <signal.h>

enum sippymodem_loop_result {
	SIPPYMODEM_LOOP_OK,
	SIPPYMODEM_LOOP_INVALID_ARGUMENT,
	SIPPYMODEM_LOOP_CLOCK_ERROR,
	SIPPYMODEM_LOOP_WAIT_ERROR,
	SIPPYMODEM_LOOP_RUNTIME_ERROR,
};

struct sippymodem_loop_error {
	enum sippymodem_runtime_result runtime_result;
	int system_error;
};

/* Run bounded runtime steps until running becomes zero or an error occurs. */
enum sippymodem_loop_result sippymodem_loop_run(
	struct sippymodem_runtime *runtime,
	volatile sig_atomic_t *running,
	struct sippymodem_loop_error *error);
const char *sippymodem_loop_result_name(enum sippymodem_loop_result result);

#endif
