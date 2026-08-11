// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_CARRIER_H
#define V34_DATA_CARRIER_H

#include "v34_mapping_frame_transmitter.h"
#include "v34_phase3_profile.h"
#include "v34_symbol_power.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_DATA_PULSE_SPAN_SYMBOLS = 8,
};

enum v34_data_carrier_result {
	V34_DATA_CARRIER_OK,
	V34_DATA_CARRIER_MAPPING_FRAME_REQUIRED,
	V34_DATA_CARRIER_MAPPING_FRAME_PENDING,
	V34_DATA_CARRIER_INVALID_ARGUMENT,
	V34_DATA_CARRIER_OUT_OF_RANGE,
	V34_DATA_CARRIER_NUMERIC_FAILURE,
};

struct v34_data_carrier {
	struct v34_phase3_profile profile;
	struct v34_frame_geometry geometry;
	struct v34_power_normalizer normalizer;
	struct qam_modulator modulator;
	struct qam_point
		normalized_symbol[V34_SYMBOLS_PER_MAPPING_FRAME];
	uint8_t next_symbol;
};

/* Derive the shared transmit/receive waveform from an exact Phase 3 profile.
 * The destination is unchanged when the profile is invalid or its occupied
 * band does not fit below the selected sample rate's Nyquist frequency. */
enum v34_data_carrier_result v34_data_carrier_channel(
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, struct qam_channel *channel);

/* Configure the exact negotiated symbol and carrier clocks. The returned
 * waveform is a raw floating-point carrier; negotiated line gain and
 * pre-emphasis remain separate downstream responsibilities. */
enum v34_data_carrier_result v34_data_carrier_init(
	struct v34_data_carrier *carrier,
	const struct v34_phase3_profile *profile,
	const struct v34_frame_geometry *geometry,
	const struct v34_power_normalizer *normalizer,
	uint32_t sample_rate);

bool v34_data_carrier_is_valid(
	const struct v34_data_carrier *carrier);
bool v34_data_carrier_needs_mapping_frame(
	const struct v34_data_carrier *carrier);

/* Normalize all eight projected symbols atomically. A second frame is
 * rejected until the queued frame has been consumed. */
enum v34_data_carrier_result
v34_data_carrier_load_mapping_frame(
	struct v34_data_carrier *carrier,
	const struct v34_transmitted_mapping_frame *frame);

/* Emit one unscaled passband sample. When a symbol boundary needs data and
 * no mapping frame is queued, no carrier state or output is changed. */
enum v34_data_carrier_result v34_data_carrier_sample(
	struct v34_data_carrier *carrier, double *sample,
	bool *symbol_accepted);

const char *v34_data_carrier_result_name(
	enum v34_data_carrier_result result);

#endif
