// SPDX-License-Identifier: BSD-3-Clause

#ifndef V8_LINE_CONDITIONER_H
#define V8_LINE_CONDITIONER_H

#include <stdbool.h>
#include <stdint.h>

enum v8_station_role {
	V8_CALLING_STATION,
	V8_ANSWERING_STATION,
};

enum v8_line_gain_mode {
	V8_LINE_GAIN_FROZEN,
	V8_LINE_GAIN_FINE,
	V8_LINE_GAIN_COARSE,
};

enum v8_line_conditioner_result {
	V8_LINE_CONDITIONER_OK,
	V8_LINE_CONDITIONER_INVALID_ARGUMENT,
};

enum {
	V8_LINE_BLOCK_SAMPLES = 4,
	V8_LINE_INPUT_QUEUE_SAMPLES = V8_LINE_BLOCK_SAMPLES + 2,
	V8_LINE_ENTRANCE_FILTER_TAPS = 34,
	V8_LINE_POWER_HISTORY_SAMPLES = 30,
};

struct v8_line_conditioned_block {
	int16_t samples[V8_LINE_BLOCK_SAMPLES];
	bool gain_reset;
};

struct v8_line_conditioner {
	int16_t entrance_history[V8_LINE_ENTRANCE_FILTER_TAPS];
	int16_t input_queue[V8_LINE_INPUT_QUEUE_SAMPLES];
	uint16_t power_history[V8_LINE_POWER_HISTORY_SAMPLES];
	uint16_t filtered_energy;
	uint16_t gain;
	uint16_t gain_delta;
	uint16_t post_filter_word;
	uint16_t power_index;
	uint16_t input_count;
	uint16_t overflow_count;
	enum v8_station_role role;
	enum v8_line_gain_mode gain_mode;
};

enum v8_line_conditioner_result v8_line_conditioner_init(
	struct v8_line_conditioner *conditioner,
	enum v8_station_role role);
bool v8_line_conditioner_is_valid(
	const struct v8_line_conditioner *conditioner);
enum v8_line_conditioner_result v8_line_conditioner_set_gain_mode(
	struct v8_line_conditioner *conditioner,
	enum v8_line_gain_mode mode);
enum v8_line_conditioner_result v8_line_conditioner_process_sample(
	struct v8_line_conditioner *conditioner, int16_t sample,
	struct v8_line_conditioned_block *block, bool *ready);

#endif
