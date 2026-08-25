// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PULSE_MODULATOR_H
#define V34_PULSE_MODULATOR_H

#include "qam.h"
#include "v34_phase3_profile.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PULSE_TAPS = 32,
	V34_PREEMPHASIS_TAPS = 16,
	V34_PREEMPHASIS_MAX_FILTER_INDEX = 10,
	V34_LINE_PULSE_SPAN_SYMBOLS = 14,
};

enum v34_line_channel_result {
	V34_LINE_CHANNEL_OK,
	V34_LINE_CHANNEL_INVALID_ARGUMENT,
	V34_LINE_CHANNEL_OUT_OF_RANGE,
};

/* Derive or validate the common Phase 3, Phase 4, and data waveform without
 * creating mutable line state. The destination is unchanged on failure. */
enum v34_line_channel_result v34_line_channel_init(
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, struct qam_channel *channel);
bool v34_line_channel_matches(
	const struct v34_phase3_profile *profile,
	const struct qam_channel *channel);

enum v34_pulse_modulator_result {
	V34_PULSE_MODULATOR_OK,
	V34_PULSE_MODULATOR_INVALID_ARGUMENT,
	V34_PULSE_MODULATOR_NUMERIC_FAILURE,
};

/* One exact rational V.34 pulse, carrier, and negotiated pre-emphasis clock.
 * The accepted symbols and line-filter history remain in this owner so
 * training and data share one continuous waveform. */
struct v34_pulse_modulator {
	struct qam_channel channel;
	int16_t in_phase_history[V34_PULSE_TAPS];
	int16_t quadrature_history[V34_PULSE_TAPS];
	int16_t preemphasis_history[V34_PREEMPHASIS_TAPS];
	uint64_t symbol_period_units;
	uint64_t symbol_phase_units;
	uint64_t generated_samples;
	uint32_t carrier_phase;
	uint32_t carrier_phase_step;
	double maximum_point_power;
	enum v34_symbol_rate symbol_rate;
	uint8_t preemphasis_filter_index;
};

enum v34_pulse_modulator_result v34_pulse_modulator_init(
	struct v34_pulse_modulator *modulator,
	const struct v34_phase3_profile *profile,
	const struct qam_channel *channel, double maximum_point_power);

bool v34_pulse_modulator_is_valid(
	const struct v34_pulse_modulator *modulator);
bool v34_pulse_modulator_needs_point(
	const struct v34_pulse_modulator *modulator);

/* Emit one unscaled, pre-emphasized passband sample. Supply a finite, in-range
 * point exactly when needs_point() is true. Rejected calls preserve owner and
 * outputs. */
enum v34_pulse_modulator_result v34_pulse_modulator_sample(
	struct v34_pulse_modulator *modulator,
	const struct qam_point *point, double *sample,
	bool *point_accepted);

#endif
