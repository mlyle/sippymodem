// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_TRAINING_LINE_H
#define V34_TRAINING_LINE_H

#include "qam.h"
#include "v34_phase3_profile.h"
#include "v34_preemphasis.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_TRAINING_LINE_PULSE_SPAN_SYMBOLS = 14,
	V34_TRAINING_LINE_PULSE_TAIL_SYMBOLS =
		V34_TRAINING_LINE_PULSE_SPAN_SYMBOLS,
	V34_TRAINING_LINE_FILTER_TAIL_SAMPLES =
		V34_PREEMPHASIS_TAPS - 1,
};

enum v34_training_line_result {
	V34_TRAINING_LINE_OK,
	V34_TRAINING_LINE_INVALID_ARGUMENT,
	V34_TRAINING_LINE_OUT_OF_RANGE,
	V34_TRAINING_LINE_CLIPPING_RISK,
	V34_TRAINING_LINE_NUMERIC_FAILURE,
};

/* One continuous Phase 3/4 training waveform. The carrier, pulse shaper,
 * pre-emphasis filter, and sample clock remain owned here across protocol
 * section boundaries. S/S-bar supplies the deterministic line-power
 * calibration, but is not retained as transmit state. */
struct v34_training_line {
	struct v34_phase3_profile profile;
	struct qam_modulator carrier;
	struct v34_preemphasis preemphasis;
	double output_gain;
	double raw_rms;
	double raw_peak;
	double target_rms;
	uint64_t generated_samples;
	int16_t nominal_peak_amplitude;
};

/* Derive or validate the exact Phase 3/4 training waveform without creating
 * mutable line state. The destination is unchanged on failure. */
enum v34_training_line_result v34_training_line_channel(
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, struct qam_channel *channel);
bool v34_training_line_channel_matches(
	const struct v34_phase3_profile *profile,
	const struct qam_channel *channel);

enum v34_training_line_result v34_training_line_init(
	struct v34_training_line *line,
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, int16_t nominal_peak_amplitude);

bool v34_training_line_is_valid(
	const struct v34_training_line *line);
bool v34_training_line_needs_point(
	const struct v34_training_line *line);

/* Emit exactly one line sample. Supply a point precisely when needs_point()
 * is true and NULL otherwise. Successful calls report whether that point was
 * accepted; all rejected calls leave state and outputs unchanged. */
enum v34_training_line_result v34_training_line_sample(
	struct v34_training_line *line,
	const struct qam_point *point, int16_t *sample,
	bool *point_accepted);

const char *v34_training_line_result_name(
	enum v34_training_line_result result);

#endif
