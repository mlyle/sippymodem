// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V32_LINE_STARTUP_H
#define SIPPYMODEM_V32_LINE_STARTUP_H

#include "sippymodem_single_tone.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V32_ANSWER_TONE_HZ = 2100,
	SIPPYMODEM_V32_CALLING_CARRIER_HZ = 1800,
	SIPPYMODEM_V32_ANSWER_GUARD_SAMPLES =
		SIPPYMODEM_SINGLE_TONE_SAMPLE_RATE,
	SIPPYMODEM_V32_MIN_REVERSAL_SPACING_SAMPLES =
		SIPPYMODEM_SINGLE_TONE_SAMPLE_RATE * 30 / 1000,
};

enum sippymodem_v32_line_startup_state {
	SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE,
	SIPPYMODEM_V32_LINE_ANSWER_GUARD,
	SIPPYMODEM_V32_LINE_SEND_CALLING_CARRIER,
	SIPPYMODEM_V32_LINE_SEND_REVERSED_CALLING_CARRIER,
	SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE_END,
	SIPPYMODEM_V32_LINE_STARTUP_COMPLETE,
	SIPPYMODEM_V32_LINE_STARTUP_FAILED,
};

enum sippymodem_v32_line_startup_result {
	SIPPYMODEM_V32_LINE_STARTUP_OK,
	SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT,
	SIPPYMODEM_V32_LINE_STARTUP_PROTOCOL_ERROR,
};

struct sippymodem_v32_line_startup {
	struct sippymodem_single_tone_detector answer_tone;
	struct sippymodem_single_tone_transmitter calling_carrier;
	uint32_t state_samples;
	enum sippymodem_v32_line_startup_state state;
};

enum sippymodem_v32_line_startup_result sippymodem_v32_line_startup_init(
	struct sippymodem_v32_line_startup *startup,
	int16_t transmit_amplitude);
bool sippymodem_v32_line_startup_is_valid(
	const struct sippymodem_v32_line_startup *startup);
bool sippymodem_v32_line_startup_is_complete(
	const struct sippymodem_v32_line_startup *startup);

enum sippymodem_v32_line_startup_result
sippymodem_v32_line_startup_process(
	struct sippymodem_v32_line_startup *startup, int16_t receive_sample,
	int16_t *transmit_sample, bool *complete);

const char *sippymodem_v32_line_startup_state_name(
	enum sippymodem_v32_line_startup_state state);
const char *sippymodem_v32_line_startup_result_name(
	enum sippymodem_v32_line_startup_result result);

#endif
