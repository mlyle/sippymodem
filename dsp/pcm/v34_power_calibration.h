// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_POWER_CALIBRATION_H
#define SIPPYMODEM_V34_POWER_CALIBRATION_H

#include "v34_data_carrier.h"
#include "v34_mapping_frame_transmitter.h"
#include "v34_symbol_power.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* Four schedule periods balance deterministic coverage with bounded
	 * startup work. No generated symbol stream is retained. */
	SIPPYMODEM_V34_POWER_CALIBRATION_SUPERFRAMES = 4,
	SIPPYMODEM_V34_LINE_POWER_WARMUP_SUPERFRAMES = 1,
};

enum sippymodem_v34_power_calibration_result {
	SIPPYMODEM_V34_POWER_CALIBRATION_OK,
	SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT,
	SIPPYMODEM_V34_POWER_CALIBRATION_OUT_OF_RANGE,
	SIPPYMODEM_V34_POWER_CALIBRATION_NUMERIC_FAILURE,
};

/* Measure a generated data stream twice: first at the exact Q7 precoder
 * boundary, then after nonlinear projection using that raw mean. One complete
 * schedule period is visited four times from the supplied starting position.
 * The input schedule, scrambler, and encoder are copied and remain unchanged;
 * the destination is also unchanged on failure. */
enum sippymodem_v34_power_calibration_result
sippymodem_v34_calibrate_mapping_power(
	const struct sippymodem_v34_frame_schedule *initial_schedule,
	const struct sippymodem_v34_scrambler *initial_scrambler,
	const struct sippymodem_v34_4d_encoder *initial_encoder,
	enum sippymodem_v34_nonlinear_parameter nonlinear_parameter,
	struct sippymodem_v34_power_normalizer *normalizer);

/* Raw post-pre-emphasis power for the generated calibration stream. The
 * measurement deliberately excludes nominal line gain and the negotiated
 * power reduction so one result can calibrate either requested line level. */
struct sippymodem_v34_line_power_measurement {
	double mean_squared_amplitude;
	double peak_amplitude;
	uint32_t sample_count;
};

bool sippymodem_v34_line_power_measurement_is_valid(
	const struct sippymodem_v34_line_power_measurement *measurement);

/* Replay the deterministic mapping calibration stream through the normalized
 * data carrier and selected pre-emphasis filter. One superframe is discarded
 * for pulse-shaper and FIR warm-up; the remaining three are measured online.
 * All inputs and the destination remain unchanged on failure. */
enum sippymodem_v34_power_calibration_result
sippymodem_v34_calibrate_line_power(
	const struct sippymodem_v34_frame_schedule *initial_schedule,
	const struct sippymodem_v34_scrambler *initial_scrambler,
	const struct sippymodem_v34_4d_encoder *initial_encoder,
	const struct sippymodem_v34_power_normalizer *normalizer,
	const struct sippymodem_v34_phase3_profile *profile,
	uint32_t sample_rate,
	struct sippymodem_v34_line_power_measurement *measurement);

const char *sippymodem_v34_power_calibration_result_name(
	enum sippymodem_v34_power_calibration_result result);

#endif
