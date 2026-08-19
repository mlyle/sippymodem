// SPDX-License-Identifier: BSD-3-Clause

#ifndef AGC_H
#define AGC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum agc_result {
	AGC_OK,
	AGC_INVALID_ARGUMENT,
	AGC_INVALID_STATE,
};

enum agc_gain_mode {
	AGC_GAIN_TRACKING,
	AGC_GAIN_FROZEN,
};

struct agc_config {
	int16_t target_level_q15;
	int16_t acquisition_threshold;
	int16_t tracking_threshold;
	uint16_t block_samples;
	int16_t history_weight_q15;
	int16_t input_weight_q15;
};

/*
 * Fixed-point block gain control.  Every process call consumes exactly one
 * configured block.  The owner retains its trained gain across freeze and
 * reacquire transitions; reacquire clears only measurement evidence.
 */
struct agc {
	struct agc_config config;
	int16_t smoothed_level;
	int16_t gain_mantissa_q15;
	int16_t gain_shift;
	enum agc_gain_mode gain_mode;
	bool signal_present;
};

enum agc_result agc_init(
	struct agc *control, const struct agc_config *config);
bool agc_is_valid(const struct agc *control);
enum agc_result agc_freeze(struct agc *control);
enum agc_result agc_reacquire(struct agc *control);

/* input and output may be identical; other overlap is rejected. */
enum agc_result agc_process(
	struct agc *control, const int16_t *input, size_t count,
	int16_t *output);

const char *agc_result_name(enum agc_result result);

#endif
