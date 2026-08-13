// SPDX-License-Identifier: BSD-3-Clause

#ifndef FRACTIONAL_DELAY_H
#define FRACTIONAL_DELAY_H

#include <stdbool.h>
#include <stdint.h>

enum {
	FRACTIONAL_DELAY_TAPS = 31,
	FRACTIONAL_DELAY_CENTER = FRACTIONAL_DELAY_TAPS / 2,
	FRACTIONAL_DELAY_HALF_SAMPLE_Q16 = 0x8000,
	FRACTIONAL_DELAY_ONE_SAMPLE_Q16 = 0x10000,
};

enum fractional_delay_result {
	FRACTIONAL_DELAY_OK,
	FRACTIONAL_DELAY_INVALID_ARGUMENT,
	FRACTIONAL_DELAY_INVALID_STATE,
	FRACTIONAL_DELAY_OUT_OF_RANGE,
	FRACTIONAL_DELAY_NUMERIC_FAILURE,
};

/*
 * A band-limited fractional delay with a fixed fifteen-sample group delay.
 * Positive phase delays the stream; negative phase advances it.  Changing
 * phase preserves the sample history so one owner can maintain a continuous
 * waveform across a protocol boundary.
 */
struct fractional_delay {
	int32_t coefficients_q15[FRACTIONAL_DELAY_TAPS];
	int16_t history[FRACTIONAL_DELAY_TAPS];
	int32_t phase_q16;
	uint8_t history_position;
};

void fractional_delay_init(struct fractional_delay *delay);
bool fractional_delay_is_valid(const struct fractional_delay *delay);
enum fractional_delay_result fractional_delay_set_phase(
	struct fractional_delay *delay,
	int32_t phase_q16);
enum fractional_delay_result fractional_delay_process(
	struct fractional_delay *delay,
	int16_t input,
	int16_t *output);
const char *fractional_delay_result_name(
	enum fractional_delay_result result);

#endif
