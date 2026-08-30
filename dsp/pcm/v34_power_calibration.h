// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_POWER_CALIBRATION_H
#define V34_POWER_CALIBRATION_H

#include "v34_data_carrier.h"
#include "v34_mapping_frame_transmitter.h"
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
};

/* Measure a generated data stream twice: first at the exact Q7 precoder
 * boundary, then after nonlinear projection using that raw mean. The selected
 * normative rate level supplies the line-profile gain, corrected so nonlinear
 * projection preserves the same mean power. One complete schedule period is
 * visited four times from the supplied starting position.
 * The input schedule, scrambler, and encoder are copied and remain unchanged;
 * the destination is also unchanged on failure. */
enum v34_power_calibration_result
v34_calibrate_mapping_power(
	const struct v34_frame_schedule *initial_schedule,
	const struct v34_scrambler *initial_scrambler,
	const struct v34_4d_encoder *initial_encoder,
	enum v34_nonlinear_parameter nonlinear_parameter,
	struct v34_power_normalizer *normalizer);

/* Raw post-pre-emphasis power for the generated calibration stream. The
 * measurement deliberately excludes nominal line gain and the negotiated
 * power reduction so one result can calibrate either requested line level. */
struct v34_line_power_measurement {
	double mean_squared_amplitude;
	double peak_amplitude;
	uint32_t sample_count;
};

bool v34_line_power_measurement_is_valid(
	const struct v34_line_power_measurement *measurement);

/* Replay the deterministic mapping calibration stream through the complete
 * normalized transmit waveform. One superframe is discarded for pulse and
 * pre-emphasis warm-up; the remaining three are measured online.
 * All inputs and the destination remain unchanged on failure. */
enum v34_power_calibration_result
v34_calibrate_line_power(
	const struct v34_frame_schedule *initial_schedule,
	const struct v34_scrambler *initial_scrambler,
	const struct v34_4d_encoder *initial_encoder,
	const struct v34_power_normalizer *normalizer,
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate,
	struct v34_line_power_measurement *measurement);

#endif
