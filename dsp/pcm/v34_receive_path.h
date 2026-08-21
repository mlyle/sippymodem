// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_RECEIVE_PATH_H
#define V34_RECEIVE_PATH_H

#include "v34_equalizer.h"
#include "v34_phase3_profile.h"
#include "v34_receive_timing.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_RECEIVE_PATH_SAMPLE_RATE = V34_RECEIVE_TIMING_SAMPLE_RATE,
	V34_RECEIVE_PATH_TIMING_OUTPUTS = V34_RECEIVE_TIMING_OUTPUTS,
	V34_RECEIVE_PATH_EQUALIZER_INPUTS =
		V34_RECEIVE_TIMING_INTERVAL_SAMPLES,
	V34_RECEIVE_PATH_INPUT_CAPACITY =
		V34_RECEIVE_TIMING_INPUT_CAPACITY,
};

enum v34_receive_path_result {
	V34_RECEIVE_PATH_OK,
	V34_RECEIVE_PATH_SYMBOL_READY,
	V34_RECEIVE_PATH_INVALID_ARGUMENT,
	V34_RECEIVE_PATH_INVALID_STATE,
};

/* Preserve the fixed-symbol boundary for compatibility consumers.
 * New adaptive paths own v34_receive_timing directly so one equalizer can
 * remain continuous across protocol phases. */
struct v34_receive_path {
	struct v34_receive_timing timing;
	struct v34_equalizer equalizer;
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
