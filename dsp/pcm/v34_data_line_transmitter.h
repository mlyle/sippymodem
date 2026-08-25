// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_LINE_TRANSMITTER_H
#define V34_DATA_LINE_TRANSMITTER_H

#include "v34_power_calibration.h"
#include "v34_training_line.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_data_line_transmitter_result {
	V34_DATA_LINE_TRANSMITTER_OK,
	V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT,
	V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE,
	V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK,
	V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE,
};

struct v34_data_line_transmitter {
	struct v34_data_carrier carrier;
	struct v34_line_power_measurement calibration;
	double gain_reference_rms;
	double output_gain;
	double target_rms;
	uint64_t generated_samples;
	int16_t nominal_peak_amplitude;
};

/* Calibrate the normalized data stream at the negotiated line shape, then
 * select the gain whose RMS equals an equal-power sinusoid at the nominal
 * peak amplitude after applying the negotiated power reduction. Inputs and
 * the destination remain unchanged on failure. */
enum v34_data_line_transmitter_result
v34_data_line_transmitter_init(
	struct v34_data_line_transmitter *transmitter,
	const struct v34_frame_schedule *calibration_schedule,
	const struct v34_scrambler *calibration_scrambler,
	const struct v34_4d_encoder *calibration_encoder,
	const struct v34_power_normalizer *normalizer,
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, int16_t nominal_peak_amplitude);

/* Continue a compatible active training line into normalized data. The
 * carrier clock, active pulses, pre-emphasis history, line gain, and sample
 * count are retained. B1 therefore begins on the trained constellation scale;
 * calibration is used only to reject a data waveform that could clip. */
enum v34_data_line_transmitter_result
v34_data_line_transmitter_continue(
	struct v34_data_line_transmitter *transmitter,
	const struct v34_training_line *active_line,
	const struct v34_frame_schedule *calibration_schedule,
	const struct v34_scrambler *calibration_scrambler,
	const struct v34_4d_encoder *calibration_encoder,
	const struct v34_power_normalizer *normalizer);

bool v34_data_line_transmitter_is_valid(
	const struct v34_data_line_transmitter *transmitter);
bool v34_data_line_transmitter_needs_point(
	const struct v34_data_line_transmitter *transmitter);

/* Emit one rounded line sample. Backpressure, clipping, and errors leave the
 * transmitter and both outputs unchanged; samples are never saturated.
 * Supply a normalized point precisely when needs_point() is true. */
enum v34_data_line_transmitter_result
v34_data_line_transmitter_sample(
	struct v34_data_line_transmitter *transmitter,
	const struct qam_point *point,
	int16_t *sample, bool *symbol_accepted);

#endif
