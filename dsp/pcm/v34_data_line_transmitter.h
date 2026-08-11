// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_H
#define SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_H

#include "v34_power_calibration.h"
#include "v34_preemphasis.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_data_line_transmitter_result {
	SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK,
	SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_MAPPING_FRAME_REQUIRED,
	SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_MAPPING_FRAME_PENDING,
	SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT,
	SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE,
	SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK,
	SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE,
};

struct sippymodem_v34_data_line_transmitter {
	struct sippymodem_v34_data_carrier carrier;
	struct sippymodem_v34_preemphasis preemphasis;
	struct sippymodem_v34_line_power_measurement calibration;
	double output_gain;
	double target_rms;
	uint64_t generated_samples;
	int16_t nominal_peak_amplitude;
};

/* Calibrate the normalized data stream at the negotiated line shape, then
 * select the gain whose RMS equals an equal-power sinusoid at the nominal
 * peak amplitude after applying the negotiated power reduction. Inputs and
 * the destination remain unchanged on failure. */
enum sippymodem_v34_data_line_transmitter_result
sippymodem_v34_data_line_transmitter_init(
	struct sippymodem_v34_data_line_transmitter *transmitter,
	const struct sippymodem_v34_frame_schedule *calibration_schedule,
	const struct sippymodem_v34_scrambler *calibration_scrambler,
	const struct sippymodem_v34_4d_encoder *calibration_encoder,
	const struct sippymodem_v34_power_normalizer *normalizer,
	const struct sippymodem_v34_phase3_profile *profile,
	uint32_t sample_rate, int16_t nominal_peak_amplitude);

bool sippymodem_v34_data_line_transmitter_is_valid(
	const struct sippymodem_v34_data_line_transmitter *transmitter);
bool sippymodem_v34_data_line_transmitter_needs_mapping_frame(
	const struct sippymodem_v34_data_line_transmitter *transmitter);

enum sippymodem_v34_data_line_transmitter_result
sippymodem_v34_data_line_transmitter_load_mapping_frame(
	struct sippymodem_v34_data_line_transmitter *transmitter,
	const struct sippymodem_v34_transmitted_mapping_frame *frame);

/* Emit one rounded line sample. Backpressure, clipping, and errors leave the
 * transmitter and both outputs unchanged; samples are never saturated. */
enum sippymodem_v34_data_line_transmitter_result
sippymodem_v34_data_line_transmitter_sample(
	struct sippymodem_v34_data_line_transmitter *transmitter,
	int16_t *sample, bool *symbol_accepted);

const char *sippymodem_v34_data_line_transmitter_result_name(
	enum sippymodem_v34_data_line_transmitter_result result);

#endif
