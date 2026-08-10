// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_NONLINEAR_ENCODER_H
#define SIPPYMODEM_V34_NONLINEAR_ENCODER_H

#include "sippymodem_v34_precoder.h"

#include <stdbool.h>

enum sippymodem_v34_nonlinear_parameter {
	SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
	SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16,
};

enum sippymodem_v34_nonlinear_encoder_result {
	SIPPYMODEM_V34_NONLINEAR_ENCODER_OK,
	SIPPYMODEM_V34_NONLINEAR_ENCODER_INVALID_ARGUMENT,
	SIPPYMODEM_V34_NONLINEAR_ENCODER_NUMERIC_FAILURE,
};

bool sippymodem_v34_nonlinear_parameter_is_valid(
	enum sippymodem_v34_nonlinear_parameter parameter);

/* Apply the clause 9.7 radial projection to one exact Q7 precoder output.
 * mean_squared_magnitude is E[|x|^2] in unscaled constellation-grid units;
 * the returned point uses the same units. Absolute transmit-power
 * normalization remains the responsibility of the modulation owner. The
 * destination is unchanged when an argument or intermediate is rejected. */
enum sippymodem_v34_nonlinear_encoder_result
sippymodem_v34_nonlinear_encode(
	enum sippymodem_v34_nonlinear_parameter parameter,
	double mean_squared_magnitude,
	const struct sippymodem_v34_q7_point *precoded_symbol,
	struct sippymodem_qam_point *projected_symbol);

const char *sippymodem_v34_nonlinear_encoder_result_name(
	enum sippymodem_v34_nonlinear_encoder_result result);

#endif
