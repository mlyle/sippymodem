// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V8_CALLING_H
#define SIPPYMODEM_V8_CALLING_H

#include "sippymodem_v8_ansam.h"
#include "sippymodem_v8_menu.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V8_CALLING_MAX_FRAGMENT_SAMPLES = 160,
	SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT_SAMPLES =
		12 * SIPPYMODEM_V8_ANSAM_SAMPLE_RATE,
	SIPPYMODEM_V8_ECHO_CONTROL_DWELL_SAMPLES =
		SIPPYMODEM_V8_ANSAM_SAMPLE_RATE,
};

enum sippymodem_v8_calling_state {
	SIPPYMODEM_V8_CALLING_WAIT_ANSWER_SIGNAL,
	SIPPYMODEM_V8_CALLING_ECHO_CONTROL_DWELL,
	SIPPYMODEM_V8_CALLING_READY_TO_EXCHANGE,
	SIPPYMODEM_V8_CALLING_FAILED,
};

enum sippymodem_v8_calling_result {
	SIPPYMODEM_V8_CALLING_OK,
	SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT,
	SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT,
};

struct sippymodem_v8_calling {
	struct sippymodem_v8_ansam_detector answer_signal;
	struct sippymodem_v8_menu local_menu;
	enum sippymodem_v8_calling_state state;
	enum sippymodem_v8_calling_result terminal_result;
	uint32_t state_samples;
};

enum sippymodem_v8_calling_result sippymodem_v8_calling_init(
	struct sippymodem_v8_calling *calling,
	const struct sippymodem_v8_menu *local_menu);
enum sippymodem_v8_calling_result sippymodem_v8_calling_process(
	struct sippymodem_v8_calling *calling, const int16_t *receive,
	int16_t *transmit, size_t count);
bool sippymodem_v8_calling_is_ready(
	const struct sippymodem_v8_calling *calling);

const char *sippymodem_v8_calling_result_name(
	enum sippymodem_v8_calling_result result);

#endif
