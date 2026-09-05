// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_GAIN_CONTROL_H
#define V34_GAIN_CONTROL_H

#include "v34_analytic_filter.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_GAIN_HISTORY_SAMPLES = 36,
	V34_GAIN_UPDATE_SAMPLES = 4,
	V34_GAIN_FRACTION_BITS = 10,
};

enum v34_gain_control_result {
	V34_GAIN_CONTROL_OK,
	V34_GAIN_CONTROL_INVALID_ARGUMENT,
	V34_GAIN_CONTROL_INVALID_STATE,
};

enum v34_gain_mode {
	V34_GAIN_TRACKING,
	V34_GAIN_FROZEN,
};

/* V.34 analytic-sample gain ownership. */
struct v34_gain_control {
	int16_t history[V34_GAIN_HISTORY_SAMPLES];
	uint32_t energy_accumulator;
	uint16_t history_index;
	uint16_t energy_count;
	uint16_t average_energy;
	int16_t gain; /* Nonnegative Q5.10 sample multiplier. */
	int16_t correction;
	enum v34_gain_mode mode;
	bool initialized;
};

enum v34_gain_control_result v34_gain_control_init(
	struct v34_gain_control *control, int16_t initial_gain);
bool v34_gain_control_is_valid(
	const struct v34_gain_control *control);
enum v34_gain_control_result v34_gain_control_set_mode(
	struct v34_gain_control *control, enum v34_gain_mode mode);

/* Refresh an insufficient gain from the owned 36-sample raw analytic history
 * at a caller-qualified carrier acquisition boundary. Raise only below the
 * existing energy deadband, round to the nearest Q10 gain, and retain the
 * ordinary increase ceiling. Quiet history, sufficient gain, and gain already
 * above the ceiling are unchanged. History, correction clock, learned energy,
 * and tracking/frozen mode are always preserved; invalid state is rejected
 * without mutation. Ordinary sample processing never invokes this policy. */
enum v34_gain_control_result v34_gain_control_refresh_level(
	struct v34_gain_control *control);

/* Scale one analytic sample and advance the four-sample correction clock.
 * Rejected calls leave the owner and output unchanged. */
enum v34_gain_control_result v34_gain_control_process(
	struct v34_gain_control *control,
	struct v34_analytic_sample input,
	struct v34_analytic_sample *output);

#endif /* V34_GAIN_CONTROL_H */
