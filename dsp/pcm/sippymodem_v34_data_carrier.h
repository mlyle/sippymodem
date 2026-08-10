// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_DATA_CARRIER_H
#define SIPPYMODEM_V34_DATA_CARRIER_H

#include "sippymodem_v34_mapping_frame_transmitter.h"
#include "sippymodem_v34_phase3_profile.h"
#include "sippymodem_v34_symbol_power.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_DATA_PULSE_SPAN_SYMBOLS = 8,
};

enum sippymodem_v34_data_carrier_result {
	SIPPYMODEM_V34_DATA_CARRIER_OK,
	SIPPYMODEM_V34_DATA_CARRIER_MAPPING_FRAME_REQUIRED,
	SIPPYMODEM_V34_DATA_CARRIER_MAPPING_FRAME_PENDING,
	SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT,
	SIPPYMODEM_V34_DATA_CARRIER_OUT_OF_RANGE,
	SIPPYMODEM_V34_DATA_CARRIER_NUMERIC_FAILURE,
};

struct sippymodem_v34_data_carrier {
	struct sippymodem_v34_phase3_profile profile;
	struct sippymodem_v34_frame_geometry geometry;
	struct sippymodem_v34_power_normalizer normalizer;
	struct sippymodem_qam_modulator modulator;
	struct sippymodem_qam_point
		normalized_symbol[SIPPYMODEM_V34_SYMBOLS_PER_MAPPING_FRAME];
	uint8_t next_symbol;
};

/* Configure the exact negotiated symbol and carrier clocks. The returned
 * waveform is a raw floating-point carrier; negotiated line gain and
 * pre-emphasis remain separate downstream responsibilities. */
enum sippymodem_v34_data_carrier_result sippymodem_v34_data_carrier_init(
	struct sippymodem_v34_data_carrier *carrier,
	const struct sippymodem_v34_phase3_profile *profile,
	const struct sippymodem_v34_frame_geometry *geometry,
	const struct sippymodem_v34_power_normalizer *normalizer,
	uint32_t sample_rate);

bool sippymodem_v34_data_carrier_is_valid(
	const struct sippymodem_v34_data_carrier *carrier);
bool sippymodem_v34_data_carrier_needs_mapping_frame(
	const struct sippymodem_v34_data_carrier *carrier);

/* Normalize all eight projected symbols atomically. A second frame is
 * rejected until the queued frame has been consumed. */
enum sippymodem_v34_data_carrier_result
sippymodem_v34_data_carrier_load_mapping_frame(
	struct sippymodem_v34_data_carrier *carrier,
	const struct sippymodem_v34_transmitted_mapping_frame *frame);

/* Emit one unscaled passband sample. When a symbol boundary needs data and
 * no mapping frame is queued, no carrier state or output is changed. */
enum sippymodem_v34_data_carrier_result sippymodem_v34_data_carrier_sample(
	struct sippymodem_v34_data_carrier *carrier, double *sample,
	bool *symbol_accepted);

const char *sippymodem_v34_data_carrier_result_name(
	enum sippymodem_v34_data_carrier_result result);

#endif
