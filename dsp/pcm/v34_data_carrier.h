// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_CARRIER_H
#define V34_DATA_CARRIER_H

#include "v34_phase3_profile.h"
#include "v34_pulse_modulator.h"
#include "v34_symbol_power.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_data_carrier_result {
	V34_DATA_CARRIER_OK,
	V34_DATA_CARRIER_INVALID_ARGUMENT,
	V34_DATA_CARRIER_NUMERIC_FAILURE,
};

struct v34_data_carrier {
	struct v34_phase3_profile profile;
	struct v34_frame_geometry geometry;
	struct v34_power_normalizer normalizer;
	struct v34_pulse_modulator modulator;
	double point_gain;
};

/* Configure the exact negotiated symbol and carrier clocks. The returned
 * waveform includes the continuous pulse, carrier, and negotiated
 * pre-emphasis response; only calibrated line gain remains downstream. */
enum v34_data_carrier_result v34_data_carrier_init(
	struct v34_data_carrier *carrier,
	const struct v34_phase3_profile *profile,
	const struct v34_frame_geometry *geometry,
	const struct v34_power_normalizer *normalizer,
	uint32_t sample_rate);

/* Adopt a compatible active pulse stream without resetting its carrier phase,
 * symbol clock, or retained pulses. point_gain applies only to data points
 * accepted after the handoff; points already in the modulator are unchanged.
 * The active modulator and destination remain unchanged on failure. */
enum v34_data_carrier_result v34_data_carrier_continue(
	struct v34_data_carrier *carrier,
	const struct v34_pulse_modulator *active_modulator,
	const struct v34_phase3_profile *profile,
	const struct v34_frame_geometry *geometry,
	const struct v34_power_normalizer *normalizer,
	double point_gain);

bool v34_data_carrier_is_valid(
	const struct v34_data_carrier *carrier);
bool v34_data_carrier_needs_point(
	const struct v34_data_carrier *carrier);

/* Emit one unscaled, pre-emphasized passband sample. Supply a normalized data
 * point precisely when needs_point() is true and NULL otherwise. */
enum v34_data_carrier_result v34_data_carrier_sample(
	struct v34_data_carrier *carrier,
	const struct qam_point *point, double *sample,
	bool *symbol_accepted);

#endif
