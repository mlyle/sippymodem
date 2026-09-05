// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_TIMING_CONTROL_H
#define V34_TIMING_CONTROL_H

#include "v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_TIMING_CONTROL_OBSERVATIONS = 7,
};

enum v34_timing_control_result {
	V34_TIMING_CONTROL_OK,
	V34_TIMING_CONTROL_INVALID_ARGUMENT,
	V34_TIMING_CONTROL_INVALID_STATE,
};

enum v34_timing_control_mode {
	/* Hold the most recently learned fractional-clock correction. */
	V34_TIMING_CONTROL_LOCKED,
	V34_TIMING_CONTROL_INITIAL_PHASE,
	V34_TIMING_CONTROL_FAST_PULL_IN,
	V34_TIMING_CONTROL_PULL_IN,
	V34_TIMING_CONTROL_WIDE_TRACK,
	V34_TIMING_CONTROL_MEDIUM_TRACK,
	V34_TIMING_CONTROL_NARROW_TRACK,
	V34_TIMING_CONTROL_FINE_TRACK,
	V34_TIMING_CONTROL_HOLD,
};

/* V.34 fractional symbol-clock control. The later timing-resampler owner
 * advances `phase` at the published increment and supplies one complete
 * timing-error observation group at each symbol boundary. */
struct v34_timing_control {
	uint16_t phase;
	uint16_t phase_increment;
	uint16_t phase_limit;
	uint16_t nominal_increment;
	int32_t phase_residue;
	int32_t integrator;
	int16_t accumulated_delta;
	uint16_t accumulated_count;
	int16_t offset_ppm;
	uint16_t average_limit;
	uint16_t state_count;
	int16_t timeout;
	int16_t proportional_gain;
	int16_t integral_gain;
	uint8_t early_index;
	uint8_t late_index;
	uint16_t nominal_symbol_rate;
	enum v34_station_role local_role;
	enum v34_timing_control_mode mode;
	bool full_pull_in;
	bool feedback_held;
	bool initialized;
};

/* Initialize a nominal four-observations-per-symbol clock in locked mode.
 * Symbol rate remains an exact fraction. */
enum v34_timing_control_result v34_timing_control_init(
	struct v34_timing_control *control, uint32_t sample_rate_hz,
	uint32_t symbol_rate_numerator, uint32_t symbol_rate_denominator,
	enum v34_station_role local_role);
bool v34_timing_control_is_valid(
	const struct v34_timing_control *control);

/* Start timing acquisition at a symbol boundary. Full pull-in traverses the
 * complete acquisition schedule; a retained clock resumes at narrow track. */
enum v34_timing_control_result v34_timing_control_begin_acquisition(
	struct v34_timing_control *control, bool full_pull_in);

/* Freeze or resume a data-mode clock that has naturally reached HOLD without
 * changing its learned phase, increment, integrator, or low-gain profile.
 * Earlier acquisition/tracking modes continue unchanged. */
enum v34_timing_control_result v34_timing_control_set_data_frozen(
	struct v34_timing_control *control, bool frozen);

/* Temporarily suppress timing-error feedback and acquisition-schedule
 * progress when no usable signal is present. Keep the learned frequency,
 * mode and gains; processing still advances its fractional increment from
 * the integrator and residue. Release resumes the same acquisition epoch;
 * initialization or explicit new acquisition clears the temporary hold. */
enum v34_timing_control_result v34_timing_control_set_feedback_held(
	struct v34_timing_control *control, bool held);

/* Consume one complete timing-error group and publish the increment for the
 * next symbol interval. Rejected calls leave the owner and output unchanged. */
enum v34_timing_control_result v34_timing_control_process(
	struct v34_timing_control *control,
	const int16_t observations[V34_TIMING_CONTROL_OBSERVATIONS],
	uint16_t *phase_increment);

#endif /* V34_TIMING_CONTROL_H */
