// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_SYMBOL_TIMING_H
#define V32_SYMBOL_TIMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V32_SYMBOL_TIMING_COEFFICIENTS = 180,
	V32_SYMBOL_TIMING_HISTORY = 18,
	V32_SYMBOL_TIMING_ERROR_STATE = 6,
	V32_SYMBOL_TIMING_STARTUP_SAMPLES = 9,
	V32_SYMBOL_TIMING_MAX_INPUT_SAMPLES = 36,
	V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES = 42,
};

enum v32_symbol_timing_mode {
	V32_SYMBOL_TIMING_ACQUISITION,
	V32_SYMBOL_TIMING_COARSE,
	V32_SYMBOL_TIMING_FINE,
};

enum v32_symbol_timing_result {
	V32_SYMBOL_TIMING_OK,
	V32_SYMBOL_TIMING_INVALID_ARGUMENT,
	V32_SYMBOL_TIMING_INVALID_STATE,
};

/* Persistent 7.2 kHz timing recovery; output remains three samples/symbol. */
struct v32_symbol_timing {
	int16_t interpolator[V32_SYMBOL_TIMING_COEFFICIENTS];
	int16_t sample_history[V32_SYMBOL_TIMING_HISTORY];
	int16_t error_state[V32_SYMBOL_TIMING_ERROR_STATE];
	int16_t startup_samples[V32_SYMBOL_TIMING_STARTUP_SAMPLES];
	int32_t detector_i_accumulator;
	int32_t detector_q_accumulator;
	int16_t loop_integral;
	int16_t smoothed_error;
	int16_t detector_level;
	int16_t fractional_phase_q11;
	int16_t polyphase_index;
	uint8_t history_index;
	uint8_t acquisition_groups;
	uint8_t detector_groups;
	uint8_t detector_sample;
	uint8_t samples_until_output;
	uint8_t startup_index;
	enum v32_symbol_timing_mode mode;
	bool signal_detected;
	bool tracking_enabled;
	bool startup_gate_enabled;
};

enum v32_symbol_timing_result v32_symbol_timing_init(
	struct v32_symbol_timing *timing);
bool v32_symbol_timing_is_valid(
	const struct v32_symbol_timing *timing);
enum v32_symbol_timing_result v32_symbol_timing_set_tracking(
	struct v32_symbol_timing *timing, bool enabled);

/* Rejected calls change no owner, output, or output_count. */
enum v32_symbol_timing_result v32_symbol_timing_process(
	struct v32_symbol_timing *timing,
	const int16_t *input, size_t count,
	int16_t *output, size_t output_capacity,
	size_t *output_count);

#endif
