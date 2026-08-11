// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_TRANSMITTER_H
#define V34_DATA_TRANSMITTER_H

#include "v34_data_line_transmitter.h"
#include "v34_mapping_frame_transmitter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v34_data_transmitter_result {
	V34_DATA_TRANSMITTER_OK,
	V34_DATA_TRANSMITTER_MAPPING_FRAME_REQUIRED,
	V34_DATA_TRANSMITTER_MAPPING_FRAME_PENDING,
	V34_DATA_TRANSMITTER_INVALID_ARGUMENT,
	V34_DATA_TRANSMITTER_OUT_OF_RANGE,
	V34_DATA_TRANSMITTER_CLIPPING_RISK,
	V34_DATA_TRANSMITTER_NUMERIC_FAILURE,
};

struct v34_data_transmitter {
	struct v34_frame_schedule schedule;
	struct v34_scrambler scrambler;
	struct v34_4d_encoder mapping_encoder;
	struct v34_data_line_transmitter line;
};

/* Own the continuously evolving mapping and line state. Calibration copies
 * its inputs; initialization leaves every input and the destination unchanged
 * on failure. */
enum v34_data_transmitter_result
v34_data_transmitter_init(
	struct v34_data_transmitter *transmitter,
	const struct v34_frame_schedule *initial_schedule,
	const struct v34_scrambler *initial_scrambler,
	const struct v34_4d_encoder *initial_mapping_encoder,
	const struct v34_power_normalizer *normalizer,
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, int16_t nominal_peak_amplitude);

bool v34_data_transmitter_is_valid(
	const struct v34_data_transmitter *transmitter);
bool v34_data_transmitter_needs_mapping_frame(
	const struct v34_data_transmitter *transmitter);

/* Describe the next frame's exact primary-bit and auxiliary-bit requirements
 * without changing state. A queued frame reports MAPPING_FRAME_PENDING and
 * leaves the destination unchanged. */
enum v34_data_transmitter_result
v34_data_transmitter_preview_mapping_frame(
	const struct v34_data_transmitter *transmitter,
	struct v34_mapping_frame *mapping_frame);

/* Generate and queue one mapping frame atomically. A completed data-frame
 * schedule advances automatically, including superframe wraparound. */
enum v34_data_transmitter_result
v34_data_transmitter_queue_mapping_frame(
	struct v34_data_transmitter *transmitter,
	const uint8_t *primary_bits, size_t primary_bit_count,
	const uint8_t *auxiliary_bit);

/* Emit one calibrated sample. Missing-frame backpressure and all failures
 * leave the transmitter and both outputs unchanged. */
enum v34_data_transmitter_result
v34_data_transmitter_sample(
	struct v34_data_transmitter *transmitter,
	int16_t *sample, bool *symbol_accepted);

const char *v34_data_transmitter_result_name(
	enum v34_data_transmitter_result result);

#endif
