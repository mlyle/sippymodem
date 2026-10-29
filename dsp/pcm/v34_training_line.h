// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_TRAINING_LINE_H
#define V34_TRAINING_LINE_H

#include "qam.h"
#include "v34_phase3_profile.h"
#include "v34_pulse_modulator.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* Point zero in the pulse owner's Q12 domain and in the fixed modem
	 * arithmetic.  Data-rate scaling uses the same coordinate bridge. */
	V34_TRAINING_PULSE_POINT_ZERO_Q12 = 2896,
	V34_TRAINING_FIXED_POINT_ZERO = 0x11e3,
	/* The ordinary V.34 profile maps this policy value to the fixed
	 * transmit scale used by the modem arithmetic. */
	V34_TRAINING_LINE_REFERENCE_AMPLITUDE = 3792,
	V34_TRAINING_LINE_PULSE_TAIL_SYMBOLS =
		V34_PULSE_TAPS,
	V34_TRAINING_LINE_FILTER_TAIL_SAMPLES =
		V34_PREEMPHASIS_TAPS - 1,
	/* The slowest admitted clock is 2400 symbols/s at 8 kHz. */
	V34_TRAINING_LINE_MAX_TAIL_SAMPLES =
		((V34_TRAINING_LINE_PULSE_TAIL_SYMBOLS + 1u) * 8000u +
		 2399u) / 2400u + V34_TRAINING_LINE_FILTER_TAIL_SAMPLES,
};

enum v34_training_line_result {
	V34_TRAINING_LINE_OK,
	V34_TRAINING_LINE_INVALID_ARGUMENT,
	V34_TRAINING_LINE_OUT_OF_RANGE,
	V34_TRAINING_LINE_CLIPPING_RISK,
	V34_TRAINING_LINE_NUMERIC_FAILURE,
};

/* One continuous V.34 transmit waveform. The pulse/carrier line owner, line
 * gain, and sample clock remain here across training, B1, and data. S/S-bar
 * supplies the deterministic line-power calibration, but is not retained as
 * transmit state. */
struct v34_training_line {
	struct v34_phase3_profile profile;
	struct v34_pulse_modulator carrier;
	double output_gain;
	double raw_rms;
	double raw_peak;
	double target_rms;
	uint64_t generated_samples;
	int16_t nominal_peak_amplitude;
};

enum v34_training_line_result v34_training_line_init(
	struct v34_training_line *line,
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, int16_t nominal_peak_amplitude);

bool v34_training_line_is_valid(
	const struct v34_training_line *line);
bool v34_training_line_needs_point(
	const struct v34_training_line *line);

/* Conservative 8-kHz flush bound for a valid profile: retain the current
 * accepted point, then emit zero points through the pulse and line filters.
 * Protocol owners retain their own tail counters and completion policy. */
uint16_t v34_training_line_tail_samples(
	const struct v34_phase3_profile *profile);

/* Emit exactly one line sample. Supply a point precisely when needs_point()
 * is true and NULL otherwise. Successful calls report whether that point was
 * accepted; all rejected calls leave state and outputs unchanged. Owner,
 * supplied point, sample and acceptance flag must occupy disjoint regions. */
enum v34_training_line_result v34_training_line_sample(
	struct v34_training_line *line,
	const struct qam_point *point, int16_t *sample,
	bool *point_accepted);

#endif
