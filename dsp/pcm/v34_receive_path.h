// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_RECEIVE_PATH_H
#define V34_RECEIVE_PATH_H

#include "v34_analytic_filter.h"
#include "v34_carrier_demodulator.h"
#include "v34_equalizer.h"
#include "v34_gain_control.h"
#include "v34_phase3_profile.h"
#include "v34_timing_control.h"
#include "v34_timing_filter.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_RECEIVE_PATH_SAMPLE_RATE = 8000,
	V34_RECEIVE_PATH_TIMING_OUTPUTS = 4,
	V34_RECEIVE_PATH_EQUALIZER_INPUTS = 2,
	V34_RECEIVE_PATH_INPUT_CAPACITY = 4,
};

enum v34_receive_path_result {
	V34_RECEIVE_PATH_OK,
	V34_RECEIVE_PATH_SYMBOL_READY,
	V34_RECEIVE_PATH_INVALID_ARGUMENT,
	V34_RECEIVE_PATH_INVALID_STATE,
};

/*
 * Own the persistent fixed-point V.34 receive front end from one 8-kHz line
 * sample through one equalized complex symbol. The four timing observations
 * and two equalizer inputs are retained across arbitrary caller fragments.
 */
struct v34_receive_path {
	struct v34_analytic_filter analytic;
	struct v34_gain_control gain;
	struct v34_carrier_demodulator carrier;
	struct v34_timing_filter timing_filter;
	struct v34_timing_control timing;
	struct v34_equalizer equalizer;
	struct v34_phase3_profile transmitting_profile;
	struct v34_analytic_sample input[V34_RECEIVE_PATH_INPUT_CAPACITY];
	struct v34_analytic_sample previous;
	struct v34_analytic_sample current;
	int16_t timing_observations[V34_TIMING_CONTROL_OBSERVATIONS];
	uint8_t input_read;
	uint8_t input_count;
	uint8_t input_advance_pending;
	uint8_t timing_output_count;
	bool current_valid;
	bool initialized;
};

enum v34_receive_path_result v34_receive_path_init(
	struct v34_receive_path *path,
	const struct v34_phase3_profile *transmitting_profile);
bool v34_receive_path_is_valid(const struct v34_receive_path *path);

/*
 * Consume one signed 8-kHz line sample. SYMBOL_READY publishes exactly one
 * equalized symbol; OK leaves the output unchanged. Rejected calls leave the
 * owner, output, and ready flag unchanged.
 */
enum v34_receive_path_result v34_receive_path_sample(
	struct v34_receive_path *path, int16_t sample,
	struct v34_analytic_sample *symbol, bool *ready);

#endif /* V34_RECEIVE_PATH_H */
