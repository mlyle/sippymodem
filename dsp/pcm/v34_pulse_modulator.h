// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PULSE_MODULATOR_H
#define V34_PULSE_MODULATOR_H

#include "qam.h"
#include "v34_phase3_profile.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PULSE_TAPS = 32,
};

enum v34_pulse_modulator_result {
	V34_PULSE_MODULATOR_OK,
	V34_PULSE_MODULATOR_INVALID_ARGUMENT,
	V34_PULSE_MODULATOR_NUMERIC_FAILURE,
};

/* One exact rational V.34 pulse and carrier clock. The 32 accepted symbols
 * remain in this owner so training and data can share one continuous line. */
struct v34_pulse_modulator {
	struct qam_channel channel;
	struct qam_point history[V34_PULSE_TAPS];
	uint64_t symbol_period_units;
	uint64_t symbol_phase_units;
	uint64_t generated_samples;
	uint32_t carrier_phase;
	uint32_t carrier_phase_step;
	double maximum_point_power;
	int16_t amplitude;
	enum v34_symbol_rate symbol_rate;
};

enum v34_pulse_modulator_result v34_pulse_modulator_init(
	struct v34_pulse_modulator *modulator,
	const struct v34_phase3_profile *profile,
	const struct qam_channel *channel, int16_t amplitude,
	double maximum_point_power);

bool v34_pulse_modulator_is_valid(
	const struct v34_pulse_modulator *modulator);
bool v34_pulse_modulator_needs_point(
	const struct v34_pulse_modulator *modulator);

/* Emit one unscaled passband sample. Supply a finite, in-range point exactly
 * when needs_point() is true. Rejected calls preserve owner and outputs. */
enum v34_pulse_modulator_result v34_pulse_modulator_sample(
	struct v34_pulse_modulator *modulator,
	const struct qam_point *point, double *sample,
	bool *point_accepted);

#endif
