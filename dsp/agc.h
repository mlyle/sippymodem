// SPDX-License-Identifier: BSD-3-Clause

#ifndef AGC_H
#define AGC_H

#include "integer_math.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Q10 sample gain with pre-rounding clipping to +/-32512 outside the signed
 * 26-bit product range. Inside that range, add rounding (0 or 512), shift,
 * and retain the low 16 bits. If supplied, clipped is set on every call. */
static inline int16_t agc_scale_q10(
	int16_t gain, int16_t sample, uint32_t rounding, bool *clipped)
{
	int32_t product = (int32_t)gain * sample;
	bool overflow = product < -INT32_C(33554432) ||
			product >= INT32_C(33554432);

	if (clipped != NULL)
		*clipped = false;
	if (!overflow)
		return wrap_s16_from_s32(arithmetic_shift_right_u32(
			(uint32_t)product + rounding, 10));
	if (clipped != NULL)
		*clipped = true;
	return product < 0 ? -32512 : 32512;
}

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

/*
 * Exact fixed-point level measurement shared by gain and timing gates.
 * Count is 1..UINT16_MAX.  samples and level may not overlap; rejection
 * leaves level unchanged.
 */
enum agc_result agc_measure_level_s16(
	const int16_t *samples, size_t count, int16_t *level);

/* input and output may be identical; other overlap is rejected. */
enum agc_result agc_process(
	struct agc *control, const int16_t *input, size_t count,
	int16_t *output);

const char *agc_result_name(enum agc_result result);

#endif
