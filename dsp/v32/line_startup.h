// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V32_LINE_STARTUP_H
#define SIPPYMODEM_V32_LINE_STARTUP_H

#include "single_tone.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V32_ANSWER_TONE_HZ = 2100,
	SIPPYMODEM_V32_CALLING_CARRIER_HZ = 1800,
	SIPPYMODEM_V32_LOW_TURNAROUND_TONE_HZ = 600,
	SIPPYMODEM_V32_HIGH_TURNAROUND_TONE_HZ = 3000,
	SIPPYMODEM_V32_STARTUP_SYMBOL_RATE = 2400,
	SIPPYMODEM_V32_ANSWER_GUARD_SAMPLES =
		SINGLE_TONE_SAMPLE_RATE,
	/* The caller changes carrier state 64 +/- 2 symbols after the first
	 * received turnaround-tone phase reversal. */
	SIPPYMODEM_V32_REVERSAL_RESPONSE_SAMPLES =
		(SINGLE_TONE_SAMPLE_RATE * 64 +
		 SIPPYMODEM_V32_STARTUP_SYMBOL_RATE / 2) /
		SIPPYMODEM_V32_STARTUP_SYMBOL_RATE,
	SIPPYMODEM_V32_REVERSAL_RESPONSE_TOLERANCE_SAMPLES =
		(SINGLE_TONE_SAMPLE_RATE * 2 +
		 SIPPYMODEM_V32_STARTUP_SYMBOL_RATE - 1) /
		SIPPYMODEM_V32_STARTUP_SYMBOL_RATE,
};

enum sippymodem_v32_line_startup_state {
	SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE,
	SIPPYMODEM_V32_LINE_ANSWER_GUARD,
	SIPPYMODEM_V32_LINE_SEND_CALLING_CARRIER,
	SIPPYMODEM_V32_LINE_DELAY_CALLING_CARRIER_REVERSAL,
	SIPPYMODEM_V32_LINE_SEND_REVERSED_CALLING_CARRIER,
	SIPPYMODEM_V32_LINE_STARTUP_COMPLETE,
	SIPPYMODEM_V32_LINE_STARTUP_FAILED,
};

enum sippymodem_v32_line_startup_result {
	SIPPYMODEM_V32_LINE_STARTUP_OK,
	SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT,
	SIPPYMODEM_V32_LINE_STARTUP_PROTOCOL_ERROR,
};

struct sippymodem_v32_line_startup {
	struct single_tone_detector answer_tone;
	struct single_tone_detector low_turnaround_tone;
	struct single_tone_detector high_turnaround_tone;
	struct single_tone_transmitter calling_carrier;
	uint32_t state_samples;
	uint32_t round_trip_samples;
	uint16_t selected_turnaround_tone_hz;
	enum sippymodem_v32_line_startup_state state;
};

enum sippymodem_v32_line_startup_result sippymodem_v32_line_startup_init(
	struct sippymodem_v32_line_startup *startup,
	int16_t transmit_amplitude);
bool sippymodem_v32_line_startup_is_valid(
	const struct sippymodem_v32_line_startup *startup);
bool sippymodem_v32_line_startup_is_complete(
	const struct sippymodem_v32_line_startup *startup);
uint32_t sippymodem_v32_line_startup_round_trip_samples(
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
