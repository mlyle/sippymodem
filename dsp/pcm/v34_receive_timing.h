// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_RECEIVE_TIMING_H
#define V34_RECEIVE_TIMING_H

#include "v34_analytic_filter.h"
#include "v34_carrier_demodulator.h"
#include "v34_gain_control.h"
#include "v34_phase3_profile.h"
#include "v34_s_tone_detector.h"
#include "v34_timing_control.h"
#include "v34_timing_filter.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_RECEIVE_TIMING_SAMPLE_RATE = 8000,
	V34_RECEIVE_TIMING_OUTPUTS = 4,
	V34_RECEIVE_TIMING_INTERVAL_SAMPLES = 2,
	V34_RECEIVE_TIMING_INPUT_CAPACITY = 4,
};

enum v34_receive_timing_result {
	V34_RECEIVE_TIMING_OK,
	V34_RECEIVE_TIMING_SAMPLES_READY,
	V34_RECEIVE_TIMING_INVALID_ARGUMENT,
	V34_RECEIVE_TIMING_INVALID_STATE,
};

struct v34_receive_timing_samples {
	struct v34_analytic_sample sample[
		V34_RECEIVE_TIMING_INTERVAL_SAMPLES];
	uint8_t count;
	bool symbol_complete;
};

/* Own the persistent fixed-point V.34 receive front end from one 8-kHz line
 * sample through the two recovered complex samples in one symbol interval.
 * Equalization and semantic decisions deliberately remain outside this owner
 * so one adaptive equalizer can span training and mapped data. */
struct v34_receive_timing {
	struct v34_analytic_filter analytic;
	struct v34_gain_control gain;
	struct v34_s_tone_detector s_detector;
	struct v34_carrier_demodulator carrier;
	struct v34_timing_filter timing_filter;
	struct v34_timing_control timing;
	struct v34_phase3_profile transmitting_profile;
	struct v34_analytic_sample input[V34_RECEIVE_TIMING_INPUT_CAPACITY];
	struct v34_analytic_sample previous;
	struct v34_analytic_sample current;
	int16_t timing_observations[V34_TIMING_CONTROL_OBSERVATIONS];
	uint8_t input_read;
	uint8_t input_count;
	uint8_t input_advance_pending;
	uint8_t timing_output_count;
	bool current_valid;
	/* Treat lower-threshold carrier pull-in as provisional until the first
	 * fully qualified S boundary. */
	bool reanchor_at_s;
	bool initialized;
};

/* REANCHOR_AT_S preserves early carrier pull-in, but starts the fractional
 * timing acquisition epoch again when S becomes fully qualified. */
enum v34_receive_timing_result v34_receive_timing_init(
	struct v34_receive_timing *timing,
	const struct v34_phase3_profile *transmitting_profile,
	int16_t initial_gain, bool reanchor_at_s);
bool v34_receive_timing_is_valid(
	const struct v34_receive_timing *timing);
bool v34_receive_timing_is_acquired(
	const struct v34_receive_timing *timing);
bool v34_receive_timing_matches_profile(
	const struct v34_receive_timing *timing,
	const struct v34_phase3_profile *transmitting_profile);

/* Freeze the AGC at a qualified training boundary. */
enum v34_receive_timing_result v34_receive_timing_freeze_gain(
	struct v34_receive_timing *timing);

/* Preserve the trained sample epoch and enter the data-mode tracking profile. */
enum v34_receive_timing_result v34_receive_timing_enter_hold(
	struct v34_receive_timing *timing);

enum v34_receive_timing_result v34_receive_timing_set_data_frozen(
	struct v34_receive_timing *timing, bool frozen);

/* Consume one line sample. SAMPLES_READY publishes the one or two recovered
 * samples produced before this call exhausts its input or completes a symbol
 * interval. symbol_complete identifies the exact second-sample boundary. OK
 * leaves the output unchanged. Invalid calls leave owner and output unchanged.
 */
enum v34_receive_timing_result v34_receive_timing_sample(
	struct v34_receive_timing *timing, int16_t sample,
	struct v34_receive_timing_samples *samples);

#endif /* V34_RECEIVE_TIMING_H */
