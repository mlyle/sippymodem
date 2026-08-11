// SPDX-License-Identifier: BSD-3-Clause

#ifndef MODEM_LOOP_H
#define MODEM_LOOP_H

#include "runtime.h"

#include <signal.h>

enum modem_loop_result {
	MODEM_LOOP_OK,
	MODEM_LOOP_INVALID_ARGUMENT,
	MODEM_LOOP_CLOCK_ERROR,
	MODEM_LOOP_WAIT_ERROR,
	MODEM_LOOP_RUNTIME_ERROR,
};

struct modem_loop_error {
	enum modem_runtime_result runtime_result;
	int system_error;
};

/* Run bounded runtime steps until running becomes zero or an error occurs. */
enum modem_loop_result modem_loop_run(
	struct modem_runtime *runtime,
	volatile sig_atomic_t *running,
	struct modem_loop_error *error);
const char *modem_loop_result_name(enum modem_loop_result result);

#endif
