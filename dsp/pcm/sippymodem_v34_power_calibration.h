// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_POWER_CALIBRATION_H
#define SIPPYMODEM_V34_POWER_CALIBRATION_H

#include "sippymodem_v34_mapping_frame_transmitter.h"
#include "sippymodem_v34_symbol_power.h"

enum {
	/* Four schedule periods balance deterministic coverage with bounded
	 * startup work. No generated symbol stream is retained. */
	SIPPYMODEM_V34_POWER_CALIBRATION_SUPERFRAMES = 4,
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

const char *sippymodem_v34_power_calibration_result_name(
	enum sippymodem_v34_power_calibration_result result);

#endif
