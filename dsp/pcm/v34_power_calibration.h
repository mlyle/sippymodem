// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_POWER_CALIBRATION_H
#define V34_POWER_CALIBRATION_H

#include "v34_mapping_frame_transmitter.h"
#include "v34_data_points.h"
#include "v34_pulse_modulator.h"
#include "v34_phase3_profile.h"
#include "v34_symbol_power.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* Four schedule periods balance deterministic coverage with bounded
	 * startup work. No generated symbol stream is retained. */
	V34_POWER_CALIBRATION_SUPERFRAMES = 4,
	V34_LINE_POWER_WARMUP_SUPERFRAMES = 1,
};

enum v34_power_calibration_result {
	V34_POWER_CALIBRATION_OK,
	V34_POWER_CALIBRATION_INVALID_ARGUMENT,
	V34_POWER_CALIBRATION_OUT_OF_RANGE,
	V34_POWER_CALIBRATION_NUMERIC_FAILURE,
	V34_POWER_CALIBRATION_IN_PROGRESS,
};

/* Resumable, instance-owned calibration. Begin copies its inputs; thereafter
 * only step may mutate the job. Each step charges at most frame_budget frames,
 * preserving completion cadence even when linear power need not be measured.
 * Outputs change only on completion; discard the job after a terminal failure. */
struct v34_mapping_power_calibration {
	struct v34_frame_schedule initial_schedule, schedule;
	struct v34_scrambler initial_scrambler, scrambler;
	struct v34_4d_encoder initial_encoder, encoder;
	struct v34_symbol_power_measurement precoded, projected;
	uint32_t random_state, frame, frame_count;
	enum v34_nonlinear_parameter nonlinear_parameter;
	bool measuring_projected;
	bool linear_bounded;
};

/* Measure a generated data stream at the exact Q7 precoder boundary, then
 * after nonlinear projection using that raw mean. A linear job with a proven
 * precoder range skips generation and reports unmeasured power; other jobs
 * retain generated-stream range validation. The selected rate profile supplies
 * the gain, corrected so nonlinear projection preserves the same mean power.
 * Generated jobs visit four complete schedule periods from the starting position.
 * The input schedule, scrambler, and encoder are copied and remain unchanged;
 * the normalizer changes only on successful completion. */
enum v34_power_calibration_result v34_mapping_power_calibration_begin(
	struct v34_mapping_power_calibration *calibration,
	const struct v34_frame_schedule *schedule,
	const struct v34_scrambler *scrambler,
	const struct v34_4d_encoder *encoder,
	enum v34_nonlinear_parameter nonlinear_parameter);
enum v34_power_calibration_result v34_mapping_power_calibration_step(
	struct v34_mapping_power_calibration *calibration,
	uint32_t frame_budget, struct v34_power_normalizer *normalizer);

/* Raw post-pre-emphasis power for the generated calibration stream. The
 * measurement deliberately excludes nominal line gain and the negotiated
 * power reduction so one result can calibrate either requested line level. */
struct v34_line_power_measurement {
	double mean_squared_amplitude;
	double peak_amplitude;
	uint32_t sample_count;
};

struct v34_line_power_calibration {
	struct v34_data_points points;
	struct v34_pulse_modulator carrier;
	struct v34_line_power_measurement measured;
	uint64_t maximum_frame_samples;
	uint32_t random_state, frame, frame_count, warmup_frames;
};

/* Replay the deterministic mapping calibration stream through the complete
 * normalized transmit waveform. One superframe is discarded for pulse and
 * pre-emphasis warm-up; the remaining three are measured online.
 * Inputs are copied; measurement changes only on successful completion. */
enum v34_power_calibration_result v34_line_power_calibration_begin(
	struct v34_line_power_calibration *calibration,
	const struct v34_frame_schedule *schedule,
	const struct v34_scrambler *scrambler,
	const struct v34_4d_encoder *encoder,
	const struct v34_power_normalizer *normalizer,
	const struct v34_phase3_profile *profile, uint32_t sample_rate);
enum v34_power_calibration_result v34_line_power_calibration_step(
	struct v34_line_power_calibration *calibration,
	uint32_t frame_budget, struct v34_line_power_measurement *measurement);

bool v34_line_power_measurement_is_valid(
	const struct v34_line_power_measurement *measurement);

#endif
